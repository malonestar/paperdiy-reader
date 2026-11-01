/*
 * paperdiy-reader — Studio: animated pieces that live on the library shelf.
 *
 * Each piece appears as a "book" at the end of the library. Opening one runs
 * an animation that redraws the whole canvas every frame but refreshes only
 * the pixels that changed (render::present's block diff), on the panel's
 * fastest waveform: a Life cell flipping refreshes an 18 px square.
 *
 * Keys inside a piece:  A click = act (reseed / next), B click = stats,
 *                       A hold = clean refresh, B hold = back to the library,
 *                       A+B hold = sleep.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <vector>

#include <M5GFX.h>

#include "reader/library.h"

namespace studio {

enum Piece : int {
    PIECE_LIFE = 0, /*!< Conway's Game of Life */
    PIECE_GALLERY,  /*!< full-screen tour of the sleep-screen designs */
    PIECE_COUNT
};

/** @brief True for a Studio shelf entry (path "studio:<name>"). */
bool isStudio(const BookEntry& b);

/** @brief Piece id of a Studio entry, -1 otherwise. */
int pieceOf(const BookEntry& b);

/** @brief Appends the Studio entries to a library list. */
void addEntries(std::vector<BookEntry>& books);

/** @brief Tile art for a Studio entry (THUMB_W x THUMB_H). */
void drawThumb(M5Canvas* cv, const BookEntry& b, int x, int y);

/**
 * @brief Runs a piece until the reader leaves it.
 * @return true when the reader asked to sleep (A+B hold).
 */
bool run(int piece);

}  // namespace studio
