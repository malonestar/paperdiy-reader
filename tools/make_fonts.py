#!/usr/bin/env python3
"""Builds fonts/fonts.bin, the anti-aliased font pack flashed to the "fonts" partition.

    pip install freetype-py fonttools
    python tools/make_fonts.py            # -> fonts/fonts.bin

Faces (all SIL Open Font License, sources in fonts/src):
  lit_*  Literata (body text, serif)      r / i / b / bi
  atk_*  Atkinson Hyperlegible (UI, sans) r / i / b / bi
  mono_* JetBrains Mono (code blocks)     r

VLW = big-endian int32 header {count, version 11, size, 0, ascent, descent},
count x 7 int32 glyph records {unicode, height, width, xAdvance, dY, dX, 0},
then 8-bit alpha bitmaps in record order. LovyanGFX binary-searches the glyph
table, so records MUST be sorted by code point and unique.

Pack layout (little-endian): "VLWP", u32 version 1, u32 count, then count x
{char name[32], u32 offset, u32 size}, then the fonts, each 4-byte aligned.
"""
import os
import struct
import sys

import freetype
from fontTools.ttLib import TTFont
from fontTools.varLib import instancer

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "fonts", "src")
CACHE = os.path.join(ROOT, "build", "font-instances")
OUT = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "fonts", "fonts.bin")

# Must match layout.cpp's cp_supported(): Latin, Latin-1, Latin Extended-A,
# Greek, typographic punctuation and a few symbols.
RANGES = [(0x20, 0x7E), (0xA0, 0x17F), (0x391, 0x3A9), (0x3B1, 0x3C9), (0x2010, 0x2027),
          (0x2030, 0x203A), (0x2044, 0x2044), (0x20AC, 0x20AC), (0x2122, 0x2122), (0x2190, 0x2193),
          (0x2212, 0x2212), (0x221E, 0x221E), (0x2248, 0x2248), (0x2260, 0x2260), (0x2264, 0x2265),
          (0x2022, 0x2022), (0x25CF, 0x25CF), (0x25B2, 0x25B2), (0x25BC, 0x25BC), (0x2713, 0x2713)]
CODEPOINTS = sorted({cp for a, b in RANGES for cp in range(a, b + 1)})

BODY = [24, 28, 32, 36, 42]
PLAN = {("lit", "r"): BODY, ("lit", "i"): BODY, ("lit", "bi"): BODY, ("lit", "b"): BODY + [50, 58],
        ("atk", "r"): BODY, ("atk", "i"): BODY, ("atk", "bi"): BODY, ("atk", "b"): BODY + [50, 58],
        ("mono", "r"): [20, 24, 28]}


def instance(src, name, axes):
    os.makedirs(CACHE, exist_ok=True)
    dst = os.path.join(CACHE, name)
    if not os.path.exists(dst):
        instancer.instantiateVariableFont(TTFont(os.path.join(SRC, src)), axes).save(dst)
    return dst


def literata(italic, weight):
    src = "Literata-Italic[opsz,wght].ttf" if italic else "Literata[opsz,wght].ttf"
    return instance(src, f"lit_{'i' if italic else 'r'}{weight}.ttf", {"wght": weight, "opsz": 14})


FACES = {
    "lit": {"r": lambda: literata(False, 420), "i": lambda: literata(True, 420),
            "b": lambda: literata(False, 680), "bi": lambda: literata(True, 680)},
    "atk": {"r": lambda: os.path.join(SRC, "AtkinsonHyperlegible-Regular.ttf"),
            "i": lambda: os.path.join(SRC, "AtkinsonHyperlegible-Italic.ttf"),
            "b": lambda: os.path.join(SRC, "AtkinsonHyperlegible-Bold.ttf"),
            "bi": lambda: os.path.join(SRC, "AtkinsonHyperlegible-BoldItalic.ttf")},
    "mono": {"r": lambda: instance("JetBrainsMono[wght].ttf", "mono_r.ttf", {"wght": 400})},
}


def make_vlw(path, px):
    face = freetype.Face(path)
    face.set_pixel_sizes(0, px)
    recs, bms = [], []
    for cp in CODEPOINTS:
        if face.get_char_index(cp) == 0:
            continue
        face.load_char(chr(cp), freetype.FT_LOAD_RENDER | freetype.FT_LOAD_TARGET_LIGHT)
        g = face.glyph
        bm = g.bitmap
        w, h = bm.width, bm.rows
        adv = (g.advance.x + 32) >> 6
        if w > 255 or adv > 255:
            continue
        data = bytearray()
        for y in range(h):
            data += bytes(bm.buffer[y * bm.pitch: y * bm.pitch + w])
        recs.append((cp, h, w, adv, g.bitmap_top, g.bitmap_left, 0))
        bms.append(bytes(data))
    asc = (face.size.ascender + 32) >> 6
    desc = abs((face.size.descender - 32) >> 6)
    out = struct.pack(">6i", len(recs), 11, px, 0, asc, desc)
    out += b"".join(struct.pack(">7i", *r) for r in recs)
    return out + b"".join(bms)


def main():
    fonts = []
    for (fam, fc), sizes in PLAN.items():
        path = FACES[fam][fc]()
        for px in sizes:
            fonts.append((f"{fam}_{fc}_{px}", make_vlw(path, px)))
    fonts.sort()
    off = (12 + 40 * len(fonts) + 3) & ~3
    index, blobs = [], []
    for name, data in fonts:
        index.append(name.encode().ljust(32, b"\0")[:32] + struct.pack("<II", off, len(data)))
        blobs.append((off, data))
        off = (off + len(data) + 3) & ~3
    img = bytearray(off)
    img[0:12] = b"VLWP" + struct.pack("<II", 1, len(fonts))
    img[12:12 + 40 * len(fonts)] = b"".join(index)
    for o, d in blobs:
        img[o:o + len(d)] = d
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    with open(OUT, "wb") as f:
        f.write(img)
    print(f"{len(fonts)} fonts, {len(img) / 1048576:.2f} MB -> {OUT}")
    if len(img) > 0x900000:
        sys.exit("font pack does not fit the 9 MB fonts partition")


if __name__ == "__main__":
    main()
