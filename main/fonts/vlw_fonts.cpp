/*
 * paperdiy-reader — VLW font pack loader.
 *
 * SPDX-License-Identifier: MIT
 */
#include "fonts/vlw_fonts.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#include "esp_log.h"
#include "esp_partition.h"

static const char* TAG = "vlw";

namespace vlw {

namespace {

struct PackEntry {
    char name[32];
    uint32_t offset;
    uint32_t size;
};

struct LoadedFont {
    std::string name;
    const uint8_t* data = nullptr;
    uint32_t size       = 0;
    lgfx::VLWfont* font = nullptr;
    lgfx::PointerWrapper* wrap = nullptr;
    uint8_t adv_latin[0x180] = {0};       /* direct advances for cp < 0x180 */
    uint8_t has_latin[0x180] = {0};
    std::vector<std::pair<uint16_t, uint8_t>> adv_other; /* sorted by cp */
    int space   = 8;
    int ascent  = 0;
    int descent = 0;
};

const uint8_t* s_base = nullptr;
uint32_t s_count      = 0;
esp_partition_mmap_handle_t s_map;
std::vector<LoadedFont*> s_loaded;

inline uint32_t be32(const uint8_t* p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

LoadedFont* find_loaded(const lgfx::IFont* f)
{
    for (LoadedFont* lf : s_loaded) {
        if (lf->font == f) return lf;
    }
    return nullptr;
}

void build_tables(LoadedFont* lf)
{
    const uint8_t* d = lf->data;
    uint32_t n       = be32(d);
    lf->ascent       = (int)be32(d + 16);
    lf->descent      = (int)be32(d + 20);
    for (uint32_t i = 0; i < n; ++i) {
        const uint8_t* r = d + 24 + i * 28;
        uint32_t cp      = be32(r);
        uint8_t adv      = (uint8_t)be32(r + 12);
        if (cp < 0x180) {
            lf->adv_latin[cp] = adv;
            lf->has_latin[cp] = 1;
        } else if (cp <= 0xFFFF) {
            lf->adv_other.emplace_back((uint16_t)cp, adv);
        }
    }
    std::sort(lf->adv_other.begin(), lf->adv_other.end());
    lf->space = lf->has_latin[0x20] ? lf->adv_latin[0x20] : (lf->ascent + lf->descent) * 2 / 7;
}

inline int advance(const LoadedFont* lf, uint32_t cp, bool* found)
{
    if (cp < 0x180) {
        *found = lf->has_latin[cp];
        return lf->has_latin[cp] ? lf->adv_latin[cp] : lf->space;
    }
    auto it = std::lower_bound(lf->adv_other.begin(), lf->adv_other.end(), std::make_pair((uint16_t)cp, (uint8_t)0));
    if (it != lf->adv_other.end() && it->first == cp) {
        *found = true;
        return it->second;
    }
    *found = false;
    return lf->space;  // LGFX draws a missing glyph as a space-wide gap
}

}  // namespace

bool init()
{
    if (s_base) return true;
    const esp_partition_t* p =
        esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "fonts");
    if (!p) {
        ESP_LOGE(TAG, "no 'fonts' partition");
        return false;
    }
    const void* ptr = nullptr;
    if (esp_partition_mmap(p, 0, p->size, ESP_PARTITION_MMAP_DATA, &ptr, &s_map) != ESP_OK) {
        ESP_LOGE(TAG, "mmap failed");
        return false;
    }
    const uint8_t* b = (const uint8_t*)ptr;
    if (memcmp(b, "VLWP", 4) != 0) {
        ESP_LOGE(TAG, "fonts partition is empty - flash fonts.bin (build.ps1 reader fonts)");
        esp_partition_munmap(s_map);
        return false;
    }
    s_base  = b;
    s_count = b[8] | (b[9] << 8) | (b[10] << 16) | ((uint32_t)b[11] << 24);
    ESP_LOGI(TAG, "font pack: %u fonts", (unsigned)s_count);
    return true;
}

bool ready()
{
    return s_base != nullptr;
}

const lgfx::IFont* get(const char* name)
{
    for (LoadedFont* lf : s_loaded) {
        if (lf->name == name) return lf->font;
    }
    if (!s_base) return &fonts::Font2;

    for (uint32_t i = 0; i < s_count; ++i) {
        PackEntry e;
        memcpy(&e, s_base + 12 + i * 40, sizeof(e));
        e.name[31] = 0;
        if (strcmp(e.name, name) != 0) continue;

        auto* lf  = new LoadedFont();
        lf->name  = name;
        lf->data  = s_base + e.offset;
        lf->size  = e.size;
        lf->wrap  = new lgfx::PointerWrapper(lf->data, lf->size);
        lf->font  = new lgfx::VLWfont();
        if (!lf->font->loadFont(lf->wrap)) {
            ESP_LOGE(TAG, "bad font %s", name);
            delete lf->font;
            delete lf->wrap;
            delete lf;
            return &fonts::Font2;
        }
        build_tables(lf);
        s_loaded.push_back(lf);
        return lf->font;
    }
    ESP_LOGW(TAG, "font %s not in pack", name);
    return &fonts::Font2;
}

uint32_t utf8_next(const char* s, size_t len, size_t& i)
{
    unsigned char c = (unsigned char)s[i];
    if (c < 0x80) {
        i++;
        return c;
    }
    int extra   = 0;
    uint32_t cp = 0;
    if ((c & 0xE0) == 0xC0) {
        cp    = c & 0x1F;
        extra = 1;
    } else if ((c & 0xF0) == 0xE0) {
        cp    = c & 0x0F;
        extra = 2;
    } else if ((c & 0xF8) == 0xF0) {
        cp    = c & 0x07;
        extra = 3;
    } else {
        i++;
        return 0xFFFD;
    }
    if (i + (size_t)extra >= len) {
        i = len;
        return 0xFFFD;
    }
    for (int k = 1; k <= extra; ++k) {
        unsigned char cc = (unsigned char)s[i + k];
        if ((cc & 0xC0) != 0x80) {
            i++;
            return 0xFFFD;
        }
        cp = (cp << 6) | (cc & 0x3F);
    }
    i += extra + 1;
    return cp;
}

int width(const lgfx::IFont* font, const char* utf8, size_t len)
{
    LoadedFont* lf = find_loaded(font);
    if (!lf) {
        return 8 * (int)len;  // only reached without the font pack
    }
    int w    = 0;
    size_t i = 0;
    while (i < len) {
        uint32_t cp = utf8_next(utf8, len, i);
        bool found;
        w += advance(lf, cp, &found);
    }
    return w;
}

int width(const lgfx::IFont* font, const char* utf8)
{
    return width(font, utf8, strlen(utf8));
}

bool has_glyph(const lgfx::IFont* font, uint32_t cp)
{
    LoadedFont* lf = find_loaded(font);
    if (!lf) return cp < 0x80;
    bool found;
    advance(lf, cp, &found);
    return found;
}

int ascent(const lgfx::IFont* font)
{
    LoadedFont* lf = find_loaded(font);
    return lf ? lf->ascent : 12;
}

int height(const lgfx::IFont* font)
{
    LoadedFont* lf = find_loaded(font);
    return lf ? lf->ascent + lf->descent : 16;
}

}  // namespace vlw
