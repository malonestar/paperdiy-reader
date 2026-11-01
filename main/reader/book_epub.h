/*
 * paperdiy-reader — EPUB support.
 *
 * An EPUB is a ZIP holding XHTML chapters. Opening a book for the first time
 * converts it once into a *normalised paragraph stream* on storage:
 *
 *     <style>\t<text>\n
 *
 * where <style> is 'p', '1', '2' or '3' and <text> is normalised UTF-8 with
 * per-word emphasis markers. Every later page turn just seeks in that file, so
 * the ZIP is never touched again while reading.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <cstdio>
#include <string>
#include <vector>

#include "reader/layout.h"

/** @brief Metadata lifted from the OPF package document. */
struct EpubMeta {
    std::string title;
    std::string author;
    uint32_t paragraphs = 0;
};

/** @brief One table-of-contents entry: a heading in the normalised stream. */
struct TocEntry {
    uint32_t off  = 0; /*!< byte offset of the heading paragraph */
    uint8_t level = 1; /*!< 1 = chapter (h1), 2 = section (h2) */
    std::string title;
};

/** @brief Progress reporting during normalisation. */
typedef void (*epub_progress_cb)(int percent, const char* what, void* ctx);

/**
 * @brief Reads only dc:title / dc:creator from an EPUB.
 *
 * Extracts META-INF/container.xml and the OPF package document and nothing
 * else, which keeps the library scan fast.
 *
 * @return false when the file is not a usable EPUB.
 */
bool epub_read_meta(const char* epub_path, EpubMeta& meta);

/**
 * @brief Converts an EPUB into a normalised paragraph stream.
 *
 * @param epub_path  source .epub
 * @param out_path   destination stream file (overwritten)
 * @param meta       receives title/author/paragraph count
 * @param toc        receives h1/h2 headings with their stream offsets
 * @param cb         optional progress callback
 * @param ctx        opaque pointer handed back to @p cb
 * @return false on any failure; @p out_path is removed in that case.
 */
bool epub_normalize(const char* epub_path, const char* out_path, EpubMeta& meta, std::vector<TocEntry>& toc,
                    epub_progress_cb cb, void* ctx);

/**
 * @brief Extracts the cover image (JPEG/PNG bytes) of an EPUB.
 *
 * Tries the EPUB 3 cover-image property, the EPUB 2 <meta name="cover">, an
 * image named like a cover, and the first image of an XHTML cover page.
 *
 * @return heap buffer to release with mz_free(), or nullptr when there is none.
 */
void* epub_cover_data(const char* epub_path, size_t* out_size);

/**
 * @brief Paragraph stream over a normalised ".txn" file.
 */
class NormParaSource : public layout::ParaSource {
public:
    NormParaSource() = default;
    ~NormParaSource() override;

    NormParaSource(const NormParaSource&)            = delete;
    NormParaSource& operator=(const NormParaSource&) = delete;

    /** @brief Opens a normalised stream file. */
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

private:
    FILE* _f       = nullptr;
    uint32_t _size = 0;
};
