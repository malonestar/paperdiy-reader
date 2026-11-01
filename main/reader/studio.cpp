/*
 * paperdiy-reader — Studio animations.
 *
 * SPDX-License-Identifier: MIT
 */
#include "reader/studio.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "esp_log.h"
#include "esp_random.h"
#include "fonts/vlw_fonts.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hal/hal.h"
#include "reader/art.h"
#include "reader/cover.h"
#include "reader/layout.h"
#include "reader/render.h"

static const char* TAG = "studio";

namespace studio {

namespace {

constexpr int W = layout::SCREEN_W;
constexpr int H = layout::SCREEN_H;

struct Info {
    const char* path;
    const char* title;
    const char* author;
};
const Info kPieces[PIECE_COUNT] = {
    {"studio:life", "Life", "Conway's Game of Life"},
    {"studio:gallery", "Gallery", "Sleep-screen designs"},
};

art::Buf canvasBuf()
{
    return art::Buf{(uint8_t*)hal.Canvas->getBuffer(), W, H, W};
}

art::Buf subBuf(M5Canvas* cv, int x, int y, int w, int h)
{
    return art::Buf{(uint8_t*)cv->getBuffer() + (size_t)y * cv->width() + x, w, h, (int)cv->width()};
}

void text(M5Canvas* cv, const char* s, int x, int y, const lgfx::IFont* f, uint32_t color, uint8_t datum)
{
    cv->setFont(f);
    cv->setTextColor(color);
    cv->setTextDatum(datum);
    cv->drawString(s, x, y);
    cv->setTextDatum(top_left);
}

/* ---- input ----------------------------------------------------------- */

struct Keys {
    bool a = false, b = false, a_hold = false, b_hold = false, sleep = false;
};

Keys poll()
{
    static uint32_t chord_t = 0;
    hal.update();
    Keys k;
    if (M5.BtnA.isPressed() && M5.BtnB.isPressed()) {
        if (!chord_t) {
            chord_t = millis_();
        } else if (millis_() - chord_t > 1500) {
            k.sleep = true;
            chord_t = 0;
        }
        return k;
    }
    chord_t  = 0;
    k.a      = M5.BtnA.wasClicked();
    k.b      = M5.BtnB.wasClicked();
    k.a_hold = M5.BtnA.wasHold();
    k.b_hold = M5.BtnB.wasHold();
    return k;
}

/* ---- frame clock + stats --------------------------------------------- */

struct Clock {
    uint32_t frame_ms;
    uint32_t next   = 0;
    uint32_t t_sec  = 0;
    int frames      = 0;
    int fps_x10     = 0;

    explicit Clock(uint32_t ms) : frame_ms(ms)
    {
        next  = millis_();
        t_sec = next;
    }
    /** Sleeps until the next frame is due, polling keys meanwhile. */
    Keys wait()
    {
        Keys acc;
        do {
            Keys k = poll();
            acc.a |= k.a;
            acc.b |= k.b;
            acc.a_hold |= k.a_hold;
            acc.b_hold |= k.b_hold;
            acc.sleep |= k.sleep;
            if (acc.b_hold || acc.sleep) {
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(5));
        } while ((int32_t)(millis_() - next) < 0);
        next += frame_ms;
        if ((int32_t)(millis_() - next) > 500) {
            next = millis_();  // fell behind: do not try to catch up
        }
        frames++;
        uint32_t now = millis_();
        if (now - t_sec >= 1000) {
            fps_x10 = (int)(frames * 10000 / (now - t_sec));
            frames  = 0;
            t_sec   = now;
        }
        return acc;
    }
};

/** Bottom strip: piece name, a live counter, key hints, optional fps. */
void hud(const char* name, const char* counter, bool stats, int fps_x10)
{
    M5Canvas* cv = hal.Canvas;
    cv->fillRect(0, H - 44, W, 44, TFT_WHITE);
    cv->drawFastHLine(24, H - 44, W - 48, 0x000000u);
    text(cv, name, 24, H - 22, layout::uiFont(24, true), 0x000000u, middle_left);
    char right[96];
    if (stats) {
        snprintf(right, sizeof(right), "%s   %d.%d fps", counter, fps_x10 / 10, fps_x10 % 10);
    } else {
        snprintf(right, sizeof(right), "%s   hold B: exit", counter);
    }
    text(cv, right, W - 24, H - 22, layout::uiFont(24), 0x000000u, middle_right);
}

/** Leaves a piece: clean the panel before the library redraws. */
void enterClean()
{
    render::nextRefresh(render::Refresh::Full);
}

/* ================================================================== */
/* Life                                                                */
/* ================================================================== */

constexpr int LC  = 18;            // cell size
constexpr int LGW = W / LC;        // 30
constexpr int LGH = (H - 44 - 8) / LC;  // 50
constexpr int LY0 = 4;

void lifeSeed(uint8_t* g, int mode)
{
    memset(g, 0, LGW * LGH);
    if (mode % 3 == 0) {
        for (int i = 0; i < LGW * LGH; i++) {
            g[i] = (esp_random() % 100) < 30;
        }
    } else if (mode % 3 == 1) {
        // R-pentomino: five cells that churn for over a thousand generations.
        int cx = LGW / 2, cy = LGH / 2;
        g[(cy - 1) * LGW + cx]     = 1;
        g[(cy - 1) * LGW + cx + 1] = 1;
        g[cy * LGW + cx - 1]       = 1;
        g[cy * LGW + cx]           = 1;
        g[(cy + 1) * LGW + cx]     = 1;
    } else {
        // A fleet of gliders.
        static const int gl[5][2] = {{1, 0}, {2, 1}, {0, 2}, {1, 2}, {2, 2}};
        for (int i = 0; i < 9; i++) {
            int ox = (i % 3) * 10 + 2, oy = (i / 3) * 15 + 3;
            for (auto& c : gl) {
                g[(oy + c[1]) * LGW + ox + c[0]] = 1;
            }
        }
    }
}

void lifeDraw(art::Buf b, const uint8_t* g, int gw, int gh, int cell, int y0)
{
    for (int y = 0; y < gh; y++) {
        for (int x = 0; x < gw; x++) {
            if (g[y * gw + x]) {
                art::rect(b, x * cell + 2, y0 + y * cell + 2, cell - 4, cell - 4, art::level(0));
            }
        }
    }
}

bool runLife()
{
    static uint8_t a[LGW * LGH], nb[LGW * LGH];
    int mode = 0;
    lifeSeed(a, mode);
    uint32_t gen = 0, still = 0, prev_hash = 0, prev2_hash = 0;
    bool stats = false;
    Clock clk(160);
    render::nextRefresh(render::Refresh::Full);

    for (;;) {
        art::Buf b = canvasBuf();
        art::fill(b, art::level(15));
        lifeDraw(b, a, LGW, LGH, LC, LY0);
        int pop = 0;
        for (int i = 0; i < LGW * LGH; i++) {
            pop += a[i];
        }
        char counter[40];
        snprintf(counter, sizeof(counter), "gen %lu  pop %d", (unsigned long)gen, pop);
        hud("Life", counter, stats, clk.fps_x10);
        render::presentStudio(render::Refresh::Fastest);

        Keys k = clk.wait();
        if (k.sleep) return true;
        if (k.b_hold) return false;
        if (k.b) stats = !stats;
        if (k.a_hold) render::nextRefresh(render::Refresh::Full);
        if (k.a || still > 40 || pop < 3) {
            lifeSeed(a, ++mode);
            gen = still = 0;
            continue;
        }

        // Step (toroidal).
        uint32_t h = 2166136261u;
        for (int y = 0; y < LGH; y++) {
            for (int x = 0; x < LGW; x++) {
                int n = 0;
                for (int dy = -1; dy <= 1; dy++) {
                    for (int dx = -1; dx <= 1; dx++) {
                        if (dx || dy) {
                            n += a[((y + dy + LGH) % LGH) * LGW + (x + dx + LGW) % LGW];
                        }
                    }
                }
                uint8_t v      = a[y * LGW + x] ? (n == 2 || n == 3) : (n == 3);
                nb[y * LGW + x] = v;
                h               = (h ^ (v * (uint32_t)(y * LGW + x + 1))) * 16777619u;
            }
        }
        memcpy(a, nb, sizeof(a));
        // Stuck in a still life or a blinker: count, then reseed.
        still      = (h == prev_hash || h == prev2_hash) ? still + 1 : 0;
        prev2_hash = prev_hash;
        prev_hash  = h;
        gen++;
    }
}

/* ================================================================== */
/* Gallery                                                             */
/* ================================================================== */

bool runGallery()
{
    int d = 0;
    uint32_t seed = esp_random();
    for (;;) {
        render::designScreen(d, seed, nullptr);
        // Caption over the bottom band.
        M5Canvas* cv = hal.Canvas;
        cv->fillRect(0, H - 72, W, 72, 0x000000u);
        char cap[64];
        snprintf(cap, sizeof(cap), "%s  \xC2\xB7  %d of %d", art::designName(d), d + 1, (int)art::DESIGN_COUNT);
        text(cv, cap, W / 2, H - 50, layout::uiFont(28, true), 0xFFFFFFu, middle_center);
        text(cv, "A next \xC2\xB7 B back \xC2\xB7 hold A: reshuffle \xC2\xB7 hold B: exit", W / 2, H - 20,
             layout::uiFont(24), 0xFFFFFFu, middle_center);
        render::presentStudio(render::Refresh::Full);

        for (;;) {
            Keys k = poll();
            if (k.sleep) return true;
            if (k.b_hold) return false;
            if (k.a) { d = (d + 1) % art::DESIGN_COUNT; seed = esp_random(); break; }
            if (k.b) { d = (d + art::DESIGN_COUNT - 1) % art::DESIGN_COUNT; seed = esp_random(); break; }
            if (k.a_hold) { seed = esp_random(); break; }
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
}

}  // namespace

/* ================================================================== */

bool isStudio(const BookEntry& b)
{
    return b.path.compare(0, 7, "studio:") == 0;
}

int pieceOf(const BookEntry& b)
{
    for (int i = 0; i < PIECE_COUNT; i++) {
        if (b.path == kPieces[i].path) {
            return i;
        }
    }
    return -1;
}

void addEntries(std::vector<BookEntry>& books)
{
    for (const Info& p : kPieces) {
        BookEntry e;
        e.path     = p.path;
        e.filename = p.title;
        e.title    = p.title;
        e.author   = p.author;
        e.cover    = -1;
        books.push_back(e);
    }
}

void drawThumb(M5Canvas* cv, const BookEntry& e, int x, int y)
{
    const int tw = cover::THUMB_W, th = cover::THUMB_H;
    art::Buf b   = subBuf(cv, x, y, tw, th);
    art::Rng rng(0xC0FFEEu + (uint32_t)pieceOf(e));
    switch (pieceOf(e)) {
        case PIECE_LIFE: {
            art::fill(b, art::level(15));
            static const int gl[5][2] = {{1, 0}, {2, 1}, {0, 2}, {1, 2}, {2, 2}};
            uint8_t g[19 * 26] = {0};
            for (int i = 0; i < 19 * 26; i++) g[i] = rng.range(100) < 22;
            for (auto& c : gl) g[(10 + c[1]) * 19 + 8 + c[0]] = 1;
            lifeDraw(b, g, 19, 26, 8, 2);
            break;
        }
        case PIECE_GALLERY:
        default: {
            art::Plaque pq = art::draw(b, art::DESIGN_STACK, 7);
            (void)pq;
            break;
        }
    }
    cv->drawRect(x, y, tw, th, 0x000000u);
    // "Studio" corner tag.
    const lgfx::IFont* tf = layout::uiFont(24, true);
    const int tagw        = vlw::width(tf, "STUDIO") + 14;
    cv->fillRect(x + tw - tagw, y + th - 28, tagw, 28, 0x000000u);
    text(cv, "STUDIO", x + tw - tagw / 2, y + th - 14, tf, 0xFFFFFFu, middle_center);
}

bool run(int piece)
{
    ESP_LOGI(TAG, "running piece %d", piece);
    bool sleep = false;
    switch (piece) {
        case PIECE_LIFE:
            sleep = runLife();
            break;
        case PIECE_GALLERY:
            sleep = runGallery();
            break;
        default:
            break;
    }
    enterClean();
    return sleep;
}

}  // namespace studio
