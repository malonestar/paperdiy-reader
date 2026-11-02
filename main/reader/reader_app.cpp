/*
 * paperdiy-reader — top-level application loop.
 *
 * A single cooperative state machine drives everything. The PaperDIY panel
 * refreshes in well under a second and M5GFX runs the waveform on its own
 * task, so input is handled immediately: every press redraws into the canvas
 * and render::present() pushes only the pixels that changed.
 *
 * Two keys, one grammar everywhere:
 *
 *            click                 hold (0.6 s)
 *   A        next / down           open, select, menu
 *   B        previous / up         back, menu (library)
 *   A + B held 1.5 s               sleep (power off)
 *
 * SPDX-License-Identifier: MIT
 */
#include "reader/reader_app.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "esp_log.h"
#include "esp_system.h"
#include "fonts/vlw_fonts.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "hal/hal.h"
#include "hal/storage/hal_storage.h"
#include "portal/portal.h"
#include "reader/cover.h"
#include "reader/layout.h"
#include "reader/library.h"
#include "reader/render.h"
#include "reader/studio.h"

static const char* TAG = "reader";

namespace reader_app {

namespace {

/* ------------------------------------------------------------------ */
/* State                                                               */
/* ------------------------------------------------------------------ */

enum class State {
    Library,   /* home: cover grid */
    LibMenu,   /* library sheet */
    Confirm,   /* delete confirmation sheet */
    Info,      /* about this book */
    Reading,
    ReadMenu,  /* reading sheet */
    TextMenu,  /* Aa sheet with live preview */
    Goto,
    Toc,
    Bookmarks,
    Portal,
};

State g_state = State::Library;

std::vector<BookEntry> g_books;
bool g_library_loaded = false;
int g_sel             = 0;

BookSession g_session;
int g_page = 0;
layout::Page g_page_data;
layout::Page g_preview; /* Aa menu: current page in the style being chosen */

int g_menu_sel    = 0;
int g_confirm_sel = 0;
int g_goto_pct    = 0;
int g_list_sel    = 0;
int g_list_first  = 0;

layout::Style g_style_at_text_open;

bool g_dirty          = true;
uint32_t g_last_input = 0;


/* ------------------------------------------------------------------ */
/* Buttons                                                             */
/* ------------------------------------------------------------------ */

struct Buttons {
    bool a      = false; /* click (released before the hold threshold) */
    bool b      = false;
    bool a_hold = false; /* fired once when the hold threshold passes */
    bool b_hold = false;
    bool sleep  = false; /* A+B chord held */

    bool any() const
    {
        return a || b || a_hold || b_hold || sleep;
    }
};

constexpr uint32_t HOLD_MS  = 600;
constexpr uint32_t CHORD_MS = 1500;

#define KEY_A M5.BtnA /* KEY1, GPIO4 */
#define KEY_B M5.BtnB /* KEY2, GPIO3 */

Buttons pollButtons()
{
    static bool s_chord       = false;
    static bool s_chord_fired = false;
    static uint32_t s_chord_t = 0;

    Buttons out;
    const bool ap = KEY_A.isPressed();
    const bool bp = KEY_B.isPressed();

    if (ap && bp) {
        if (!s_chord) {
            s_chord       = true;
            s_chord_fired = false;
            s_chord_t     = millis_();
        } else if (!s_chord_fired && millis_() - s_chord_t >= CHORD_MS) {
            s_chord_fired = true;
            out.sleep     = true;
        }
        return out;
    }
    if (s_chord) {
        // Swallow the release events of a chord.
        if (!ap && !bp) {
            s_chord = false;
        }
        return out;
    }

    out.a      = KEY_A.wasClicked();
    out.b      = KEY_B.wasClicked();
    out.a_hold = KEY_A.wasHold();
    out.b_hold = KEY_B.wasHold();
    if (out.any()) {
        ESP_LOGI(TAG, "keys: A%s%s B%s%s", out.a ? " click" : "", out.a_hold ? " hold" : "", out.b ? " click" : "",
                 out.b_hold ? " hold" : "");
    }
    return out;
}

/** Blocks until a key is pressed (or the timeout passes). */
void waitForAnyButton(uint32_t timeout_ms = 120000)
{
    uint32_t t0 = millis_();
    // Let go of whatever was held to get here first.
    while ((KEY_A.isPressed() || KEY_B.isPressed()) && millis_() - t0 < 5000) {
        hal.update();
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    while (millis_() - t0 < timeout_ms) {
        hal.update();
        if (KEY_A.wasReleased() || KEY_B.wasReleased()) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    g_last_input = millis_();
}

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

/** Progress screen for long operations; silent when the work is quick. */
class ProgressThrottle {
public:
    explicit ProgressThrottle(const char* title) : _title(title), _t0(millis_())
    {
    }

    void operator()(int pct, const char* what)
    {
        uint32_t now = millis_();
        if (now - _t0 < 700 || pct == _last_pct || now - _last_ms < 400) {
            return;
        }
        if (pct >= 100 && _last_pct >= 0) {
            return;  // the result screen follows straight away
        }
        _last_pct = pct;
        _last_ms  = now;
        render::progress(_title, what, pct);
    }

private:
    const char* _title;
    uint32_t _t0;
    uint32_t _last_ms = 0;
    int _last_pct     = -1;
};

int gridFirst()
{
    return (g_sel / render::GRID_PER_PAGE) * render::GRID_PER_PAGE;
}

void clampSelection()
{
    if (g_books.empty()) {
        g_sel = 0;
        return;
    }
    if (g_sel < 0) {
        g_sel = (int)g_books.size() - 1;
    }
    if (g_sel >= (int)g_books.size()) {
        g_sel = 0;
    }
}

void scanLibrary()
{
    ProgressThrottle prog("Library");
    library::scan(g_books, [&prog](int pct, const char* what) { prog(pct, what); });
    g_library_loaded = true;
    clampSelection();
}

void ensureLibrary()
{
    if (!g_library_loaded) {
        scanLibrary();
    }
}

void selectPath(const std::string& path)
{
    for (size_t i = 0; i < g_books.size(); i++) {
        if (g_books[i].path == path) {
            g_sel = (int)i;
            return;
        }
    }
}

void saveProgress()
{
    if (!g_session.isOpen()) {
        return;
    }
    const char* path = g_session.book().path.c_str();
    layout::Pos pos  = g_session.pagePos(g_page);
    // Page numbers are stored as 16-bit; clamp absurdly long books.
    hal.progressSet(path, (uint16_t)std::min(g_page, 65535), (uint16_t)std::min(g_session.pageCount(), 65535));
    hal.positionSet(path, pos.off, pos.word);

    for (BookEntry& b : g_books) {
        if (b.path == g_session.book().path) {
            b.page  = (uint16_t)std::min(g_page, 65535);
            b.total = (uint16_t)std::min(g_session.pageCount(), 65535);
            break;
        }
    }
}

bool layoutCurrentPage()
{
    if (!g_session.isOpen()) {
        return false;
    }
    int count = g_session.pageCount();
    g_page    = std::max(0, std::min(g_page, count - 1));
    return g_session.renderPage(g_page, g_page_data);
}

void goToPage(int page)
{
    g_page = page;
    layoutCurrentPage();
    saveProgress();
    g_dirty = true;
}

/**
 * Opens a book.
 * @param start_page page to land on; -1 = the stored reading position.
 */
bool openBook(const BookEntry& book, int start_page)
{
    ProgressThrottle prog("Opening");

    g_session.close();
    if (!g_session.open(book, hal.Canvas, layout::currentStyle(),
                        [&prog](int pct, const char* what) { prog(pct, what); })) {
        render::message("Cannot open",
                        "This file could not be read. It may be damaged, or an EPUB variant this reader does not "
                        "support.");
        waitForAnyButton();
        return false;
    }

    int page = start_page;
    if (page < 0) {
        // The stream position survives font changes; the page number is only
        // a fallback for books read before positions were recorded.
        uint32_t off  = 0;
        uint32_t word = 0;
        if (hal.positionGet(book.path.c_str(), &off, &word)) {
            page = g_session.pageForPos(layout::Pos{off, word});
        } else {
            page = (int)book.page;
        }
    }
    g_page = std::max(0, std::min(page, g_session.pageCount() - 1));

    hal.resumeSave(book.path.c_str());
    hal.recentTouch(book.path.c_str());
    for (BookEntry& b : g_books) {
        if (b.path == book.path) {
            b.recent = hal.recentGet(book.path.c_str());
        }
    }
    saveProgress();

    if (!layoutCurrentPage()) {
        ESP_LOGW(TAG, "page %d could not be laid out", g_page);
    }
    return true;
}

/** Re-opens the current book with the current typography, keeping the place. */
void repaginateKeepingPosition()
{
    if (!g_session.isOpen()) {
        return;
    }
    BookEntry book  = g_session.book();
    layout::Pos pos = g_session.pagePos(g_page);
    g_session.close();

    if (!openBook(book, 0)) {
        g_state = State::Library;
        ensureLibrary();
        return;
    }
    goToPage(g_session.pageForPos(pos));
}

render::PageInfo pageInfo()
{
    render::PageInfo info;
    info.bookmarked = g_session.isBookmarked(g_page);
    int first       = 0;
    int end         = 0;
    std::string title;
    if (g_session.chapterSpan(g_page, &first, &end, &title)) {
        info.chapter      = title;
        info.chapter_left = std::max(0, end - g_page - 1);
    }
    return info;
}

/** First words on the current page, used as a bookmark label. */
std::string pageSnippet()
{
    std::string s;
    for (const layout::Line& line : g_page_data.lines) {
        for (const layout::Word& w : line.words) {
            if (!s.empty()) {
                s.push_back(' ');
            }
            s += w.text;
            if (s.size() > 80) {
                return s;
            }
        }
    }
    return s;
}

void backToLibrary()
{
    saveProgress();
    std::string path = g_session.isOpen() ? g_session.book().path : std::string();
    g_session.close();
    hal.resumeClear();  // waking up should land on the library now
    ensureLibrary();
    if (hal.settings.sort_mode == library::SORT_RECENT) {
        library::sortBooks(g_books, library::SORT_RECENT);
    }
    selectPath(path);
    g_state = State::Library;
    g_dirty = true;
}

void goToSleep()
{
    ESP_LOGI(TAG, "going to sleep");
    saveProgress();

    const BookEntry* shown = nullptr;
    BookEntry recent;
    if (g_session.isOpen()) {
        recent = g_session.book();
        shown  = &recent;
    } else if (!g_books.empty()) {
        // The most recently read book, like a Kindle's lock screen.
        const BookEntry* best = nullptr;
        for (const BookEntry& b : g_books) {
            if (b.recent && (!best || b.recent > best->recent)) {
                best = &b;
            }
        }
        if (best) {
            recent = *best;
            shown  = &recent;
        }
    }
    render::sleepScreen(shown);
    render::waitIdle(8000);
    vTaskDelay(pdMS_TO_TICKS(300));

    M5.Power.powerOff();

    // Still running: USB power keeps the PMIC on. Stay on the sleep screen
    // until a key is pressed, then carry on where we were.
    vTaskDelay(pdMS_TO_TICKS(1500));
    ESP_LOGI(TAG, "power-off did not take (USB power?); waiting for a key");
    waitForAnyButton(UINT32_MAX);
    render::nextRefresh(render::Refresh::Full);
    g_dirty = true;
}

/* ------------------------------------------------------------------ */
/* Menus                                                               */
/* ------------------------------------------------------------------ */

const uint8_t kTimeouts[]  = {2, 5, 10, 30, 0};
const uint8_t kFullEvery[] = {4, 8, 16, 0};

std::string timeoutLabel(uint8_t minutes)
{
    if (minutes == 0) {
        return "Never";
    }
    char buf[16];
    snprintf(buf, sizeof(buf), "%u min", (unsigned)minutes);
    return std::string(buf);
}

std::string fullEveryLabel(uint8_t n)
{
    if (n == 0) {
        return "Never";
    }
    char buf[24];
    snprintf(buf, sizeof(buf), "Every %u pages", (unsigned)n);
    return std::string(buf);
}

template <size_t N>
uint8_t cycle(const uint8_t (&opts)[N], uint8_t cur)
{
    for (size_t i = 0; i < N; i++) {
        if (opts[i] == cur) {
            return opts[(i + 1) % N];
        }
    }
    return opts[0];
}

/* ---- library sheet ---- */

enum LibItem { LM_OPEN = 0, LM_INFO, LM_SORT, LM_WIFI, LM_REFRESH, LM_SAVER, LM_STUDIO, LM_FULL, LM_SLEEP_AFTER,
               LM_DELETE, LM_SLEEP, LM_COUNT };

void libItems(std::vector<std::string>& items, std::vector<std::string>& values)
{
    static const char* kSort[] = {"Recent", "Title", "Author"};
    items  = {"Open", "About this book", "Sort by", "Wi-Fi & files", "Rescan library", "Sleep screen",
              "Studio shelf", "Screen clean-up", "Sleep after", "Delete book", "Sleep now"};
    values = {"", "", kSort[hal.settings.sort_mode % 3],
              hal.settings.sta_ssid[0] ? std::string(hal.settings.sta_ssid) : std::string("Set up"), "",
              render::sleepModeName(hal.settings.sleep_mode),
              hal.settings.show_studio ? "Shown" : "Hidden", fullEveryLabel(hal.settings.full_every),
              timeoutLabel(hal.settings.timeout_min), "", ""};
}

/* ---- reading sheet ---- */

enum ReadItem { RM_CONTENTS = 0, RM_BOOKMARK, RM_BOOKMARKS, RM_TEXT, RM_GOTO, RM_LIBRARY, RM_SAVER, RM_FULL,
                RM_SLEEP_AFTER, RM_SLEEP, RM_COUNT };

void readItems(std::vector<std::string>& items, std::vector<std::string>& values)
{
    char nbm[16];
    snprintf(nbm, sizeof(nbm), "%u", (unsigned)g_session.bookmarks().size());
    char ntoc[16];
    snprintf(ntoc, sizeof(ntoc), "%u", (unsigned)g_session.toc().size());
    items  = {"Contents", g_session.isBookmarked(g_page) ? "Remove bookmark" : "Bookmark this page", "Bookmarks",
              "Text & font  (Aa)", "Go to\xE2\x80\xA6", "Library", "Sleep screen", "Screen clean-up", "Sleep after",
              "Sleep now"};
    values = {ntoc, "", nbm, "", "", "", render::sleepModeName(hal.settings.sleep_mode),
              fullEveryLabel(hal.settings.full_every), timeoutLabel(hal.settings.timeout_min), ""};
}

/* ---- Aa sheet ---- */

enum TextItem { TX_SIZE = 0, TX_FONT, TX_SPACING, TX_MARGINS, TX_DONE, TX_COUNT };

void textItems(std::vector<std::string>& items, std::vector<std::string>& values)
{
    static const char* kSpacing[] = {"Tight", "Normal", "Loose"};
    static const char* kMargins[] = {"Narrow", "Normal", "Wide"};
    char size[16];
    snprintf(size, sizeof(size), "%u of %u", (unsigned)hal.settings.font_size + 1, (unsigned)FONT_SIZE_COUNT);
    items  = {"Size", "Typeface", "Line spacing", "Margins", "Done"};
    values = {size, hal.settings.font_family == FONT_SANS ? "Atkinson (sans)" : "Literata (serif)",
              kSpacing[hal.settings.spacing % SPACING_COUNT], kMargins[hal.settings.margins % MARGIN_COUNT], ""};
}

void refreshPreview()
{
    g_session.previewPage(g_session.pagePos(g_page), layout::currentStyle(), g_preview);
}

/* ---- lists ---- */

int listCount()
{
    return (g_state == State::Toc) ? (int)g_session.toc().size() : (int)g_session.bookmarks().size();
}

void listSetSel(int sel)
{
    int n = listCount();
    if (n <= 0) {
        g_list_sel   = 0;
        g_list_first = 0;
        return;
    }
    g_list_sel   = (sel + n) % n;
    g_list_first = (g_list_sel / render::LIST_ROWS) * render::LIST_ROWS;
}

/* ------------------------------------------------------------------ */
/* Drawing                                                             */
/* ------------------------------------------------------------------ */

/**
 * Resolves every already-cached cover on the visible grid page before it is
 * drawn, so returning to the library paints all covers in one refresh. Only
 * books never seen before are left for loadOneCover() to decode.
 */
void prepareCovers()
{
    const int first = gridFirst();
    for (int i = 0; i < render::GRID_PER_PAGE && first + i < (int)g_books.size(); i++) {
        cover::loadCached(g_books[(size_t)(first + i)]);
    }
}

void draw()
{
    const bool sd = hal_storage_sd_available();
    switch (g_state) {
        case State::Library:
            prepareCovers();
            render::library(g_books, g_sel, gridFirst(), sd);
            break;

        case State::LibMenu: {
            std::vector<std::string> items, values;
            libItems(items, values);
            std::string title = g_books.empty() ? std::string("Library") : g_books[(size_t)g_sel].title;
            render::libraryMenu(g_books, g_sel, gridFirst(), sd, title.c_str(), items, values, g_menu_sel);
            break;
        }

        case State::Confirm: {
            std::vector<std::string> items  = {"Keep it", "Delete it"};
            std::vector<std::string> values = {"", ""};
            render::libraryMenu(g_books, g_sel, gridFirst(), sd, "Delete this book?", items, values, g_confirm_sel);
            break;
        }

        case State::Info:
            if (!g_books.empty()) {
                render::bookInfo(g_books[(size_t)g_sel]);
            }
            break;

        case State::Reading:
            render::readingPage(g_session.book(), g_page_data, g_page, g_session.pageCount(), pageInfo());
            break;

        case State::ReadMenu: {
            std::vector<std::string> items, values;
            readItems(items, values);
            render::overlayMenu(g_session.book(), g_page_data, g_page, g_session.pageCount(), pageInfo(), "Menu",
                                items, values, g_menu_sel);
            break;
        }

        case State::TextMenu: {
            std::vector<std::string> items, values;
            textItems(items, values);
            render::overlayMenu(g_session.book(), g_preview.lines.empty() ? g_page_data : g_preview, g_page,
                                g_session.pageCount(), pageInfo(), "Text & font", items, values, g_menu_sel);
            break;
        }

        case State::Goto: {
            int count = std::max(1, g_session.pageCount());
            int page  = std::min(count - 1, (g_goto_pct * count) / 100);
            char pct[24];
            snprintf(pct, sizeof(pct), "%d%%", g_goto_pct);
            char pg[32];
            snprintf(pg, sizeof(pg), "page %d of %d", page + 1, count);
            std::vector<std::string> items  = {"A +5%   B \xE2\x88\x92" "5%", "Jump"};
            std::vector<std::string> values = {pct, pg};
            render::overlayMenu(g_session.book(), g_page_data, g_page, g_session.pageCount(), pageInfo(), "Go to",
                                items, values, 1);
            break;
        }

        case State::Toc: {
            std::vector<render::ListRow> rows;
            for (size_t i = 0; i < g_session.toc().size(); i++) {
                const TocEntry& e = g_session.toc()[i];
                render::ListRow r;
                r.text   = e.title;
                r.indent = (e.level > 1) ? 1 : 0;
                char pg[12];
                snprintf(pg, sizeof(pg), "%d", g_session.tocPage((int)i) + 1);
                r.right = pg;
                rows.push_back(r);
            }
            render::listScreen("Contents", rows, g_list_sel, g_list_first, g_session.tocIndexForPage(g_page));
            break;
        }

        case State::Bookmarks: {
            std::vector<render::ListRow> rows;
            int here = -1;
            for (size_t i = 0; i < g_session.bookmarks().size(); i++) {
                const Bookmark& b = g_session.bookmarks()[i];
                render::ListRow r;
                r.text = b.snippet.empty() ? std::string("(bookmark)") : b.snippet;
                int pg = g_session.pageForPos(b.pos);
                if (pg == g_page) {
                    here = (int)i;
                }
                char buf[12];
                snprintf(buf, sizeof(buf), "%d", pg + 1);
                r.right = buf;
                rows.push_back(r);
            }
            render::listScreen("Bookmarks", rows, g_list_sel, g_list_first, here);
            break;
        }

        case State::Portal:
        {
            const portal::Info& in = portal::info();
            render::portalScreen((int)in.mode, in.ap_ssid.c_str(), in.url.c_str(), in.ssid.c_str(),
                                 in.message.c_str());
        }
            break;
    }
}

/* ------------------------------------------------------------------ */
/* State handlers                                                      */
/* ------------------------------------------------------------------ */

/** Moves a menu cursor: A down, B up, wrapping. @return true if consumed. */
bool menuMove(const Buttons& btn, int& sel, int n)
{
    // Menus follow the physical stack: top key (A) up, bottom key (B) down.
    if (btn.b) {
        sel = (sel + 1) % n;
        g_dirty = true;
        return true;
    }
    if (btn.a) {
        sel = (sel + n - 1) % n;
        g_dirty = true;
        return true;
    }
    return false;
}

void startPortal()
{
    g_state = State::Portal;
    if (hal.settings.sta_ssid[0]) {
        // Show "Connecting to ..." while the join runs (it blocks up to 15 s).
        render::portalScreen(1, "", "", hal.settings.sta_ssid, "");
    } else {
        render::message("Wi-Fi & files", "Starting the setup network. This takes a few seconds.", "Please wait");
    }
    if (portal::start() != ESP_OK) {
        portal::stop();
        render::message("Wi-Fi failed", "Wi-Fi could not be started. Try again, or restart the reader.");
        waitForAnyButton();
        g_state = State::Library;
    }
    g_dirty = true;
}

/** Opens a library entry: a book, or a Studio piece. */
void openEntry(int idx)
{
    BookEntry& e = g_books[(size_t)idx];
    if (studio::isStudio(e)) {
        bool sleep = studio::run(studio::pieceOf(e));
        g_state    = State::Library;
        g_last_input = millis_();
        if (sleep) {
            goToSleep();
        }
        render::nextRefresh(render::Refresh::Full);
        g_dirty = true;
        return;
    }
    if (openBook(e, -1)) {
        g_state = State::Reading;
    } else {
        g_state = State::Library;
    }
    g_dirty = true;
}

void cycleSleepMode()
{
    hal.settings.sleep_mode = (uint8_t)((hal.settings.sleep_mode + 1) % render::SLEEP_MODE_COUNT);
    hal.settingsSave();
}

void handleLibrary(const Buttons& btn)
{
    if (btn.b_hold) {
        g_menu_sel = g_books.empty() ? LM_WIFI : LM_OPEN;
        g_state    = State::LibMenu;
        g_dirty    = true;
        return;
    }
    if (g_books.empty()) {
        return;
    }
    if (btn.b) {  // bottom key: forward through the shelf
        g_sel++;
        clampSelection();
        g_dirty = true;
        return;
    }
    if (btn.a) {  // top key: back
        g_sel--;
        clampSelection();
        g_dirty = true;
        return;
    }
    if (btn.a_hold) {
        openEntry(g_sel);
    }
}

void handleLibMenu(const Buttons& btn)
{
    if (btn.b_hold) {
        g_state = State::Library;
        g_dirty = true;
        return;
    }
    if (menuMove(btn, g_menu_sel, LM_COUNT) || !btn.a_hold) {
        return;
    }

    const bool have = !g_books.empty();
    switch (g_menu_sel) {
        case LM_OPEN:
            if (have) {
                openEntry(g_sel);
                return;
            }
            g_state = State::Library;
            break;
        case LM_INFO:
            g_state = (have && !studio::isStudio(g_books[(size_t)g_sel])) ? State::Info : State::Library;
            break;
        case LM_SAVER:
            cycleSleepMode();
            break;
        case LM_STUDIO: {
            std::string path        = have ? g_books[(size_t)g_sel].path : std::string();
            hal.settings.show_studio = hal.settings.show_studio ? 0 : 1;
            hal.settingsSave();
            g_books.erase(std::remove_if(g_books.begin(), g_books.end(),
                                         [](const BookEntry& b) { return studio::isStudio(b); }),
                          g_books.end());
            if (hal.settings.show_studio) {
                studio::addEntries(g_books);
            }
            selectPath(path);
            clampSelection();
            break;
        }
        case LM_SORT: {
            std::string path       = have ? g_books[(size_t)g_sel].path : std::string();
            hal.settings.sort_mode = (uint8_t)((hal.settings.sort_mode + 1) % 3);
            hal.settingsSave();
            library::sortBooks(g_books, hal.settings.sort_mode);
            selectPath(path);
            break;
        }
        case LM_WIFI:
            startPortal();
            return;
        case LM_REFRESH: {
            std::string path = have ? g_books[(size_t)g_sel].path : std::string();
            scanLibrary();
            selectPath(path);
            g_state = State::Library;
            render::nextRefresh(render::Refresh::Full);
            break;
        }
        case LM_FULL:
            hal.settings.full_every = cycle(kFullEvery, hal.settings.full_every);
            hal.settingsSave();
            break;
        case LM_SLEEP_AFTER:
            hal.settings.timeout_min = cycle(kTimeouts, hal.settings.timeout_min);
            hal.settingsSave();
            break;
        case LM_DELETE:
            if (have && !studio::isStudio(g_books[(size_t)g_sel])) {
                g_confirm_sel = 0;
                g_state       = State::Confirm;
            }
            break;
        case LM_SLEEP:
            g_state = State::Library;
            goToSleep();
            break;
        default:
            break;
    }
    g_dirty = true;
}

void handleConfirm(const Buttons& btn)
{
    if (btn.b_hold) {
        g_state = State::LibMenu;
        g_dirty = true;
        return;
    }
    if (menuMove(btn, g_confirm_sel, 2) || !btn.a_hold) {
        return;
    }
    if (g_confirm_sel == 1 && !g_books.empty()) {
        BookEntry victim = g_books[(size_t)g_sel];
        char rpath[256];
        if (hal.resumeLoad(rpath, sizeof(rpath)) && victim.path == rpath) {
            hal.resumeClear();
        }
        cover::forget(victim);
        library::removeBook(victim);
        g_books.erase(g_books.begin() + g_sel);
        clampSelection();
    }
    g_state = State::Library;
    g_dirty = true;
}

void handleInfo(const Buttons& btn)
{
    if (btn.any()) {
        g_state = State::Library;
        g_dirty = true;
    }
}

void handleReading(const Buttons& btn)
{
    if (btn.a) {
        if (g_page + 1 < g_session.pageCount()) {
            goToPage(g_page + 1);
        }
        return;
    }
    if (btn.b) {
        if (g_page > 0) {
            goToPage(g_page - 1);
        }
        return;
    }
    if (btn.a_hold) {
        g_menu_sel = 0;
        g_state    = State::ReadMenu;
        g_dirty    = true;
        return;
    }
    if (btn.b_hold) {
        backToLibrary();
    }
}

void handleReadMenu(const Buttons& btn)
{
    if (btn.b_hold) {
        g_state = State::Reading;
        g_dirty = true;
        return;
    }
    if (menuMove(btn, g_menu_sel, RM_COUNT) || !btn.a_hold) {
        return;
    }

    switch (g_menu_sel) {
        case RM_CONTENTS:
            g_state = State::Toc;
            listSetSel(std::max(0, g_session.tocIndexForPage(g_page)));
            break;

        case RM_BOOKMARK:
            g_session.toggleBookmark(g_page, pageSnippet());
            g_state = State::Reading;  // the ribbon shows the result
            break;

        case RM_BOOKMARKS: {
            g_state = State::Bookmarks;
            int sel = 0;
            for (size_t i = 0; i < g_session.bookmarks().size(); i++) {
                if (g_session.pageForPos(g_session.bookmarks()[i].pos) <= g_page) {
                    sel = (int)i;
                }
            }
            listSetSel(sel);
            break;
        }

        case RM_TEXT:
            g_menu_sel           = 0;
            g_style_at_text_open = layout::currentStyle();
            g_preview            = g_page_data;
            g_state              = State::TextMenu;
            break;

        case RM_GOTO: {
            int count  = g_session.pageCount();
            g_goto_pct = (count > 0) ? (((g_page + 1) * 100) / count) / 5 * 5 : 0;
            g_state    = State::Goto;
            break;
        }

        case RM_LIBRARY:
            backToLibrary();
            return;

        case RM_SAVER:
            cycleSleepMode();
            break;

        case RM_FULL:
            hal.settings.full_every = cycle(kFullEvery, hal.settings.full_every);
            hal.settingsSave();
            break;

        case RM_SLEEP_AFTER:
            hal.settings.timeout_min = cycle(kTimeouts, hal.settings.timeout_min);
            hal.settingsSave();
            break;

        case RM_SLEEP:
            g_state = State::Reading;
            goToSleep();
            break;

        default:
            break;
    }
    g_dirty = true;
}

/** Leaves the Aa sheet, repaginating only if the typography changed. */
void closeTextMenu()
{
    hal.settingsSave();
    g_state = State::Reading;
    if (layout::styleHash(layout::currentStyle()) != layout::styleHash(g_style_at_text_open)) {
        repaginateKeepingPosition();
        render::nextRefresh(render::Refresh::Full);
    }
    g_preview.lines.clear();
    g_dirty = true;
}

void handleTextMenu(const Buttons& btn)
{
    if (btn.b_hold) {
        closeTextMenu();
        return;
    }
    if (menuMove(btn, g_menu_sel, TX_COUNT) || !btn.a_hold) {
        return;
    }
    switch (g_menu_sel) {
        case TX_SIZE:
            hal.settings.font_size = (uint8_t)((hal.settings.font_size + 1) % FONT_SIZE_COUNT);
            break;
        case TX_FONT:
            hal.settings.font_family = (uint8_t)((hal.settings.font_family + 1) % FONT_FAMILY_COUNT);
            break;
        case TX_SPACING:
            hal.settings.spacing = (uint8_t)((hal.settings.spacing + 1) % SPACING_COUNT);
            break;
        case TX_MARGINS:
            hal.settings.margins = (uint8_t)((hal.settings.margins + 1) % MARGIN_COUNT);
            break;
        default:
            closeTextMenu();
            return;
    }
    refreshPreview();
    render::nextRefresh(render::Refresh::Text);  // the text behind the sheet changed
    g_dirty = true;
}

void handleGoto(const Buttons& btn)
{
    if (btn.b_hold) {
        g_state = State::ReadMenu;
        g_dirty = true;
        return;
    }
    if (btn.a || btn.b) {
        g_goto_pct = std::max(0, std::min(100, g_goto_pct + (btn.a ? 5 : -5)));
        g_dirty    = true;
        return;
    }
    if (btn.a_hold) {
        int count = g_session.pageCount();
        int page  = (count > 0) ? (g_goto_pct * count) / 100 : 0;
        g_state   = State::Reading;
        goToPage(std::max(0, std::min(page, count - 1)));
    }
}

void handleList(const Buttons& btn)
{
    const int n = listCount();
    if (btn.b_hold) {
        g_state = State::Reading;
        g_dirty = true;
        return;
    }
    if (n == 0) {
        if (btn.a_hold) {
            g_state = State::Reading;
            g_dirty = true;
        }
        return;
    }
    if (btn.a || btn.b) {
        listSetSel(g_list_sel + (btn.b ? 1 : -1));  // top up, bottom down
        g_dirty = true;
        return;
    }
    if (btn.a_hold) {
        int page = (g_state == State::Toc) ? g_session.tocPage(g_list_sel)
                                           : g_session.pageForPos(g_session.bookmarks()[(size_t)g_list_sel].pos);
        g_state  = State::Reading;
        goToPage(page);
    }
}

void leavePortal()
{
    portal::stop();  // radio off
    scanLibrary();   // pick up uploads, renames and deletions
    g_state = State::Library;
    render::nextRefresh(render::Refresh::Full);
    g_dirty = true;
}

void handlePortal(const Buttons& btn)
{
    if (portal::finishRequested() || btn.b_hold) {
        leavePortal();
        return;
    }
    if (btn.a_hold && portal::info().mode == portal::Mode::Lan) {
        render::message("Wi-Fi & files", "Starting the setup network. This takes a few seconds.", "Please wait");
        portal::forceSetup();
    }
}

void dispatch(const Buttons& btn)
{
    if (btn.sleep && g_state != State::Portal) {
        goToSleep();
        return;
    }
    switch (g_state) {
        case State::Library:
            handleLibrary(btn);
            break;
        case State::LibMenu:
            handleLibMenu(btn);
            break;
        case State::Confirm:
            handleConfirm(btn);
            break;
        case State::Info:
            handleInfo(btn);
            break;
        case State::Reading:
            handleReading(btn);
            break;
        case State::ReadMenu:
            handleReadMenu(btn);
            break;
        case State::TextMenu:
            handleTextMenu(btn);
            break;
        case State::Goto:
            handleGoto(btn);
            break;
        case State::Toc:
        case State::Bookmarks:
            handleList(btn);
            break;
        case State::Portal:
            handlePortal(btn);
            break;
    }
}

/**
 * Background work while the library is idle: decode one missing cover on
 * the current grid page per call, so covers fill in tile by tile.
 * @return true when a cover was produced (the grid needs a redraw).
 */
/** True while a book on the visible grid page still has no cover state. */
bool visibleCoversPending()
{
    if (g_state != State::Library) {
        return false;
    }
    const int first = gridFirst();
    for (int i = 0; i < render::GRID_PER_PAGE && first + i < (int)g_books.size(); i++) {
        if (!cover::known(g_books[(size_t)(first + i)])) {
            return true;
        }
    }
    return false;
}

bool loadOneCover()
{
    if (g_state != State::Library || g_books.empty()) {
        return false;
    }
    const int first = gridFirst();
    // The visible page first, then the next one so paging forward is instant.
    for (int i = 0; i < render::GRID_PER_PAGE * 2; i++) {
        int idx = first + i;
        if (idx >= (int)g_books.size()) {
            break;
        }
        BookEntry& b = g_books[(size_t)idx];
        if (!cover::known(b)) {
            cover::ensureThumb(b);
            return i < render::GRID_PER_PAGE;
        }
    }
    return false;
}

}  // namespace

/* ================================================================== */

void run()
{
    render::init();
    KEY_A.setHoldThresh(HOLD_MS);
    KEY_B.setHoldThresh(HOLD_MS);

    if (!vlw::init()) {
        render::message("Fonts missing",
                        "The font pack is not installed. Flash fonts.bin to the fonts partition "
                        "(build.ps1 reader fonts) and restart.",
                        "");
        while (true) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }

    if (hal_storage_init() != ESP_OK) {
        render::message("Storage error", "The internal storage partition could not be mounted.", "");
    }

    // Books flashed or uploaded to internal storage move to the SD card,
    // where there is room for their caches.
    if (hal_storage_sd_available()) {
        int moved = 0;
        hal_storage_copy_books_to_sd(
            &moved, [](const char* from, const char* to, void*) { library::bookMoved(from, to); }, nullptr);
        if (moved > 0) {
            ESP_LOGI(TAG, "moved %d book(s) from internal storage to the SD card", moved);
        }
    }

    g_last_input = millis_();

    // Resume straight into the book that was open at power-off.
    bool resumed = false;
    char rpath[256];
    if (hal.resumeLoad(rpath, sizeof(rpath))) {
        BookEntry book;
        if (library::entryForPath(rpath, book)) {
            ESP_LOGI(TAG, "resuming %s", rpath);
            if (openBook(book, -1)) {
                g_state = State::Reading;
                resumed = true;
            }
        } else {
            hal.resumeClear();
        }
    }
    if (!resumed) {
        scanLibrary();
        g_sel   = 0;
        g_state = State::Library;
    }
    render::nextRefresh(render::Refresh::Full);
    g_dirty = true;

    uint32_t usb_check_ms = 0;
    bool on_usb           = false;

    while (true) {
        hal.update();

        Buttons btn = pollButtons();
        if (btn.any()) {
            g_last_input = millis_();
            dispatch(btn);
        } else if (g_state == State::Portal) {
            if (portal::poll()) {
                g_dirty = true;  // Wi-Fi state changed: redraw the page
            }
            handlePortal(btn);  // honour "Done" from the browser
        }

        if (g_dirty) {
            g_dirty = false;
            draw();
        } else if (millis_() - g_last_input > 150) {
            // First-time covers: decode one per pass, but batch the redraws
            // (every 2.5 s, or once the page is complete) instead of one panel
            // refresh per cover.
            static bool s_cover_dirty  = false;
            static uint32_t s_cover_ms = 0;
            if (loadOneCover()) {
                if (!s_cover_dirty) {
                    s_cover_ms = millis_();
                }
                s_cover_dirty = true;
            }
            if (s_cover_dirty && (!visibleCoversPending() || millis_() - s_cover_ms > 2500)) {
                s_cover_dirty = false;
                render::nextRefresh(render::Refresh::Quality);
                g_dirty = true;
            }
        }

        // Idle power-off (never on USB power: it would also drop the USB link).
        if (millis_() - usb_check_ms > 5000) {
            usb_check_ms = millis_();
            on_usb       = hal.usbPowered();
        }
        if (hal.settings.timeout_min > 0 && !on_usb) {
            uint32_t idle_ms  = millis_() - g_last_input;
            uint32_t limit_ms = (uint32_t)hal.settings.timeout_min * 60000u;
            if (g_state == State::Portal) {
                if (portal::idleMs() > 2 * limit_ms && idle_ms > 2 * limit_ms) {
                    leavePortal();
                    goToSleep();
                }
            } else if (idle_ms > limit_ms) {
                goToSleep();
            }
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

}  // namespace reader_app
