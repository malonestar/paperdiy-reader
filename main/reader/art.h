/*
 * paperdiy-reader — procedural 16-grey artwork.
 *
 * Everything here draws straight into an 8-bit greyscale pixel buffer (the
 * M5Canvas buffer on the device, a plain array in host previews), so the
 * same code renders sleep screens, library thumbnails and Studio frames.
 * Greys are snapped to the panel's 16 levels (multiples of 17) so the panel
 * driver never has to dither.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <cstdint>

namespace art {

/** @brief A window onto an 8-bit greyscale buffer (0 = black, 255 = white). */
struct Buf {
    uint8_t* p = nullptr;
    int w      = 0; /*!< drawable width */
    int h      = 0;
    int stride = 0; /*!< bytes per row of the underlying buffer */
};

/** @brief The 16 panel greys: level 0 = black ... 15 = white. */
inline uint8_t level(int l)
{
    return (uint8_t)((l < 0 ? 0 : l > 15 ? 15 : l) * 17);
}

/** @brief Snaps any 8-bit grey to the nearest panel level. */
inline uint8_t snap(int v)
{
    v = v < 0 ? 0 : v > 255 ? 255 : v;
    return (uint8_t)(((v + 8) / 17) * 17);
}

/* ---- primitives ------------------------------------------------------ */

void fill(Buf b, uint8_t g);
void rect(Buf b, int x, int y, int w, int h, uint8_t g);
void frame(Buf b, int x, int y, int w, int h, int t, uint8_t g);
/** Anti-aliased (4 grey steps) filled disc. */
void disc(Buf b, float cx, float cy, float r, uint8_t g);
/** Anti-aliased ring of thickness @p t. */
void ring(Buf b, float cx, float cy, float r, float t, uint8_t g);
/** Anti-aliased thick line with round caps. */
void line(Buf b, float x0, float y0, float x1, float y1, float t, uint8_t g);
/** Filled convex quad (corners in order). */
void quad(Buf b, const float* xy, uint8_t g);
/** Filled rounded rectangle. */
void rrect(Buf b, int x, int y, int w, int h, int r, uint8_t g);

/* ---- designs --------------------------------------------------------- */

enum Design : uint8_t {
    DESIGN_CONTOURS = 0, /*!< topographic map, 14 elevation bands */
    DESIGN_STACK,        /*!< isometric stack of modules over a 16-step sky */
    DESIGN_RIDGES,       /*!< mountain ridges in atmospheric perspective */
    DESIGN_CIRCUIT,      /*!< routed PCB traces around a central chip */
    DESIGN_RINGS,        /*!< two-source moire rings */
    DESIGN_COUNT
};

/** @brief Where a design leaves room for the wordmark. */
struct Plaque {
    int cx = 0, cy = 0;    /*!< centre of the wordmark */
    int w = 0, h = 0;      /*!< box the wordmark should fit */
    bool dark_text = true; /*!< black text on a light plaque (else white on dark) */
    bool box       = true; /*!< caller draws a plaque box behind the text */
};

const char* designName(int d);

/**
 * @brief Renders design @p d filling @p b.
 *
 * @param seed varies the composition (noise phase, routing, sources); the
 *             same seed always draws the same picture.
 * @return where the wordmark goes.
 */
Plaque draw(Buf b, int d, uint32_t seed);

/** @brief Small deterministic PRNG (xorshift32). */
struct Rng {
    uint32_t s;
    explicit Rng(uint32_t seed) : s(seed ? seed : 0x9E3779B9u)
    {
    }
    uint32_t next()
    {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        return s;
    }
    int range(int n)
    {
        return (int)(next() % (uint32_t)n);
    }
    float unit()
    {
        return (float)(next() & 0xFFFFFF) / (float)0x1000000;
    }
};

}  // namespace art
