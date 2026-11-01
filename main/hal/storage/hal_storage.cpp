/*
 * paperdiy-reader — storage HAL implementation.
 *
 * SPDX-License-Identifier: MIT
 */
#include "hal/storage/hal_storage.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <string.h>
#include <sys/stat.h>

#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdmmc_cmd.h"

static const char *TAG = "hal_storage";

/* PaperDIY microSD wiring (dedicated SPI bus, not shared with the panel). */
#define SD_SPI_HOST SPI2_HOST
#define SD_PIN_MOSI GPIO_NUM_38
#define SD_PIN_MISO GPIO_NUM_40
#define SD_PIN_SCLK GPIO_NUM_39
#define SD_PIN_CS   GPIO_NUM_47

#define INTERNAL_CACHE_DIR STORAGE_INTERNAL_BASE "/.cache"
#define SD_CACHE_DIR       STORAGE_SD_BOOKS "/.cache"

static wl_handle_t s_wl_handle    = WL_INVALID_HANDLE;
static bool s_internal_mounted    = false;
static sdmmc_card_t *s_sd_card    = NULL;
static bool s_sd_mounted          = false;
static bool s_spi_bus_inited      = false;
static SemaphoreHandle_t s_lock   = NULL;

/* ------------------------------------------------------------------ */
/* Locking                                                             */
/* ------------------------------------------------------------------ */

static void ensure_lock(void)
{
    if (!s_lock) {
        s_lock = xSemaphoreCreateRecursiveMutex();
    }
}

void hal_storage_lock(void)
{
    ensure_lock();
    if (s_lock) {
        xSemaphoreTakeRecursive(s_lock, portMAX_DELAY);
    }
}

void hal_storage_unlock(void)
{
    if (s_lock) {
        xSemaphoreGiveRecursive(s_lock);
    }
}

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

esp_err_t hal_storage_mkdir(const char *path)
{
    if (!path || !path[0]) {
        return ESP_ERR_INVALID_ARG;
    }

    struct stat st;
    if (stat(path, &st) == 0) {
        return ESP_OK;
    }
    if (mkdir(path, 0775) == 0) {
        return ESP_OK;
    }
    if (errno == EEXIST) {
        return ESP_OK;
    }

    ESP_LOGW(TAG, "mkdir(%s) failed: %s", path, strerror(errno));
    return ESP_FAIL;
}

/* ------------------------------------------------------------------ */
/* Internal FAT partition                                              */
/* ------------------------------------------------------------------ */

static esp_err_t mount_internal(void)
{
    if (s_internal_mounted) {
        return ESP_OK;
    }

    esp_vfs_fat_mount_config_t cfg = {};
    cfg.max_files                  = 6;
    cfg.format_if_mount_failed     = true;
    cfg.allocation_unit_size       = CONFIG_WL_SECTOR_SIZE;

    esp_err_t err = esp_vfs_fat_spiflash_mount_rw_wl(STORAGE_INTERNAL_BASE, "storage", &cfg, &s_wl_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "internal FAT mount failed: %s", esp_err_to_name(err));
        return err;
    }

    s_internal_mounted = true;
    hal_storage_mkdir(INTERNAL_CACHE_DIR);
    ESP_LOGI(TAG, "internal FAT mounted at %s", STORAGE_INTERNAL_BASE);
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* microSD                                                             */
/* ------------------------------------------------------------------ */

static esp_err_t spi_bus_init_once(void)
{
    if (s_spi_bus_inited) {
        return ESP_OK;
    }

    spi_bus_config_t bus_cfg = {};
    bus_cfg.mosi_io_num      = SD_PIN_MOSI;
    bus_cfg.miso_io_num      = SD_PIN_MISO;
    bus_cfg.sclk_io_num      = SD_PIN_SCLK;
    bus_cfg.quadwp_io_num    = -1;
    bus_cfg.quadhd_io_num    = -1;
    bus_cfg.max_transfer_sz  = 8192;

    esp_err_t err = spi_bus_initialize(SD_SPI_HOST, &bus_cfg, SDSPI_DEFAULT_DMA);
    if (err == ESP_OK || err == ESP_ERR_INVALID_STATE) {
        s_spi_bus_inited = true;
        return ESP_OK;
    }
    ESP_LOGE(TAG, "spi_bus_initialize failed: %s", esp_err_to_name(err));
    return err;
}

esp_err_t hal_storage_mount_sd(void)
{
    hal_storage_lock();

    if (s_sd_mounted) {
        hal_storage_unlock();
        return ESP_OK;
    }

    esp_err_t err = spi_bus_init_once();
    if (err != ESP_OK) {
        hal_storage_unlock();
        return err;
    }

    sdmmc_host_t host      = SDSPI_HOST_DEFAULT();
    host.slot              = SD_SPI_HOST;
    host.max_freq_khz      = 20000;

    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.host_id               = SD_SPI_HOST;
    slot_config.gpio_cs               = SD_PIN_CS;

    esp_vfs_fat_mount_config_t cfg = {};
    cfg.max_files                  = 6;
    cfg.format_if_mount_failed     = false;
    cfg.allocation_unit_size       = 16 * 1024;

    err = esp_vfs_fat_sdspi_mount(STORAGE_SD_BASE, &host, &slot_config, &cfg, &s_sd_card);
    if (err != ESP_OK) {
        // Retry once at a slower clock — some cards are unhappy at 20 MHz on
        // long or loose SD wiring.
        host.max_freq_khz = 10000;
        err               = esp_vfs_fat_sdspi_mount(STORAGE_SD_BASE, &host, &slot_config, &cfg, &s_sd_card);
    }

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SD mount failed: %s", esp_err_to_name(err));
        s_sd_card = NULL;
        hal_storage_unlock();
        return err;
    }

    s_sd_mounted = true;
    hal_storage_mkdir(STORAGE_SD_BOOKS);
    hal_storage_mkdir(SD_CACHE_DIR);
    ESP_LOGI(TAG, "SD mounted at %s (%lluMB)", STORAGE_SD_BASE,
             (unsigned long long)(((uint64_t)s_sd_card->csd.capacity) * s_sd_card->csd.sector_size / (1024 * 1024)));

    hal_storage_unlock();
    return ESP_OK;
}


bool hal_storage_sd_available(void)
{
    return s_sd_mounted;
}


esp_err_t hal_storage_copy_books_to_sd(int *copied, hal_storage_moved_cb on_moved, void *ctx)
{
    if (copied) {
        *copied = 0;
    }
    if (!s_sd_mounted || !s_internal_mounted) {
        return ESP_ERR_INVALID_STATE;
    }

    hal_storage_lock();
    hal_storage_mkdir(STORAGE_SD_BOOKS);

    DIR *d = opendir(STORAGE_INTERNAL_BASE);
    if (!d) {
        hal_storage_unlock();
        return ESP_FAIL;
    }

    /* Collect names first: removing entries while readdir() walks the
     * directory is undefined on FatFs. */
    char (*names)[256] = NULL;
    size_t count = 0, cap = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        const char *n = e->d_name;
        size_t len    = strlen(n);
        if (n[0] == '.' || len < 5 || len > 200 || e->d_type == DT_DIR) {
            continue;
        }
        if (strcasecmp(n + len - 5, ".epub") != 0 && strcasecmp(n + len - 4, ".txt") != 0) {
            continue;
        }
        if (count == cap) {
            size_t ncap = cap ? cap * 2 : 8;
            void *nn    = realloc(names, ncap * sizeof(*names));
            if (!nn) {
                break;
            }
            names = (char (*)[256])nn;
            cap   = ncap;
        }
        strlcpy(names[count++], n, 256);
    }
    closedir(d);

    esp_err_t result = ESP_OK;
    char *buf        = (char *)malloc(16 * 1024);
    for (size_t i = 0; buf && i < count; i++) {
        const char *n = names[i];
        char src[300];
        char dst[300];
        snprintf(src, sizeof(src), "%s/%s", STORAGE_INTERNAL_BASE, n);
        snprintf(dst, sizeof(dst), "%s/%s", STORAGE_SD_BOOKS, n);

        struct stat ss;
        struct stat ds;
        if (stat(src, &ss) != 0) {
            continue;
        }
        if (stat(dst, &ds) == 0) {
            /* A file of that name is already on the card: leave both alone
             * rather than guess which one is wanted. */
            ESP_LOGW(TAG, "not moving %s: the card already has a file of that name", n);
            continue;
        }

        FILE *in  = fopen(src, "rb");
        FILE *out = in ? fopen(dst, "wb") : NULL;
        bool ok   = in && out;
        while (ok) {
            size_t r = fread(buf, 1, 16 * 1024, in);
            if (r == 0) {
                ok = !ferror(in);
                break;
            }
            ok = fwrite(buf, 1, r, out) == r;
        }
        if (in) {
            fclose(in);
        }
        if (out && fclose(out) != 0) {
            ok = false;
        }
        if (ok) {
            /* Only trust the copy once its size matches the original. */
            ok = stat(dst, &ds) == 0 && ds.st_size == ss.st_size;
        }
        if (ok) {
            if (on_moved) {
                on_moved(src, dst, ctx);  /* migrate progress, drop old caches */
            }
            remove(src);
            ESP_LOGI(TAG, "moved %s -> SD", n);
            if (copied) {
                (*copied)++;
            }
        } else {
            ESP_LOGE(TAG, "copy failed: %s (internal copy kept)", n);
            remove(dst);
            result = ESP_FAIL;
        }
    }
    free(buf);
    free(names);
    hal_storage_unlock();
    return result;
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

esp_err_t hal_storage_init(void)
{
    ensure_lock();
    hal_storage_lock();

    esp_err_t err = mount_internal();

    // The SD-detect line is read through the PM1 by the caller; trying the
    // mount unconditionally is cheap and copes with a card inserted while the
    // detect switch is flaky.
    hal_storage_mount_sd();

    hal_storage_unlock();
    return err;
}

const char *hal_storage_books_dir(store_id_t store)
{
    if (store == STORE_INTERNAL) {
        return s_internal_mounted ? STORAGE_INTERNAL_BASE : NULL;
    }
    if (store == STORE_SD) {
        return s_sd_mounted ? STORAGE_SD_BOOKS : NULL;
    }
    return NULL;
}

const char *hal_storage_cache_dir(store_id_t store)
{
    if (store == STORE_INTERNAL) {
        return s_internal_mounted ? INTERNAL_CACHE_DIR : NULL;
    }
    if (store == STORE_SD) {
        return s_sd_mounted ? SD_CACHE_DIR : NULL;
    }
    return NULL;
}

const char *hal_storage_label(store_id_t store)
{
    return store == STORE_SD ? "SD card" : "Internal";
}

bool hal_storage_space(store_id_t store, uint64_t *total_bytes, uint64_t *free_bytes)
{
    const char *base = NULL;
    if (store == STORE_INTERNAL && s_internal_mounted) {
        base = STORAGE_INTERNAL_BASE;
    } else if (store == STORE_SD && s_sd_mounted) {
        base = STORAGE_SD_BASE;
    }
    if (!base) {
        return false;
    }

    uint64_t total = 0;
    uint64_t freeb = 0;
    if (esp_vfs_fat_info(base, &total, &freeb) != ESP_OK) {
        return false;
    }
    if (total_bytes) {
        *total_bytes = total;
    }
    if (free_bytes) {
        *free_bytes = freeb;
    }
    return true;
}
