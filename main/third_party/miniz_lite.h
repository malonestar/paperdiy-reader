/*
 * miniz_lite — a compact, self-contained ZIP reader + DEFLATE decompressor.
 *
 * WHY THIS EXISTS
 * ---------------
 * The EPUB reader only ever needs to do one thing with a ZIP container: locate
 * a member by name and inflate it into memory.  Upstream miniz (miniz.c) does
 * that plus compression, ZIP writing, PNG writing and a full zlib shim — around
 * 10k lines that we would have to vendor byte-for-byte.  The build host for
 * this project had no outbound access to github.com when the firmware was
 * written, so instead of shipping a stub this file implements the small subset
 * we need from scratch:
 *
 *   - RFC 1951 raw DEFLATE inflate (canonical Huffman, stored/fixed/dynamic
 *     blocks), structured after Mark Adler's public-domain "puff" reference
 *     decoder.
 *   - ZIP end-of-central-directory + central-directory parsing.
 *   - Per-member extraction (stored / deflated) straight to a heap buffer,
 *     preferring PSRAM.
 *
 * The public entry points deliberately mirror the miniz names/semantics
 * (mz_zip_reader_init_file / mz_zip_reader_locate_file /
 * mz_zip_reader_extract_to_heap / mz_zip_reader_end / mz_free) so that dropping
 * the real miniz.c + miniz.h into this directory and switching the two
 * #includes is a mechanical change if full miniz is ever wanted.
 *
 * Limitations (documented in README.md as EPUB limitations):
 *   - No ZIP64 (archives > 4 GiB, or > 65535 members).
 *   - No encryption, no multi-disk spanning.
 *   - Only compression methods 0 (stored) and 8 (deflate).
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* Raw DEFLATE                                                         */
/* ------------------------------------------------------------------ */

/** @brief Inflate a raw DEFLATE stream (no zlib/gzip wrapper).
 *
 * @param src      compressed bytes
 * @param src_len  number of compressed bytes available
 * @param dst      destination buffer
 * @param dst_len  capacity of @p dst (also the expected output size)
 * @param written  optional, receives the number of bytes produced
 * @return 0 on success, negative on a malformed stream, 1 if @p dst is too
 *         small, 2 if the input ended early.
 */
int mzl_inflate_raw(const void *src, size_t src_len, void *dst, size_t dst_len, size_t *written);

/* ------------------------------------------------------------------ */
/* ZIP reader (miniz-compatible surface)                               */
/* ------------------------------------------------------------------ */

typedef struct {
    uint32_t index;             /**< member index in the central directory */
    uint16_t method;            /**< 0 = stored, 8 = deflate */
    uint32_t crc32;             /**< stored CRC-32 (not verified) */
    uint32_t comp_size;         /**< compressed size in bytes */
    uint32_t uncomp_size;       /**< uncompressed size in bytes */
    uint32_t local_header_ofs;  /**< offset of the local file header */
    char filename[256];         /**< NUL-terminated member name */
} mz_zip_archive_file_stat;

typedef struct mz_zip_entry_s mz_zip_entry_t;

typedef struct {
    void *fp;                 /**< FILE* of the open archive */
    uint32_t num_files;
    mz_zip_entry_t *entries;  /**< num_files entries */
    uint32_t file_size;       /**< archive size, for bounds checks */
} mz_zip_archive;

/** @brief Default ceiling for one extracted member (zip-bomb guard). */
#define MZL_DEFAULT_MAX_EXTRACT (8u * 1024u * 1024u)

/** @brief Open a ZIP file and read its central directory.
 *  @return non-zero on success (miniz convention). */
int mz_zip_reader_init_file(mz_zip_archive *zip, const char *path);

/** @brief Release everything owned by @p zip. Safe to call twice. */
void mz_zip_reader_end(mz_zip_archive *zip);

/** @brief Number of members in the archive. */
uint32_t mz_zip_reader_get_num_files(const mz_zip_archive *zip);

/** @brief Fill @p st for member @p index. Returns non-zero on success. */
int mz_zip_reader_file_stat(const mz_zip_archive *zip, uint32_t index, mz_zip_archive_file_stat *st);

/** @brief Case-insensitive exact-name lookup. Returns the index or -1. */
int mz_zip_reader_locate_file(const mz_zip_archive *zip, const char *name);

/**
 * @brief Extract member @p index to a freshly allocated buffer.
 *
 * The buffer is allocated with one extra byte which is set to 0, so the result
 * can be used directly as a C string for XML/XHTML members.
 *
 * @param out_size receives the uncompressed size (excluding the NUL)
 * @return heap pointer to free with mz_free(), or NULL on failure.
 */
void *mz_zip_reader_extract_to_heap(mz_zip_archive *zip, uint32_t index, size_t *out_size);

/** @brief As above, refusing members whose stored or uncompressed size
 *  exceeds @p max_bytes (returns NULL without allocating). */
void *mz_zip_reader_extract_to_heap_max(mz_zip_archive *zip, uint32_t index, size_t max_bytes, size_t *out_size);

/** @brief Convenience: locate by name then extract. */
void *mz_zip_reader_extract_file_to_heap(mz_zip_archive *zip, const char *name, size_t *out_size);

/** @brief Free a buffer returned by the extract helpers. */
void mz_free(void *p);

#ifdef __cplusplus
}
#endif
