/*
 * paperdiy-reader — screen drawing and refresh policy.
 *
 * SPDX-License-Identifier: MIT
 */
#include "reader/render.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "fonts/vlw_fonts.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hal/hal.h"
#include "qrcode.h"
#include "reader/art.h"
#include "reader/cover.h"
#include "reader/studio.h"
#include "esp_random.h"
#include "hal/storage/hal_storage.h"
#include <cctype>
#include <dirent.h>
#include <strings.h>
#include <sys/stat.h>
#include <string>
#include <vector>

static const char* TAG = "render";

namespace render {

namespace {

constexpr int W = layout::SCREEN_W;
constexpr int H = layout::SCREEN_H;
constexpr int MX = layout::MARGIN_X;

/* Colours as RGB888 so they convert the same everywhere (TFT_* are RGB565 ints). */
constexpr uint32_t C_WHITE = 0xFFFFFFu;
constexpr uint32_t C_BLACK = 0x000000u;

/* Greys (8-bit; the panel shows 16 levels). */
constexpr uint8_t G_DARK  = 0x18;
constexpr uint8_t G_MID   = 0x30;
constexpr uint8_t G_LIGHT = 0xB0;
constexpr uint8_t G_PALE  = 0xDC;

M5Canvas* cv()
{
    return hal.Canvas;
}

uint32_t grey(uint8_t g)
{
    return cv()->color888(g, g, g);
}

/* ---- presentation ---------------------------------------------------- */

enum class Screen : uint8_t { None, Library, LibraryMenu, Reading, ReadingMenu, List, Message, Progress, Info,
                              Portal, Sleep, Studio };

uint8_t* s_shadow   = nullptr; /* what the panel currently shows */
Screen s_last       = Screen::None;
Refresh s_next      = Refresh::Auto;
int s_turns         = 0;       /* text refreshes since the last full clean-up */

/* The screen a menu sheet sits on: closing the sheet is not a screen change. */
Screen baseOf(Screen s)
{
    if (s == Screen::LibraryMenu) {
        return Screen::Library;
    }
    if (s == Screen::ReadingMenu) {
        return Screen::Reading;
    }
    return s;
}

struct Rect {
    int x, y, w, h;
};

/*
 * Finds what changed, as a short list of rectangles.
 *
 * The frame is cut into 16x16 blocks; changed blocks are merged into
 * horizontal runs per block row, and identical runs on consecutive rows are
 * merged vertically. Moving the selection frame therefore touches only the
 * frame's own outline, never the cover inside it. That matters: fast
 * waveforms re-drive every pixel in a refreshed area with few grey levels,
 * so a cover caught inside a refresh rectangle would wash out.
 *
 * @return number of rectangles, or -1 when the change is too scattered (the
 *         caller then refreshes one bounding box instead).
 */
constexpr int BLK = 16;
constexpr int BW  = (W + BLK - 1) / BLK;
constexpr int BH  = (H + BLK - 1) / BLK;

int diffRects(Rect* out, int max_rects, Rect* bbox)
{
    static uint8_t dirty[BH][BW];
    memset(dirty, 0, sizeof(dirty));
    const uint8_t* cur = (const uint8_t*)cv()->getBuffer();
    int x0 = W, y0 = H, x1 = -1, y1 = -1;

    for (int y = 0; y < H; y++) {
        const uint8_t* a = cur + (size_t)y * W;
        const uint8_t* b = s_shadow + (size_t)y * W;
        if (memcmp(a, b, W) == 0) {
            continue;
        }
        for (int bx = 0; bx < BW; bx++) {
            int xs = bx * BLK;
            int n  = std::min(BLK, W - xs);
            if (memcmp(a + xs, b + xs, n) != 0) {
                dirty[y / BLK][bx] = 1;
                x0 = std::min(x0, xs);
                x1 = std::max(x1, xs + n);
            }
        }
        y0 = std::min(y0, y);
        y1 = std::max(y1, y + 1);
    }
    if (x1 < 0) {
        return 0;
    }
    *bbox = Rect{x0, y0, x1 - x0, y1 - y0};

    int n = 0;
    // Open runs from the previous block row, for vertical merging.
    struct Run { int bx0, bx1, idx; };
    Run prev[BW];
    int nprev = 0;
    for (int by = 0; by < BH; by++) {
        Run curr[BW];
        int ncur = 0;
        int bx   = 0;
        while (bx < BW) {
            if (!dirty[by][bx]) {
                bx++;
                continue;
            }
            int s = bx;
            while (bx < BW && dirty[by][bx]) {
                bx++;
            }
            // Extend a run with the same span from the row above.
            int idx = -1;
            for (int k = 0; k < nprev; k++) {
                if (prev[k].bx0 == s && prev[k].bx1 == bx) {
                    idx = prev[k].idx;
                    break;
                }
            }
            if (idx >= 0) {
                out[idx].h = std::min(H, (by + 1) * BLK) - out[idx].y;
            } else {
                if (n >= max_rects) {
                    return -1;
                }
                idx    = n++;
                out[idx] = Rect{s * BLK, by * BLK, std::min(W, bx * BLK) - s * BLK, std::min(H, (by + 1) * BLK) - by * BLK};
            }
            curr[ncur++] = Run{s, bx, idx};
        }
        memcpy(prev, curr, sizeof(Run) * ncur);
        nprev = ncur;
    }
    return n;
}

epd_mode_t modeFor(Refresh r)
{
    switch (r) {
        case Refresh::Fast:
            return epd_mode_t::epd_fast;
        case Refresh::Text:
            return epd_mode_t::epd_text;
        case Refresh::Fastest:
            return epd_mode_t::epd_fastest;
        default:
            return epd_mode_t::epd_quality;
    }
}

void present(Screen kind)
{
    if (!cv()) {
        return;
    }
    Refresh r = s_next;
    s_next    = Refresh::Auto;

    if (r == Refresh::Auto) {
        if (s_last == Screen::None || kind == Screen::Sleep) {
            r = Refresh::Full;
        } else if (baseOf(kind) != baseOf(s_last)) {
            // A different screen: clean slate, but only where pixels change.
            r = (kind == Screen::Progress || s_last == Screen::Progress) ? Refresh::Fast : Refresh::Quality;
        } else if (kind == Screen::Reading) {
            r = Refresh::Text;  // page turn, or a menu sheet closing over text
        } else {
            r = Refresh::Fast;  // cursor moves inside the same screen
        }
    }

    if (r == Refresh::Text && kind == Screen::Reading) {
        // Text updates leave faint ghosts in grey anti-aliasing; flash the
        // whole panel clean every N page turns (Kindle's "page refresh").
        if (hal.settings.full_every > 0 && ++s_turns >= hal.settings.full_every) {
            r = Refresh::Full;
        }
    }
    if (r == Refresh::Full) {
        s_turns = 0;
    }

    const uint32_t t0 = millis_();
    M5.Display.startWrite();
    if (r == Refresh::Full || !s_shadow) {
        M5.Display.setEpdMode(epd_mode_t::epd_quality);
        cv()->pushSprite(0, 0);
        M5.Display.display();
    } else {
        Rect rects[24];
        Rect bbox{0, 0, W, H};
        int n = diffRects(rects, 24, &bbox);
        if (n < 0) {
            rects[0] = bbox;
            n        = 1;
        }
        M5.Display.setEpdMode(modeFor(r));
        for (int i = 0; i < n; i++) {
            const Rect& rc = rects[i];
            M5.Display.setClipRect(rc.x, rc.y, rc.w, rc.h);
            cv()->pushSprite(0, 0);
            M5.Display.clearClipRect();
            M5.Display.display(rc.x, rc.y, rc.w, rc.h);
        }
        ESP_LOGD(TAG, "present: %d band(s)", n);
    }
    M5.Display.endWrite();

    if (s_shadow) {
        memcpy(s_shadow, cv()->getBuffer(), (size_t)W * H);
    }
    s_last = kind;
    ESP_LOGI(TAG, "present %d mode=%d in %lu ms", (int)kind, (int)r, (unsigned long)(millis_() - t0));
}

/* ---- drawing helpers --------------------------------------------------- */

void clear()
{
    cv()->fillScreen(C_WHITE);
    cv()->setTextWrap(false);
    cv()->setTextDatum(top_left);
    cv()->setTextColor(C_BLACK);
}

void text(const char* s, int x, int y, const lgfx::IFont* f, uint32_t color, uint8_t datum = top_left)
{
    cv()->setFont(f);
    cv()->setTextColor(color);
    cv()->setTextDatum(datum);
    cv()->drawString(s, x, y);
    cv()->setTextDatum(top_left);
}

/** Battery pictogram with the level beside it; right-aligned at @p right_x. */
void drawBattery(int right_x, int y)
{
    int pct = hal.batteryPercent();
    const int bw = 34;
    const int bh = 17;
    int bx       = right_x - bw - 3;

    cv()->drawRoundRect(bx, y, bw, bh, 3, C_BLACK);
    cv()->fillRect(bx + bw, y + 5, 3, bh - 10, C_BLACK);
    if (pct >= 0) {
        int fill = (pct * (bw - 6)) / 100;
        if (fill > 0) {
            cv()->fillRect(bx + 3, y + 3, fill, bh - 6, pct <= 15 ? grey(G_MID) : C_BLACK);
        }
    }
    if (hal.charging()) {
        // A small bolt over the cell.
        int cx = bx + bw / 2;
        cv()->fillTriangle(cx + 3, y + 1, cx - 5, y + bh / 2 + 1, cx, y + bh / 2 + 1, C_WHITE);
        cv()->fillTriangle(cx - 3, y + bh - 1, cx + 5, y + bh / 2 - 1, cx, y + bh / 2 - 1, C_WHITE);
    }
    char buf[16];
    if (pct >= 0) {
        snprintf(buf, sizeof(buf), "%d%%", pct);
        text(buf, bx - 8, y + bh / 2 + 1, layout::uiFont(24), C_BLACK, middle_right);
    }
}

/** Header band: bold title, optional grey note, battery, hairline. */
void drawHeader(const char* title, const char* note = nullptr)
{
    const lgfx::IFont* tf = layout::uiFont(32, true);
    std::string t         = fitText(title ? title : "", tf, W - 2 * MX - 150);
    text(t.c_str(), MX, 14, tf, C_BLACK);
    if (note && note[0]) {
        int tw = vlw::width(tf, t.c_str());
        text(note, MX + tw + 14, 21, layout::uiFont(24), grey(G_MID));
    }
    drawBattery(W - MX, 21);
    cv()->fillRect(MX, 62, W - 2 * MX, 2, C_BLACK);
}

/** Footer band: hairline, then key hints left and right. */
void drawFooter(const char* left, const char* right)
{
    const int y = H - layout::FOOTER_H;
    cv()->drawFastHLine(MX, y, W - 2 * MX, grey(G_LIGHT));
    const lgfx::IFont* f = layout::uiFont(24);
    if (left) {
        text(left, MX, y + layout::FOOTER_H / 2, f, grey(G_DARK), middle_left);
    }
    if (right) {
        text(right, W - MX, y + layout::FOOTER_H / 2, f, grey(G_DARK), middle_right);
    }
}

/** Draws the laid-out body text of a page. Line y values are baselines. */
void drawBody(const layout::Page& page)
{
    // Use the style the page was laid out with, not the live settings.
    const layout::Style st = page.style;
    cv()->setTextDatum(baseline_left);
    cv()->setTextColor(C_BLACK);

    for (const layout::Line& line : page.lines) {
        for (const layout::Word& w : line.words) {
            cv()->setFont(layout::fontFor(st, line.style, w.face));
            cv()->drawString(w.text.c_str(), w.x, line.y);
        }
        if (line.style == layout::PARA_CODE) {
            int m = layout::bodyMarginX(st);
            int lh = layout::lineHeight(cv(), st, layout::PARA_CODE);
            cv()->fillRect(m - 10, line.y - (lh * 3) / 4, 3, lh, grey(G_LIGHT));
        }
    }
    cv()->setTextDatum(top_left);
}

/** Bookmark ribbon in the top-right corner. */
void drawRibbon()
{
    const int x = W - MX - 26;
    cv()->fillRect(x, 0, 22, 40, C_BLACK);
    cv()->fillTriangle(x, 40, x + 22, 40, x + 11, 30, C_WHITE);
}

/** Chrome of a reading page: chapter at the top, progress at the bottom. */
void drawReadingChrome(const BookEntry& book, int page_no, int page_total, const PageInfo& info)
{
    const lgfx::IFont* f = layout::uiFont(24);
    std::string t        = fitText(info.chapter.empty() ? book.title : info.chapter, f, W - 2 * MX - 190);
    text(t.c_str(), W / 2, 14, f, grey(G_MID), top_center);
    if (info.bookmarked) {
        drawRibbon();
    } else {
        drawBattery(W - MX, 16);
    }

    const int total = page_total > 0 ? page_total : 1;
    int pct         = ((page_no + 1) * 100) / total;

    char left[48];
    if (info.chapter_left == 0) {
        snprintf(left, sizeof(left), "Last page in chapter");
    } else if (info.chapter_left > 0) {
        snprintf(left, sizeof(left), "%d page%s left in chapter", info.chapter_left,
                 info.chapter_left == 1 ? "" : "s");
    } else {
        snprintf(left, sizeof(left), "%s", "");
    }
    char right[40];
    snprintf(right, sizeof(right), "%d of %d  \xC2\xB7  %d%%", page_no + 1, total, pct);

    const int y = H - layout::FOOTER_H;
    cv()->drawFastHLine(MX, y, W - 2 * MX, grey(G_PALE));
    int fill = ((W - 2 * MX) * (page_no + 1)) / total;
    cv()->fillRect(MX, y - 1, fill, 3, C_BLACK);
    text(left, MX, y + layout::FOOTER_H / 2, f, grey(G_MID), middle_left);
    text(right, W - MX, y + layout::FOOTER_H / 2, f, grey(G_MID), middle_right);
}

/** Bottom sheet: a menu panel over the lower part of the screen. */
void drawSheet(const char* title, const std::vector<std::string>& items, const std::vector<std::string>& values,
               int sel, const char* hint)
{
    const int row_h  = 58;
    const int head_h = 62;
    const int foot_h = 44;
    const int ph     = head_h + (int)items.size() * row_h + foot_h;
    const int py     = H - ph;

    cv()->fillRect(0, py - 10, W, ph + 10, C_WHITE);
    cv()->fillRect(0, py - 10, W, 3, C_BLACK);
    // Grab handle.
    cv()->fillRoundRect(W / 2 - 30, py + 2, 60, 6, 3, grey(G_LIGHT));

    text(title ? title : "", MX, py + 18, layout::uiFont(32, true), C_BLACK);

    const lgfx::IFont* f  = layout::uiFont(28);
    const lgfx::IFont* fv = layout::uiFont(28);
    for (size_t i = 0; i < items.size(); i++) {
        int ry      = py + head_h + (int)i * row_h;
        bool active = ((int)i == sel);
        uint32_t fg = active ? C_WHITE : C_BLACK;
        if (active) {
            cv()->fillRoundRect(MX - 10, ry + 3, W - 2 * MX + 20, row_h - 6, 8, C_BLACK);
        } else if (i > 0) {
            cv()->drawFastHLine(MX, ry, W - 2 * MX, grey(G_PALE));
        }
        std::string v = (i < values.size()) ? values[i] : std::string();
        int vw        = v.empty() ? 0 : vlw::width(fv, v.c_str()) + 20;
        std::string label = fitText(items[i], f, W - 2 * MX - vw - 10);
        text(label.c_str(), MX + 6, ry + row_h / 2, f, fg, middle_left);
        if (!v.empty()) {
            text(v.c_str(), W - MX - 6, ry + row_h / 2, fv, active ? grey(G_PALE) : grey(G_MID), middle_right);
        }
    }
    if (hint) {
        text(hint, W / 2, H - foot_h / 2, layout::uiFont(24), grey(G_MID), middle_center);
    }
}

/* ---- QR code ------------------------------------------------------------ */

int g_qr_cx  = W / 2;
int g_qr_cy  = 360;
int g_qr_max = 300;

void qr_display_cb(esp_qrcode_handle_t qrcode)
{
    int size = esp_qrcode_get_size(qrcode);
    if (size <= 0) {
        return;
    }
    int scale = g_qr_max / (size + 4);
    if (scale < 1) {
        scale = 1;
    }
    int qr_px = scale * size;
    int ox    = g_qr_cx - qr_px / 2;
    int oy    = g_qr_cy - qr_px / 2;
    for (int y = 0; y < size; y++) {
        for (int x = 0; x < size; x++) {
            if (esp_qrcode_get_module(qrcode, x, y)) {
                cv()->fillRect(ox + x * scale, oy + y * scale, scale, scale, C_BLACK);
            }
        }
    }
}

void drawQr(const char* payload, int cx, int cy, int max_px)
{
    g_qr_cx  = cx;
    g_qr_cy  = cy;
    g_qr_max = max_px;

    esp_qrcode_config_t cfg = ESP_QRCODE_CONFIG_DEFAULT();
    cfg.display_func        = qr_display_cb;
    cfg.max_qrcode_version  = 10;
    cfg.qrcode_ecc_level    = ESP_QRCODE_ECC_LOW;
    if (esp_qrcode_generate(&cfg, payload) != ESP_OK) {
        ESP_LOGW(TAG, "QR generation failed");
    }
}

/* Integer-only formatting: newlib's nano printf has no float support. */
void formatBytes(uint64_t bytes, char* out, size_t n)
{
    if (bytes >= 1024ull * 1024ull * 1024ull) {
        uint64_t tenths = (bytes * 10ull) / (1024ull * 1024ull * 1024ull);
        snprintf(out, n, "%llu.%llu GB", (unsigned long long)(tenths / 10ull), (unsigned long long)(tenths % 10ull));
    } else if (bytes >= 1024ull * 1024ull) {
        uint64_t tenths = (bytes * 10ull) / (1024ull * 1024ull);
        snprintf(out, n, "%llu.%llu MB", (unsigned long long)(tenths / 10ull), (unsigned long long)(tenths % 10ull));
    } else {
        snprintf(out, n, "%llu KB", (unsigned long long)(bytes / 1024ull));
    }
}

/* ---- library grid ------------------------------------------------------- */

constexpr int GRID_TOP   = 80;
constexpr int TILE_GAP_X = (W - 2 * MX - GRID_COLS * cover::THUMB_W) / (GRID_COLS - 1);
constexpr int ROW_PITCH  = 276;

void drawTile(const BookEntry& b, int x, int y, bool selected)
{
    cover::drawThumb(cv(), b, x, y);

    const int tw          = cover::THUMB_W;
    const lgfx::IFont* tf = layout::uiFont(24, true);
    std::string t         = fitText(b.title, tf, tw);
    text(t.c_str(), x, y + cover::THUMB_H + 4, tf, C_BLACK);

    // Reading progress: bar + percent, or "New".
    const int py = y + cover::THUMB_H + 40;
    int pct      = b.percent();
    const lgfx::IFont* pf = layout::uiFont(24);
    if (studio::isStudio(b)) {
        text(fitText(b.author, pf, tw).c_str(), x, py, pf, grey(G_MID), middle_left);
    } else if (pct < 0 || (pct == 0 && b.recent == 0)) {
        text("New", x, py, pf, grey(G_MID), middle_left);
    } else if (pct >= 100) {
        text("Finished", x, py, pf, grey(G_MID), middle_left);
    } else {
        char buf[16];
        snprintf(buf, sizeof(buf), "%d%%", pct);
        int bar_w = tw - 56;
        cv()->fillRect(x, py - 3, bar_w, 6, grey(G_PALE));
        cv()->fillRect(x, py - 3, (bar_w * pct) / 100, 6, C_BLACK);
        text(buf, x + tw, py, pf, grey(G_DARK), middle_right);
    }

    if (selected) {
        for (int k = 0; k < 3; k++) {
            cv()->drawRoundRect(x - 8 + k, y - 8 + k, tw + 16 - 2 * k, ROW_PITCH - 2 - 2 * k, 10, C_BLACK);
        }
    }
}

void composeLibrary(const std::vector<BookEntry>& books, int selection, int first, bool sd_present)
{
    clear();
    char note[48];
    static const char* kSort[] = {"recent", "title", "author"};
    unsigned nb = 0;
    for (const BookEntry& e : books) {
        nb += studio::isStudio(e) ? 0 : 1;
    }
    snprintf(note, sizeof(note), "%u book%s \xC2\xB7 %s", nb, nb == 1 ? "" : "s", kSort[hal.settings.sort_mode % 3]);
    drawHeader("Library", books.empty() ? (sd_present ? nullptr : "no SD card") : note);

    if (books.empty()) {
        const lgfx::IFont* f = layout::uiFont(28);
        const char* lines[]  = {
            sd_present ? "No books yet." : "No SD card found.",
            "",
            "Copy .epub or .txt files to the",
            "SD card (any folder), then",
            "restart the reader.",
            "",
            "Or hold B and choose",
            "Wi-Fi upload to send books",
            "from your phone or computer.",
        };
        int y = 260;
        for (const char* l : lines) {
            text(l, W / 2, y, l == lines[0] ? layout::uiFont(32, true) : f, C_BLACK, top_center);
            y += 40;
        }
        drawFooter("Hold B: menu", nullptr);
        return;
    }

    for (int i = 0; i < GRID_PER_PAGE; i++) {
        int idx = first + i;
        if (idx >= (int)books.size()) {
            break;
        }
        int col = i % GRID_COLS;
        int row = i / GRID_COLS;
        int x   = MX + col * (cover::THUMB_W + TILE_GAP_X);
        int y   = GRID_TOP + row * ROW_PITCH;
        drawTile(books[(size_t)idx], x, y, idx == selection);
    }

    int pages = ((int)books.size() + GRID_PER_PAGE - 1) / GRID_PER_PAGE;
    char left[24];
    snprintf(left, sizeof(left), "%d / %d", first / GRID_PER_PAGE + 1, pages > 0 ? pages : 1);
    drawFooter(left, "Hold A: open   Hold B: menu");
}

}  // namespace

/* ================================================================== */

void init()
{
    if (!s_shadow) {
        s_shadow = (uint8_t*)heap_caps_malloc((size_t)W * H, MALLOC_CAP_SPIRAM);
        if (s_shadow) {
            memset(s_shadow, 0xFF, (size_t)W * H);
        }
    }
    if (cv()) {
        cv()->setTextWrap(false);
        cv()->setTextDatum(top_left);
        cv()->setTextColor(C_BLACK);
    }
}

void nextRefresh(Refresh r)
{
    s_next = r;
}

bool busy()
{
    return M5.Display.displayBusy();
}

void waitIdle(uint32_t timeout_ms)
{
    uint32_t t0 = millis_();
    while (M5.Display.displayBusy() && (millis_() - t0) < timeout_ms) {
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

std::string fitText(const std::string& s, const lgfx::IFont* font, int max_width)
{
    return layout::ellipsize(font, s, max_width);
}

void splash(const char* title, const char* subtitle)
{
    clear();
    text(title ? title : "paperdiy reader", W / 2, H / 2 - 40, layout::serifFont(58, layout::FACE_BOLD), C_BLACK,
         middle_center);
    if (subtitle) {
        text(subtitle, W / 2, H / 2 + 30, layout::uiFont(28), grey(G_MID), middle_center);
    }
    present(Screen::Message);
}

void progress(const char* title, const char* what, int percent)
{
    percent = std::max(0, std::min(100, percent));
    clear();
    drawHeader(title ? title : "Please wait");

    text(what ? what : "Working", W / 2, H / 2 - 50, layout::uiFont(28), C_BLACK, middle_center);

    const int bar_w = W - 2 * MX - 60;
    const int bar_x = (W - bar_w) / 2;
    const int bar_y = H / 2;
    cv()->drawRoundRect(bar_x, bar_y, bar_w, 26, 6, C_BLACK);
    int fill = ((bar_w - 6) * percent) / 100;
    if (fill > 0) {
        cv()->fillRoundRect(bar_x + 3, bar_y + 3, fill, 20, 4, C_BLACK);
    }
    char pct[16];
    snprintf(pct, sizeof(pct), "%d%%", percent);
    text(pct, W / 2, bar_y + 64, layout::uiFont(32, true), C_BLACK, middle_center);
    drawFooter("First open only", "cached afterwards");
    present(Screen::Progress);
}

void message(const char* title, const char* body, const char* hint)
{
    clear();
    drawHeader(title ? title : "paperdiy reader");
    const lgfx::IFont* f           = layout::uiFont(28);
    std::vector<std::string> lines = layout::wrapText(f, body ? body : "", W - 2 * MX - 20, 10);
    int lh                         = vlw::height(f) + 10;
    int y                          = H / 2 - ((int)lines.size() * lh) / 2;
    for (const std::string& l : lines) {
        text(l.c_str(), W / 2, y, f, C_BLACK, top_center);
        y += lh;
    }
    drawFooter(hint ? hint : "Press any key", nullptr);
    present(Screen::Message);
}

void library(const std::vector<BookEntry>& books, int selection, int first, bool sd_present)
{
    composeLibrary(books, selection, first, sd_present);
    present(Screen::Library);
}

void libraryMenu(const std::vector<BookEntry>& books, int selection, int first, bool sd_present, const char* title,
                 const std::vector<std::string>& items, const std::vector<std::string>& values, int sel)
{
    composeLibrary(books, selection, first, sd_present);
    drawSheet(title, items, values, sel, "A/B move   Hold A: select   Hold B: close");
    present(Screen::LibraryMenu);
}

void readingPage(const BookEntry& book, const layout::Page& page, int page_no, int page_total, const PageInfo& info)
{
    clear();
    drawBody(page);
    drawReadingChrome(book, page_no, page_total, info);
    present(Screen::Reading);
}

void overlayMenu(const BookEntry& book, const layout::Page& page, int page_no, int page_total, const PageInfo& info,
                 const char* title, const std::vector<std::string>& items, const std::vector<std::string>& values,
                 int sel)
{
    clear();
    drawBody(page);
    drawReadingChrome(book, page_no, page_total, info);
    drawSheet(title, items, values, sel, "A/B move   Hold A: select   Hold B: close");
    present(Screen::ReadingMenu);
}

void listScreen(const char* title, const std::vector<ListRow>& rows, int sel, int first, int marker)
{
    clear();
    char note[24];
    snprintf(note, sizeof(note), "%u", (unsigned)rows.size());
    drawHeader(title ? title : "", rows.empty() ? nullptr : note);

    if (rows.empty()) {
        text("Nothing here yet.", W / 2, H / 2, layout::uiFont(28), grey(G_MID), middle_center);
        drawFooter("Hold B: back", nullptr);
        present(Screen::List);
        return;
    }

    const int top   = 76;
    const int row_h = (H - layout::FOOTER_H - 8 - top) / LIST_ROWS;
    const lgfx::IFont* f0 = layout::uiFont(28, true);
    const lgfx::IFont* f1 = layout::uiFont(28);
    const lgfx::IFont* fr = layout::uiFont(24);

    for (int r = 0; r < LIST_ROWS; r++) {
        int idx = first + r;
        if (idx >= (int)rows.size()) {
            break;
        }
        const ListRow& row = rows[(size_t)idx];
        int ry             = top + r * row_h;
        bool active        = (idx == sel);
        uint32_t fg        = active ? C_WHITE : C_BLACK;
        if (active) {
            cv()->fillRoundRect(MX - 10, ry + 2, W - 2 * MX + 20, row_h - 4, 8, C_BLACK);
        } else if (r > 0) {
            cv()->drawFastHLine(MX, ry, W - 2 * MX, grey(G_PALE));
        }
        if (idx == marker) {
            cv()->fillCircle(MX - 2 + (active ? 6 : 0), ry + row_h / 2, 5, active ? C_WHITE : C_BLACK);
        }
        const lgfx::IFont* f = (row.indent == 0) ? f0 : f1;
        int x                = MX + 14 + row.indent * 28;
        int right_w          = row.right.empty() ? 0 : vlw::width(fr, row.right.c_str()) + 16;
        if (!row.right.empty()) {
            text(row.right.c_str(), W - MX - 4, ry + row_h / 2, fr, active ? grey(G_PALE) : grey(G_MID),
                 middle_right);
        }
        std::string t = fitText(row.text, f, W - MX - 4 - x - right_w);
        text(t.c_str(), x, ry + row_h / 2, f, fg, middle_left);
    }

    char pg[32];
    int pages = ((int)rows.size() + LIST_ROWS - 1) / LIST_ROWS;
    snprintf(pg, sizeof(pg), "%d / %d", first / LIST_ROWS + 1, pages);
    drawFooter(pg, "Hold A: go   Hold B: back");
    present(Screen::List);
}

void bookInfo(const BookEntry& book)
{
    clear();
    drawHeader("About this book");

    const int cw = 220;
    const int ch = 320;
    cover::drawFull(cv(), book, MX, 86, cw, ch);

    const int tx = MX + cw + 22;
    const int tw = W - MX - tx;
    int y        = 92;
    const lgfx::IFont* tf = layout::serifFont(32, layout::FACE_BOLD);
    for (const std::string& l : layout::wrapText(tf, book.title, tw, 5)) {
        text(l.c_str(), tx, y, tf, C_BLACK);
        y += vlw::height(tf) + 4;
    }
    y += 8;
    const lgfx::IFont* af = layout::serifFont(28, layout::FACE_ITALIC);
    for (const std::string& l : layout::wrapText(af, book.author.empty() ? "Unknown author" : book.author, tw, 3)) {
        text(l.c_str(), tx, y, af, grey(G_DARK));
        y += vlw::height(af) + 2;
    }

    char sz[24];
    formatBytes(book.size, sz, sizeof(sz));
    char pages[40];
    if (book.total > 0) {
        snprintf(pages, sizeof(pages), "%u pages \xC2\xB7 %d%% read", (unsigned)book.total, book.percent());
    } else {
        snprintf(pages, sizeof(pages), "Not opened yet");
    }
    std::string where = book.path;
    struct Row {
        const char* label;
        std::string value;
    };
    Row rows[] = {
        {"Progress", pages},
        {"Format", book.is_epub ? "EPUB" : "Plain text"},
        {"Size", sz},
        {"Location", where},
    };
    y = 86 + ch + 40;
    for (const Row& r : rows) {
        text(r.label, MX, y, layout::uiFont(24, true), grey(G_MID));
        const lgfx::IFont* vf = layout::uiFont(28);
        for (const std::string& l : layout::wrapText(vf, r.value, W - 2 * MX, 2)) {
            y += 32;
            text(l.c_str(), MX, y, vf, C_BLACK);
        }
        y += 50;
    }
    drawFooter("Any key: back", nullptr);
    present(Screen::Info);
}

/* Wi-Fi draws 100-200 mA against a 140 mAh cell: warn before it runs dry. */
static void drawWifiBatteryWarning()
{
    const int pct = hal.batteryPercent();
    if (pct < 0 || pct >= 30 || hal.usbPowered()) {
        return;
    }
    const int bh = 64;
    const int by = H - layout::FOOTER_H - bh - 6;
    cv()->fillRoundRect(MX - 6, by, W - 2 * MX + 12, bh, 10, C_BLACK);
    char line[64];
    snprintf(line, sizeof(line), "Battery %d%%: Wi-Fi drains it fast", pct);
    text(line, W / 2, by + 20, layout::uiFont(24, true), C_WHITE, middle_center);
    text("Plug in to charge while you upload.", W / 2, by + 46, layout::uiFont(24), C_WHITE, middle_center);
}

void portalScreen(int mode, const char* ap_ssid, const char* url, const char* ssid, const char* message)
{
    clear();
    drawHeader("Wi-Fi & files");
    drawWifiBatteryWarning();
    const lgfx::IFont* f  = layout::uiFont(28);
    const lgfx::IFont* fb = layout::uiFont(32, true);
    const lgfx::IFont* fs = layout::uiFont(24);
    int y                 = 92;

    auto para = [&](const char* t, const lgfx::IFont* font, uint32_t col) {
        for (const std::string& l : layout::wrapText(font, t ? t : "", W - 2 * MX, 4)) {
            text(l.c_str(), W / 2, y, font, col, top_center);
            y += vlw::height(font) + 6;
        }
    };

    if (mode == 1) {  // joining
        y = H / 2 - 80;
        para("Connecting to", f, C_BLACK);
        para(ssid, fb, C_BLACK);
        y += 20;
        para("This takes up to 15 seconds.", fs, grey(G_MID));
        drawFooter("Please wait", nullptr);
        present(Screen::Portal);
        return;
    }

    if (mode == 2) {  // on the home network
        para("Open this address on any phone or computer on", f, C_BLACK);
        para(ssid, fb, C_BLACK);
        y += 10;
        drawQr(url, W / 2, y + 150, 300);
        y += 320;
        para(url, fb, C_BLACK);
        para("or  http://reader.local", f, grey(G_DARK));
        y += 16;
        para("Upload books, manage the SD card, make screensavers.", fs, grey(G_MID));
        drawFooter("Hold A: setup network", "Hold B: done");
        present(Screen::Portal);
        return;
    }

    // Setup access point.
    if (message && message[0]) {
        para(message, fs, C_BLACK);
        y += 6;
    }
    para("1. Join this Wi-Fi on your phone", f, C_BLACK);
    char payload[96];
    snprintf(payload, sizeof(payload), "WIFI:S:%s;T:nopass;;", ap_ssid ? ap_ssid : "");
    drawQr(payload, W / 2, y + 125, 250);
    y += 262;
    para(ap_ssid, fb, C_BLACK);
    y += 10;
    para("2. The setup page opens by itself, or go to", f, C_BLACK);
    para(url, fb, C_BLACK);
    y += 10;
    para("Choose your home Wi-Fi there to use the reader from your own network. Books and screensavers can be "
         "added right away too.",
         fs, grey(G_MID));
    drawFooter(nullptr, "Hold B: done");
    present(Screen::Portal);
}

/* ---- screensaver ------------------------------------------------------ */

namespace {

constexpr const char* SAVER_DIR = STORAGE_SD_BASE "/screensaver";

/* Logos the owner dropped on the card: /screensaver/logo*.png|jpg. Every
 * file whose name starts with "logo" is a variant; names containing "white"
 * or "light" are for dark backgrounds. Read once and kept in PSRAM. */
struct Logo {
    std::vector<uint8_t> data;
    bool light = false;
};
std::vector<Logo> s_logos;
bool s_logo_checked = false;

bool readFile(const std::string& path, std::vector<uint8_t>& out, size_t max_bytes)
{
    struct stat st;
    if (stat(path.c_str(), &st) != 0 || st.st_size <= 0 || (size_t)st.st_size > max_bytes) {
        return false;
    }
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) {
        return false;
    }
    out.resize((size_t)st.st_size);
    bool ok = fread(out.data(), 1, out.size(), f) == out.size();
    fclose(f);
    if (!ok) {
        out.clear();
    }
    return ok;
}

const std::vector<Logo>& logos()
{
    if (!s_logo_checked && hal_storage_sd_available()) {
        s_logo_checked = true;
        hal_storage_lock();
        DIR* d = opendir(SAVER_DIR);
        if (d) {
            struct dirent* e;
            while ((e = readdir(d)) != nullptr && s_logos.size() < 6) {
                const char* n   = e->d_name;
                const char* ext = strrchr(n, '.');
                if (n[0] == '.' || strncasecmp(n, "logo", 4) != 0 || !ext ||
                    (strcasecmp(ext, ".png") && strcasecmp(ext, ".jpg") && strcasecmp(ext, ".jpeg"))) {
                    continue;
                }
                Logo lg;
                if (readFile(std::string(SAVER_DIR) + "/" + n, lg.data, 2u * 1024u * 1024u)) {
                    std::string lower(n);
                    for (char& c : lower) {
                        c = (char)tolower((unsigned char)c);
                    }
                    lg.light = lower.find("white") != std::string::npos || lower.find("light") != std::string::npos;
                    ESP_LOGI(TAG, "screensaver logo: %s%s", n, lg.light ? " (light)" : "");
                    s_logos.push_back(std::move(lg));
                }
            }
            closedir(d);
        }
        hal_storage_unlock();
    }
    return s_logos;
}

/* Card images for SLEEP_PHOTOS: every JPEG/PNG in /screensaver except the logo. */
std::vector<std::string> cardImages()
{
    std::vector<std::string> out;
    if (!hal_storage_sd_available()) {
        return out;
    }
    hal_storage_lock();
    DIR* d = opendir(SAVER_DIR);
    if (d) {
        struct dirent* e;
        while ((e = readdir(d)) != nullptr) {
            const char* n = e->d_name;
            size_t len    = strlen(n);
            if (n[0] == '.' || strncasecmp(n, "logo", 4) == 0 || len < 5) {
                continue;
            }
            const char* ext = strrchr(n, '.');
            if (ext && (!strcasecmp(ext, ".jpg") || !strcasecmp(ext, ".jpeg") || !strcasecmp(ext, ".png"))) {
                out.push_back(std::string(SAVER_DIR) + "/" + n);
            }
        }
        closedir(d);
    }
    hal_storage_unlock();
    return out;
}

/* The wordmark in the space a design left for it: a logo from the card when
 * there is one (a light variant straight onto dark designs, a dark one onto
 * light designs or a white plaque), otherwise "M5Stack / PaperDIY" set in
 * Literata and Atkinson. */
void wordmark(const art::Plaque& pq)
{
    const std::vector<Logo>& all = logos();
    const bool dark_bg           = !pq.dark_text && !pq.box;
    std::vector<const Logo*> fit;
    for (const Logo& l : all) {
        if (l.light == dark_bg) {
            fit.push_back(&l);
        }
    }
    const Logo* lg = nullptr;
    bool plaque    = pq.box;
    if (!fit.empty()) {
        lg = fit[esp_random() % fit.size()];
    } else if (!all.empty()) {
        // Only the wrong variant: put it on a white plaque so it reads.
        lg     = &all[esp_random() % all.size()];
        plaque = !lg->light;
    }
    const int bw = pq.w, bh = pq.h * 4 / 5;
    if (plaque) {
        art::Buf b{(uint8_t*)cv()->getBuffer(), W, H, W};
        art::rrect(b, pq.cx - bw / 2 - 3, pq.cy - bh / 2 - 3, bw + 6, bh + 6, 22, art::level(0));
        art::rrect(b, pq.cx - bw / 2, pq.cy - bh / 2, bw, bh, 19, art::level(15));
    }
    if (lg) {
        const int pad = plaque ? 18 : 0;
        if (cover::drawImage(cv(), lg->data.data(), lg->data.size(), pq.cx - bw / 2 + pad, pq.cy - bh / 2 + pad,
                             bw - 2 * pad, bh - 2 * pad, false)) {
            return;
        }
    }
    const bool dark    = pq.dark_text || plaque;
    const uint32_t fg  = dark ? C_BLACK : C_WHITE;
    const uint32_t fg2 = dark ? grey(G_DARK) : grey(0xDD);
    text("M5Stack", pq.cx, pq.cy - 14, layout::serifFont(58, layout::FACE_BOLD), fg, middle_center);
    text("PaperDIY  \xC2\xB7  reader", pq.cx, pq.cy + 34, layout::uiFont(24), fg2, middle_center);
}

/* The sleep band: what you were reading, and how to wake up. */
void sleepBand(const BookEntry* book)
{
    const int band = 72;
    cv()->fillRect(0, H - band, W, band, C_BLACK);
    if (book && !studio::isStudio(*book)) {
        std::string line = book->title;
        int pct          = book->percent();
        char tail[24]    = "";
        if (pct > 0) {
            snprintf(tail, sizeof(tail), "  \xC2\xB7  %d%%", pct);
        }
        const lgfx::IFont* bf = layout::uiFont(24, true);
        line = fitText(line, bf, W - 2 * MX - vlw::width(bf, tail)) + tail;
        text(line.c_str(), W / 2, H - band + 22, bf, C_WHITE, middle_center);
        text("Sleeping \xC2\xB7 press the power button to wake", W / 2, H - band + 52, layout::uiFont(24),
             grey(0xCC), middle_center);
    } else {
        text("Sleeping \xC2\xB7 press the power button to wake", W / 2, H - band / 2, layout::uiFont(24), C_WHITE,
             middle_center);
    }
}

}  // namespace

const char* sleepModeName(int mode)
{
    if (mode == SLEEP_COVER) {
        return "Book cover";
    }
    if (mode >= SLEEP_DESIGN && mode < SLEEP_DESIGN + (int)art::DESIGN_COUNT) {
        return art::designName(mode - SLEEP_DESIGN);
    }
    if (mode == SLEEP_RANDOM) {
        return "Random";
    }
    if (mode == SLEEP_PHOTOS) {
        return "Card images";
    }
    return "?";
}

void designScreen(int d, uint32_t seed, const BookEntry* book)
{
    (void)book;
    art::Buf b{(uint8_t*)cv()->getBuffer(), W, H, W};
    const uint32_t t0 = millis_();
    art::Plaque pq    = art::draw(b, d, seed);
    wordmark(pq);
    ESP_LOGI(TAG, "design %s drawn in %lu ms", art::designName(d), (unsigned long)(millis_() - t0));
}

void presentStudio(Refresh r)
{
    if (s_next != Refresh::Full) {
        s_next = r;
    }
    present(Screen::Studio);
}

void sleepScreen(const BookEntry* book)
{
    clear();
    const int band = 72;
    int mode       = hal.settings.sleep_mode;
    if (mode >= SLEEP_MODE_COUNT) {
        mode = SLEEP_RANDOM;
    }
    const bool has_cover = book && !studio::isStudio(*book) && book->is_epub && book->cover >= 0;
    std::vector<std::string> photos;
    if (mode == SLEEP_RANDOM || mode == SLEEP_PHOTOS) {
        photos = cardImages();
    }

    if (mode == SLEEP_RANDOM) {
        // Weighted: the cover and the card images count as one choice each.
        int choices = art::DESIGN_COUNT + (has_cover ? 1 : 0) + (photos.empty() ? 0 : 1);
        int pick    = (int)(esp_random() % (uint32_t)choices);
        if (pick < art::DESIGN_COUNT) {
            mode = SLEEP_DESIGN + pick;
        } else if (has_cover && pick == art::DESIGN_COUNT) {
            mode = SLEEP_COVER;
        } else {
            mode = SLEEP_PHOTOS;
        }
    }
    if (mode == SLEEP_COVER && !has_cover) {
        mode = SLEEP_DESIGN + (int)(esp_random() % art::DESIGN_COUNT);
    }

    bool drawn = false;
    if (mode == SLEEP_COVER) {
        cover::drawFull(cv(), *book, 0, 0, W, H - band);
        drawn = true;
    } else if (mode == SLEEP_PHOTOS && !photos.empty()) {
        std::vector<uint8_t> img;
        const std::string& p = photos[esp_random() % photos.size()];
        hal_storage_lock();
        bool ok = readFile(p, img, 6u * 1024u * 1024u);
        hal_storage_unlock();
        if (ok && cover::drawImage(cv(), img.data(), img.size(), 0, 0, W, H - band, true)) {
            ESP_LOGI(TAG, "sleep image %s", p.c_str());
            drawn = true;
        }
    }
    if (!drawn) {
        if (mode < SLEEP_DESIGN || mode >= SLEEP_DESIGN + (int)art::DESIGN_COUNT) {
            mode = SLEEP_DESIGN + (int)(esp_random() % art::DESIGN_COUNT);
        }
        designScreen(mode - SLEEP_DESIGN, esp_random(), book);
    }
    sleepBand(book);
    present(Screen::Sleep);
}

}  // namespace render
