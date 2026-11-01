/*
 * paperdiy-reader — book covers.
 *
 * SPDX-License-Identifier: MIT
 */
#include "reader/cover.h"

#include <cstdio>
#include <cstring>
#include <list>
#include <string>

#include "esp_log.h"
#include "fonts/vlw_fonts.h"
#include "hal/hal.h"
#include "miniz_lite.h"
#include "reader/book_epub.h"
#include "reader/layout.h"
#include "reader/studio.h"

static const char* TAG = "cover";

namespace cover {

namespace {

constexpr uint32_t COV_MAGIC = 0x564F4352u; /* 'RCOV' */

/* Small LRU of decoded thumbnails in PSRAM (32 KB each). */
struct Thumb {
    std::string path;
    M5Canvas* sprite = nullptr;
};
std::list<Thumb> s_lru;
constexpr size_t LRU_MAX = 40;

M5Canvas* newSprite(int w, int h)
{
    auto* s = new M5Canvas(hal.Canvas);
    s->setPsram(true);
    s->setColorDepth(lgfx::color_depth_t::grayscale_8bit);
    if (!s->createSprite(w, h)) {
        delete s;
        return nullptr;
    }
    return s;
}

M5Canvas* lruGet(const std::string& path)
{
    for (auto it = s_lru.begin(); it != s_lru.end(); ++it) {
        if (it->path == path) {
            s_lru.splice(s_lru.begin(), s_lru, it);
            return s_lru.front().sprite;
        }
    }
    return nullptr;
}

void lruPut(const std::string& path, M5Canvas* sprite)
{
    s_lru.push_front(Thumb{path, sprite});
    while (s_lru.size() > LRU_MAX) {
        Thumb& t = s_lru.back();
        t.sprite->deleteSprite();
        delete t.sprite;
        s_lru.pop_back();
    }
}

/* ---- image header probing ------------------------------------------ */

enum class Kind { None, Jpeg, Png };

/* Reads pixel dimensions from a JPEG SOF or PNG IHDR. Progressive JPEGs are
 * rejected: the TJpgDec decoder in LovyanGFX handles baseline only. */
Kind probe(const uint8_t* d, size_t n, int* w, int* h)
{
    if (n > 24 && memcmp(d, "\x89PNG", 4) == 0) {
        *w = (int)((d[16] << 24) | (d[17] << 16) | (d[18] << 8) | d[19]);
        *h = (int)((d[20] << 24) | (d[21] << 16) | (d[22] << 8) | d[23]);
        return (*w > 0 && *h > 0) ? Kind::Png : Kind::None;
    }
    if (n < 4 || d[0] != 0xFF || d[1] != 0xD8) {
        return Kind::None;
    }
    size_t i = 2;
    while (i + 9 < n) {
        if (d[i] != 0xFF) {
            i++;
            continue;
        }
        uint8_t m = d[i + 1];
        if (m == 0xFF) {
            i++;
            continue;
        }
        if (m == 0xD8 || m == 0x01 || (m >= 0xD0 && m <= 0xD7)) {
            i += 2;
            continue;
        }
        size_t len = ((size_t)d[i + 2] << 8) | d[i + 3];
        if (m == 0xC0 || m == 0xC1) {
            *h = (d[i + 5] << 8) | d[i + 6];
            *w = (d[i + 7] << 8) | d[i + 8];
            return (*w > 0 && *h > 0) ? Kind::Jpeg : Kind::None;
        }
        if (m >= 0xC2 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC) {
            return Kind::None;  // progressive / arithmetic / lossless
        }
        i += 2 + len;
    }
    return Kind::None;
}

/* Draws an image scaled to cover (crop) or fit the box, centred. */
bool drawInto(M5Canvas* dst, const uint8_t* data, size_t n, int x, int y, int w, int h, bool crop)
{
    int iw = 0;
    int ih = 0;
    Kind k = probe(data, n, &iw, &ih);
    if (k == Kind::None) {
        return false;
    }
    const float sx = (float)w / (float)iw;
    const float sy = (float)h / (float)ih;
    const float s  = crop ? (sx > sy ? sx : sy) : (sx < sy ? sx : sy);
    if (k == Kind::Jpeg) {
        return dst->drawJpg(data, n, x, y, w, h, 0, 0, s, s, middle_center);
    }
    return dst->drawPng(data, n, x, y, w, h, 0, 0, s, s, middle_center);
}

/* Decodes @p data into @p dst, scaled to cover (crop) or fit the sprite. */
bool decodeInto(M5Canvas* dst, const uint8_t* data, size_t n, bool crop)
{
    dst->fillScreen(TFT_WHITE);
    return drawInto(dst, data, n, 0, 0, dst->width(), dst->height(), crop);
}

/* ---- sidecar cache --------------------------------------------------- */

bool cacheRead(const std::string& file, M5Canvas* dst, bool* none)
{
    *none   = false;
    FILE* f = fopen(file.c_str(), "rb");
    if (!f) {
        return false;
    }
    uint32_t hdr[2] = {0, 0};
    bool ok         = fread(hdr, sizeof(hdr), 1, f) == 1 && hdr[0] == COV_MAGIC;
    if (ok) {
        uint16_t w = (uint16_t)(hdr[1] & 0xFFFF);
        uint16_t h = (uint16_t)(hdr[1] >> 16);
        if (w == 0 && h == 0) {
            *none = true;  // remembered "this book has no usable cover"
            ok    = false;
        } else if (w == THUMB_W && h == THUMB_H && dst) {
            ok = fread(dst->getBuffer(), (size_t)w * h, 1, f) == 1;
        } else {
            ok = false;
        }
    }
    fclose(f);
    return ok;
}

void cacheWrite(const std::string& file, M5Canvas* src)
{
    FILE* f = fopen(file.c_str(), "wb");
    if (!f) {
        return;
    }
    uint32_t hdr[2] = {COV_MAGIC, src ? ((uint32_t)THUMB_H << 16 | THUMB_W) : 0u};
    fwrite(hdr, sizeof(hdr), 1, f);
    if (src) {
        fwrite(src->getBuffer(), (size_t)THUMB_W * THUMB_H, 1, f);
    }
    fclose(f);
}

/* ---- typeset covers -------------------------------------------------- */

uint32_t hash32(const std::string& s)
{
    uint32_t h = 2166136261u;
    for (unsigned char c : s) {
        h = (h ^ c) * 16777619u;
    }
    return h;
}

/* A quiet, bookish cover: tinted field, a rule, the title in Literata Bold
 * and the author in Atkinson. The tint varies per book so a shelf of TXT
 * files does not look like one book repeated. */
void typeset(M5Canvas* cv, const BookEntry& b, int x, int y, int w, int h)
{
    static const uint8_t kTints[] = {0xE8, 0xD8, 0xC8, 0xF2};
    const uint32_t hv             = hash32(b.title);
    const uint8_t tint            = kTints[hv % sizeof(kTints)];
    const bool big                = w > 300;

    cv->fillRect(x, y, w, h, cv->color888(tint, tint, tint));
    cv->drawRect(x, y, w, h, 0x000000u);
    const int inset = big ? 28 : 7;
    cv->drawRect(x + inset, y + inset, w - 2 * inset, h - 2 * inset, cv->color888(0x40, 0x40, 0x40));

    const lgfx::IFont* tf = layout::serifFont(big ? 58 : 24, layout::FACE_BOLD);
    const lgfx::IFont* af = layout::uiFont(big ? 32 : 24);
    const int pad         = inset + (big ? 30 : 4);
    const int tw          = w - 2 * pad;

    std::vector<std::string> lines = layout::wrapText(tf, b.title, tw, big ? 5 : 4);
    const int lh                   = vlw::height(tf) + (big ? 8 : 2);

    cv->setTextColor(0x000000u);
    cv->setTextDatum(top_center);
    cv->setFont(tf);
    int ty = y + (big ? h / 4 : inset + 16);
    for (const std::string& l : lines) {
        cv->drawString(l.c_str(), x + w / 2, ty);
        ty += lh;
    }

    const int rule_y = ty + (big ? 24 : 8);
    cv->drawFastHLine(x + w / 2 - (big ? 60 : 20), rule_y, big ? 120 : 40, 0x000000u);

    if (!b.author.empty()) {
        cv->setFont(af);
        std::vector<std::string> al = layout::wrapText(af, b.author, tw, 2);
        int ay                      = rule_y + (big ? 28 : 10);
        for (const std::string& l : al) {
            cv->drawString(l.c_str(), x + w / 2, ay);
            ay += vlw::height(af) + 2;
        }
    }
    if (!b.is_epub) {
        cv->setFont(layout::uiFont(24));
        cv->setTextDatum(bottom_center);
        cv->setTextColor(cv->color888(0x50, 0x50, 0x50));
        cv->drawString("TXT", x + w / 2, y + h - inset - 6);
    }
    cv->setTextDatum(top_left);
    cv->setTextColor(0x000000u);
}

}  // namespace

/* ================================================================== */

void ensureThumb(BookEntry& book)
{
    if (book.cover != 0) {
        return;
    }
    if (lruGet(book.path)) {
        book.cover = 1;
        return;
    }
    if (!book.is_epub) {
        book.cover = -1;
        return;
    }

    std::string file = library::cachePath(book, ".cov");
    M5Canvas* sprite = newSprite(THUMB_W, THUMB_H);
    if (!sprite) {
        book.cover = -1;
        return;
    }

    bool none = false;
    hal_storage_lock();
    bool ok = !file.empty() && cacheRead(file, sprite, &none);
    hal_storage_unlock();
    if (ok) {
        lruPut(book.path, sprite);
        book.cover = 1;
        return;
    }
    if (none) {
        sprite->deleteSprite();
        delete sprite;
        book.cover = -1;
        return;
    }

    uint32_t t0 = millis_();
    size_t n    = 0;
    hal_storage_lock();
    uint8_t* data = (uint8_t*)epub_cover_data(book.path.c_str(), &n);
    hal_storage_unlock();
    ok = data && decodeInto(sprite, data, n, true);
    if (data) {
        mz_free(data);
    }

    hal_storage_lock();
    if (!file.empty()) {
        cacheWrite(file, ok ? sprite : nullptr);
    }
    hal_storage_unlock();

    ESP_LOGI(TAG, "%s: %s in %lu ms", book.filename.c_str(), ok ? "cover cached" : "no usable cover",
             (unsigned long)(millis_() - t0));
    if (ok) {
        lruPut(book.path, sprite);
        book.cover = 1;
    } else {
        sprite->deleteSprite();
        delete sprite;
        book.cover = -1;
    }
}

bool drawImage(M5Canvas* cv, const uint8_t* data, size_t n, int x, int y, int w, int h, bool crop)
{
    return drawInto(cv, data, n, x, y, w, h, crop);
}

void drawThumb(M5Canvas* cv, const BookEntry& book, int x, int y)
{
    if (studio::isStudio(book)) {
        studio::drawThumb(cv, book, x, y);
        return;
    }
    M5Canvas* s = (book.cover > 0) ? lruGet(book.path) : nullptr;
    if (s) {
        s->pushSprite(cv, x, y);
        cv->drawRect(x, y, THUMB_W, THUMB_H, cv->color888(0x60, 0x60, 0x60));
        return;
    }
    if (book.cover == 0) {
        // Not decoded yet: a plain placeholder that the cover replaces shortly.
        cv->fillRect(x, y, THUMB_W, THUMB_H, cv->color888(0xEC, 0xEC, 0xEC));
        cv->drawRect(x, y, THUMB_W, THUMB_H, cv->color888(0xA0, 0xA0, 0xA0));
        cv->setFont(layout::serifFont(24, layout::FACE_BOLD));
        cv->setTextColor(cv->color888(0x60, 0x60, 0x60));
        cv->setTextDatum(middle_center);
        std::vector<std::string> l = layout::wrapText(layout::serifFont(24, layout::FACE_BOLD), book.title,
                                                      THUMB_W - 20, 3);
        int ty = y + THUMB_H / 2 - (int)(l.size() - 1) * 15;
        for (const std::string& s2 : l) {
            cv->drawString(s2.c_str(), x + THUMB_W / 2, ty);
            ty += 30;
        }
        cv->setTextDatum(top_left);
        cv->setTextColor(0x000000u);
        return;
    }
    typeset(cv, book, x, y, THUMB_W, THUMB_H);
}

void drawFull(M5Canvas* cv, const BookEntry& book, int x, int y, int w, int h)
{
    bool ok = false;
    if (book.is_epub && book.cover >= 0) {
        size_t n = 0;
        hal_storage_lock();
        uint8_t* data = (uint8_t*)epub_cover_data(book.path.c_str(), &n);
        hal_storage_unlock();
        if (data) {
            M5Canvas* s = newSprite(w, h);
            if (s) {
                ok = decodeInto(s, data, n, false);
                if (ok) {
                    s->pushSprite(cv, x, y);
                }
                s->deleteSprite();
                delete s;
            }
            mz_free(data);
        }
    }
    if (!ok) {
        typeset(cv, book, x, y, w, h);
    }
}

void forget(const BookEntry& book)
{
    for (auto it = s_lru.begin(); it != s_lru.end(); ++it) {
        if (it->path == book.path) {
            it->sprite->deleteSprite();
            delete it->sprite;
            s_lru.erase(it);
            return;
        }
    }
}

}  // namespace cover
