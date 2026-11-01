/*
 * miniz_lite — compact ZIP reader + DEFLATE inflate.  See miniz_lite.h.
 *
 * The inflate core follows the structure of Mark Adler's public-domain "puff"
 * reference decoder (zlib/contrib/puff), rewritten to report errors by return
 * value instead of setjmp/longjmp and to keep every scratch table inside one
 * heap-allocated state block (the ESP32-S3 task stacks here are modest).
 *
 * SPDX-License-Identifier: MIT
 */

#include "miniz_lite.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#if defined(ESP_PLATFORM)
#include "esp_heap_caps.h"
#endif

/* ================================================================== */
/* Allocation helpers — large book buffers belong in PSRAM.            */
/* ================================================================== */

static void *mzl_big_alloc(size_t n)
{
#if defined(ESP_PLATFORM)
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (p) {
        return p;
    }
#endif
    return malloc(n);
}

void mz_free(void *p)
{
    free(p);
}

/* ================================================================== */
/* DEFLATE (RFC 1951)                                                  */
/* ================================================================== */

#define MZL_MAXBITS   15  /* maximum bits in a Huffman code            */
#define MZL_MAXLCODES 286 /* maximum number of literal/length codes    */
#define MZL_MAXDCODES 30  /* maximum number of distance codes          */
#define MZL_FIXLCODES 288 /* number of fixed literal/length codes      */
#define MZL_MAXCODES  (MZL_MAXLCODES + MZL_MAXDCODES)

struct mzl_huffman {
    short *count;  /* number of symbols of each length */
    short *symbol; /* canonically ordered symbols      */
};

struct mzl_state {
    /* output */
    unsigned char *out;
    size_t outlen;
    size_t outcnt;

    /* input */
    const unsigned char *in;
    size_t inlen;
    size_t incnt;
    int bitbuf;
    int bitcnt;
    int err; /* set when the bit reader runs off the end of the input */

    /* scratch tables (kept off the stack) */
    short lencnt[MZL_MAXBITS + 1];
    short lensym[MZL_FIXLCODES];
    short distcnt[MZL_MAXBITS + 1];
    short distsym[MZL_MAXDCODES];
    short lengths[MZL_MAXCODES];
};

/* Return @p need bits from the stream, LSB first. */
static int mzl_bits(struct mzl_state *s, int need)
{
    long val = s->bitbuf;

    while (s->bitcnt < need) {
        if (s->incnt == s->inlen) {
            s->err = 1;
            return 0;
        }
        val |= (long)(s->in[s->incnt++]) << s->bitcnt;
        s->bitcnt += 8;
    }

    s->bitbuf = (int)(val >> need);
    s->bitcnt -= need;
    return (int)(val & ((1L << need) - 1));
}

/* Stored (uncompressed) block. */
static int mzl_stored(struct mzl_state *s)
{
    unsigned len;

    s->bitbuf = 0;
    s->bitcnt = 0;

    if (s->incnt + 4 > s->inlen) {
        return 2;
    }
    len = s->in[s->incnt++];
    len |= (unsigned)s->in[s->incnt++] << 8;
    if (s->in[s->incnt++] != ((~len) & 0xFF) || s->in[s->incnt++] != (((~len) >> 8) & 0xFF)) {
        return -2;
    }

    if (s->incnt + len > s->inlen) {
        return 2;
    }
    if (s->outcnt + len > s->outlen) {
        return 1;
    }

    memcpy(s->out + s->outcnt, s->in + s->incnt, len);
    s->outcnt += len;
    s->incnt += len;
    return 0;
}

/* Decode one symbol using canonical Huffman table @p h. Negative = error. */
static int mzl_decode(struct mzl_state *s, const struct mzl_huffman *h)
{
    int len;
    int code = 0;
    int first = 0;
    int index = 0;

    for (len = 1; len <= MZL_MAXBITS; len++) {
        code |= mzl_bits(s, 1);
        if (s->err) {
            return -10;
        }
        {
            int count = h->count[len];
            if (code - count < first) {
                return h->symbol[index + (code - first)];
            }
            index += count;
            first += count;
            first <<= 1;
            code <<= 1;
        }
    }
    return -10;
}

/* Build a canonical Huffman table from a code-length vector. */
static int mzl_construct(struct mzl_huffman *h, const short *length, int n)
{
    int symbol;
    int len;
    int left;
    short offs[MZL_MAXBITS + 1];

    for (len = 0; len <= MZL_MAXBITS; len++) {
        h->count[len] = 0;
    }
    for (symbol = 0; symbol < n; symbol++) {
        h->count[length[symbol]]++;
    }
    if (h->count[0] == n) {
        return 0; /* complete but empty code */
    }

    left = 1;
    for (len = 1; len <= MZL_MAXBITS; len++) {
        left <<= 1;
        left -= h->count[len];
        if (left < 0) {
            return left; /* over-subscribed */
        }
    }

    offs[1] = 0;
    for (len = 1; len < MZL_MAXBITS; len++) {
        offs[len + 1] = offs[len] + h->count[len];
    }
    for (symbol = 0; symbol < n; symbol++) {
        if (length[symbol] != 0) {
            h->symbol[offs[length[symbol]]++] = (short)symbol;
        }
    }

    return left; /* 0 == complete */
}

static const short mzl_lens[29]  = {3,  4,  5,  6,  7,  8,  9,  10,  11,  13,  15,  17,  19,  23, 27,
                                    31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
static const short mzl_lext[29]  = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                    2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
static const short mzl_dists[30] = {1,    2,    3,    4,    5,    7,     9,     13,    17,   25,
                                    33,   49,   65,   97,   129,  193,   257,   385,   513,  769,
                                    1025, 1537, 2049, 3073, 4097, 6145,  8193,  12289, 16385, 24577};
static const short mzl_dext[30]  = {0, 0, 0, 0, 1, 1, 2,  2,  3,  3,  4,  4,  5,  5,  6,
                                    6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

/* Decode the literal/length + distance stream of one compressed block. */
static int mzl_codes(struct mzl_state *s, const struct mzl_huffman *lencode, const struct mzl_huffman *distcode)
{
    int symbol;

    do {
        symbol = mzl_decode(s, lencode);
        if (symbol < 0) {
            return symbol;
        }

        if (symbol < 256) {
            if (s->outcnt == s->outlen) {
                return 1;
            }
            s->out[s->outcnt++] = (unsigned char)symbol;
        } else if (symbol > 256) {
            unsigned len;
            unsigned dist;

            symbol -= 257;
            if (symbol >= 29) {
                return -10;
            }
            len = (unsigned)mzl_lens[symbol] + (unsigned)mzl_bits(s, mzl_lext[symbol]);
            if (s->err) {
                return -10;
            }

            symbol = mzl_decode(s, distcode);
            if (symbol < 0) {
                return symbol;
            }
            if (symbol >= 30) {
                return -10;
            }
            dist = (unsigned)mzl_dists[symbol] + (unsigned)mzl_bits(s, mzl_dext[symbol]);
            if (s->err) {
                return -10;
            }
            if ((size_t)dist > s->outcnt) {
                return -11; /* distance points before the start of the output */
            }
            if (s->outcnt + len > s->outlen) {
                return 1;
            }
            while (len--) {
                s->out[s->outcnt] = s->out[s->outcnt - dist];
                s->outcnt++;
            }
        }
    } while (symbol != 256);

    return 0;
}

/* Fixed Huffman block. */
static int mzl_fixed(struct mzl_state *s)
{
    struct mzl_huffman lencode;
    struct mzl_huffman distcode;
    int symbol;

    for (symbol = 0; symbol < 144; symbol++) {
        s->lengths[symbol] = 8;
    }
    for (; symbol < 256; symbol++) {
        s->lengths[symbol] = 9;
    }
    for (; symbol < 280; symbol++) {
        s->lengths[symbol] = 7;
    }
    for (; symbol < MZL_FIXLCODES; symbol++) {
        s->lengths[symbol] = 8;
    }

    lencode.count  = s->lencnt;
    lencode.symbol = s->lensym;
    mzl_construct(&lencode, s->lengths, MZL_FIXLCODES);

    for (symbol = 0; symbol < MZL_MAXDCODES; symbol++) {
        s->lengths[symbol] = 5;
    }
    distcode.count  = s->distcnt;
    distcode.symbol = s->distsym;
    mzl_construct(&distcode, s->lengths, MZL_MAXDCODES);

    return mzl_codes(s, &lencode, &distcode);
}

/* Dynamic Huffman block. */
static int mzl_dynamic(struct mzl_state *s)
{
    static const short order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
    struct mzl_huffman lencode;
    struct mzl_huffman distcode;
    int nlen;
    int ndist;
    int ncode;
    int index;
    int err;

    lencode.count   = s->lencnt;
    lencode.symbol  = s->lensym;
    distcode.count  = s->distcnt;
    distcode.symbol = s->distsym;

    nlen  = mzl_bits(s, 5) + 257;
    ndist = mzl_bits(s, 5) + 1;
    ncode = mzl_bits(s, 4) + 4;
    if (s->err) {
        return -10;
    }
    if (nlen > MZL_MAXLCODES || ndist > MZL_MAXDCODES) {
        return -3;
    }

    for (index = 0; index < ncode; index++) {
        s->lengths[order[index]] = (short)mzl_bits(s, 3);
    }
    for (; index < 19; index++) {
        s->lengths[order[index]] = 0;
    }
    if (s->err) {
        return -10;
    }

    err = mzl_construct(&lencode, s->lengths, 19);
    if (err != 0) {
        return -4; /* the code-length code must be complete */
    }

    index = 0;
    while (index < nlen + ndist) {
        int symbol = mzl_decode(s, &lencode);
        if (symbol < 0) {
            return symbol;
        }

        if (symbol < 16) {
            s->lengths[index++] = (short)symbol;
        } else {
            short len = 0;
            if (symbol == 16) {
                if (index == 0) {
                    return -5;
                }
                len    = s->lengths[index - 1];
                symbol = 3 + mzl_bits(s, 2);
            } else if (symbol == 17) {
                symbol = 3 + mzl_bits(s, 3);
            } else {
                symbol = 11 + mzl_bits(s, 7);
            }
            if (s->err) {
                return -10;
            }
            if (index + symbol > nlen + ndist) {
                return -6;
            }
            while (symbol--) {
                s->lengths[index++] = len;
            }
        }
    }

    if (s->lengths[256] == 0) {
        return -9; /* no end-of-block code */
    }

    err = mzl_construct(&lencode, s->lengths, nlen);
    if (err && (err < 0 || nlen != lencode.count[0] + lencode.count[1])) {
        return -7;
    }
    err = mzl_construct(&distcode, s->lengths + nlen, ndist);
    if (err && (err < 0 || ndist != distcode.count[0] + distcode.count[1])) {
        return -8;
    }

    return mzl_codes(s, &lencode, &distcode);
}

int mzl_inflate_raw(const void *src, size_t src_len, void *dst, size_t dst_len, size_t *written)
{
    struct mzl_state *s;
    int err  = 0;
    int last = 0;

    if (written) {
        *written = 0;
    }
    if (!src || !dst) {
        return -20;
    }

    s = (struct mzl_state *)calloc(1, sizeof(struct mzl_state));
    if (!s) {
        return -21;
    }

    s->out    = (unsigned char *)dst;
    s->outlen = dst_len;
    s->outcnt = 0;
    s->in     = (const unsigned char *)src;
    s->inlen  = src_len;
    s->incnt  = 0;
    s->bitbuf = 0;
    s->bitcnt = 0;
    s->err    = 0;

    do {
        int type;

        last = mzl_bits(s, 1);
        type = mzl_bits(s, 2);
        if (s->err) {
            err = 2;
            break;
        }

        if (type == 0) {
            err = mzl_stored(s);
        } else if (type == 1) {
            err = mzl_fixed(s);
        } else if (type == 2) {
            err = mzl_dynamic(s);
        } else {
            err = -1;
        }

        if (err != 0) {
            break;
        }
    } while (!last);

    if (written) {
        *written = s->outcnt;
    }
    free(s);
    return err;
}

/* ================================================================== */
/* ZIP central directory                                               */
/* ================================================================== */

struct mz_zip_entry_s {
    uint16_t method;
    uint32_t crc32;
    uint32_t comp_size;
    uint32_t uncomp_size;
    uint32_t local_header_ofs;
    char *name; /* heap, NUL-terminated */
};

#define MZL_SIG_EOCD  0x06054B50u
#define MZL_SIG_CDH   0x02014B50u
#define MZL_SIG_LFH   0x04034B50u

static uint16_t mzl_rd16(const unsigned char *p)
{
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t mzl_rd32(const unsigned char *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static long mzl_file_size(FILE *f)
{
    long cur;
    long end;

    cur = ftell(f);
    if (cur < 0 || fseek(f, 0, SEEK_END) != 0) {
        return -1;
    }
    end = ftell(f);
    if (fseek(f, cur, SEEK_SET) != 0) {
        return -1;
    }
    return end;
}

/* Scan backwards from the end of the file for the end-of-central-directory
 * record. The ZIP comment may be up to 64 KiB, so bound the search there. */
static int mzl_find_eocd(FILE *f, long file_size, uint32_t *cd_size, uint32_t *cd_ofs, uint16_t *num_entries)
{
    const long max_back = 66000;
    long start;
    long span;
    unsigned char *buf;
    long i;
    int found = 0;

    if (file_size < 22) {
        return 0;
    }
    span = file_size < max_back ? file_size : max_back;
    start = file_size - span;

    buf = (unsigned char *)malloc((size_t)span);
    if (!buf) {
        return 0;
    }
    if (fseek(f, start, SEEK_SET) != 0 || fread(buf, 1, (size_t)span, f) != (size_t)span) {
        free(buf);
        return 0;
    }

    for (i = span - 22; i >= 0; i--) {
        if (mzl_rd32(buf + i) == MZL_SIG_EOCD) {
            *num_entries = mzl_rd16(buf + i + 10);
            *cd_size     = mzl_rd32(buf + i + 12);
            *cd_ofs      = mzl_rd32(buf + i + 16);
            found        = 1;
            break;
        }
    }

    free(buf);
    return found;
}

int mz_zip_reader_init_file(mz_zip_archive *zip, const char *path)
{
    FILE *f;
    long fsize;
    uint32_t cd_size = 0;
    uint32_t cd_ofs  = 0;
    uint16_t nent    = 0;
    unsigned char *cd = NULL;
    uint32_t pos = 0;
    uint32_t i;

    if (!zip || !path) {
        return 0;
    }
    memset(zip, 0, sizeof(*zip));

    f = fopen(path, "rb");
    if (!f) {
        return 0;
    }

    fsize = mzl_file_size(f);
    if (fsize <= 0 || !mzl_find_eocd(f, fsize, &cd_size, &cd_ofs, &nent) || nent == 0) {
        fclose(f);
        return 0;
    }
    if (cd_size == 0 || (uint64_t)cd_ofs + (uint64_t)cd_size > (uint64_t)fsize) {
        fclose(f);
        return 0;
    }

    cd = (unsigned char *)mzl_big_alloc(cd_size);
    if (!cd) {
        fclose(f);
        return 0;
    }
    if (fseek(f, (long)cd_ofs, SEEK_SET) != 0 || fread(cd, 1, cd_size, f) != cd_size) {
        free(cd);
        fclose(f);
        return 0;
    }

    zip->entries = (mz_zip_entry_t *)calloc(nent, sizeof(mz_zip_entry_t));
    if (!zip->entries) {
        free(cd);
        fclose(f);
        return 0;
    }

    for (i = 0; i < nent; i++) {
        uint16_t name_len;
        uint16_t extra_len;
        uint16_t cmt_len;
        char *nm;

        if ((size_t)pos + 46 > cd_size || mzl_rd32(cd + pos) != MZL_SIG_CDH) {
            break;
        }

        zip->entries[i].method           = mzl_rd16(cd + pos + 10);
        zip->entries[i].crc32            = mzl_rd32(cd + pos + 16);
        zip->entries[i].comp_size        = mzl_rd32(cd + pos + 20);
        zip->entries[i].uncomp_size      = mzl_rd32(cd + pos + 24);
        name_len                         = mzl_rd16(cd + pos + 28);
        extra_len                        = mzl_rd16(cd + pos + 30);
        cmt_len                          = mzl_rd16(cd + pos + 32);
        zip->entries[i].local_header_ofs = mzl_rd32(cd + pos + 42);

        if ((size_t)pos + 46 + name_len > cd_size) {
            break;
        }
        nm = (char *)malloc((size_t)name_len + 1);
        if (!nm) {
            break;
        }
        memcpy(nm, cd + pos + 46, name_len);
        nm[name_len]       = '\0';
        zip->entries[i].name = nm;

        pos += 46u + name_len + extra_len + cmt_len;
        zip->num_files = i + 1;
    }

    free(cd);

    if (zip->num_files == 0) {
        free(zip->entries);
        zip->entries = NULL;
        fclose(f);
        return 0;
    }

    zip->fp        = (void *)f;
    zip->file_size = (uint32_t)fsize;
    return 1;
}

void mz_zip_reader_end(mz_zip_archive *zip)
{
    uint32_t i;

    if (!zip) {
        return;
    }
    if (zip->entries) {
        for (i = 0; i < zip->num_files; i++) {
            free(zip->entries[i].name);
        }
        free(zip->entries);
        zip->entries = NULL;
    }
    if (zip->fp) {
        fclose((FILE *)zip->fp);
        zip->fp = NULL;
    }
    zip->num_files = 0;
}

uint32_t mz_zip_reader_get_num_files(const mz_zip_archive *zip)
{
    return zip ? zip->num_files : 0;
}

int mz_zip_reader_file_stat(const mz_zip_archive *zip, uint32_t index, mz_zip_archive_file_stat *st)
{
    const mz_zip_entry_t *e;

    if (!zip || !st || !zip->entries || index >= zip->num_files) {
        return 0;
    }
    e = &zip->entries[index];

    st->index            = index;
    st->method           = e->method;
    st->crc32            = e->crc32;
    st->comp_size        = e->comp_size;
    st->uncomp_size      = e->uncomp_size;
    st->local_header_ofs = e->local_header_ofs;
    snprintf(st->filename, sizeof(st->filename), "%s", e->name ? e->name : "");
    return 1;
}

int mz_zip_reader_locate_file(const mz_zip_archive *zip, const char *name)
{
    uint32_t i;

    if (!zip || !name || !zip->entries) {
        return -1;
    }
    for (i = 0; i < zip->num_files; i++) {
        if (zip->entries[i].name && strcasecmp(zip->entries[i].name, name) == 0) {
            return (int)i;
        }
    }
    return -1;
}

void *mz_zip_reader_extract_to_heap(mz_zip_archive *zip, uint32_t index, size_t *out_size)
{
    return mz_zip_reader_extract_to_heap_max(zip, index, MZL_DEFAULT_MAX_EXTRACT, out_size);
}

void *mz_zip_reader_extract_to_heap_max(mz_zip_archive *zip, uint32_t index, size_t max_bytes, size_t *out_size)
{
    FILE *f;
    const mz_zip_entry_t *e;
    unsigned char hdr[30];
    uint16_t name_len;
    uint16_t extra_len;
    long data_ofs;
    unsigned char *comp = NULL;
    unsigned char *out  = NULL;

    if (out_size) {
        *out_size = 0;
    }
    if (!zip || !zip->fp || !zip->entries || index >= zip->num_files) {
        return NULL;
    }

    f = (FILE *)zip->fp;
    e = &zip->entries[index];

    if (out_size) {
        *out_size = 0;
    }
    if (e->method != 0 && e->method != 8) {
        return NULL; /* unsupported compression method */
    }
    /* Size claims are untrusted: refuse oversize members before allocating,
     * and anything pointing outside the archive. */
    if (e->uncomp_size > max_bytes || e->comp_size > max_bytes || e->uncomp_size >= 0xFFFFFFF0u ||
        (uint64_t)e->local_header_ofs + 30u + (uint64_t)e->comp_size > (uint64_t)zip->file_size) {
        return NULL;
    }

    if (fseek(f, (long)e->local_header_ofs, SEEK_SET) != 0 || fread(hdr, 1, sizeof(hdr), f) != sizeof(hdr)) {
        return NULL;
    }
    if (mzl_rd32(hdr) != MZL_SIG_LFH) {
        return NULL;
    }
    name_len  = mzl_rd16(hdr + 26);
    extra_len = mzl_rd16(hdr + 28);
    data_ofs  = (long)e->local_header_ofs + 30 + name_len + extra_len;
    if ((uint64_t)data_ofs + (uint64_t)e->comp_size > (uint64_t)zip->file_size) {
        return NULL;
    }

    out = (unsigned char *)mzl_big_alloc((size_t)e->uncomp_size + 1);
    if (!out) {
        return NULL;
    }

    if (e->method == 0) {
        if (e->comp_size != e->uncomp_size) {
            free(out);
            return NULL;
        }
        if (fseek(f, data_ofs, SEEK_SET) != 0 ||
            fread(out, 1, e->uncomp_size, f) != (size_t)e->uncomp_size) {
            free(out);
            return NULL;
        }
    } else {
        size_t produced = 0;
        int rc;

        comp = (unsigned char *)mzl_big_alloc(e->comp_size ? e->comp_size : 1);
        if (!comp) {
            free(out);
            return NULL;
        }
        if (fseek(f, data_ofs, SEEK_SET) != 0 || fread(comp, 1, e->comp_size, f) != (size_t)e->comp_size) {
            free(comp);
            free(out);
            return NULL;
        }

        rc = mzl_inflate_raw(comp, e->comp_size, out, e->uncomp_size, &produced);
        free(comp);
        if (rc != 0 || produced != (size_t)e->uncomp_size) {
            free(out);
            return NULL;
        }
    }

    out[e->uncomp_size] = 0;
    if (out_size) {
        *out_size = e->uncomp_size;
    }
    return out;
}

void *mz_zip_reader_extract_file_to_heap(mz_zip_archive *zip, const char *name, size_t *out_size)
{
    int idx = mz_zip_reader_locate_file(zip, name);
    if (idx < 0) {
        if (out_size) {
            *out_size = 0;
        }
        return NULL;
    }
    return mz_zip_reader_extract_to_heap(zip, (uint32_t)idx, out_size);
}
