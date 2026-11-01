/*
 * paperdiy-reader — plain-text book support.
 *
 * SPDX-License-Identifier: MIT
 */
#include "reader/book_txt.h"

#include <cstring>

#include "esp_log.h"

static const char* TAG = "book_txt";

/** Lines at or below this length are treated as the end of a paragraph in
 *  hard-wrapped files that do not separate paragraphs with blank lines. */
static constexpr size_t SHORT_LINE_HINT = 45;

/** Guard against a pathological single-line file eating all of the heap. */
static constexpr size_t MAX_LINE_BYTES = 16384;
static constexpr size_t MAX_PARA_BYTES = 24576;

TxtParaSource::~TxtParaSource()
{
    close();
}

void TxtParaSource::close()
{
    if (_f) {
        fclose(_f);
        _f = nullptr;
    }
    _size       = 0;
    _body_start = 0;
}

bool TxtParaSource::open(const char* path)
{
    close();
    if (!path || !path[0]) {
        return false;
    }

    _f = fopen(path, "rb");
    if (!_f) {
        ESP_LOGW(TAG, "cannot open %s", path);
        return false;
    }

    fseek(_f, 0, SEEK_END);
    long end = ftell(_f);
    _size    = end > 0 ? (uint32_t)end : 0;
    fseek(_f, 0, SEEK_SET);

    // UTF-8 BOM
    unsigned char bom[3] = {0, 0, 0};
    if (fread(bom, 1, 3, _f) == 3 && bom[0] == 0xEF && bom[1] == 0xBB && bom[2] == 0xBF) {
        _body_start = 3;
    } else {
        _body_start = 0;
    }

    detectWrapStyle();
    fseek(_f, (long)_body_start, SEEK_SET);
    return true;
}

uint32_t TxtParaSource::size()
{
    return _size;
}

bool TxtParaSource::seek(uint32_t off)
{
    if (!_f) {
        return false;
    }
    if (off < _body_start) {
        off = _body_start;
    }
    if (off > _size) {
        return false;
    }
    return fseek(_f, (long)off, SEEK_SET) == 0;
}

uint32_t TxtParaSource::tell()
{
    if (!_f) {
        return 0;
    }
    long p = ftell(_f);
    return p > 0 ? (uint32_t)p : 0;
}

bool TxtParaSource::readLine(std::string& line)
{
    line.clear();
    if (!_f) {
        return false;
    }

    int c;
    bool got_any = false;
    while ((c = fgetc(_f)) != EOF) {
        got_any = true;
        if (c == '\n') {
            break;
        }
        if (c == '\r') {
            // Peek: swallow the LF of a CRLF pair.
            int nxt = fgetc(_f);
            if (nxt != '\n' && nxt != EOF) {
                ungetc(nxt, _f);
            }
            break;
        }
        line.push_back((char)c);
        if (line.size() >= MAX_LINE_BYTES) {
            // An endless line (a whole book with no newlines): stop here and
            // let the next call continue from this point, backing off to a
            // UTF-8 lead byte so no character is split between two reads.
            size_t keep = line.size();
            while (keep > 1 && ((unsigned char)line[keep - 1] & 0xC0) == 0x80) {
                keep--;
            }
            if (keep > 1 && (unsigned char)line[keep - 1] >= 0xC0) {
                keep--;  // the lead byte of the split character goes back too
            }
            if (keep < line.size()) {
                fseek(_f, -(long)(line.size() - keep), SEEK_CUR);
                line.resize(keep);
            }
            break;
        }
    }

    return got_any;
}

void TxtParaSource::detectWrapStyle()
{
    if (!_f) {
        return;
    }

    long saved = ftell(_f);
    fseek(_f, (long)_body_start, SEEK_SET);

    size_t total_lines = 0;
    size_t blank_lines = 0;
    size_t long_lines  = 0;
    size_t nonblank    = 0;
    size_t scanned     = 0;

    std::string line;
    while (scanned < 64 * 1024 && total_lines < 2000 && readLine(line)) {
        scanned += line.size() + 1;
        total_lines++;

        // A line of only whitespace counts as blank.
        bool blank = true;
        for (char ch : line) {
            if (ch != ' ' && ch != '\t') {
                blank = false;
                break;
            }
        }
        if (blank) {
            blank_lines++;
        } else {
            nonblank++;
            if (line.size() > 100) {
                long_lines++;
            }
        }
    }

    if (nonblank > 0 && long_lines * 100 >= nonblank * 30) {
        _style = TXT_UNWRAPPED;
    } else if (total_lines > 0 && blank_lines * 100 < total_lines * 2) {
        _style = TXT_WRAPPED_NOBLANKS;
    } else {
        _style = TXT_WRAPPED_BLANKS;
    }

    ESP_LOGI(TAG, "wrap style %d (lines=%u blank=%u long=%u)", (int)_style, (unsigned)total_lines,
             (unsigned)blank_lines, (unsigned)long_lines);

    fseek(_f, saved, SEEK_SET);
}

bool TxtParaSource::next(layout::Para& out)
{
    out.style = layout::PARA_BODY;
    out.text.clear();

    if (!_f) {
        return false;
    }

    std::string raw;
    std::string line;

    for (;;) {
        long line_start = ftell(_f);
        if (!readLine(line)) {
            break;  // EOF
        }

        // Trim trailing whitespace so the "short line" test is meaningful.
        size_t endp = line.size();
        while (endp > 0 && (line[endp - 1] == ' ' || line[endp - 1] == '\t')) {
            endp--;
        }
        line.resize(endp);

        bool blank = line.empty();

        if (blank) {
            if (raw.empty()) {
                continue;  // leading blank lines before a paragraph
            }
            break;  // blank line terminates the paragraph
        }

        if (_style == TXT_UNWRAPPED) {
            raw = line;
            break;  // one line == one paragraph
        }

        if (!raw.empty()) {
            raw.push_back(' ');
        }
        raw += line;

        if (_style == TXT_WRAPPED_NOBLANKS && line.size() < SHORT_LINE_HINT) {
            break;  // last line of a hard-wrapped paragraph
        }

        if (raw.size() >= MAX_PARA_BYTES) {
            // Absurdly long paragraph; cut it here and resume next call.
            (void)line_start;
            break;
        }
    }

    if (raw.empty()) {
        return false;
    }

    out.text = layout::text_normalize(txt_to_utf8(raw));
    if (out.text.empty()) {
        // The paragraph folded away entirely (for example a run of CJK).
        out.text = " ";
    }
    return true;
}

/* ------------------------------------------------------------------ */

static bool valid_utf8(const std::string& s)
{
    size_t i = 0;
    const size_t n = s.size();
    while (i < n) {
        unsigned char c = (unsigned char)s[i];
        int extra       = 0;
        if (c < 0x80) {
            i++;
            continue;
        } else if ((c & 0xE0) == 0xC0 && c >= 0xC2) {
            extra = 1;
        } else if ((c & 0xF0) == 0xE0) {
            extra = 2;
        } else if ((c & 0xF8) == 0xF0 && c <= 0xF4) {
            extra = 3;
        } else {
            return false;
        }
        if (i + (size_t)extra >= n) {
            return false;
        }
        for (int k = 1; k <= extra; k++) {
            if (((unsigned char)s[i + k] & 0xC0) != 0x80) {
                return false;
            }
        }
        i += (size_t)extra + 1;
    }
    return true;
}

std::string txt_to_utf8(const std::string& raw_in)
{
    std::string raw;
    raw.reserve(raw_in.size());
    for (char c : raw_in) {
        if (c < 1 || c > 3) {
            raw.push_back(c);  // in-band face markers are ours, never the book's
        }
    }
    if (valid_utf8(raw)) {
        return raw;
    }
    // Not UTF-8: old Project Gutenberg files are often Windows-1252.
    static const uint16_t kCp1252[32] = {0x20AC, 0,      0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
                                         0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0,      0x017D, 0,
                                         0,      0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
                                         0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0,      0x017E, 0x0178};
    std::string out;
    out.reserve(raw.size() + raw.size() / 8);
    for (unsigned char c : raw) {
        uint32_t cp = c;
        if (c >= 0x80 && c < 0xA0) {
            cp = kCp1252[c - 0x80];
            if (cp == 0) {
                continue;
            }
        }
        if (cp < 0x80) {
            out.push_back((char)cp);
        } else if (cp < 0x800) {
            out.push_back((char)(0xC0 | (cp >> 6)));
            out.push_back((char)(0x80 | (cp & 0x3F)));
        } else {
            out.push_back((char)(0xE0 | (cp >> 12)));
            out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back((char)(0x80 | (cp & 0x3F)));
        }
    }
    return out;
}

std::string txt_title_from_filename(const char* path)
{
    if (!path) {
        return std::string("Untitled");
    }

    const char* slash = strrchr(path, '/');
    std::string name  = slash ? (slash + 1) : path;

    size_t dot = name.find_last_of('.');
    if (dot != std::string::npos && dot > 0) {
        name = name.substr(0, dot);
    }

    for (char& c : name) {
        if (c == '_') {
            c = ' ';
        }
    }

    // Collapse runs of spaces.
    std::string out;
    bool prev_space = false;
    for (char c : name) {
        if (c == ' ') {
            if (prev_space) {
                continue;
            }
            prev_space = true;
        } else {
            prev_space = false;
        }
        out.push_back(c);
    }

    if (out.empty()) {
        out = "Untitled";
    }
    // "atomic habits" -> "Atomic Habits" when the name has no capitals at all.
    bool any_upper = false;
    for (char c : out) {
        any_upper |= (c >= 'A' && c <= 'Z');
    }
    if (!any_upper) {
        bool start = true;
        for (char& c : out) {
            if (start && c >= 'a' && c <= 'z') {
                c = (char)(c - 'a' + 'A');
            }
            start = (c == ' ' || c == '-' || c == '(');
        }
    }
    return layout::text_normalize(txt_to_utf8(out));
}
