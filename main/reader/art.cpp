/*
 * paperdiy-reader — procedural 16-grey artwork.
 *
 * SPDX-License-Identifier: MIT
 */
#include "reader/art.h"

#include <cmath>
#include <cstring>
#include <vector>

namespace art {

namespace {

inline int imin(int a, int b)
{
    return a < b ? a : b;
}
inline int imax(int a, int b)
{
    return a > b ? a : b;
}
inline float fclamp(float v, float lo, float hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

inline void put(Buf& b, int x, int y, uint8_t g)
{
    if ((unsigned)x < (unsigned)b.w && (unsigned)y < (unsigned)b.h) {
        b.p[y * b.stride + x] = g;
    }
}

/* Blends grey @p g over the pixel with coverage @p a (0..1), snapped to a
 * panel level so edges stay within the 16 greys. */
inline void blend(Buf& b, int x, int y, uint8_t g, float a)
{
    if ((unsigned)x >= (unsigned)b.w || (unsigned)y >= (unsigned)b.h || a <= 0.f) {
        return;
    }
    uint8_t& d = b.p[y * b.stride + x];
    if (a >= 1.f) {
        d = g;
        return;
    }
    d = snap((int)(d + (g - d) * a + 0.5f));
}

/* ---- value noise --------------------------------------------------- */

inline uint32_t hash2(int x, int y, uint32_t seed)
{
    uint32_t h = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u + seed * 2246822519u;
    h          = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}

inline float vnoise(float x, float y, uint32_t seed)
{
    int xi   = (int)floorf(x);
    int yi   = (int)floorf(y);
    float fx = x - xi;
    float fy = y - yi;
    float u  = fx * fx * (3.f - 2.f * fx);
    float v  = fy * fy * (3.f - 2.f * fy);
    float a  = (hash2(xi, yi, seed) & 0xFFFF) / 65535.f;
    float bb = (hash2(xi + 1, yi, seed) & 0xFFFF) / 65535.f;
    float c  = (hash2(xi, yi + 1, seed) & 0xFFFF) / 65535.f;
    float d  = (hash2(xi + 1, yi + 1, seed) & 0xFFFF) / 65535.f;
    return (a + (bb - a) * u) + ((c + (d - c) * u) - (a + (bb - a) * u)) * v;
}

float fbm(float x, float y, uint32_t seed)
{
    float s   = 0.f;
    float amp = 0.55f;
    for (int o = 0; o < 4; o++) {
        s += amp * vnoise(x, y, seed + (uint32_t)o * 101u);
        x = x * 2.03f + 17.1f;
        y = y * 2.03f - 9.7f;
        amp *= 0.48f;
    }
    return s;
}

/* ================================================================== */
/* Contours                                                            */
/* ================================================================== */

Plaque contours(Buf b, uint32_t seed)
{
    Rng rng(seed);
    const float scale = 260.f * (float)imin(b.w, b.h) / 540.f;
    // Two summits, placed away from the plaque.
    const float p1x = b.w * (0.18f + 0.2f * rng.unit()), p1y = b.h * (0.12f + 0.15f * rng.unit());
    const float p2x = b.w * (0.62f + 0.2f * rng.unit()), p2y = b.h * (0.72f + 0.15f * rng.unit());
    const float rs  = (float)(b.w * b.w) * 0.09f;
    const int bands = 14;

    // Elevation per pixel, stored as a band index.
    std::vector<uint8_t> band((size_t)b.w * b.h);
    for (int y = 0; y < b.h; y++) {
        for (int x = 0; x < b.w; x++) {
            float e  = fbm(x / scale, y / scale, seed) * 0.75f;
            float d1 = (x - p1x) * (x - p1x) + (y - p1y) * (y - p1y);
            float d2 = (x - p2x) * (x - p2x) + (y - p2y) * (y - p2y);
            e += 0.55f * expf(-d1 / rs) + 0.45f * expf(-d2 / (rs * 1.4f));
            int bi = (int)(fclamp(e, 0.f, 0.999f) * bands);
            band[(size_t)y * b.w + x] = (uint8_t)bi;
        }
    }
    // Fill: low ground pale, summits dark; every 5th line is an index contour.
    for (int y = 0; y < b.h; y++) {
        for (int x = 0; x < b.w; x++) {
            int bi = band[(size_t)y * b.w + x];
            uint8_t g = level(15 - bi);
            int r = (x + 1 < b.w) ? band[(size_t)y * b.w + x + 1] : bi;
            int d = (y + 1 < b.h) ? band[(size_t)(y + 1) * b.w + x] : bi;
            if (r != bi || d != bi) {
                int top = imax(bi, imax(r, d));
                g       = (top % 5 == 0) ? level(0) : level(imax(0, 15 - top - 4));
            }
            b.p[y * b.stride + x] = g;
        }
    }
    Plaque pq;
    pq.cx = b.w / 2;
    pq.cy = b.h * 44 / 100;
    pq.w  = b.w * 70 / 100;
    pq.h  = b.h * 17 / 100;
    return pq;
}

/* ================================================================== */
/* Stack                                                               */
/* ================================================================== */

/* One isometric slab: top rhombus + two side faces. (cx, cy) is the centre
 * of the top face; s the half-width; t the thickness. */
void slab(Buf b, float cx, float cy, float s, float t, int top_l, int left_l, int right_l)
{
    const float hh = s * 0.5f;  // 2:1 isometric
    float top[8]   = {cx, cy - hh, cx + s, cy, cx, cy + hh, cx - s, cy};
    float left[8]  = {cx - s, cy, cx, cy + hh, cx, cy + hh + t, cx - s, cy + t};
    float right[8] = {cx, cy + hh, cx + s, cy, cx + s, cy + t, cx, cy + hh + t};
    quad(b, left, level(left_l));
    quad(b, right, level(right_l));
    quad(b, top, level(top_l));
    // Crisp edges.
    line(b, cx - s, cy, cx, cy + hh, 1.2f, level(0));
    line(b, cx, cy + hh, cx + s, cy, 1.2f, level(0));
    line(b, cx, cy + hh, cx, cy + hh + t, 1.2f, level(0));
    line(b, cx - s, cy, cx - s, cy + t, 1.2f, level(0));
    line(b, cx + s, cy, cx + s, cy + t, 1.2f, level(0));
    line(b, cx - s, cy + t, cx, cy + hh + t, 1.2f, level(0));
    line(b, cx, cy + hh + t, cx + s, cy + t, 1.2f, level(0));
    line(b, cx - s, cy, cx, cy - hh, 1.2f, level(0));
    line(b, cx, cy - hh, cx + s, cy, 1.2f, level(0));
}

Plaque stack(Buf b, uint32_t seed)
{
    Rng rng(seed);
    // Sky: the 16 levels as bands, dark at the top to white at the horizon.
    const int horizon = b.h * 62 / 100;
    for (int y = 0; y < b.h; y++) {
        uint8_t g = (y < horizon) ? level(y * 16 / horizon) : level(15);
        memset(b.p + y * b.stride, g, (size_t)b.w);
    }
    // Ground: a fine isometric floor grid fading out.
    const float k = b.w / 540.f;
    for (int i = -20; i <= 20; i++) {
        float x0 = b.w / 2.f + i * 40.f * k;
        line(b, x0, (float)horizon, x0 - 600.f * k, horizon + 300.f * k, 1.f, level(12));
        line(b, x0, (float)horizon, x0 + 600.f * k, horizon + 300.f * k, 1.f, level(12));
    }
    // A few stars in the dark bands.
    for (int i = 0; i < (int)(70 * k * k) + 8; i++) {
        float sx = rng.unit() * b.w;
        float sy = rng.unit() * horizon * 0.55f;
        disc(b, sx, sy, (rng.range(5) == 0 ? 2.2f : 1.3f) * k, level(15));
    }

    // The stack: four modules, base to top, each a little different.
    const float s  = 150.f * k;
    const float t  = 38.f * k;
    const float cx = b.w / 2.f;
    float cy       = horizon + 40.f * k;
    const int n    = 4;
    for (int i = 0; i < n; i++) {
        int shade = (i % 2 == 0) ? 0 : 1;
        slab(b, cx, cy, s, t, 13 - shade, 7 - shade, 4 - shade);
        // Module details on the front faces: vents / pin headers.
        for (int v = 0; v < 6; v++) {
            float fx = cx + s * (0.15f + v * 0.13f);
            float fy = cy + s * 0.5f * (1.f - (0.15f + v * 0.13f)) + t * 0.5f;
            line(b, fx, fy - t * 0.2f, fx, fy + t * 0.2f, 2.f * k, level(1));
        }
        if (i < n - 1) {
            cy -= t + 6.f * k;
        }
    }
    // Top module: a screen and an LED on its top face.
    {
        const float hh = s * 0.5f;
        float scr[8]   = {cx - s * 0.05f, cy - hh * 0.75f, cx + s * 0.62f, cy - hh * 0.05f + 2,
                          cx + s * 0.05f, cy + hh * 0.62f, cx - s * 0.62f, cy - hh * 0.08f};
        quad(b, scr, level(1));
        disc(b, cx + s * 0.62f, cy + hh * 0.2f, 6.f * k, level(15));
        ring(b, cx + s * 0.62f, cy + hh * 0.2f, 6.f * k, 1.5f * k, level(0));
    }

    Plaque pq;
    pq.cx        = b.w / 2;
    pq.cy        = b.h * 86 / 100;
    pq.w         = b.w * 80 / 100;
    pq.h         = b.h * 12 / 100;
    pq.dark_text = true;
    pq.box       = false;
    (void)rng;
    return pq;
}

/* ================================================================== */
/* Rings                                                               */
/* ================================================================== */

Plaque rings(Buf b, uint32_t seed)
{
    Rng rng(seed);
    const float k = b.w / 540.f;
    // Two point sources slightly apart: their ring systems beat against
    // each other into a moire, quantised to the 16 panel levels.
    struct Src {
        float x, y;
    } src[2];
    const float a  = rng.unit() * 6.2832f;
    const float sep = b.w * (0.10f + 0.06f * rng.unit());
    src[0] = {b.w * 0.5f + cosf(a) * sep, b.h * 0.5f + sinf(a) * sep};
    src[1] = {b.w * 0.5f - cosf(a) * sep, b.h * 0.5f - sinf(a) * sep};
    const float f = 0.16f / k;
    for (int y = 0; y < b.h; y++) {
        for (int x = 0; x < b.w; x++) {
            float d0 = sqrtf((x - src[0].x) * (x - src[0].x) + (y - src[0].y) * (y - src[0].y));
            float d1 = sqrtf((x - src[1].x) * (x - src[1].x) + (y - src[1].y) * (y - src[1].y));
            float v  = sinf(d0 * f) + sinf(d1 * f);  // -2..2
            int l    = (int)((v + 2.f) * 0.25f * 15.99f);
            b.p[y * b.stride + x] = level(l);
        }
    }
    Plaque pq;
    pq.cx = b.w / 2;
    pq.cy = b.h / 2;
    pq.w  = b.w * 72 / 100;
    pq.h  = b.h * 17 / 100;
    return pq;
}

/* ================================================================== */
/* Ridges                                                              */
/* ================================================================== */

Plaque ridges(Buf b, uint32_t seed)
{
    Rng rng(seed);
    const float k = b.w / 540.f;
    // Sky: level 10 at the top to white at the horizon, ordered-dithered
    // between neighbouring levels so the gradient has no visible bands.
    static const uint8_t kBayer[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};
    const int sky_h = b.h * 45 / 100;
    for (int y = 0; y < b.h; y++) {
        float lv = (y < sky_h) ? 10.f + 5.f * (float)y / sky_h : 15.f;
        int li   = (int)lv;
        int frac = (int)((lv - li) * 16.f);
        for (int x = 0; x < b.w; x++) {
            b.p[y * b.stride + x] = level(li + (frac > kBayer[y & 3][x & 3] ? 1 : 0));
        }
    }
    // Sun with halo rings.
    const float sx = b.w * (0.25f + 0.5f * rng.unit()), sy = b.h * 0.29f;
    for (int i = 4; i >= 1; i--) {
        ring(b, sx, sy, (40.f + i * 16.f) * k, 2.f * k, level(13 - i / 2));
    }
    disc(b, sx, sy, 38.f * k, level(15));
    ring(b, sx, sy, 38.f * k, 2.f * k, level(9));

    // Eight ridges, far (pale) to near (black): atmospheric perspective.
    const int layers = 8;
    for (int i = 0; i < layers; i++) {
        const float base = b.h * (0.42f + i * 0.07f);
        const float amp  = b.h * (0.10f - i * 0.006f);
        const float fq   = (1.6f + i * 0.5f) / b.w;
        const uint32_t ls = seed + (uint32_t)i * 7919u;
        const uint8_t g  = level(12 - (i * 12) / (layers - 1));
        for (int x = 0; x < b.w; x++) {
            float n = fbm(x * fq * 3.f, i * 3.7f, ls);
            // Sharpen into peaks.
            n       = 1.f - fabsf(n * 2.f - 1.1f);
            int top = (int)(base - amp * n);
            if (top < 0) {
                top = 0;
            }
            for (int y = top; y < b.h; y++) {
                b.p[y * b.stride + x] = g;
            }
            // A light rim along each crest.
            if (i > 0 && top > 0) {
                b.p[(top - 1) * b.stride + x] = level(13 - (i * 12) / (layers - 1) + 1 > 15 ? 15 : 13 - (i * 12) / (layers - 1) + 1);
            }
        }
    }
    // Birds.
    for (int i = 0; i < 5; i++) {
        float bx = b.w * (0.1f + 0.8f * rng.unit()), by = b.h * (0.12f + 0.15f * rng.unit());
        float s2 = (6.f + 5.f * rng.unit()) * k;
        line(b, bx - s2, by - s2 * 0.4f, bx, by, 1.6f * k, level(3));
        line(b, bx, by, bx + s2, by - s2 * 0.4f, 1.6f * k, level(3));
    }
    Plaque pq;
    pq.cx        = b.w / 2;
    pq.cy        = b.h * 13 / 100;
    pq.w         = b.w * 80 / 100;
    pq.h         = b.h * 12 / 100;
    pq.dark_text = true;
    pq.box       = false;
    return pq;
}

/* ================================================================== */
/* Circuit                                                             */
/* ================================================================== */

Plaque circuit(Buf b, uint32_t seed)
{
    Rng rng(seed);
    const float k  = b.w / 540.f;
    const int cell = imax(6, (int)(24 * k));
    const int gw = b.w / cell + 1, gh = b.h / cell + 1;
    fill(b, level(2));

    // Faint copper pour pattern.
    for (int y = 0; y < b.h; y += imax(2, (int)(6 * k))) {
        for (int x = (y / imax(2, (int)(6 * k))) % 2 * imax(1, (int)(3 * k)); x < b.w; x += imax(2, (int)(6 * k))) {
            put(b, x, y, level(3));
        }
    }

    // Central chip occupies the middle; traces route around it.
    const int chip_w = (int)(b.w * 0.62f), chip_h = (int)(b.h * 0.2f);
    const int chip_x = (b.w - chip_w) / 2, chip_y = (int)(b.h * 0.42f) - chip_h / 2;
    std::vector<uint8_t> used((size_t)gw * gh, 0);
    auto mark_rect = [&](int x, int y, int w, int h) {
        for (int gy = y / cell; gy <= (y + h) / cell && gy < gh; gy++) {
            for (int gx = x / cell; gx <= (x + w) / cell && gx < gw; gx++) {
                used[(size_t)gy * gw + gx] = 1;
            }
        }
    };
    mark_rect(chip_x - cell, chip_y - cell, chip_w + 2 * cell, chip_h + 2 * cell);

    // Traces: start next to a chip pin, wander outward on a 45-degree grid.
    static const int DX[8] = {1, 1, 0, -1, -1, -1, 0, 1};
    static const int DY[8] = {0, 1, 1, 1, 0, -1, -1, -1};
    const int pins_x = chip_w / cell;
    const int pins_y = chip_h / cell;
    const float tw   = fmaxf(1.5f, 3.2f * k);
    for (int i = 0; i < pins_x * 2 + pins_y * 2; i++) {
        if (rng.range(10) < 3) {
            continue;  // not every pin is routed
        }
        int side = (i < pins_x) ? 0 : (i < 2 * pins_x) ? 1 : (i < 2 * pins_x + pins_y) ? 2 : 3;
        int gx, gy, dir;
        if (side == 0) {
            gx = chip_x / cell + (i % pins_x) + 1; gy = chip_y / cell - 2; dir = 6;
        } else if (side == 1) {
            gx = chip_x / cell + (i % pins_x) + 1; gy = (chip_y + chip_h) / cell + 2; dir = 2;
        } else if (side == 2) {
            gx = chip_x / cell - 2; gy = chip_y / cell + ((i - 2 * pins_x) % pins_y) + 1; dir = 4;
        } else {
            gx = (chip_x + chip_w) / cell + 2; gy = chip_y / cell + ((i - 2 * pins_x) % pins_y) + 1; dir = 0;
        }
        if (gx < 0 || gy < 0 || gx >= gw || gy >= gh || used[(size_t)gy * gw + gx]) {
            continue;
        }
        // Stub from the pin to the routing grid.
        float px = gx * cell + cell / 2.f, py = gy * cell + cell / 2.f;
        float sx = px - DX[dir] * cell * 1.5f, sy = py - DY[dir] * cell * 1.5f;
        uint8_t g = level(rng.range(3) == 0 ? 13 : 9);
        line(b, sx, sy, px, py, tw, g);
        used[(size_t)gy * gw + gx] = 1;

        int len = 6 + rng.range(30);
        for (int stepn = 0; stepn < len; stepn++) {
            int nd = dir;
            int r  = rng.range(12);
            if (r == 0) nd = (dir + 1) & 7;
            else if (r == 1) nd = (dir + 7) & 7;
            int nx = gx + DX[nd], ny = gy + DY[nd];
            if (nx < 0 || ny < 0 || nx >= gw || ny >= gh || used[(size_t)ny * gw + nx]) {
                nd = dir;
                nx = gx + DX[nd];
                ny = gy + DY[nd];
                if (nx < 0 || ny < 0 || nx >= gw || ny >= gh || used[(size_t)ny * gw + nx]) {
                    break;
                }
            }
            line(b, gx * cell + cell / 2.f, gy * cell + cell / 2.f, nx * cell + cell / 2.f, ny * cell + cell / 2.f,
                 tw, g);
            used[(size_t)ny * gw + nx] = 1;
            gx = nx;
            gy = ny;
            dir = nd;
        }
        // Via at the end of the run.
        float vx = gx * cell + cell / 2.f, vy = gy * cell + cell / 2.f;
        disc(b, vx, vy, tw * 1.9f, g);
        disc(b, vx, vy, tw * 0.8f, level(2));
    }

    // Scattered passives: little 0603 resistors and caps in free cells.
    for (int i = 0; i < (int)(60 * k) + 4; i++) {
        int gx = rng.range(gw - 2), gy = rng.range(gh - 2);
        if (used[(size_t)gy * gw + gx] || used[(size_t)gy * gw + gx + 1]) {
            continue;
        }
        used[(size_t)gy * gw + gx] = used[(size_t)gy * gw + gx + 1] = 1;
        int x = gx * cell + cell / 4, y = gy * cell + cell / 3;
        int w = cell + cell / 2, h = cell / 3 + 1;
        rect(b, x, y, w, h, level(rng.range(2) ? 6 : 11));
        rect(b, x, y, w / 4, h, level(13));
        rect(b, x + w - w / 4, y, w / 4, h, level(13));
    }

    // The chip: body, pins, notch.
    for (int p = 0; p <= pins_x; p++) {
        int x = chip_x + p * chip_w / (pins_x + 1) + cell / 2;
        rect(b, x - cell / 5, chip_y - cell, cell * 2 / 5, cell, level(12));
        rect(b, x - cell / 5, chip_y + chip_h, cell * 2 / 5, cell, level(12));
    }
    for (int p = 0; p <= pins_y; p++) {
        int y = chip_y + p * chip_h / (pins_y + 1) + cell / 2;
        rect(b, chip_x - cell, y - cell / 5, cell, cell * 2 / 5, level(12));
        rect(b, chip_x + chip_w, y - cell / 5, cell, cell * 2 / 5, level(12));
    }
    rrect(b, chip_x, chip_y, chip_w, chip_h, imax(2, (int)(10 * k)), level(1));
    frame(b, chip_x + (int)(6 * k), chip_y + (int)(6 * k), chip_w - (int)(12 * k), chip_h - (int)(12 * k),
          imax(1, (int)(1.5f * k)), level(4));
    disc(b, chip_x + 22 * k, chip_y + 22 * k, 7 * k, level(4));

    Plaque pq;
    pq.cx        = b.w / 2;
    pq.cy        = chip_y + chip_h / 2;
    pq.w         = chip_w * 85 / 100;
    pq.h         = chip_h * 80 / 100;
    pq.dark_text = false;
    pq.box       = false;
    return pq;
}

}  // namespace

/* ================================================================== */
/* Primitives                                                          */
/* ================================================================== */

void fill(Buf b, uint8_t g)
{
    for (int y = 0; y < b.h; y++) {
        memset(b.p + y * b.stride, g, (size_t)b.w);
    }
}

void rect(Buf b, int x, int y, int w, int h, uint8_t g)
{
    int x0 = imax(0, x), y0 = imax(0, y), x1 = imin(b.w, x + w), y1 = imin(b.h, y + h);
    for (int yy = y0; yy < y1; yy++) {
        if (x1 > x0) {
            memset(b.p + yy * b.stride + x0, g, (size_t)(x1 - x0));
        }
    }
}

void frame(Buf b, int x, int y, int w, int h, int t, uint8_t g)
{
    rect(b, x, y, w, t, g);
    rect(b, x, y + h - t, w, t, g);
    rect(b, x, y, t, h, g);
    rect(b, x + w - t, y, t, h, g);
}

void disc(Buf b, float cx, float cy, float r, uint8_t g)
{
    int x0 = (int)floorf(cx - r - 1), x1 = (int)ceilf(cx + r + 1);
    int y0 = (int)floorf(cy - r - 1), y1 = (int)ceilf(cy + r + 1);
    for (int y = imax(0, y0); y <= imin(b.h - 1, y1); y++) {
        for (int x = imax(0, x0); x <= imin(b.w - 1, x1); x++) {
            float d = sqrtf((x + 0.5f - cx) * (x + 0.5f - cx) + (y + 0.5f - cy) * (y + 0.5f - cy));
            blend(b, x, y, g, fclamp(r - d + 0.5f, 0.f, 1.f));
        }
    }
}

void ring(Buf b, float cx, float cy, float r, float t, uint8_t g)
{
    int x0 = (int)floorf(cx - r - t - 1), x1 = (int)ceilf(cx + r + t + 1);
    int y0 = (int)floorf(cy - r - t - 1), y1 = (int)ceilf(cy + r + t + 1);
    for (int y = imax(0, y0); y <= imin(b.h - 1, y1); y++) {
        for (int x = imax(0, x0); x <= imin(b.w - 1, x1); x++) {
            float d = sqrtf((x + 0.5f - cx) * (x + 0.5f - cx) + (y + 0.5f - cy) * (y + 0.5f - cy));
            blend(b, x, y, g, fclamp(t * 0.5f - fabsf(d - r) + 0.5f, 0.f, 1.f));
        }
    }
}

void line(Buf b, float x0, float y0, float x1, float y1, float t, uint8_t g)
{
    const float r = t * 0.5f;
    int bx0 = (int)floorf(fminf(x0, x1) - r - 1), bx1 = (int)ceilf(fmaxf(x0, x1) + r + 1);
    int by0 = (int)floorf(fminf(y0, y1) - r - 1), by1 = (int)ceilf(fmaxf(y0, y1) + r + 1);
    const float dx = x1 - x0, dy = y1 - y0;
    const float ll = dx * dx + dy * dy;
    for (int y = imax(0, by0); y <= imin(b.h - 1, by1); y++) {
        for (int x = imax(0, bx0); x <= imin(b.w - 1, bx1); x++) {
            float px = x + 0.5f - x0, py = y + 0.5f - y0;
            float tt = ll > 0.f ? fclamp((px * dx + py * dy) / ll, 0.f, 1.f) : 0.f;
            float ex = px - tt * dx, ey = py - tt * dy;
            float d  = sqrtf(ex * ex + ey * ey);
            blend(b, x, y, g, fclamp(r - d + 0.5f, 0.f, 1.f));
        }
    }
}

void quad(Buf b, const float* q, uint8_t g)
{
    float minx = q[0], maxx = q[0], miny = q[1], maxy = q[1];
    for (int i = 1; i < 4; i++) {
        minx = fminf(minx, q[2 * i]);
        maxx = fmaxf(maxx, q[2 * i]);
        miny = fminf(miny, q[2 * i + 1]);
        maxy = fmaxf(maxy, q[2 * i + 1]);
    }
    // Orientation of the quad, so the inside test works either winding.
    float area = 0.f;
    for (int i = 0; i < 4; i++) {
        int j = (i + 1) & 3;
        area += q[2 * i] * q[2 * j + 1] - q[2 * j] * q[2 * i + 1];
    }
    const float sgn = area >= 0.f ? 1.f : -1.f;
    for (int y = imax(0, (int)floorf(miny)); y <= imin(b.h - 1, (int)ceilf(maxy)); y++) {
        for (int x = imax(0, (int)floorf(minx)); x <= imin(b.w - 1, (int)ceilf(maxx)); x++) {
            float px = x + 0.5f, py = y + 0.5f;
            bool in  = true;
            for (int i = 0; i < 4 && in; i++) {
                int j    = (i + 1) & 3;
                float cr = (q[2 * j] - q[2 * i]) * (py - q[2 * i + 1]) - (q[2 * j + 1] - q[2 * i + 1]) * (px - q[2 * i]);
                in       = cr * sgn >= 0.f;
            }
            if (in) {
                b.p[y * b.stride + x] = g;
            }
        }
    }
}

void rrect(Buf b, int x, int y, int w, int h, int r, uint8_t g)
{
    r = imin(r, imin(w, h) / 2);
    rect(b, x + r, y, w - 2 * r, h, g);
    rect(b, x, y + r, r, h - 2 * r, g);
    rect(b, x + w - r, y + r, r, h - 2 * r, g);
    disc(b, x + r, y + r, (float)r, g);
    disc(b, x + w - r, y + r, (float)r, g);
    disc(b, x + r, y + h - r, (float)r, g);
    disc(b, x + w - r, y + h - r, (float)r, g);
}

/* ================================================================== */

const char* designName(int d)
{
    static const char* kNames[DESIGN_COUNT] = {"Contours", "Stack", "Ridges", "Circuit", "Moire"};
    return (d >= 0 && d < DESIGN_COUNT) ? kNames[d] : "?";
}

Plaque draw(Buf b, int d, uint32_t seed)
{
    switch (d) {
        case DESIGN_STACK:
            return stack(b, seed);
        case DESIGN_RIDGES:
            return ridges(b, seed);
        case DESIGN_RINGS:
            return rings(b, seed);
        case DESIGN_CIRCUIT:
            return circuit(b, seed);
        case DESIGN_CONTOURS:
        default:
            return contours(b, seed);
    }
}

}  // namespace art
