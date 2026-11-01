/*
 * paperdiy-reader — HAL implementation for the M5Stack PaperDIY (K159).
 *
 * The board is fully described by M5Unified/M5GFX (board_M5PaperDIY): the
 * 4.7" 960x540 16-grey ED047-class panel on an 8-bit parallel bus, the M5PM1
 * PMIC (battery, charge status, power-off), and buttons A=GPIO4 / B=GPIO3.
 *
 * SPDX-License-Identifier: MIT
 */
#include "hal/hal.h"

#include <cstdio>
#include <cstring>

#include "esp_log.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

static const char* TAG = "hal";

uint32_t millis_()
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

/* ------------------------------------------------------------------ */
/* Bring-up                                                            */
/* ------------------------------------------------------------------ */

void Hal::init()
{
    auto cfg          = M5.config();
    cfg.clear_display = false;
    cfg.internal_imu  = false;
    cfg.internal_mic  = false;
    cfg.internal_spk  = false;
    M5.begin(cfg);

    // Native portrait 540x960. Refreshes are driven explicitly per region
    // (render::push), never by endWrite().
    M5.Display.setRotation(0);
    M5.Display.setAutoDisplay(false);
    M5.Display.setEpdMode(epd_mode_t::epd_quality);

    // 8-bit greyscale working canvas in PSRAM: anti-aliased VLW glyphs blend
    // into it, and the panel driver reduces it to its 16 grey levels.
    Canvas = new M5Canvas(&M5.Display);
    Canvas->setPsram(true);
    Canvas->setColorDepth(lgfx::color_depth_t::grayscale_8bit);
    Canvas->createSprite(M5.Display.width(), M5.Display.height());

    M5.BtnA.setHoldThresh(600);
    M5.BtnB.setHoldThresh(600);

    ESP_LOGI(TAG, "hal ready: display %dx%d, canvas %dx%d, battery %d%%", (int)M5.Display.width(),
             (int)M5.Display.height(), (int)Canvas->width(), (int)Canvas->height(), batteryPercent());
}

void Hal::update()
{
    M5.update();
}

void Hal::getDeviceMac(uint8_t* mac)
{
    esp_read_mac(mac, ESP_MAC_EFUSE_FACTORY);
}

void Hal::getShortId(char* out, size_t out_size)
{
    uint8_t mac[6] = {0};
    getDeviceMac(mac);
    snprintf(out, out_size, "%02X%02X%02X", mac[3], mac[4], mac[5]);
}

int Hal::batteryPercent()
{
    int32_t level = M5.Power.getBatteryLevel();
    if (level < 0) {
        return -1;
    }
    return level > 100 ? 100 : (int)level;
}

bool Hal::charging()
{
    return M5.Power.isCharging() == m5::Power_Class::is_charging_t::is_charging;
}

bool Hal::usbPowered()
{
    return M5.Power.getVBUSVoltage() > 4000 || charging();
}

void Hal::powerOff()
{
    // M5PM1 cuts every rail; the e-paper keeps the last image.
    M5.Display.waitDisplay();
    while (true) {
        M5.Power.powerOff();
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

/* ------------------------------------------------------------------ */
/* Settings                                                            */
/* ------------------------------------------------------------------ */

void Hal::settingsDefaults()
{
    memset(&settings, 0, sizeof(settings));
    settings.font_size   = FONT_SIZE_3;
    settings.font_family = FONT_SERIF;
    settings.spacing     = SPACING_NORMAL;
    settings.margins     = MARGIN_NORMAL;
    settings.theme       = THEME_LIGHT;
    settings.timeout_min = 5;
    settings.full_every  = 8;
    settings.sleep_mode  = 6; /* random */
    settings.show_studio = 1;
}

void Hal::settingsLoad()
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    settingsDefaults();

    nvs_handle_t h;
    if (nvs_open(READER_NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        ESP_LOGI(TAG, "no stored settings, using defaults");
        return;
    }

    uint8_t v8 = 0;
    // "font2" replaces the old 3-step "font" key so old S/M/L values are not
    // misread as the new 5-step scale.
    if (nvs_get_u8(h, "font2", &v8) == ESP_OK && v8 < FONT_SIZE_COUNT) {
        settings.font_size = v8;
    }
    if (nvs_get_u8(h, "theme", &v8) == ESP_OK && v8 < THEME_COUNT) {
        settings.theme = v8;
    }
    if (nvs_get_u8(h, "timeout", &v8) == ESP_OK && v8 <= 120) {
        settings.timeout_min = v8;
    }
    if (nvs_get_u8(h, "fullevery", &v8) == ESP_OK && v8 <= 50) {
        settings.full_every = v8;
    }
    if (nvs_get_u8(h, "sort", &v8) == ESP_OK && v8 < 3) {
        settings.sort_mode = v8;
    }
    if (nvs_get_u8(h, "sleepmode", &v8) == ESP_OK && v8 < 8) {
        settings.sleep_mode = v8;
    }
    if (nvs_get_u8(h, "studio", &v8) == ESP_OK && v8 < 2) {
        settings.show_studio = v8;
    }
    if (nvs_get_u8(h, "family", &v8) == ESP_OK && v8 < FONT_FAMILY_COUNT) {
        settings.font_family = v8;
    }
    if (nvs_get_u8(h, "spacing", &v8) == ESP_OK && v8 < SPACING_COUNT) {
        settings.spacing = v8;
    }
    if (nvs_get_u8(h, "margins", &v8) == ESP_OK && v8 < MARGIN_COUNT) {
        settings.margins = v8;
    }

    size_t sz = sizeof(settings.sta_ssid);
    nvs_get_str(h, "sta_ssid", settings.sta_ssid, &sz);
    sz = sizeof(settings.sta_pass);
    nvs_get_str(h, "sta_pass", settings.sta_pass, &sz);

    nvs_close(h);

    ESP_LOGI(TAG, "settings: size=%u family=%u spacing=%u margins=%u theme=%u timeout=%u sta='%s'",
             settings.font_size, settings.font_family, settings.spacing, settings.margins, settings.theme,
             settings.timeout_min, settings.sta_ssid);
}

void Hal::settingsSave()
{
    nvs_handle_t h;
    if (nvs_open(READER_NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGW(TAG, "settingsSave: nvs_open failed");
        return;
    }

    nvs_set_u8(h, "font2", settings.font_size);
    nvs_set_u8(h, "fullevery", settings.full_every);
    nvs_set_u8(h, "family", settings.font_family);
    nvs_set_u8(h, "sort", settings.sort_mode);
    nvs_set_u8(h, "sleepmode", settings.sleep_mode);
    nvs_set_u8(h, "studio", settings.show_studio);
    nvs_set_u8(h, "spacing", settings.spacing);
    nvs_set_u8(h, "margins", settings.margins);
    nvs_set_u8(h, "theme", settings.theme);
    nvs_set_u8(h, "timeout", settings.timeout_min);
    nvs_set_str(h, "sta_ssid", settings.sta_ssid);
    nvs_set_str(h, "sta_pass", settings.sta_pass);

    nvs_commit(h);
    nvs_close(h);
}

/* ------------------------------------------------------------------ */
/* Reading progress                                                    */
/* ------------------------------------------------------------------ */

uint32_t Hal::pathHash(const char* path)
{
    // FNV-1a; stable across boots and firmware builds.
    uint32_t h = 2166136261u;
    if (!path) {
        return h;
    }
    for (const unsigned char* p = (const unsigned char*)path; *p; ++p) {
        h ^= (uint32_t)(*p);
        h *= 16777619u;
    }
    return h;
}

static void progress_key(const char* path, char* out, size_t out_size)
{
    snprintf(out, out_size, "p%08lx", (unsigned long)Hal::pathHash(path));
}

bool Hal::progressGet(const char* path, uint16_t* page, uint16_t* total_pages)
{
    if (page) {
        *page = 0;
    }
    if (total_pages) {
        *total_pages = 0;
    }
    if (!path || !path[0]) {
        return false;
    }

    nvs_handle_t h;
    if (nvs_open(READER_NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }

    char key[16];
    progress_key(path, key, sizeof(key));

    uint32_t packed = 0;
    esp_err_t err   = nvs_get_u32(h, key, &packed);
    nvs_close(h);

    if (err != ESP_OK) {
        return false;
    }
    if (page) {
        *page = (uint16_t)(packed & 0xFFFFu);
    }
    if (total_pages) {
        *total_pages = (uint16_t)((packed >> 16) & 0xFFFFu);
    }
    return true;
}

void Hal::progressSet(const char* path, uint16_t page, uint16_t total_pages)
{
    if (!path || !path[0]) {
        return;
    }

    nvs_handle_t h;
    if (nvs_open(READER_NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }

    char key[16];
    progress_key(path, key, sizeof(key));

    uint32_t packed = ((uint32_t)total_pages << 16) | (uint32_t)page;
    uint32_t prev   = 0;
    if (nvs_get_u32(h, key, &prev) != ESP_OK || prev != packed) {
        nvs_set_u32(h, key, packed);
        nvs_commit(h);
    }
    nvs_close(h);
}

void Hal::progressClear(const char* path)
{
    if (!path || !path[0]) {
        return;
    }

    nvs_handle_t h;
    if (nvs_open(READER_NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }

    char key[16];
    progress_key(path, key, sizeof(key));
    nvs_erase_key(h, key);
    key[0] = 'q';
    nvs_erase_key(h, key);
    key[0] = 'r';
    nvs_erase_key(h, key);
    nvs_commit(h);
    nvs_close(h);
}

bool Hal::positionGet(const char* path, uint32_t* off, uint32_t* word)
{
    if (!path || !path[0]) {
        return false;
    }
    nvs_handle_t h;
    if (nvs_open(READER_NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    char key[16];
    progress_key(path, key, sizeof(key));
    key[0]         = 'q';
    uint64_t packed = 0;
    esp_err_t err   = nvs_get_u64(h, key, &packed);
    nvs_close(h);
    if (err != ESP_OK) {
        return false;
    }
    if (off) {
        *off = (uint32_t)(packed >> 32);
    }
    if (word) {
        *word = (uint32_t)(packed & 0xFFFFFFFFu);
    }
    return true;
}

void Hal::positionSet(const char* path, uint32_t off, uint32_t word)
{
    if (!path || !path[0]) {
        return;
    }
    nvs_handle_t h;
    if (nvs_open(READER_NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    char key[16];
    progress_key(path, key, sizeof(key));
    key[0]          = 'q';
    uint64_t packed = ((uint64_t)off << 32) | (uint64_t)word;
    uint64_t prev   = 0;
    if (nvs_get_u64(h, key, &prev) != ESP_OK || prev != packed) {
        nvs_set_u64(h, key, packed);
        nvs_commit(h);
    }
    nvs_close(h);
}

void Hal::progressMove(const char* from, const char* to)
{
    if (!from || !to || !from[0] || !to[0] || strcmp(from, to) == 0) {
        return;
    }
    nvs_handle_t h;
    if (nvs_open(READER_NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    char kf[16];
    char kt[16];
    progress_key(from, kf, sizeof(kf));
    progress_key(to, kt, sizeof(kt));
    uint32_t v32 = 0;
    uint64_t v64 = 0;
    kf[0] = kt[0] = 'p';
    if (nvs_get_u32(h, kf, &v32) == ESP_OK) {
        nvs_set_u32(h, kt, v32);
        nvs_erase_key(h, kf);
    }
    kf[0] = kt[0] = 'q';
    if (nvs_get_u64(h, kf, &v64) == ESP_OK) {
        nvs_set_u64(h, kt, v64);
        nvs_erase_key(h, kf);
    }
    kf[0] = kt[0] = 'r';
    if (nvs_get_u32(h, kf, &v32) == ESP_OK) {
        nvs_set_u32(h, kt, v32);
        nvs_erase_key(h, kf);
    }
    char rpath[256];
    size_t sz = sizeof(rpath);
    if (nvs_get_str(h, "rpath", rpath, &sz) == ESP_OK && strcmp(rpath, from) == 0) {
        nvs_set_str(h, "rpath", to);
    }
    nvs_commit(h);
    nvs_close(h);
}

uint32_t Hal::recentGet(const char* path)
{
    if (!path || !path[0]) {
        return 0;
    }
    nvs_handle_t h;
    if (nvs_open(READER_NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return 0;
    }
    char key[16];
    progress_key(path, key, sizeof(key));
    key[0]       = 'r';
    uint32_t v   = 0;
    nvs_get_u32(h, key, &v);
    nvs_close(h);
    return v;
}

void Hal::recentTouch(const char* path)
{
    if (!path || !path[0]) {
        return;
    }
    nvs_handle_t h;
    if (nvs_open(READER_NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    char key[16];
    progress_key(path, key, sizeof(key));
    key[0]       = 'r';
    uint32_t seq = 0;
    nvs_get_u32(h, "rseq", &seq);
    uint32_t mine = 0;
    if (nvs_get_u32(h, key, &mine) != ESP_OK || mine != seq) {
        seq++;
        nvs_set_u32(h, "rseq", seq);
        nvs_set_u32(h, key, seq);
        nvs_commit(h);
    }
    nvs_close(h);
}

/* ------------------------------------------------------------------ */
/* Resume (NVS): the book to reopen at power-on                        */
/* ------------------------------------------------------------------ */

void Hal::resumeSave(const char* path)
{
    nvs_handle_t h;
    if (nvs_open(READER_NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        if (path && path[0]) {
            nvs_set_str(h, "rpath", path);
        } else {
            nvs_erase_key(h, "rpath");
        }
        nvs_commit(h);
        nvs_close(h);
    }
}

bool Hal::resumeLoad(char* path_out, size_t path_out_size)
{
    if (!path_out || path_out_size == 0) {
        return false;
    }
    path_out[0] = '\0';
    nvs_handle_t h;
    if (nvs_open(READER_NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    size_t sz     = path_out_size;
    esp_err_t err = nvs_get_str(h, "rpath", path_out, &sz);
    nvs_close(h);
    return err == ESP_OK && path_out[0];
}

void Hal::resumeClear()
{
    resumeSave(nullptr);
}
