/*
 * paperdiy-reader — text layout engine.
 *
 * Everything the reader renders arrives here as a stream of *paragraphs*
 * (a style code plus a line of text) produced by a ParaSource. TXT files are
 * streamed straight from the source file; EPUBs are normalised into a cached
 * paragraph stream first. That keeps a single pagination/rendering path for
 * both formats.
 *
 * Word-level emphasis is carried in-band: a word may start with one marker
 * byte (\x01 italic, \x02 bold, \x03 bold-italic). Markers are stripped before
 * measuring or drawing.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <M5GFX.h>

namespace layout {

/* ---- Page geometry (portrait canvas) ---------------------------------- */

static constexpr int SCREEN_W    = 540;
static constexpr int SCREEN_H    = 960;
static constexpr int MARGIN_X    = 24; /*!< margin for UI chrome (header, footer, menus) */
static constexpr int HEADER_H    = 46;
static constexpr int FOOTER_H    = 50;
static constexpr int TEXT_TOP    = HEADER_H + 14;
static constexpr int TEXT_BOTTOM = SCREEN_H - FOOTER_H - 12;

/**
 * @brief Everything that changes where glyphs land on a page.
 *
 * Any field changing invalidates the cached page index (see styleHash()).
 * The colour theme is deliberately not in here: it recolours, never moves.
 */
struct Style {
    uint8_t size    = 1; /*!< FontSizePreset */
    uint8_t family  = 0; /*!< FontFamily */
    uint8_t spacing = 1; /*!< LineSpacing */
    uint8_t margins = 1; /*!< MarginPreset */
};

/** @brief The style described by the current hal.settings. */
Style currentStyle();

/** @brief Stable hash of a style, mixed into the page-index cache key. */
uint32_t styleHash(const Style& st);

/** @brief Left/right body-text margin for a style. */
int bodyMarginX(const Style& st);

/** @brief Width of the body text column for a style. */
int bodyTextW(const Style& st);

/* ---- Paragraph model --------------------------------------------------- */

/** @brief Paragraph style codes, stored verbatim in the normalised stream. */
enum ParaStyle : char {
    PARA_BODY = 'p',
    PARA_H1   = '1',
    PARA_H2   = '2',
    PARA_H3   = '3',
    PARA_CAPTION = 'f', /*!< figure/table captions (h5/h6, figcaption) */
    PARA_CODE    = 'c', /*!< one line of preformatted text, spaces kept */
};

/** @brief True for the three heading levels. */
inline bool isHeading(char style)
{
    return style == PARA_H1 || style == PARA_H2 || style == PARA_H3;
}

/** @brief In-band per-word face markers. */
enum FaceMark : char {
    FACE_MARK_ITALIC   = '\x01',
    FACE_MARK_BOLD     = '\x02',
    FACE_MARK_BOLDITAL = '\x03',
};

/** @brief Resolved typeface for one word. */
enum Face : uint8_t {
    FACE_NORMAL   = 0,
    FACE_ITALIC   = 1,
    FACE_BOLD     = 2,
    FACE_BOLDITAL = 3,
};

/** @brief One paragraph as delivered by a ParaSource. */
struct Para {
    char style = PARA_BODY;
    std::string text;
};

/** @brief A resumable position: byte offset of a paragraph plus a word index. */
struct Pos {
    uint32_t off  = 0;
    uint32_t word = 0;

    bool operator==(const Pos& o) const
    {
        return off == o.off && word == o.word;
    }
    bool operator!=(const Pos& o) const
    {
        return !(*this == o);
    }
};

/** @brief One measured word ready to draw. */
struct Word {
    std::string text; /*!< marker already stripped */
    int x    = 0;
    uint8_t face = FACE_NORMAL;
};

/** @brief One laid-out line. */
struct Line {
    int y       = 0;
    char style  = PARA_BODY;
    std::vector<Word> words;
};

/** @brief One screen of laid-out text. */
struct Page {
    Pos start;
    Pos next;
    bool eof = false;
    Style style; /*!< typography the page was laid out with */
    std::vector<Line> lines;
};

/**
 * @brief Abstract paragraph stream.
 *
 * Offsets are byte offsets into whatever the concrete source reads from, and
 * must be stable so pagination results can be cached to disk.
 */
class ParaSource {
public:
    virtual ~ParaSource() = default;

    /** @brief Positions the stream at a paragraph boundary. */
    virtual bool seek(uint32_t off) = 0;

    /** @brief Byte offset of the paragraph that the next next() will return. */
    virtual uint32_t tell() = 0;

    /** @brief Reads the next paragraph. Returns false at end of stream. */
    virtual bool next(Para& out) = 0;

    /** @brief Total stream length in bytes, used for progress reporting. */
    virtual uint32_t size() = 0;
};

/* ---- Fonts ------------------------------------------------------------- */

/** @brief Font for a style, paragraph style and face. */
const lgfx::IFont* fontFor(const Style& st, char para_style, uint8_t face);

/** @brief Body em size in pixels for a style. */
int bodyPx(const Style& st);

/** @brief Atkinson Hyperlegible for UI chrome: largest pack size <= @p px. */
const lgfx::IFont* uiFont(int px, bool bold = false);

/** @brief Literata at the largest pack size <= @p px (titles, covers). */
const lgfx::IFont* serifFont(int px, uint8_t face = 0);

/** @brief Shortens UTF-8 @p text with an ellipsis so it fits @p max_w pixels. */
std::string ellipsize(const lgfx::IFont* font, const std::string& text, int max_w);

/** @brief Greedy word wrap into at most @p max_lines lines (last one ellipsized). */
std::vector<std::string> wrapText(const lgfx::IFont* font, const std::string& text, int max_w, int max_lines);

/** @brief Line advance in pixels for a style and paragraph style. */
int lineHeight(M5Canvas* canvas, const Style& st, char para_style);

/* ---- Pagination -------------------------------------------------------- */

/**
 * @brief Lays out exactly one screen of text starting at @p start.
 *
 * @param canvas       used only for font metrics
 * @param src          paragraph stream (repositioned by this call)
 * @param start        first paragraph offset + word index to place
 * @param st           typography (size, family, spacing, margins)
 * @param out          receives the laid-out lines and the next start position
 * @return false when @p start is already past the end of the stream.
 */
bool layoutPage(M5Canvas* canvas, ParaSource& src, const Pos& start, const Style& st, Page& out);

/* ---- Text normalisation ------------------------------------------------ */

/**
 * @brief Cleans UTF-8 text for the bundled VLW fonts.
 *
 * Code points the font pack covers (Latin, Latin-1, Latin Extended-A, Greek,
 * typographic punctuation, a few symbols) are kept as UTF-8. Ligature
 * presentation forms are expanded, invisible characters (soft hyphen,
 * zero-width space, BOM) dropped, exotic spaces become ' ', and anything else
 * is folded to an ASCII look-alike or dropped rather than drawn as a box.
 */
std::string text_normalize(const std::string& in);

/** @brief True when the font pack has a glyph for @p cp (serif face). */
bool cp_supported(uint32_t cp);

/** @brief Appends one Unicode code point to @p out in an ASCII fallback form. */
void append_ascii(std::string& out, uint32_t cp);

/** @brief Splits a normalised paragraph into whitespace-separated tokens. */
void split_words(const std::string& text, std::vector<std::string>& out);

/** @brief Strips a leading face marker, returning the resolved face. */
uint8_t strip_face(const std::string& token, std::string& out_text);

}  // namespace layout
