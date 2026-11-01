/*
 * paperdiy-reader — plain-text book support.
 *
 * TXT files are paginated straight from the source file: no normalised copy is
 * written, so a 2 MB novel costs 2 MB of storage rather than 4 MB.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <cstdio>
#include <string>

#include "reader/layout.h"

/** @brief How the source file lays out its paragraphs. */
enum TxtWrapStyle : uint8_t {
    TXT_WRAPPED_BLANKS = 0,  /*!< hard-wrapped, blank line between paragraphs */
    TXT_WRAPPED_NOBLANKS,    /*!< hard-wrapped, no blank lines (short line ends a paragraph) */
    TXT_UNWRAPPED,           /*!< one long line per paragraph */
};

/**
 * @brief Paragraph stream over a UTF-8 (or ASCII / Latin-1) text file.
 *
 * CRLF, LF and a leading UTF-8 BOM are all handled. Text is folded to ASCII on
 * the way out, matching what the bundled GFX fonts can draw.
 */
class TxtParaSource : public layout::ParaSource {
public:
    TxtParaSource() = default;
    ~TxtParaSource() override;

    TxtParaSource(const TxtParaSource&)            = delete;
    TxtParaSource& operator=(const TxtParaSource&) = delete;

    /** @brief Opens @p path and samples it to pick a wrapping strategy. */
    bool open(const char* path);

    /** @brief Closes the underlying file. */
    void close();

    /** @brief True while a file is open. */
    bool isOpen() const
    {
        return _f != nullptr;
    }

    bool seek(uint32_t off) override;
    uint32_t tell() override;
    bool next(layout::Para& out) override;
    uint32_t size() override;

    /** @brief Detected paragraph wrapping style. */
    TxtWrapStyle wrapStyle() const
    {
        return _style;
    }

private:
    bool readLine(std::string& line);
    void detectWrapStyle();

    FILE* _f              = nullptr;
    uint32_t _size        = 0;
    uint32_t _body_start  = 0;  /*!< first byte after any BOM */
    TxtWrapStyle _style   = TXT_WRAPPED_BLANKS;
};

/** @brief Derives a readable book title from a .txt file path. */
std::string txt_title_from_filename(const char* path);

/** @brief Returns @p raw unchanged when it is valid UTF-8, else decodes it as Windows-1252. */
std::string txt_to_utf8(const std::string& raw);
