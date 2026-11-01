/*
 * paperdiy-reader — EPUB support.
 *
 * Contains a deliberately small, tolerant XML/XHTML scanner instead of a real
 * parser. EPUBs in the wild are frequently not well-formed, and a strict
 * parser that bails on the first stray '&' is worse than useless on a device
 * with no error console. The scanner here never allocates a tree: it walks
 * characters, tracks a handful of tags, and emits paragraphs.
 *
 * SPDX-License-Identifier: MIT
 */
#include "reader/book_epub.h"

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <strings.h>
#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "miniz_lite.h"

static const char* TAG = "book_epub";

namespace {

/* ================================================================== */
/* Small string / path helpers                                         */
/* ================================================================== */

void utf8_encode(std::string& out, uint32_t cp)
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
    } else if (cp <= 0x10FFFF) {
        out.push_back((char)(0xF0 | (cp >> 18)));
        out.push_back((char)(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back((char)(0x80 | (cp & 0x3F)));
    }
}

/* Untrusted-input limits. EPUBs come from SD cards and web uploads; every
 * size below is far above anything a real book uses. */
constexpr size_t MAX_CONTAINER_BYTES = 64 * 1024;
constexpr size_t MAX_OPF_BYTES       = 1024 * 1024;
constexpr size_t MAX_XHTML_BYTES     = 3 * 1024 * 1024;
constexpr size_t MAX_COVER_BYTES     = 3 * 1024 * 1024;
constexpr size_t MAX_MANIFEST_ITEMS  = 10000;
constexpr size_t MAX_SPINE_ITEMS     = 4000;
constexpr uint64_t MAX_STREAM_BYTES  = 96ull * 1024 * 1024;

void* extract_named(mz_zip_archive& zip, const char* name, size_t max_bytes, size_t* out_size)
{
    int idx = mz_zip_reader_locate_file(&zip, name);
    if (idx < 0) {
        *out_size = 0;
        return nullptr;
    }
    return mz_zip_reader_extract_to_heap_max(&zip, (uint32_t)idx, max_bytes, out_size);
}

std::string percent_decode(const std::string& in)
{
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); i++) {
        if (in[i] == '%' && i + 2 < in.size() && isxdigit((unsigned char)in[i + 1]) &&
            isxdigit((unsigned char)in[i + 2])) {
            char hex[3] = {in[i + 1], in[i + 2], 0};
            long v      = strtol(hex, nullptr, 16);
            if (v != 0) {  // never let "%00" cut a name short
                out.push_back((char)v);
                i += 2;
                continue;
            }
        }
        out.push_back(in[i]);
    }
    return out;
}

std::string dir_of(const std::string& path)
{
    size_t slash = path.find_last_of('/');
    if (slash == std::string::npos) {
        return std::string();
    }
    return path.substr(0, slash + 1);  // keeps the trailing '/'
}

/** Joins a ZIP-internal directory and a relative href, collapsing . and .. */
std::string path_join(const std::string& dir, const std::string& rel)
{
    std::string combined;
    if (!rel.empty() && rel[0] == '/') {
        combined = rel.substr(1);
    } else {
        combined = dir + rel;
    }

    std::vector<std::string> parts;
    size_t i = 0;
    while (i < combined.size()) {
        size_t j = combined.find('/', i);
        std::string seg =
            (j == std::string::npos) ? combined.substr(i) : combined.substr(i, j - i);
        if (seg == "..") {
            if (!parts.empty()) {
                parts.pop_back();
            }
        } else if (!seg.empty() && seg != ".") {
            parts.push_back(seg);
        }
        if (j == std::string::npos) {
            break;
        }
        i = j + 1;
    }

    std::string out;
    for (size_t k = 0; k < parts.size(); k++) {
        if (k) {
            out.push_back('/');
        }
        out += parts[k];
    }
    return out;
}

/* ================================================================== */
/* Tolerant tag scanner                                                */
/* ================================================================== */

inline bool is_space(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v';
}

/** Local name of a tag (everything after the last ':'), lowercased. */
std::string local_name(const char* s, const char* e)
{
    const char* colon = nullptr;
    for (const char* p = s; p < e; p++) {
        if (*p == ':') {
            colon = p;
        }
    }
    const char* start = colon ? colon + 1 : s;

    std::string out;
    out.reserve((size_t)(e - start));
    for (const char* p = start; p < e; p++) {
        char c = *p;
        if (c >= 'A' && c <= 'Z') {
            c = (char)(c - 'A' + 'a');
        }
        out.push_back(c);
    }
    return out;
}

/**
 * Finds the end of the tag that starts at @p lt (which points at '<').
 * Quote-aware so that attribute values containing '>' do not confuse it.
 * @return pointer to the closing '>', or @p end when the tag is unterminated.
 */
const char* find_tag_end(const char* lt, const char* end)
{
    const char* p = lt + 1;
    char quote    = 0;
    while (p < end) {
        char c = *p;
        if (quote) {
            if (c == quote) {
                quote = 0;
            }
        } else if (c == '"' || c == '\'') {
            quote = c;
        } else if (c == '>') {
            return p;
        }
        p++;
    }
    return end;
}

/** Reads one attribute value out of a tag body. Case-insensitive on the name. */
bool get_attr(const char* tag_start, const char* tag_end, const char* attr, std::string& value)
{
    size_t alen = strlen(attr);
    const char* p = tag_start;

    while (p < tag_end) {
        // Attributes are always preceded by whitespace.
        if (!is_space(*p)) {
            p++;
            continue;
        }
        while (p < tag_end && is_space(*p)) {
            p++;
        }
        if (p + alen > tag_end) {
            break;
        }
        if (strncasecmp(p, attr, alen) == 0) {
            const char* q = p + alen;
            while (q < tag_end && is_space(*q)) {
                q++;
            }
            if (q < tag_end && *q == '=') {
                q++;
                while (q < tag_end && is_space(*q)) {
                    q++;
                }
                if (q < tag_end && (*q == '"' || *q == '\'')) {
                    char quote      = *q++;
                    const char* vs  = q;
                    while (q < tag_end && *q != quote) {
                        q++;
                    }
                    value.assign(vs, (size_t)(q - vs));
                    return true;
                }
                const char* vs = q;
                while (q < tag_end && !is_space(*q) && *q != '/' && *q != '>') {
                    q++;
                }
                value.assign(vs, (size_t)(q - vs));
                return true;
            }
        }
        // Skip to the next whitespace so we do not re-match inside a value.
        while (p < tag_end && !is_space(*p)) {
            p++;
        }
    }
    return false;
}

struct TagInfo {
    std::string name;   /*!< lowercase local name */
    bool closing = false;
    bool self    = false;
    const char* body_start = nullptr; /*!< just after the name */
    const char* body_end   = nullptr; /*!< the '>' */
    const char* next       = nullptr; /*!< just after the '>' */
    bool decl              = false;   /*!< <?xml ...?>, <!-- --> or <!DOCTYPE> */
};

/** Parses the tag starting at @p lt. Always advances (never returns lt). */
TagInfo parse_tag(const char* lt, const char* end)
{
    TagInfo t;

    if (lt + 1 < end && (lt[1] == '?' || lt[1] == '!')) {
        t.decl = true;
        if (lt + 3 < end && lt[1] == '!' && lt[2] == '-' && lt[3] == '-') {
            const char* p = lt + 4;
            while (p + 2 < end && !(p[0] == '-' && p[1] == '-' && p[2] == '>')) {
                p++;
            }
            t.next = (p + 2 < end) ? (p + 3) : end;
        } else {
            const char* gt = find_tag_end(lt, end);
            t.next         = (gt < end) ? (gt + 1) : end;
        }
        return t;
    }

    const char* gt = find_tag_end(lt, end);
    const char* p  = lt + 1;

    if (p < gt && *p == '/') {
        t.closing = true;
        p++;
    }

    const char* ns = p;
    while (p < gt && !is_space(*p) && *p != '/' && *p != '>') {
        p++;
    }
    t.name       = local_name(ns, p);
    t.body_start = p;
    t.body_end   = gt;

    if (gt > lt && gt[-1] == '/') {
        t.self = true;
    }
    t.next = (gt < end) ? (gt + 1) : end;
    return t;
}

/** Finds the first tag with local name @p want at or after @p p. */
const char* find_tag(const char* p, const char* end, const char* want, TagInfo* info)
{
    while (p < end) {
        const char* lt = (const char*)memchr(p, '<', (size_t)(end - p));
        if (!lt) {
            return nullptr;
        }
        TagInfo t = parse_tag(lt, end);
        if (!t.decl && !t.closing && t.name == want) {
            if (info) {
                *info = t;
            }
            return lt;
        }
        p = (t.next > lt) ? t.next : (lt + 1);
    }
    return nullptr;
}

/* ---- entities ---------------------------------------------------- */

struct NamedEntity {
    const char* name;
    uint32_t cp;
};

const NamedEntity kEntities[] = {
    {"amp", '&'},      {"lt", '<'},        {"gt", '>'},       {"quot", '"'},      {"apos", '\''},
    {"nbsp", 0xA0},    {"mdash", 0x2014},  {"ndash", 0x2013}, {"hellip", 0x2026}, {"lsquo", 0x2018},
    {"rsquo", 0x2019}, {"ldquo", 0x201C},  {"rdquo", 0x201D}, {"sbquo", 0x201A},  {"bdquo", 0x201E},
    {"copy", 0xA9},    {"reg", 0xAE},      {"trade", 0x2122}, {"deg", 0xB0},      {"middot", 0xB7},
    {"bull", 0x2022},  {"laquo", 0xAB},    {"raquo", 0xBB},   {"times", 0xD7},    {"divide", 0xF7},
    {"frac12", 0xBD},  {"frac14", 0xBC},   {"frac34", 0xBE},  {"pound", 0xA3},    {"euro", 0x20AC},
    {"agrave", 0xE0},  {"aacute", 0xE1},   {"acirc", 0xE2},   {"atilde", 0xE3},   {"auml", 0xE4},
    {"aring", 0xE5},   {"aelig", 0xE6},    {"ccedil", 0xE7},  {"egrave", 0xE8},   {"eacute", 0xE9},
    {"ecirc", 0xEA},   {"euml", 0xEB},     {"igrave", 0xEC},  {"iacute", 0xED},   {"icirc", 0xEE},
    {"iuml", 0xEF},    {"ntilde", 0xF1},   {"ograve", 0xF2},  {"oacute", 0xF3},   {"ocirc", 0xF4},
    {"otilde", 0xF5},  {"ouml", 0xF6},     {"oslash", 0xF8},  {"ugrave", 0xF9},   {"uacute", 0xFA},
    {"ucirc", 0xFB},   {"uuml", 0xFC},     {"yacute", 0xFD},  {"yuml", 0xFF},     {"szlig", 0xDF},
    {"Agrave", 0xC0},  {"Aacute", 0xC1},   {"Auml", 0xC4},    {"Ccedil", 0xC7},   {"Eacute", 0xC9},
    {"Egrave", 0xC8},  {"Ntilde", 0xD1},   {"Ouml", 0xD6},    {"Uuml", 0xDC},     {"AElig", 0xC6},
    {"oelig", 0x153},  {"OElig", 0x152},   {"ensp", ' '},     {"emsp", ' '},      {"thinsp", ' '},
    {"shy", 0},        {"zwnj", 0},        {"zwj", 0},
};

/**
 * Decodes the entity starting at @p s (which points at '&').
 * @return number of bytes consumed, or 0 when this is not an entity.
 */
size_t decode_entity(const char* s, size_t n, std::string& out)
{
    if (n < 3 || s[0] != '&') {
        return 0;
    }

    // Bound the search: a real entity is short.
    size_t limit = n < 12 ? n : 12;
    size_t semi  = 0;
    for (size_t i = 1; i < limit; i++) {
        if (s[i] == ';') {
            semi = i;
            break;
        }
        if (is_space(s[i]) || s[i] == '&' || s[i] == '<') {
            return 0;
        }
    }
    if (semi == 0) {
        return 0;
    }

    if (s[1] == '#') {
        uint32_t cp = 0;
        if (semi > 2 && (s[2] == 'x' || s[2] == 'X')) {
            for (size_t i = 3; i < semi; i++) {
                char c = s[i];
                uint32_t d;
                if (c >= '0' && c <= '9') {
                    d = (uint32_t)(c - '0');
                } else if (c >= 'a' && c <= 'f') {
                    d = (uint32_t)(c - 'a' + 10);
                } else if (c >= 'A' && c <= 'F') {
                    d = (uint32_t)(c - 'A' + 10);
                } else {
                    return 0;
                }
                cp = cp * 16 + d;
            }
        } else {
            for (size_t i = 2; i < semi; i++) {
                if (s[i] < '0' || s[i] > '9') {
                    return 0;
                }
                cp = cp * 10 + (uint32_t)(s[i] - '0');
            }
        }
        if (cp) {
            utf8_encode(out, cp);
        }
        return semi + 1;
    }

    std::string name(s + 1, semi - 1);
    for (const NamedEntity& e : kEntities) {
        if (name == e.name) {
            if (e.cp) {
                utf8_encode(out, e.cp);
            }
            return semi + 1;
        }
    }
    return 0;
}

/* ================================================================== */
/* XHTML -> normalised paragraphs                                      */
/* ================================================================== */

/* Longest paragraph kept in one piece; longer runs are split. */
constexpr size_t MAX_PARA_BYTES = 16 * 1024;

inline bool is_face_mark(char c)
{
    return c == layout::FACE_MARK_ITALIC || c == layout::FACE_MARK_BOLD || c == layout::FACE_MARK_BOLDITAL;
}

class ParagraphWriter {
public:
    explicit ParagraphWriter(FILE* f, std::vector<TocEntry>* toc) : _f(f), _toc(toc)
    {
        _buf.reserve(9000);
    }

    /* ---- preformatted text (<pre>): every character kept, one paragraph
     * per source line, no face markers. */

    void putPreByte(char c)
    {
        if (c == '\r' || is_face_mark(c)) {
            return;
        }
        if (c == '\n') {
            flushCode();
            return;
        }
        if (c == '\t') {
            _cur.append("    ");
            return;
        }
        _cur.push_back(c);
    }

    void putPreText(const std::string& s)
    {
        for (char c : s) {
            putPreByte(c);
        }
    }

    /** Starts a <pre> block: anything pending is closed as a normal paragraph. */
    void beginPre(char style)
    {
        flush(style);
        _code_lines    = 0;
        _pending_blank = 0;
    }

    /** Ends a <pre> block, dropping trailing blank lines. */
    void endPre()
    {
        flushCode();
        _pending_blank = 0;
    }

    void flushCode()
    {
        std::string ascii = layout::text_normalize(_cur);
        _cur.clear();
        while (!ascii.empty() && ascii.back() == ' ') {
            ascii.pop_back();
        }
        if (ascii.empty()) {
            // Leading blank lines are dropped; inner ones are held back until
            // we know more code follows them.
            if (_code_lines > 0) {
                _pending_blank++;
            }
            return;
        }
        for (; _pending_blank > 0; _pending_blank--) {
            emit(layout::PARA_CODE, std::string());
        }
        emit(layout::PARA_CODE, ascii);
        _code_lines++;
    }

    /** Separator between table cells in a flattened row. */
    void cellBreak()
    {
        if (!_cur.empty()) {
            _cur.append(" |");
            _word_open     = false;
            _pending_space = true;
        }
    }

    void putByte(char c, uint8_t face)
    {
        if (is_face_mark(c)) {
            return;  // book text must not inject our in-band face markers
        }
        if (is_space(c)) {
            if (_word_open) {
                _word_open     = false;
                _pending_space = true;
            }
            return;
        }
        if (!_word_open) {
            if (_pending_space && !_cur.empty()) {
                _cur.push_back(' ');
            }
            _pending_space = false;
            if (face == layout::FACE_ITALIC) {
                _cur.push_back(layout::FACE_MARK_ITALIC);
            } else if (face == layout::FACE_BOLD) {
                _cur.push_back(layout::FACE_MARK_BOLD);
            } else if (face == layout::FACE_BOLDITAL) {
                _cur.push_back(layout::FACE_MARK_BOLDITAL);
            }
            _word_open = true;
        }
        _cur.push_back(c);
    }

    void putText(const std::string& s, uint8_t face)
    {
        for (char c : s) {
            putByte(c, face);
        }
    }

    void flush(char style)
    {
        _word_open     = false;
        _pending_space = false;

        if (_cur.empty()) {
            return;
        }

        std::string ascii = layout::text_normalize(_cur);
        _cur.clear();

        // Drop paragraphs that folded down to nothing but markers/spaces.
        bool has_text = false;
        for (char c : ascii) {
            if ((unsigned char)c > ' ' && c != layout::FACE_MARK_ITALIC && c != layout::FACE_MARK_BOLD &&
                c != layout::FACE_MARK_BOLDITAL) {
                has_text = true;
                break;
            }
        }
        if (!has_text) {
            return;
        }

        if (_toc && (style == layout::PARA_H1 || style == layout::PARA_H2) && _toc->size() < 2000) {
            TocEntry e;
            e.off   = _written + (uint32_t)_buf.size();
            e.level = (style == layout::PARA_H1) ? 1 : 2;
            for (char c : ascii) {
                if (c == layout::FACE_MARK_ITALIC || c == layout::FACE_MARK_BOLD ||
                    c == layout::FACE_MARK_BOLDITAL) {
                    continue;
                }
                if (e.title.size() >= 96 && ((unsigned char)c & 0xC0) != 0x80) {
                    break;
                }
                {
                    e.title.push_back(c);
                }
            }
            _toc->push_back(e);
        }

        emit(style, ascii);
    }

    void emit(char style, const std::string& text)
    {
        _buf.push_back(style);
        _buf.push_back('\t');
        _buf += text;
        _buf.push_back('\n');
        _count++;

        if (_buf.size() >= 8192) {
            commit();
        }
    }

    void commit()
    {
        if (!_buf.empty() && _f && !_io_err) {
            if (fwrite(_buf.data(), 1, _buf.size(), _f) != _buf.size()) {
                _io_err = true;
            }
            _written += (uint32_t)_buf.size();
        }
        _buf.clear();
    }

    uint32_t count() const
    {
        return _count;
    }

    size_t pendingSize() const
    {
        return _cur.size();
    }

    /** True when a write failed (e.g. card full): the output is incomplete. */
    bool ioError() const
    {
        return _io_err;
    }

    /** Bytes written so far (output size cap). */
    uint64_t written() const
    {
        return (uint64_t)_written + _buf.size();
    }

private:
    FILE* _f;
    std::vector<TocEntry>* _toc;
    std::string _cur;
    std::string _buf;
    uint32_t _count         = 0;
    uint32_t _written       = 0;
    uint32_t _code_lines    = 0;
    uint32_t _pending_blank = 0;
    bool _word_open         = false;
    bool _pending_space     = false;
    bool _io_err            = false;
};

bool is_block_tag(const std::string& n)
{
    return n == "p" || n == "div" || n == "li" || n == "blockquote" || n == "tr" || n == "section" ||
           n == "article" || n == "dd" || n == "dt" || n == "hr" || n == "center" || n == "ul" || n == "ol" ||
           n == "table" || n == "body" || n == "aside" || n == "header" || n == "footer" || n == "nav";
}

char heading_style(const std::string& n)
{
    if (n == "h1") {
        return layout::PARA_H1;
    }
    if (n == "h2") {
        return layout::PARA_H2;
    }
    if (n == "h3" || n == "h4") {
        return layout::PARA_H3;
    }
    if (n == "h5" || n == "h6" || n == "figcaption" || n == "caption") {
        // Manning and O'Reilly put figure/table captions in h5; render them
        // as small italic captions rather than as giant headings.
        return layout::PARA_CAPTION;
    }
    return 0;
}

/** Converts one XHTML document into paragraphs appended to @p pw. */
void html_to_paragraphs(const char* buf, size_t n, ParagraphWriter& pw)
{
    const char* p   = buf;
    const char* end = buf + n;

    char style       = layout::PARA_BODY;
    int emph         = 0;
    int strong       = 0;
    std::string skip_name;
    int skip_depth   = 0;
    int pre_depth    = 0;
    int cell_in_row  = 0;

    auto face_now = [&]() -> uint8_t {
        if (emph > 0 && strong > 0) {
            return layout::FACE_BOLDITAL;
        }
        if (emph > 0) {
            return layout::FACE_ITALIC;
        }
        if (strong > 0) {
            return layout::FACE_BOLD;
        }
        return layout::FACE_NORMAL;
    };

    while (p < end) {
        // Hostile or broken XHTML can hold megabytes with no block tag; split
        // runaway paragraphs so memory stays bounded (and below the 32 KB
        // line limit of NormParaSource).
        if (pw.pendingSize() >= MAX_PARA_BYTES) {
            if (pre_depth > 0) {
                pw.flushCode();
            } else {
                pw.flush(style);
            }
        }
        if (*p == '<') {
            TagInfo t = parse_tag(p, end);
            p         = (t.next > p) ? t.next : (p + 1);
            if (t.decl) {
                continue;
            }

            if (skip_depth > 0) {
                if (t.name == skip_name) {
                    if (t.closing) {
                        skip_depth--;
                    } else if (!t.self) {
                        skip_depth++;
                    }
                }
                continue;
            }

            if (!t.closing && !t.self &&
                (t.name == "head" || t.name == "script" || t.name == "style" || t.name == "svg" ||
                 t.name == "title")) {
                skip_name  = t.name;
                skip_depth = 1;
                continue;
            }

            if (t.name == "pre") {
                if (t.closing) {
                    if (pre_depth > 0 && --pre_depth == 0) {
                        pw.endPre();
                    }
                } else if (!t.self) {
                    if (pre_depth++ == 0) {
                        pw.beginPre(style);
                        style = layout::PARA_BODY;
                    }
                }
                continue;
            }
            if (pre_depth > 0) {
                if (t.name == "br") {
                    pw.flushCode();
                }
                continue;  // markup inside a listing is transparent
            }

            if (t.name == "br") {
                pw.flush(style);
                continue;
            }
            if (t.name == "td" || t.name == "th") {
                if (!t.closing) {
                    if (cell_in_row++ > 0) {
                        pw.cellBreak();
                    }
                }
                continue;
            }
            if (t.name == "tr") {
                cell_in_row = 0;
            }
            if (t.name == "img" || t.name == "image" || t.name == "figure") {
                continue;  // images are not rendered
            }

            char h = heading_style(t.name);
            if (h) {
                pw.flush(style);
                style = t.closing ? (char)layout::PARA_BODY : h;
                continue;
            }

            if (is_block_tag(t.name)) {
                pw.flush(style);
                style = layout::PARA_BODY;
                continue;
            }

            if (t.name == "em" || t.name == "i" || t.name == "cite" || t.name == "var" || t.name == "dfn") {
                if (t.closing) {
                    if (emph > 0) {
                        emph--;
                    }
                } else if (!t.self) {
                    emph++;
                }
                continue;
            }
            if (t.name == "strong" || t.name == "b") {
                if (t.closing) {
                    if (strong > 0) {
                        strong--;
                    }
                } else if (!t.self) {
                    strong++;
                }
                continue;
            }

            continue;  // every other tag is transparent
        }

        if (skip_depth > 0) {
            p++;
            continue;
        }

        if (*p == '&') {
            std::string decoded;
            size_t used = decode_entity(p, (size_t)(end - p), decoded);
            if (used) {
                if (pre_depth > 0) {
                    pw.putPreText(decoded);
                } else {
                    pw.putText(decoded, face_now());
                }
                p += used;
                continue;
            }
        }

        if (pre_depth > 0) {
            pw.putPreByte(*p);
        } else {
            pw.putByte(*p, face_now());
        }
        p++;
    }

    if (pre_depth > 0) {
        pw.endPre();
    }
    pw.flush(style);
}

/* ================================================================== */
/* OPF / container plumbing                                            */
/* ================================================================== */

struct OpfInfo {
    std::string title;
    std::string author;
    std::vector<std::string> spine_paths;  /*!< ZIP member names, in reading order */
};

/** Extracts the text content of the first <local> element. */
bool element_text(const char* buf, size_t n, const char* local, std::string& out)
{
    const char* end = buf + n;
    TagInfo t;
    const char* lt = find_tag(buf, end, local, &t);
    if (!lt) {
        return false;
    }
    if (t.self) {
        out.clear();
        return true;
    }

    const char* content = t.next;
    const char* p       = content;
    while (p < end) {
        const char* nxt = (const char*)memchr(p, '<', (size_t)(end - p));
        if (!nxt) {
            break;
        }
        TagInfo c = parse_tag(nxt, end);
        if (c.closing && c.name == local) {
            // Metadata is short; a hostile multi-megabyte <dc:title> must not
            // take the library scan down with it.
            std::string raw(content, std::min<size_t>((size_t)(nxt - content), 1024));
            // Decode entities inside the metadata value.
            std::string dec;
            for (size_t i = 0; i < raw.size();) {
                if (raw[i] == '&') {
                    std::string piece;
                    size_t used = decode_entity(raw.c_str() + i, raw.size() - i, piece);
                    if (used) {
                        dec += piece;
                        i += used;
                        continue;
                    }
                }
                dec.push_back(raw[i]);
                i++;
            }
            out = dec;
            return true;
        }
        p = (c.next > nxt) ? c.next : (nxt + 1);
    }
    return false;
}

/** Locates the OPF package document named by META-INF/container.xml. */
bool find_opf_path(mz_zip_archive& zip, std::string& opf_path)
{
    size_t sz  = 0;
    char* data = (char*)extract_named(zip, "META-INF/container.xml", MAX_CONTAINER_BYTES, &sz);
    if (!data) {
        // Some broken EPUBs omit the container; fall back to a scan.
        uint32_t count = mz_zip_reader_get_num_files(&zip);
        for (uint32_t i = 0; i < count; i++) {
            mz_zip_archive_file_stat st;
            if (mz_zip_reader_file_stat(&zip, i, &st)) {
                size_t len = strlen(st.filename);
                if (len > 4 && strcasecmp(st.filename + len - 4, ".opf") == 0) {
                    opf_path = st.filename;
                    return true;
                }
            }
        }
        return false;
    }

    TagInfo t;
    const char* lt = find_tag(data, data + sz, "rootfile", &t);
    bool ok        = false;
    if (lt) {
        std::string full;
        if (get_attr(t.body_start, t.body_end, "full-path", full) && !full.empty()) {
            opf_path = percent_decode(full);
            ok       = true;
        }
    }

    mz_free(data);
    return ok;
}

/** Case-insensitive member lookup with a couple of tolerant fallbacks. */
int locate_member(mz_zip_archive& zip, const std::string& name)
{
    int idx = mz_zip_reader_locate_file(&zip, name.c_str());
    if (idx >= 0) {
        return idx;
    }

    // Some producers store names with a leading "./" or a different case for
    // the directory part; fall back to comparing basenames.
    size_t slash          = name.find_last_of('/');
    std::string base_name = (slash == std::string::npos) ? name : name.substr(slash + 1);
    if (base_name.empty()) {
        return -1;
    }

    uint32_t count = mz_zip_reader_get_num_files(&zip);
    for (uint32_t i = 0; i < count; i++) {
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&zip, i, &st)) {
            continue;
        }
        const char* s  = strrchr(st.filename, '/');
        const char* bn = s ? (s + 1) : st.filename;
        if (strcasecmp(bn, base_name.c_str()) == 0) {
            return (int)i;
        }
    }
    return -1;
}

/** Reads the OPF: metadata plus the spine resolved to ZIP member names. */
bool read_opf(mz_zip_archive& zip, const std::string& opf_path, OpfInfo& info, bool want_spine)
{
    int idx = locate_member(zip, opf_path);
    if (idx < 0) {
        return false;
    }

    size_t sz  = 0;
    char* data = (char*)mz_zip_reader_extract_to_heap_max(&zip, (uint32_t)idx, MAX_OPF_BYTES, &sz);
    if (!data) {
        return false;
    }

    element_text(data, sz, "title", info.title);
    element_text(data, sz, "creator", info.author);

    if (want_spine) {
        const std::string base = dir_of(opf_path);

        // manifest: id -> (href, is_document). Hash map + caps: a hostile OPF
        // can hold tens of thousands of items.
        std::unordered_map<std::string, std::pair<std::string, bool> > items;
        {
            const char* p   = data;
            const char* end = data + sz;
            while (p < end && items.size() < MAX_MANIFEST_ITEMS) {
                TagInfo t;
                if (!find_tag(p, end, "item", &t)) {
                    break;
                }
                std::string id;
                std::string href;
                std::string mtype;
                if (get_attr(t.body_start, t.body_end, "id", id) &&
                    get_attr(t.body_start, t.body_end, "href", href)) {
                    get_attr(t.body_start, t.body_end, "media-type", mtype);
                    bool is_doc = mtype.empty() || mtype.find("html") != std::string::npos;
                    items.emplace(id, std::make_pair(href, is_doc));
                }
                p = t.next;
            }
        }

        {
            std::unordered_set<std::string> seen;  // a chapter listed twice is read once
            const char* p   = data;
            const char* end = data + sz;
            while (p < end && info.spine_paths.size() < MAX_SPINE_ITEMS) {
                TagInfo t;
                if (!find_tag(p, end, "itemref", &t)) {
                    break;
                }
                p = t.next;
                std::string idref;
                if (!get_attr(t.body_start, t.body_end, "idref", idref)) {
                    continue;
                }
                auto it = items.find(idref);
                if (it == items.end() || !it->second.second) {
                    continue;
                }
                std::string href = percent_decode(it->second.first);
                size_t hash      = href.find('#');
                if (hash != std::string::npos) {
                    href = href.substr(0, hash);
                }
                std::string full = path_join(base, href);
                if (seen.insert(full).second) {
                    info.spine_paths.push_back(full);
                }
            }
        }
    }

    mz_free(data);
    return true;
}

/** Finds the cover image member named by an OPF document. Empty if none. */
std::string find_cover_member(mz_zip_archive& zip, const std::string& opf_path, const char* data, size_t sz)
{
    const char* end        = data + sz;
    const std::string base = dir_of(opf_path);

    // EPUB 2: <meta name="cover" content="item-id"/>
    std::string cover_id;
    for (const char* p = data; p < end;) {
        TagInfo t;
        if (!find_tag(p, end, "meta", &t)) {
            break;
        }
        std::string name;
        if (get_attr(t.body_start, t.body_end, "name", name) && strcasecmp(name.c_str(), "cover") == 0) {
            get_attr(t.body_start, t.body_end, "content", cover_id);
            break;
        }
        p = t.next;
    }

    struct Item {
        std::string id, href, type, props;
    };
    std::vector<Item> items;
    for (const char* p = data; p < end && items.size() < MAX_MANIFEST_ITEMS;) {
        TagInfo t;
        if (!find_tag(p, end, "item", &t)) {
            break;
        }
        Item it;
        get_attr(t.body_start, t.body_end, "id", it.id);
        get_attr(t.body_start, t.body_end, "href", it.href);
        get_attr(t.body_start, t.body_end, "media-type", it.type);
        get_attr(t.body_start, t.body_end, "properties", it.props);
        if (!it.href.empty()) {
            items.push_back(it);
        }
        p = t.next;
    }

    auto is_image = [](const Item& it) { return it.type.compare(0, 6, "image/") == 0; };
    auto lower    = [](std::string v) {
        for (char& c : v) {
            c = (char)tolower((unsigned char)c);
        }
        return v;
    };

    const Item* pick = nullptr;
    // 1. EPUB 3 properties="cover-image"
    for (const Item& it : items) {
        if (it.props.find("cover-image") != std::string::npos) {
            pick = &it;
            break;
        }
    }
    // 2. EPUB 2 meta cover id
    if (!pick && !cover_id.empty()) {
        for (const Item& it : items) {
            if (it.id == cover_id) {
                pick = &it;
                break;
            }
        }
    }
    // 3. An image whose id or file name says "cover".
    if (!pick) {
        for (const Item& it : items) {
            if (is_image(it) && (lower(it.id).find("cover") != std::string::npos ||
                                 lower(it.href).find("cover") != std::string::npos)) {
                pick = &it;
                break;
            }
        }
    }
    if (!pick) {
        return std::string();
    }

    std::string href = percent_decode(pick->href);
    if (is_image(*pick)) {
        return path_join(base, href);
    }

    // The "cover" is an XHTML page: use the first image it shows.
    std::string page = path_join(base, href);
    int idx          = locate_member(zip, page);
    if (idx < 0) {
        return std::string();
    }
    size_t psz  = 0;
    char* pdata = (char*)mz_zip_reader_extract_to_heap_max(&zip, (uint32_t)idx, MAX_XHTML_BYTES, &psz);
    if (!pdata) {
        return std::string();
    }
    std::string src;
    TagInfo t;
    if (find_tag(pdata, pdata + psz, "img", &t)) {
        get_attr(t.body_start, t.body_end, "src", src);
    } else if (find_tag(pdata, pdata + psz, "image", &t)) {
        if (!get_attr(t.body_start, t.body_end, "xlink:href", src)) {
            get_attr(t.body_start, t.body_end, "href", src);
        }
    }
    mz_free(pdata);
    if (src.empty()) {
        return std::string();
    }
    return path_join(dir_of(page), percent_decode(src));
}

}  // namespace

/* ================================================================== */
/* Public API                                                          */
/* ================================================================== */

bool epub_read_meta(const char* epub_path, EpubMeta& meta)
{
    meta.title.clear();
    meta.author.clear();
    meta.paragraphs = 0;

    if (!epub_path) {
        return false;
    }

    mz_zip_archive zip;
    if (!mz_zip_reader_init_file(&zip, epub_path)) {
        ESP_LOGW(TAG, "not a readable zip: %s", epub_path);
        return false;
    }

    std::string opf;
    OpfInfo info;
    bool ok = find_opf_path(zip, opf) && read_opf(zip, opf, info, false);

    mz_zip_reader_end(&zip);

    if (!ok) {
        return false;
    }

    meta.title  = layout::text_normalize(info.title);
    meta.author = layout::text_normalize(info.author);
    return true;
}

bool epub_normalize(const char* epub_path, const char* out_path, EpubMeta& meta, std::vector<TocEntry>& toc,
                    epub_progress_cb cb, void* ctx)
{
    toc.clear();
    meta.title.clear();
    meta.author.clear();
    meta.paragraphs = 0;

    if (!epub_path || !out_path) {
        return false;
    }

    mz_zip_archive zip;
    if (!mz_zip_reader_init_file(&zip, epub_path)) {
        ESP_LOGW(TAG, "not a readable zip: %s", epub_path);
        return false;
    }

    std::string opf;
    OpfInfo info;
    if (!find_opf_path(zip, opf) || !read_opf(zip, opf, info, true)) {
        ESP_LOGW(TAG, "no usable OPF in %s", epub_path);
        mz_zip_reader_end(&zip);
        return false;
    }

    if (info.spine_paths.empty()) {
        ESP_LOGW(TAG, "empty spine in %s", epub_path);
        mz_zip_reader_end(&zip);
        return false;
    }

    FILE* out = fopen(out_path, "wb");
    if (!out) {
        ESP_LOGE(TAG, "cannot create %s", out_path);
        mz_zip_reader_end(&zip);
        return false;
    }

    ParagraphWriter pw(out, &toc);
    const size_t total = info.spine_paths.size();

    for (size_t i = 0; i < total; i++) {
        // Yield first: the "missing item" path below skips the end of the loop.
        vTaskDelay(1);
        if (cb) {
            cb((int)((i * 100) / total), "Extracting", ctx);
        }
        if (pw.written() > MAX_STREAM_BYTES || pw.ioError()) {
            ESP_LOGW(TAG, "stopping: %s", pw.ioError() ? "write error" : "output too large");
            break;
        }

        int idx = locate_member(zip, info.spine_paths[i]);
        if (idx < 0) {
            ESP_LOGW(TAG, "spine item missing: %s", info.spine_paths[i].c_str());
            continue;
        }

        size_t sz  = 0;
        char* data = (char*)mz_zip_reader_extract_to_heap_max(&zip, (uint32_t)idx, MAX_XHTML_BYTES, &sz);
        if (!data) {
            ESP_LOGW(TAG, "cannot inflate (or too large): %s", info.spine_paths[i].c_str());
            continue;
        }

        html_to_paragraphs(data, sz, pw);
        mz_free(data);
    }

    pw.commit();
    bool io_ok = !pw.ioError() && fflush(out) == 0;
    io_ok      = (fclose(out) == 0) && io_ok;
    mz_zip_reader_end(&zip);

    if (!io_ok) {
        // A truncated stream must never be cached as the whole book.
        ESP_LOGE(TAG, "write failed for %s (card full?)", out_path);
        remove(out_path);
        return false;
    }

    meta.title      = layout::text_normalize(info.title);
    meta.author     = layout::text_normalize(info.author);
    meta.paragraphs = pw.count();

    if (pw.count() == 0) {
        ESP_LOGW(TAG, "no text extracted from %s", epub_path);
        remove(out_path);
        return false;
    }

    if (cb) {
        cb(100, "Extracting", ctx);
    }
    ESP_LOGI(TAG, "normalised %s -> %s (%u paragraphs)", epub_path, out_path, (unsigned)pw.count());
    return true;
}

/* ------------------------------------------------------------------ */
/* NormParaSource                                                      */
/* ------------------------------------------------------------------ */

NormParaSource::~NormParaSource()
{
    close();
}

void NormParaSource::close()
{
    if (_f) {
        fclose(_f);
        _f = nullptr;
    }
    _size = 0;
}

bool NormParaSource::open(const char* path)
{
    close();
    if (!path || !path[0]) {
        return false;
    }

    _f = fopen(path, "rb");
    if (!_f) {
        return false;
    }

    fseek(_f, 0, SEEK_END);
    long end = ftell(_f);
    _size    = end > 0 ? (uint32_t)end : 0;
    fseek(_f, 0, SEEK_SET);
    return true;
}

uint32_t NormParaSource::size()
{
    return _size;
}

bool NormParaSource::seek(uint32_t off)
{
    if (!_f || off > _size) {
        return false;
    }
    return fseek(_f, (long)off, SEEK_SET) == 0;
}

uint32_t NormParaSource::tell()
{
    if (!_f) {
        return 0;
    }
    long p = ftell(_f);
    return p > 0 ? (uint32_t)p : 0;
}

bool NormParaSource::next(layout::Para& out)
{
    out.style = layout::PARA_BODY;
    out.text.clear();

    if (!_f) {
        return false;
    }

    std::string line;
    int c;
    bool got_any = false;
    while ((c = fgetc(_f)) != EOF) {
        got_any = true;
        if (c == '\n') {
            break;
        }
        if (line.size() < 32768) {
            line.push_back((char)c);
        }
    }
    if (!got_any) {
        return false;
    }

    size_t tab = line.find('\t');
    if (tab == std::string::npos) {
        out.text = line;
        return true;
    }

    out.style = line[0];
    if (!layout::isHeading(out.style) && out.style != layout::PARA_CAPTION && out.style != layout::PARA_CODE) {
        out.style = layout::PARA_BODY;
    }
    out.text = line.substr(tab + 1);
    return true;
}

/* ================================================================== */
/* Cover image                                                         */
/* ================================================================== */

void* epub_cover_data(const char* epub_path, size_t* out_size)
{
    if (out_size) {
        *out_size = 0;
    }
    if (!epub_path) {
        return nullptr;
    }
    mz_zip_archive zip;
    if (!mz_zip_reader_init_file(&zip, epub_path)) {
        return nullptr;
    }

    void* result = nullptr;
    std::string opf;
    if (find_opf_path(zip, opf)) {
        int idx = locate_member(zip, opf);
        size_t sz  = 0;
        char* data = (idx >= 0) ? (char*)mz_zip_reader_extract_to_heap_max(&zip, (uint32_t)idx, MAX_OPF_BYTES, &sz)
                                : nullptr;
        if (data) {
            std::string member = find_cover_member(zip, opf, data, sz);
            mz_free(data);
            if (!member.empty()) {
                int cidx = locate_member(zip, member);
                if (cidx >= 0) {
                    mz_zip_archive_file_stat st;
                    // Refuse absurd images (> 6 MB); they would not decode anyway.
                    if (mz_zip_reader_file_stat(&zip, (uint32_t)cidx, &st) && st.uncomp_size < 6u * 1024u * 1024u) {
                        result = mz_zip_reader_extract_to_heap_max(&zip, (uint32_t)cidx, MAX_COVER_BYTES, out_size);
                    }
                }
            }
        }
    }
    mz_zip_reader_end(&zip);
    return result;
}
