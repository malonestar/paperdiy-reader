/*
 * paperdiy-reader — book covers for the library grid and the sleep screen.
 *
 * EPUB cover images (JPEG / baseline or PNG) are decoded once, scaled and
 * centre-cropped to the grid tile size and cached next to the book's other
 * sidecars as raw 8-bit greyscale (<hash8>.cov). Books without a usable image
 * (TXT files, progressive JPEGs, GIF/WebP covers) get a typeset cover drawn on
 * the fly, so every tile in the grid looks intentional.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <M5GFX.h>

#include "reader/library.h"

namespace cover {

/** @brief Size of a grid-tile cover thumbnail. */
static constexpr int THUMB_W = 152;
static constexpr int THUMB_H = 212;

/**
 * @brief Makes sure the thumbnail of @p book is known.
 *
 * Loads it from the sidecar cache or, the first time, extracts and decodes
 * the EPUB cover (0.3-2 s). Sets book.cover to 1 (thumbnail available) or -1
 * (draw a typeset cover).
 */
void ensureThumb(BookEntry& book);

/**
 * @brief Fast path: resolves the cover from memory or the on-card cache
 *        (~20 ms) without ever decoding an EPUB.
 * @return true when the cover state is now known.
 */
bool loadCached(BookEntry& book);

/** @brief True once ensureThumb() has run for this book. */
inline bool known(const BookEntry& book)
{
    return book.cover != 0;
}

/** @brief Draws the thumbnail (or a typeset cover) at THUMB_W x THUMB_H. */
void drawThumb(M5Canvas* cv, const BookEntry& book, int x, int y);

/**
 * @brief Draws the full-resolution cover fitted into a rectangle.
 *
 * Used by the sleep screen. Falls back to a large typeset cover.
 */
void drawFull(M5Canvas* cv, const BookEntry& book, int x, int y, int w, int h);

/** @brief Draws a JPEG/PNG (baseline JPEG only) scaled to fit or cover a box. */
bool drawImage(M5Canvas* cv, const uint8_t* data, size_t n, int x, int y, int w, int h, bool crop);

/** @brief Drops the in-memory thumbnail of a book (after deletion). */
void forget(const BookEntry& book);

}  // namespace cover
