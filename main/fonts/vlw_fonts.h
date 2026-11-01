/*
 * paperdiy-reader — anti-aliased VLW fonts from the "fonts" flash partition.
 *
 * tools/gen_vlw.py renders Literata, Atkinson Hyperlegible and JetBrains
 * Mono into 8-bit-alpha VLW fonts (Latin, Latin-1, Latin Ext-A, Greek and
 * typographic punctuation); tools/pack_fonts.py packs them into fonts.bin,
 * flashed once to the "fonts" partition. The partition is memory-mapped, so
 * glyph bitmaps are read straight from flash with no copies.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <cstddef>
#include <cstdint>

#include <M5GFX.h>

namespace vlw {

/** @brief Maps the fonts partition. Returns false if it is missing/corrupt. */
bool init();

/** @brief True after a successful init(). */
bool ready();

/**
 * @brief Font by name, e.g. "lit_r_32" (family_face_px). Loaded on first use
 *        and kept. Falls back to a built-in GFX font if the pack is missing.
 */
const lgfx::IFont* get(const char* name);

/** @brief Pixel width of UTF-8 text in @p font (fast: cached advance table). */
int width(const lgfx::IFont* font, const char* utf8, size_t len);
int width(const lgfx::IFont* font, const char* utf8);

/** @brief True when the font has a glyph for code point @p cp. */
bool has_glyph(const lgfx::IFont* font, uint32_t cp);

/** @brief Ascent (px above baseline) and line height of a font. */
int ascent(const lgfx::IFont* font);
int height(const lgfx::IFont* font);

/** @brief Decodes one UTF-8 code point; advances @p i. Invalid bytes -> 0xFFFD. */
uint32_t utf8_next(const char* s, size_t len, size_t& i);

}  // namespace vlw
