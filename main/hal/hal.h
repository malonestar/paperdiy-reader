/*
 * paperdiy-reader — hardware abstraction for the M5Stack PaperDIY (K159).
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <cstddef>
#include <cstdint>

#include <M5GFX.h>
#include <M5Unified.hpp>

/* ------------------------------------------------------------------ */
/* Settings                                                            */
/* ------------------------------------------------------------------ */

/** @brief Body-text size presets (1..5 in the UI). */
enum FontSizePreset : uint8_t {
    FONT_SIZE_1 = 0, /*!< 24 px em (~7.5 pt at 234 ppi) */
    FONT_SIZE_2 = 1, /*!< 28 px */
    FONT_SIZE_3 = 2, /*!< 32 px (default, ~10 pt) */
    FONT_SIZE_4 = 3, /*!< 36 px */
    FONT_SIZE_5 = 4, /*!< 42 px */
    FONT_SIZE_COUNT
};

enum FontFamily : uint8_t {
    FONT_SERIF = 0, /*!< Literata */
    FONT_SANS  = 1, /*!< Atkinson Hyperlegible */
    FONT_FAMILY_COUNT
};

enum LineSpacing : uint8_t {
    SPACING_TIGHT  = 0,
    SPACING_NORMAL = 1,
    SPACING_LOOSE  = 2,
    SPACING_COUNT
};

enum MarginPreset : uint8_t {
    MARGIN_NARROW = 0,
    MARGIN_NORMAL = 1,
    MARGIN_WIDE   = 2,
    MARGIN_COUNT
};

/** @brief Kept for settings compatibility; greyscale UI has no colour. */
enum ThemeMode : uint8_t {
    THEME_LIGHT = 0,
    THEME_COUNT = 1
};

/** @brief Persisted user settings (NVS namespace "reader"). */
struct ReaderSettings {
    uint8_t font_size;   /*!< FontSizePreset */
    uint8_t font_family; /*!< FontFamily */
    uint8_t spacing;     /*!< LineSpacing */
    uint8_t margins;     /*!< MarginPreset */
    uint8_t theme;       /*!< unused */
    uint8_t timeout_min; /*!< idle minutes before power-off, 0 = never */
    uint8_t full_every;  /*!< full (flashing) refresh every N page turns, 0 = never */
    uint8_t sort_mode;   /*!< library order: 0 recent, 1 title, 2 author */
    uint8_t sleep_mode;  /*!< render::SleepMode: cover, a design, random, card images */
    uint8_t show_studio; /*!< show the Studio animations on the library shelf */
    char sta_ssid[33];   /*!< optional home Wi-Fi for the upload portal */
    char sta_pass[65];
};

static constexpr const char* READER_NVS_NAMESPACE = "reader";

/* ------------------------------------------------------------------ */
/* Hal                                                                 */
/* ------------------------------------------------------------------ */

class Hal {
public:
    M5Canvas* Canvas = nullptr; /*!< 540x960 8-bit greyscale working canvas */

    ReaderSettings settings{};

    void init();
    void update();

    void settingsLoad();
    void settingsSave();
    void settingsDefaults();

    void getDeviceMac(uint8_t* mac);
    void getShortId(char* out, size_t out_size);

    /** @brief Battery charge 0..100, or -1 when unknown. */
    int batteryPercent();
    bool charging();
    bool usbPowered();

    /* -------- Reading progress (NVS) ---------------------------------- */
    static uint32_t pathHash(const char* path);
    bool progressGet(const char* path, uint16_t* page, uint16_t* total_pages);
    void progressSet(const char* path, uint16_t page, uint16_t total_pages);
    void progressClear(const char* path);
    /** @brief Font-independent position (stream offset + word index). */
    bool positionGet(const char* path, uint32_t* off, uint32_t* word);
    void positionSet(const char* path, uint32_t off, uint32_t word);

    /** @brief Moves progress, position and recent stamp to a new path (rename/move). */
    void progressMove(const char* from, const char* to);

    /** @brief "Last opened" stamp (monotonic counter, no RTC), 0 = never. */
    uint32_t recentGet(const char* path);
    /** @brief Marks a book as the most recently opened one. */
    void recentTouch(const char* path);

    /* -------- Resume: the book reopened at power-on -------------------- */
    void resumeSave(const char* path);
    bool resumeLoad(char* path_out, size_t path_out_size);
    void resumeClear();

    /** @brief Powers the device off through the M5PM1. Does not return. */
    [[noreturn]] void powerOff();
};

extern Hal hal;

uint32_t millis_();
