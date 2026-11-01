/*
 * paperdiy-reader — screen drawing and refresh policy.
 *
 * Every screen is composed into hal.Canvas (540x960, 8-bit grey, anti-aliased
 * VLW text) and then *presented*: the canvas is diffed against a shadow copy
 * of what the panel shows, and only the changed bands are pushed and
 * refreshed. Moving the selection in the library therefore refreshes two
 * small tile frames, not the whole screen.
 *
 * Waveform per update (M5GFX Panel_EPD modes):
 *   - epd_fast     selection moves, menus, progress: changed pixels only, no flash
 *   - epd_text     page turns and closing menus: re-inks text, cleans ghosts
 *   - epd_quality  covers, screen changes, and a periodic full clean-up
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <string>
#include <vector>

#include "reader/layout.h"
#include "reader/library.h"

namespace render {

/* ---- library grid ---------------------------------------------------- */
static constexpr int GRID_COLS     = 3;
static constexpr int GRID_ROWS     = 3;
static constexpr int GRID_PER_PAGE = GRID_COLS * GRID_ROWS;

/** @brief Rows shown per screen in the contents / bookmark lists. */
static constexpr int LIST_ROWS = 14;

/** @brief Refresh request for the next present. */
enum class Refresh : uint8_t {
    Auto,    /*!< pick from the screen transition */
    Fast,    /*!< changed pixels only, no flash */
    Text,    /*!< re-ink the changed area */
    Quality, /*!< best greys on the changed area */
    Full,    /*!< whole screen, flashing clean-up */
    Fastest, /*!< animation: changed pixels, quickest waveform, black and white */
};

/** @brief Prepares canvas defaults and the shadow frame. Call once after hal.init(). */
void init();

/** @brief Overrides the waveform of the next screen drawn. */
void nextRefresh(Refresh r);

/** @brief True while the panel is still running an update. */
bool busy();

/** @brief Blocks until queued panel updates finish. */
void waitIdle(uint32_t timeout_ms = 5000);

/** @brief Extra context for drawing a reading page. */
struct PageInfo {
    bool bookmarked  = false;
    int chapter_left = -1; /*!< pages left in this chapter, -1 = unknown */
    std::string chapter;   /*!< current chapter title, may be empty */
};

/** @brief One row of a full-screen list (contents, bookmarks). */
struct ListRow {
    std::string text;
    std::string right; /*!< e.g. page number */
    int indent = 0;    /*!< 0 = top level, 1 = nested */
};

/** @brief Full-screen selectable list (@p first is a multiple of LIST_ROWS). */
void listScreen(const char* title, const std::vector<ListRow>& rows, int sel, int first, int marker);

/** @brief Boot / status splash. */
void splash(const char* title, const char* subtitle);

/** @brief Full-screen progress screen used while paginating or extracting. */
void progress(const char* title, const char* what, int percent);

/** @brief Full-screen informational message. */
void message(const char* title, const char* body, const char* hint = nullptr);

/**
 * @brief Home screen: a 3x3 grid of covers with titles and reading progress.
 * @param first index of the first book on this grid page
 */
void library(const std::vector<BookEntry>& books, int selection, int first, bool sd_present);

/** @brief Library with a menu sheet over it. */
void libraryMenu(const std::vector<BookEntry>& books, int selection, int first, bool sd_present, const char* title,
                 const std::vector<std::string>& items, const std::vector<std::string>& values, int sel);

/** @brief One page of a book. */
void readingPage(const BookEntry& book, const layout::Page& page, int page_no, int page_total, const PageInfo& info);

/** @brief A page with a menu sheet over its lower part. */
void overlayMenu(const BookEntry& book, const layout::Page& page, int page_no, int page_total, const PageInfo& info,
                 const char* title, const std::vector<std::string>& items, const std::vector<std::string>& values,
                 int sel);

/** @brief Book information sheet. */
void bookInfo(const BookEntry& book);

/** @brief Wi-Fi upload portal screen with a join QR code. */
/**
 * @brief Wi-Fi & files screen.
 * @param mode 1 joining, 2 on the home network, 3 setup access point
 */
void portalScreen(int mode, const char* ap_ssid, const char* url, const char* ssid, const char* message);

/** @brief Sleep screen: the book's cover (or a title card) with a wake hint. */
void sleepScreen(const BookEntry* book);

/** @brief Sleep-screen choices (hal.settings.sleep_mode). */
enum SleepMode : uint8_t {
    SLEEP_COVER  = 0, /*!< the book's cover (a design when there is none) */
    SLEEP_DESIGN = 1, /*!< 1..DESIGN_COUNT: one fixed design */
    SLEEP_RANDOM = 6, /*!< cover, any design, or a card image, at random */
    SLEEP_PHOTOS = 7, /*!< a random image from /sdcard/screensaver */
    SLEEP_MODE_COUNT = 8
};

/** @brief Menu label of a sleep mode. */
const char* sleepModeName(int mode);

/**
 * @brief Composes design @p d (with the wordmark or the card's logo) into the
 *        canvas without presenting it.
 */
void designScreen(int d, uint32_t seed, const BookEntry* book);

/** @brief Presents the canvas for an animation frame (Studio). */
void presentStudio(Refresh r);

/** @brief Shortens @p text with an ellipsis so it fits @p max_width pixels. */
std::string fitText(const std::string& text, const lgfx::IFont* font, int max_width);

}  // namespace render
