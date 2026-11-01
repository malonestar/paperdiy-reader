/*
 * paperdiy-reader — text layout engine implementation.
 *
 * SPDX-License-Identifier: MIT
 */
#include "reader/layout.h"

#include <cstdio>
#include <cstring>

#include "fonts/vlw_fonts.h"
#include "hal/hal.h"

namespace layout {

/* ------------------------------------------------------------------ */
/* Fonts                                                               */
/* ------------------------------------------------------------------ */

namespace {

/* Pixel em sizes of the five body presets (see tools/gen_vlw.py). Bold has
 * two extra steps (50, 58) for H1/H2 headings at the largest presets. */
constexpr int kBodyPx[FONT_SIZE_COUNT]      = {24, 28, 32, 36, 42};
constexpr int kBoldPx[FONT_SIZE_COUNT + 2]  = {24, 28, 32, 36, 42, 50, 58};
constexpr int kMonoPx[FONT_SIZE_COUNT]      = {20, 20, 24, 24, 28};
constexpr int kMargins[MARGIN_COUNT]        = {20, 36, 56};
constexpr int kSpacingPct[SPACING_COUNT]    = {122, 142, 166};

Style sanitize(const Style& in)
{
    Style st = in;
    if (st.size >= FONT_SIZE_COUNT) {
        st.size = FONT_SIZE_3;
    }
    if (st.family >= FONT_FAMILY_COUNT) {
        st.family = FONT_SERIF;
    }
    if (st.spacing >= SPACING_COUNT) {
        st.spacing = SPACING_NORMAL;
    }
    if (st.margins >= MARGIN_COUNT) {
        st.margins = MARGIN_NORMAL;
    }
    return st;
}

/* Bold index used for a heading level at a body size. */
int headingIndex(int size, char para_style)
{
    int up = (para_style == PARA_H1) ? 2 : (para_style == PARA_H2) ? 1 : 0;
    return size + up;
}

/* Font cache: resolving a name through vlw::get() is a string search, and the
 * layout engine asks for a font per word. */
const lgfx::IFont* s_cache[FONT_FAMILY_COUNT][4][FONT_SIZE_COUNT + 2] = {};
const lgfx::IFont* s_mono[FONT_SIZE_COUNT]                         = {};

const lgfx::IFont* face_font(int family, int face, int px_index, int px)
{
    const lgfx::IFont*& slot = s_cache[family][face][px_index];
    if (!slot) {
        static const char* kFace[4] = {"r", "i", "b", "bi"};
        char name[24];
        snprintf(name, sizeof(name), "%s_%s_%d", family == FONT_SANS ? "atk" : "lit", kFace[face], px);
        slot = vlw::get(name);
    }
    return slot;
}

}  // namespace

int bodyPx(const Style& st)
{
    return kBodyPx[sanitize(st).size];
}

Style currentStyle()
{
    Style st;
    st.size    = hal.settings.font_size;
    st.family  = hal.settings.font_family;
    st.spacing = hal.settings.spacing;
    st.margins = hal.settings.margins;
    return sanitize(st);
}

uint32_t styleHash(const Style& in)
{
    Style st = sanitize(in);
    return (uint32_t)st.size | ((uint32_t)st.family << 4) | ((uint32_t)st.spacing << 8) |
           ((uint32_t)st.margins << 12);
}

int bodyMarginX(const Style& st)
{
    return kMargins[sanitize(st).margins];
}

int bodyTextW(const Style& st)
{
    return SCREEN_W - 2 * bodyMarginX(st);
}

const lgfx::IFont* uiFont(int px, bool bold)
{
    // UI chrome is set in Atkinson Hyperlegible: built for legibility at
    // small sizes, which is exactly what headers, footers and menus need.
    int idx = 0;
    for (int i = 0; i < FONT_SIZE_COUNT + 2; i++) {
        if (kBoldPx[i] <= px) {
            idx = i;
        }
    }
    if (!bold && idx >= FONT_SIZE_COUNT) {
        idx = FONT_SIZE_COUNT - 1;
    }
    return face_font(FONT_SANS, bold ? FACE_BOLD : FACE_NORMAL, idx, kBoldPx[idx]);
}

const lgfx::IFont* serifFont(int px, uint8_t face)
{
    int idx = 0;
    for (int i = 0; i < FONT_SIZE_COUNT + 2; i++) {
        if (kBoldPx[i] <= px) {
            idx = i;
        }
    }
    if (face != FACE_BOLD && idx >= FONT_SIZE_COUNT) {
        idx = FONT_SIZE_COUNT - 1;
    }
    return face_font(FONT_SERIF, face & 3, idx, kBoldPx[idx]);
}

const lgfx::IFont* fontFor(const Style& in, char para_style, uint8_t face)
{
    Style st       = sanitize(in);
    const int fam  = st.family;

    if (para_style == PARA_CODE) {
        const lgfx::IFont*& slot = s_mono[st.size];
        if (!slot) {
            char name[24];
            snprintf(name, sizeof(name), "mono_r_%d", kMonoPx[st.size]);
            slot = vlw::get(name);
        }
        return slot;
    }

    if (isHeading(para_style)) {
        int i = headingIndex(st.size, para_style);
        return face_font(fam, FACE_BOLD, i, kBoldPx[i]);
    }

    int size = st.size;
    if (para_style == PARA_CAPTION) {
        size = (size > 0) ? size - 1 : 0;
        face = (face == FACE_BOLD || face == FACE_BOLDITAL) ? FACE_BOLDITAL : FACE_ITALIC;
    }
    return face_font(fam, face & 3, size, kBodyPx[size]);
}

std::string ellipsize(const lgfx::IFont* font, const std::string& text, int max_w)
{
    if (max_w <= 0) {
        return std::string();
    }
    if (vlw::width(font, text.data(), text.size()) <= max_w) {
        return text;
    }
    // U+2026 where the font has it (Literata); Atkinson lacks it, so "...".
    const bool has_ell = vlw::has_glyph(font, 0x2026);
    const char* kEll   = has_ell ? "\xE2\x80\xA6" : "...";
    const int ell_w    = vlw::width(font, kEll, 3);
    size_t keep              = 0;
    size_t i                 = 0;
    while (i < text.size()) {
        size_t prev = i;
        vlw::utf8_next(text.data(), text.size(), i);
        if (vlw::width(font, text.data(), i) + ell_w > max_w) {
            i = prev;
            break;
        }
        keep = i;
    }
    std::string out = text.substr(0, keep);
    while (!out.empty() && out.back() == ' ') {
        out.pop_back();
    }
    return out + kEll;
}

std::vector<std::string> wrapText(const lgfx::IFont* font, const std::string& text, int max_w, int max_lines)
{
    std::vector<std::string> lines;
    std::vector<std::string> words;
    split_words(text, words);
    std::string cur;
    size_t wi = 0;
    while (wi < words.size()) {
        std::string trial = cur.empty() ? words[wi] : cur + " " + words[wi];
        if (vlw::width(font, trial.data(), trial.size()) <= max_w || cur.empty()) {
            cur = trial;
            wi++;
            continue;
        }
        if ((int)lines.size() + 1 >= max_lines) {
            break;
        }
        lines.push_back(cur);
        cur.clear();
    }
    if (!cur.empty() || wi < words.size()) {
        std::string last = cur;
        for (; wi < words.size(); wi++) {
            last += " " + words[wi];
        }
        lines.push_back(ellipsize(font, last, max_w));
    }
    for (std::string& l : lines) {
        l = ellipsize(font, l, max_w);  // a single over-long word
    }
    return lines;
}

int lineHeight(M5Canvas* canvas, const Style& in, char para_style)
{
    (void)canvas;
    Style st = sanitize(in);

    if (para_style == PARA_CODE) {
        return (kMonoPx[st.size] * 130 + 50) / 100;
    }

    if (isHeading(para_style)) {
        return (kBoldPx[headingIndex(st.size, para_style)] * 124 + 50) / 100;
    }

    int em = kBodyPx[st.size];
    if (para_style == PARA_CAPTION && st.size > 0) {
        em = kBodyPx[st.size - 1];
    }
    return (em * kSpacingPct[st.spacing] + 50) / 100;
}

/* ------------------------------------------------------------------ */
/* UTF-8 -> ASCII folding                                              */
/* ------------------------------------------------------------------ */

/* Base letters for U+0100..U+017F (Latin Extended-A), one char per code point. */
static const char kLatinExtA[129] =
    "AaAaAa"              /* 0100-0105 */
    "CcCcCcCc"            /* 0106-010D */
    "DdDd"                /* 010E-0111 */
    "EeEeEeEeEe"          /* 0112-011B */
    "GgGgGgGg"            /* 011C-0123 */
    "HhHh"                /* 0124-0127 */
    "IiIiIiIiIi"          /* 0128-0131 */
    "Ii"                  /* 0132-0133 */
    "Jj"                  /* 0134-0135 */
    "Kkk"                 /* 0136-0138 */
    "LlLlLlLlLl"          /* 0139-0142 */
    "NnNnNnn"             /* 0143-0149 */
    "Nn"                  /* 014A-014B */
    "OoOoOoOo"            /* 014C-0153 */
    "RrRrRr"              /* 0154-0159 */
    "SsSsSsSs"            /* 015A-0161 */
    "TtTtTt"              /* 0162-0167 */
    "UuUuUuUuUuUu"        /* 0168-0173 */
    "Ww"                  /* 0174-0175 */
    "Yy"                  /* 0176-0177 */
    "Y"                   /* 0178      */
    "ZzZzZz"              /* 0179-017E */
    "s";                  /* 017F      */

void append_ascii(std::string& out, uint32_t cp)
{
    if (cp < 0x80) {
        char c = (char)cp;
        if (c == '\t' || c == '\n' || c == '\r') {
            out.push_back(' ');
        } else if ((unsigned char)c >= 0x20) {
            out.push_back(c);
        }
        return;
    }

    /* Latin-1 supplement */
    if (cp >= 0xC0 && cp <= 0xFF) {
        static const char* kLatin1 =
            "AAAAAAECEEEEIIIIDNOOOOO*OUUUUYPs"  /* C0-DF (0xD7 multiply sign -> '*') */
            "aaaaaaeceeeeiiiidnooooo/ouuuuypy"; /* E0-FF (0xF7 divide sign -> '/')   */
        char c = kLatin1[cp - 0xC0];
        if (cp == 0xC6) {
            out += "AE";
            return;
        }
        if (cp == 0xE6) {
            out += "ae";
            return;
        }
        if (cp == 0xDF) {
            out += "ss";
            return;
        }
        out.push_back(c);
        return;
    }

    if (cp >= 0x100 && cp <= 0x17F) {
        if (cp == 0x152) {
            out += "OE";
            return;
        }
        if (cp == 0x153) {
            out += "oe";
            return;
        }
        out.push_back(kLatinExtA[cp - 0x100]);
        return;
    }

    switch (cp) {
        case 0xA0:
            out.push_back(' ');
            return;  /* nbsp */
        case 0xA9:
            out += "(c)";
            return;
        case 0xAE:
            out += "(R)";
            return;
        case 0xAB:
            out += "\"";
            return;
        case 0xBB:
            out += "\"";
            return;
        case 0xB0:
            out += "deg";
            return;
        case 0xB7:
        case 0x2022:
            out += "*";
            return;
        case 0xBC:
            out += "1/4";
            return;
        case 0xBD:
            out += "1/2";
            return;
        case 0xBE:
            out += "3/4";
            return;
        case 0x2018:
        case 0x2019:
        case 0x201A:
        case 0x2032:
            out.push_back('\'');
            return;
        case 0x201C:
        case 0x201D:
        case 0x201E:
        case 0x2033:
            out.push_back('"');
            return;
        case 0x2010:
        case 0x2011:
        case 0x2012:
        case 0x2013:
        case 0x2212:
            out.push_back('-');
            return;
        case 0x2014:
        case 0x2015:
            out += "--";
            return;
        case 0x2026:
            out += "...";
            return;
        case 0x2003:
        case 0x2002:
        case 0x2009:
        case 0x202F:
        case 0x3000:
            out.push_back(' ');
            return;
        case 0x2122:
            out += "(TM)";
            return;
        case 0x20AC:
            out += "EUR";
            return;
        case 0xA3:
            out += "GBP";
            return;
        case 0x2190:
            out += "<-";
            return;
        case 0x2192:
            out += "->";
            return;
        default:
            break;
    }

    /* Anything else (CJK, symbols, emoji) is dropped rather than drawn as a
     * blank box; a lost glyph reads better than a run of tofu. */
}

bool cp_supported(uint32_t cp)
{
    static const uint32_t kRanges[][2] = {
        {0x20, 0x7E},     {0xA0, 0x17F},    {0x391, 0x3A9},   {0x3B1, 0x3C9},   {0x2010, 0x2027},
        {0x2030, 0x203A}, {0x2044, 0x2044}, {0x20AC, 0x20AC}, {0x2122, 0x2122}, {0x2190, 0x2193},
        {0x2212, 0x2212}, {0x221E, 0x221E}, {0x2248, 0x2248}, {0x2260, 0x2260}, {0x2264, 0x2265},
    };
    for (const auto& r : kRanges) {
        if (cp >= r[0] && cp <= r[1]) {
            return true;
        }
    }
    return false;
}

static void utf8_put(std::string& out, uint32_t cp)
{
    if (cp < 0x80) {
        out.push_back((char)cp);
    } else if (cp < 0x800) {
        out.push_back((char)(0xC0 | (cp >> 6)));
        out.push_back((char)(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back((char)(0xE0 | (cp >> 12)));
        out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back((char)(0x80 | (cp & 0x3F)));
    } else {
        out.push_back((char)(0xF0 | (cp >> 18)));
        out.push_back((char)(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back((char)(0x80 | (cp & 0x3F)));
    }
}

std::string text_normalize(const std::string& in)
{
    std::string out;
    out.reserve(in.size());

    size_t i = 0;
    size_t n = in.size();
    while (i < n) {
        unsigned char c = (unsigned char)in[i];

        // Preserve our own in-band face markers untouched.
        if (c == (unsigned char)FACE_MARK_ITALIC || c == (unsigned char)FACE_MARK_BOLD ||
            c == (unsigned char)FACE_MARK_BOLDITAL) {
            out.push_back((char)c);
            i++;
            continue;
        }
        if (c >= 0x80 && (c & 0xC0) == 0x80) {
            i++;  // stray continuation byte
            continue;
        }

        uint32_t cp = vlw::utf8_next(in.data(), n, i);
        if (cp == 0xFFFD) {
            continue;
        }

        if (cp == '\t' || cp == '\n' || cp == '\r' || (cp >= 0x2000 && cp <= 0x200A) || cp == 0x202F ||
            cp == 0x205F || cp == 0x3000) {
            out.push_back(' ');
            continue;
        }
        if (cp < 0x20 || cp == 0x7F || (cp >= 0x80 && cp < 0xA0)) {
            continue;  // control characters
        }
        if (cp == 0xAD || (cp >= 0x200B && cp <= 0x200F) || cp == 0x2060 || cp == 0xFEFF) {
            continue;  // invisible: soft hyphen, zero-width, direction marks, BOM
        }
        if (cp >= 0xFB00 && cp <= 0xFB06) {
            static const char* kLig[] = {"ff", "fi", "fl", "ffi", "ffl", "st", "st"};
            out += kLig[cp - 0xFB00];
            continue;
        }
        if (cp_supported(cp)) {
            utf8_put(out, cp);
            continue;
        }
        append_ascii(out, cp);
    }

    return out;
}

/* ------------------------------------------------------------------ */
/* Word helpers                                                        */
/* ------------------------------------------------------------------ */

void split_words(const std::string& text, std::vector<std::string>& out)
{
    out.clear();
    size_t i = 0;
    size_t n = text.size();
    while (i < n) {
        while (i < n && text[i] == ' ') {
            i++;
        }
        if (i >= n) {
            break;
        }
        size_t start = i;
        while (i < n && text[i] != ' ') {
            i++;
        }
        out.emplace_back(text.substr(start, i - start));
    }
}

uint8_t strip_face(const std::string& token, std::string& out_text)
{
    if (!token.empty()) {
        char c = token[0];
        if (c == FACE_MARK_ITALIC) {
            out_text = token.substr(1);
            return FACE_ITALIC;
        }
        if (c == FACE_MARK_BOLD) {
            out_text = token.substr(1);
            return FACE_BOLD;
        }
        if (c == FACE_MARK_BOLDITAL) {
            out_text = token.substr(1);
            return FACE_BOLDITAL;
        }
    }
    out_text = token;
    return FACE_NORMAL;
}

/* ------------------------------------------------------------------ */
/* Pagination                                                          */
/* ------------------------------------------------------------------ */

namespace {

struct Measurer {
    M5Canvas* c;
    const Style& st;
    char style;

    int width(const std::string& s, uint8_t face)
    {
        return vlw::width(fontFor(st, style, face), s.data(), s.size());
    }

    int spaceWidth(uint8_t face)
    {
        return vlw::width(fontFor(st, style, face), " ", 1);
    }
};

/* Splits a token that is wider than the text column into chunks that fit. */
void hard_break(Measurer& m, const std::string& text, uint8_t face, int avail, std::string& head, std::string& tail)
{
    head.clear();
    tail.clear();
    if (text.empty()) {
        return;
    }

    // Walk code point boundaries; leave room for a trailing hyphen.
    const int hy = m.width("-", face);
    size_t cut   = 0;
    size_t i     = 0;
    while (i < text.size()) {
        vlw::utf8_next(text.data(), text.size(), i);
        if (m.width(text.substr(0, i), face) + hy <= avail) {
            cut = i;
        } else {
            break;
        }
    }
    if (cut == 0) {
        size_t one = 0;
        vlw::utf8_next(text.data(), text.size(), one);
        cut = one;  // always make progress
    }
    head = text.substr(0, cut);
    tail = text.substr(cut);
    if (!tail.empty()) {
        head.push_back('-');
    }
}

/* Baseline of a line box of height @p lh starting at @p top. */
inline int baselineFor(int top, int lh)
{
    return top + (lh * 72) / 100;
}

}  // namespace

bool layoutPage(M5Canvas* canvas, ParaSource& src, const Pos& start, const Style& st, Page& out)
{
    out.lines.clear();
    out.start = start;
    out.next  = start;
    out.eof   = false;
    out.style = st;

    if (!canvas) {
        return false;
    }
    if (!src.seek(start.off)) {
        out.eof = true;
        return false;
    }

    const int margin  = bodyMarginX(st);
    const int text_w  = bodyTextW(st);
    const int indent  = (bodyPx(st) * 9) / 10;

    int y             = TEXT_TOP;
    bool placed_any   = false;
    bool first_para   = true;
    uint32_t para_off = start.off;
    uint32_t word_ix  = start.word;

    Para para;
    std::vector<std::string> tokens;

    for (;;) {
        para_off = src.tell();
        if (!src.next(para)) {
            out.eof  = true;
            out.next = Pos{para_off, 0};
            return placed_any || out.lines.size() > 0;
        }

        const char pstyle     = para.style;
        const int lh          = lineHeight(canvas, st, pstyle);
        const bool is_heading = isHeading(pstyle);
        const bool is_code    = (pstyle == PARA_CODE);

        // Vertical air before a paragraph (skipped for the paragraph that the
        // page resumes inside, and for the very first thing on the page).
        // Consecutive code lines sit flush, like a listing.
        if (!first_para && word_ix == 0 && !is_code) {
            int gap = is_heading ? (lh * 2 / 3) : (lh / 5);
            if (y + gap + lh > TEXT_BOTTOM) {
                out.next = Pos{para_off, 0};
                return true;
            }
            y += gap;
        }
        first_para = false;

        if (is_code) {
            // Preformatted: Pos.word is a character offset; wrap by columns.
            Measurer m{canvas, st, pstyle};
            int cw = m.width("M", FACE_NORMAL);
            if (cw <= 0) {
                cw = 11;
            }
            const size_t cols = (size_t)((text_w > cw) ? (text_w / cw) : 1);
            const std::string& t = para.text;
            if (word_ix > t.size()) {
                word_ix = (uint32_t)t.size();
            }
            if (t.empty()) {
                // A blank line inside a listing still takes vertical space.
                if (y + lh > TEXT_BOTTOM) {
                    out.next = Pos{para_off, 0};
                    return true;
                }
                y += lh;
                word_ix = 0;
                continue;
            }
            while (word_ix < t.size()) {
                if (y + lh > TEXT_BOTTOM) {
                    out.next = Pos{para_off, word_ix};
                    return true;
                }
                Line line;
                line.y     = baselineFor(y, lh);
                line.style = pstyle;
                // Take up to `cols` code points (Pos.word is a byte offset).
                size_t e = word_ix;
                for (size_t k = 0; k < cols && e < t.size(); k++) {
                    vlw::utf8_next(t.data(), t.size(), e);
                }
                Word wd;
                wd.text = t.substr(word_ix, e - word_ix);
                wd.x    = margin;
                wd.face = FACE_NORMAL;
                line.words.push_back(wd);
                out.lines.push_back(line);
                placed_any = true;
                word_ix = (uint32_t)e;
                y += lh;
            }
            word_ix = 0;
            continue;
        }

        split_words(para.text, tokens);
        if (word_ix > tokens.size()) {
            word_ix = (uint32_t)tokens.size();
        }

        Measurer m{canvas, st, pstyle};

        if (tokens.empty()) {
            word_ix = 0;
            continue;
        }

        // Build lines out of the remaining tokens of this paragraph.
        while (word_ix < tokens.size()) {
            if (y + lh > TEXT_BOTTOM) {
                out.next = Pos{para_off, word_ix};
                return true;
            }

            bool first_line_of_para = (word_ix == 0) && pstyle == PARA_BODY;
            int x                   = margin + (first_line_of_para ? indent : 0);
            int avail               = text_w - (first_line_of_para ? indent : 0);
            int used                = 0;

            Line line;
            line.y     = baselineFor(y, lh);
            line.style = pstyle;

            while (word_ix < tokens.size()) {
                std::string txt;
                uint8_t face = strip_face(tokens[word_ix], txt);
                if (txt.empty()) {
                    word_ix++;
                    continue;
                }

                int sp = line.words.empty() ? 0 : m.spaceWidth(face);
                int w  = m.width(txt, face);

                if (used + sp + w > avail) {
                    if (line.words.empty()) {
                        // A single token wider than the column: break it.
                        std::string head;
                        std::string tail;
                        hard_break(m, txt, face, avail, head, tail);

                        Word wd;
                        wd.text = head;
                        wd.x    = x;
                        wd.face = face;
                        line.words.push_back(wd);
                        used += m.width(head, face);

                        // Put the remainder back so the next line picks it up.
                        std::string marker;
                        if (face == FACE_ITALIC) {
                            marker.push_back(FACE_MARK_ITALIC);
                        } else if (face == FACE_BOLD) {
                            marker.push_back(FACE_MARK_BOLD);
                        } else if (face == FACE_BOLDITAL) {
                            marker.push_back(FACE_MARK_BOLDITAL);
                        }
                        tokens[word_ix] = marker + tail;
                        if (tail.empty()) {
                            word_ix++;
                        }
                    }
                    break;
                }

                Word wd;
                wd.text = txt;
                wd.x    = x + used + sp;
                wd.face = face;
                line.words.push_back(wd);
                used += sp + w;
                word_ix++;
            }

            if (line.words.empty()) {
                // Nothing could be placed at all — bail out rather than spin.
                word_ix++;
                continue;
            }

            // Justify body lines that are not the last line of their
            // paragraph: spread the slack over the inter-word gaps, the way
            // a Kindle sets text by default.
            bool last_line = (word_ix >= tokens.size());
            if (pstyle == PARA_BODY && !last_line && line.words.size() > 1) {
                int slack = avail - used;
                int gaps  = (int)line.words.size() - 1;
                // Cap the stretch per gap: on a narrow column at big sizes,
                // a ragged line reads better than rivers of white.
                int max_stretch = (indent * 2) / 3;
                if (slack > 0 && slack <= gaps * max_stretch) {
                    for (int k = 1; k <= gaps; k++) {
                        line.words[(size_t)k].x += (slack * k) / gaps;
                    }
                }
            }

            out.lines.push_back(line);
            placed_any = true;
            y += lh;
        }

        // Paragraph finished; the next one starts at the current stream offset.
        word_ix = 0;

        if (is_heading) {
            y += lh / 3;
        }
    }
}

}  // namespace layout
