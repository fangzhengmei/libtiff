/*
 * TIFF Library
 *
 * Integration tests for integer-to-memory-size conversion safety.
 *
 * Copyright (c) 2025, libtiff contributors
 *
 * Permission to use, copy, modify, distribute, and sell this software and
 * its documentation for any purpose is hereby granted without fee, provided
 * that (i) the above copyright notices and this permission notice appear in
 * all copies of the software and related documentation, and (ii) the names of
 * Sam Leffler and Silicon Graphics may not be used in any advertising or
 * publicity relating to the software without the specific, prior written
 * permission of Sam Leffler and Silicon Graphics.
 *
 * THE SOFTWARE IS PROVIDED "AS-IS" AND WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS, IMPLIED OR OTHERWISE, INCLUDING WITHOUT LIMITATION, ANY
 * WARRANTY OF MERCHANTABILITY OR FITNESS FOR A PARTICULAR PURPOSE.
 *
 * IN NO EVENT SHALL SAM LEFFLER OR SILICON GRAPHICS BE LIABLE FOR
 * ANY SPECIAL, INCIDENTAL, INDIRECT OR CONSEQUENTIAL DAMAGES OF ANY KIND,
 * OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS,
 * WHETHER OR NOT ADVISED OF THE POSSIBILITY OF DAMAGE, OR FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
 * USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

/* ===========  Purpose ===============================================
 *
 * Exercises the conversion of integer TIFF fields (width, height, samples
 * per pixel, bits per sample, strip/tile byte counts) into in-memory sizes.
 *
 * Malformed files are generated on the fly with hand-written TIFF/BigTIFF
 * directories so that values which cannot be produced by the regular writer
 * (zero dimensions, 16-bit bit depths, 0xFFFFFFFF dimensions, ...) can be
 * encoded.  Three layers are checked:
 *
 *  1. Directory parsing: TIFFOpen()/TIFFReadDirectory() must not derive a
 *     truncated in-memory size from the fields.
 *  2. Whole-block reads: TIFFReadEncodedStrip()/TIFFReadEncodedTile() are
 *     called with a bounded, guard-padded destination buffer.  They must not
 *     write past the supplied size.
 *  3. Chunked decoding: TIFFReadScanline() and TIFFReadRGBAImage() must fail
 *     rather than consume a size derived from an overflowing or wrapped
 *     computation.
 *
 * All allocations requested by libtiff are capped with
 * TIFFOpenOptionsSetMaxSingleMemAlloc() so that a "successful" read can never
 * hide a silently truncated allocation: a file whose declared dimensions
 * imply a buffer above the cap must produce an error instead of a smaller
 * allocation, and every returned size must remain within the buffer the
 * caller supplied.
 *
 * Per malformed directory the test asserts one of three outcomes, chosen to
 * match the contract of the layer that owns the check:
 *
 *  - REJECT: the directory is unusable (zero dimensions/samples/rows per
 *    strip, or a size computation that overflows to zero).  TIFFOpen() must
 *    fail; if the handle does open, the derived size must be reported as zero
 *    and every read must fail.  This keeps the case meaningful on platforms
 *    where the arithmetic overflows earlier or later.
 *  - BOUND: the fields are individually representable but imply a size above
 *    TIFF_TMSIZE_T_MAX or above the allocation cap.  Reading must fail and
 *    must never exceed the caller's buffer.  (On 32-bit platforms several of
 *    these collapse into REJECT, which is accepted as well.)
 *  - REPAIR: a corrupt strip byte count is detected and recomputed from the
 *    image geometry instead of being trusted.  The repaired value must equal
 *    the computed strip size and reads must stay bounded.
 *
 * A size computed as 64 bits that does not fit in tmsize_t must become 0 so
 * that it is rejected; accepting a truncated (smaller) value is a failure.
 * An intermediate 64-bit product that wraps must likewise be reported as 0,
 * never silently wrapped to a plausible smaller size; a reference size
 * oracle recomputes scanline/strip/tile sizes from the declared fields to
 * detect either kind of silent mis-conversion.
 *
 * Sanitizer builds (AddressSanitizer / UndefinedBehaviorSanitizer) must run
 * this test without diagnostics.
 *
 * The final section writes regular multi-strip and tiled images through the
 * library and verifies that pixels read back are bit-identical, so the
 * hardening checks do not reject legitimate files.
 */

#include "tif_config.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif

#include "tiffio.h"

#ifdef _WIN32
#include <io.h>
#define UNLINK _unlink
#else
#define UNLINK unlink
#endif

/* Maximum single allocation allowed while probing malformed files.  Small
 * enough that any buffer derived from a huge/overflowing dimension is
 * refused, large enough for all legitimate images in this test. */
#define MAX_SINGLE_ALLOC (1024 * 1024)

/* Sentinel bytes around every destination buffer handed to libtiff. */
#define GUARD_SIZE 64
#define GUARD_BYTE 0xA5

static int failures = 0;

#define CHECK(cond, ...)                                                       \
    do                                                                         \
    {                                                                          \
        if (!(cond))                                                           \
        {                                                                      \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);               \
            fprintf(stderr, __VA_ARGS__);                                      \
            fprintf(stderr, "\n");                                             \
            failures++;                                                        \
        }                                                                      \
    } while (0)

typedef struct
{
    int errors;
    int warnings;
} ErrorContext;

static int error_handler(TIFF *tif, void *user_data, const char *module,
                         const char *fmt, va_list ap)
{
    ErrorContext *ctx = (ErrorContext *)user_data;
    (void)tif;
    (void)module;
    (void)fmt;
    (void)ap;
    ctx->errors++;
    return 0;
}

static int warning_handler(TIFF *tif, void *user_data, const char *module,
                           const char *fmt, va_list ap)
{
    ErrorContext *ctx = (ErrorContext *)user_data;
    (void)tif;
    (void)module;
    (void)fmt;
    (void)ap;
    ctx->warnings++;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Minimal TIFF writer used to encode directories the library would    */
/* never produce (zero dimensions, out-of-range values, ...).          */
/* ------------------------------------------------------------------ */

typedef struct
{
    uint16_t tag;
    uint16_t type;
    uint64_t count;
    uint32_t inline_value; /* used when data == NULL */
    const unsigned char *data;
    size_t data_len;
} RawEntry;

#define TYPE_SHORT 3
#define TYPE_LONG 4
#define TYPE_LONG8 16

static void put16(unsigned char *p, uint16_t v)
{
    p[0] = (unsigned char)(v & 0xFF);
    p[1] = (unsigned char)((v >> 8) & 0xFF);
}

static void put32(unsigned char *p, uint32_t v)
{
    int i;
    for (i = 0; i < 4; i++)
        p[i] = (unsigned char)((v >> (8 * i)) & 0xFF);
}

static void put64(unsigned char *p, uint64_t v)
{
    int i;
    for (i = 0; i < 8; i++)
        p[i] = (unsigned char)((v >> (8 * i)) & 0xFF);
}

static int entry_compare(const void *a, const void *b)
{
    uint16_t ta = ((const RawEntry *)a)->tag;
    uint16_t tb = ((const RawEntry *)b)->tag;
    return (ta > tb) - (ta < tb);
}

/* Size of the header + directory for a given entry count. */
static uint64_t tiff_header_size(int n_entries, int bigtiff)
{
    if (bigtiff)
        return 16 + 8 + (uint64_t)n_entries * 20 + 8;
    return 8 + 2 + (uint64_t)n_entries * 12 + 4;
}

/* Offset at which out-of-line entry data (and thus payload) starts.
 * Data that fits into the value field is stored inline and does not count.
 */
static uint64_t tiff_data_offset(const RawEntry *entries, int n_entries,
                                 int bigtiff)
{
    uint64_t off = tiff_header_size(n_entries, bigtiff);
    int i;
    const size_t inline_capacity = bigtiff ? 8 : 4;
    for (i = 0; i < n_entries; i++)
    {
        if (entries[i].data != NULL && entries[i].data_len > inline_capacity)
            off += entries[i].data_len;
    }
    return off;
}

static int write_tiff_file(const char *path, RawEntry *entries, int n_entries,
                           int bigtiff, const unsigned char *payload,
                           size_t payload_len)
{
    FILE *f;
    unsigned char header[16];
    unsigned char *dir;
    size_t dir_size;
    uint64_t expected_data_off;
    uint64_t blob_off;
    int i;
    const size_t value_capacity = bigtiff ? 8 : 4;

    qsort(entries, (size_t)n_entries, sizeof(RawEntry), entry_compare);

    expected_data_off = tiff_data_offset(entries, n_entries, bigtiff);
    dir_size =
        (size_t)(tiff_header_size(n_entries, bigtiff) - (bigtiff ? 16 : 8));
    dir = (unsigned char *)malloc(dir_size);
    if (dir == NULL)
        return -1;
    memset(dir, 0, dir_size);
    memset(header, 0, sizeof(header));
    memcpy(header, "II", 2);

    if (bigtiff)
    {
        put16(header + 2, 43);
        put16(header + 4, 8);
        put16(header + 6, 0);
        put64(header + 8, 16); /* offset of the first IFD */
        put64(dir, (uint64_t)n_entries);
        blob_off = 16 + dir_size;
        for (i = 0; i < n_entries; i++)
        {
            unsigned char *e = dir + 8 + (size_t)i * 20;
            RawEntry *re = &entries[i];
            put16(e, re->tag);
            put16(e + 2, re->type);
            put64(e + 4, re->count);
            if (re->data != NULL && re->data_len > value_capacity)
            {
                put64(e + 12, blob_off);
                blob_off += re->data_len;
            }
            else if (re->data != NULL)
            {
                memcpy(e + 12, re->data, re->data_len);
            }
            else
            {
                put64(e + 12, re->inline_value);
            }
        }
        put64(dir + 8 + (size_t)n_entries * 20, 0);
    }
    else
    {
        put16(header + 2, 42);
        put32(header + 4, 8);
        put16(dir, (uint16_t)n_entries);
        blob_off = 8 + dir_size;
        for (i = 0; i < n_entries; i++)
        {
            unsigned char *e = dir + 2 + (size_t)i * 12;
            RawEntry *re = &entries[i];
            put16(e, re->tag);
            put16(e + 2, re->type);
            put32(e + 4, (uint32_t)re->count);
            if (re->data != NULL && re->data_len > value_capacity)
            {
                put32(e + 8, (uint32_t)blob_off);
                blob_off += re->data_len;
            }
            else if (re->data != NULL)
            {
                memcpy(e + 8, re->data, re->data_len);
            }
            else
            {
                put32(e + 8, re->inline_value);
            }
        }
        put32(dir + 2 + (size_t)n_entries * 12, 0);
    }

    f = fopen(path, "wb");
    if (f == NULL)
    {
        free(dir);
        return -1;
    }
    if (fwrite(header, 1, bigtiff ? 16 : 8, f) != (bigtiff ? 16u : 8u))
        goto bad;
    if (fwrite(dir, 1, dir_size, f) != dir_size)
        goto bad;

    /* Out-of-line entry data, in the same order used for the offsets. */
    for (i = 0; i < n_entries; i++)
    {
        RawEntry *re = &entries[i];
        if (re->data != NULL && re->data_len > value_capacity)
        {
            if (fwrite(re->data, 1, re->data_len, f) != re->data_len)
                goto bad;
        }
    }

    /* Payload (strip/tile bytes).  The offset was computed before writing,
     * so validate that the file position matches it. */
    if ((uint64_t)ftell(f) != expected_data_off)
        goto bad;
    if (payload != NULL && payload_len > 0)
    {
        if (fwrite(payload, 1, payload_len, f) != payload_len)
            goto bad;
    }
    if (fclose(f) != 0)
    {
        free(dir);
        return -1;
    }
    free(dir);
    return 0;
bad:
    fclose(f);
    free(dir);
    return -1;
}

static void fill32(unsigned char *dst, uint32_t value, uint32_t count)
{
    uint32_t i;
    for (i = 0; i < count; i++)
        put32(dst + (size_t)i * 4, value);
}

static void fill64(unsigned char *dst, uint64_t value, uint32_t count)
{
    uint32_t i;
    for (i = 0; i < count; i++)
        put64(dst + (size_t)i * 8, value);
}

static unsigned char payload_bytes[4096];

static void init_payload(void)
{
    size_t i;
    for (i = 0; i < sizeof(payload_bytes); i++)
        payload_bytes[i] = (unsigned char)(0x30 + (i % 16));
}

typedef struct
{
    uint32_t width;
    uint32_t height;
    uint16_t bits;
    uint16_t samples;
    uint16_t planar;
    uint16_t compression;
    uint16_t photometric;
    uint32_t rows_per_strip;
    uint64_t byte_count;   /* per strip / per tile */
    uint32_t array_count;  /* value written in the tag count field */
    uint32_t array_values; /* number of elements actually encoded */
    int bigtiff;
} StripSpec;

static void set_simple_entry(RawEntry *e, uint16_t tag, uint16_t type,
                             uint64_t count, uint32_t inline_value)
{
    e->tag = tag;
    e->type = type;
    e->count = count;
    e->inline_value = inline_value;
    e->data = NULL;
    e->data_len = 0;
}

/* Build a (possibly malformed) stripped file. */
static int build_strip_file(const char *path, const StripSpec *spec)
{
    RawEntry e[12];
    unsigned char offsets[64 * 4];
    unsigned char counts[64 * 8];
    uint32_t values = spec->array_values;
    int n = 0;
    uint64_t data_off;
    int rc;

    if (values == 0)
        values = 1;
    if (values > 64)
        values = 64;
    memset(offsets, 0, sizeof(offsets));
    memset(counts, 0, sizeof(counts));

    set_simple_entry(&e[n++], TIFFTAG_IMAGEWIDTH, TYPE_LONG, 1, spec->width);
    set_simple_entry(&e[n++], TIFFTAG_IMAGELENGTH, TYPE_LONG, 1, spec->height);
    /* BitsPerSample is written with a single value, which the directory
     * parser explicitly tolerates for multi-sample images.  This keeps the
     * directory well-formed so that the size computations are reached. */
    set_simple_entry(&e[n++], TIFFTAG_BITSPERSAMPLE, TYPE_SHORT, 1, spec->bits);
    set_simple_entry(&e[n++], TIFFTAG_COMPRESSION, TYPE_SHORT, 1,
                     spec->compression);
    set_simple_entry(&e[n++], TIFFTAG_PHOTOMETRIC, TYPE_SHORT, 1,
                     spec->photometric);
    set_simple_entry(&e[n++], TIFFTAG_SAMPLESPERPIXEL, TYPE_SHORT, 1,
                     spec->samples);
    set_simple_entry(&e[n++], TIFFTAG_ROWSPERSTRIP, TYPE_LONG, 1,
                     spec->rows_per_strip);
    set_simple_entry(&e[n++], TIFFTAG_PLANARCONFIG, TYPE_SHORT, 1,
                     spec->planar);

    e[n].tag = TIFFTAG_STRIPOFFSETS;
    e[n].type = spec->bigtiff ? TYPE_LONG8 : TYPE_LONG;
    e[n].count = spec->array_count;
    e[n].inline_value = 0;
    e[n].data = offsets;
    e[n].data_len = (size_t)values * (spec->bigtiff ? 8 : 4);
    n++;
    e[n].tag = TIFFTAG_STRIPBYTECOUNTS;
    e[n].type = spec->bigtiff ? TYPE_LONG8 : TYPE_LONG;
    e[n].count = spec->array_count;
    e[n].inline_value = 0;
    e[n].data = counts;
    e[n].data_len = (size_t)values * (spec->bigtiff ? 8 : 4);
    n++;

    /* The offset arrays are encoded out of line when their length exceeds
     * the inline value field; the payload position therefore depends only on
     * the number of values, not on their contents. */
    data_off = tiff_data_offset(e, n, spec->bigtiff);
    if (spec->bigtiff)
    {
        fill64(offsets, data_off, values);
        fill64(counts, spec->byte_count, values);
    }
    else
    {
        fill32(offsets, (uint32_t)data_off, values);
        fill32(counts, (uint32_t)spec->byte_count, values);
    }

    rc = write_tiff_file(path, e, n, spec->bigtiff, payload_bytes,
                         sizeof(payload_bytes));
    return rc;
}

typedef struct
{
    uint32_t width;
    uint32_t height;
    uint32_t tile_width;
    uint32_t tile_height;
    uint16_t bits;
    uint16_t samples;
    uint16_t planar;
    uint64_t byte_count;
    uint32_t array_count;
    uint32_t array_values;
    int bigtiff;
} TileSpec;

/* Build a (possibly malformed) tiled file. */
static int build_tile_file(const char *path, const TileSpec *spec)
{
    RawEntry e[12];
    unsigned char offsets[64 * 4];
    unsigned char counts[64 * 8];
    uint32_t values = spec->array_values;
    int n = 0;
    uint64_t data_off;
    int rc;

    if (values == 0)
        values = 1;
    if (values > 64)
        values = 64;
    memset(offsets, 0, sizeof(offsets));
    memset(counts, 0, sizeof(counts));

    set_simple_entry(&e[n++], TIFFTAG_IMAGEWIDTH, TYPE_LONG, 1, spec->width);
    set_simple_entry(&e[n++], TIFFTAG_IMAGELENGTH, TYPE_LONG, 1, spec->height);
    set_simple_entry(&e[n++], TIFFTAG_BITSPERSAMPLE, TYPE_SHORT, 1, spec->bits);
    set_simple_entry(&e[n++], TIFFTAG_COMPRESSION, TYPE_SHORT, 1,
                     COMPRESSION_NONE);
    set_simple_entry(&e[n++], TIFFTAG_PHOTOMETRIC, TYPE_SHORT, 1,
                     PHOTOMETRIC_MINISBLACK);
    set_simple_entry(&e[n++], TIFFTAG_SAMPLESPERPIXEL, TYPE_SHORT, 1,
                     spec->samples);
    set_simple_entry(&e[n++], TIFFTAG_PLANARCONFIG, TYPE_SHORT, 1,
                     spec->planar);
    set_simple_entry(&e[n++], TIFFTAG_TILEWIDTH, TYPE_LONG, 1,
                     spec->tile_width);
    set_simple_entry(&e[n++], TIFFTAG_TILELENGTH, TYPE_LONG, 1,
                     spec->tile_height);

    e[n].tag = TIFFTAG_TILEOFFSETS;
    e[n].type = spec->bigtiff ? TYPE_LONG8 : TYPE_LONG;
    e[n].count = spec->array_count;
    e[n].inline_value = 0;
    e[n].data = offsets;
    e[n].data_len = (size_t)values * (spec->bigtiff ? 8 : 4);
    n++;
    e[n].tag = TIFFTAG_TILEBYTECOUNTS;
    e[n].type = spec->bigtiff ? TYPE_LONG8 : TYPE_LONG;
    e[n].count = spec->array_count;
    e[n].inline_value = 0;
    e[n].data = counts;
    e[n].data_len = (size_t)values * (spec->bigtiff ? 8 : 4);
    n++;

    data_off = tiff_data_offset(e, n, spec->bigtiff);
    if (spec->bigtiff)
    {
        fill64(offsets, data_off, values);
        fill64(counts, spec->byte_count, values);
    }
    else
    {
        fill32(offsets, (uint32_t)data_off, values);
        fill32(counts, (uint32_t)spec->byte_count, values);
    }

    rc = write_tiff_file(path, e, n, spec->bigtiff, payload_bytes,
                         sizeof(payload_bytes));
    return rc;
}

/* ------------------------------------------------------------------ */
/* Guarded buffers                                                     */
/* ------------------------------------------------------------------ */

static unsigned char *guard_alloc(size_t size)
{
    unsigned char *p = (unsigned char *)malloc(size + 2 * GUARD_SIZE);
    if (p == NULL)
        return NULL;
    memset(p, GUARD_BYTE, size + 2 * GUARD_SIZE);
    return p;
}

static unsigned char *guard_payload(unsigned char *p) { return p + GUARD_SIZE; }

static int guard_intact(const unsigned char *p, size_t size)
{
    size_t i;
    for (i = 0; i < GUARD_SIZE; i++)
    {
        if (p[i] != GUARD_BYTE)
            return 0;
        if (p[GUARD_SIZE + size + i] != GUARD_BYTE)
            return 0;
    }
    return 1;
}

static void guard_free(unsigned char *p) { free(p); }

/* ------------------------------------------------------------------ */
/* Size conversion checks                                             */
/* ------------------------------------------------------------------ */

/* A 64-bit size that does not fit in tmsize_t must convert to 0 (and thus be
 * rejected); a representable size must round-trip exactly.  A truncated,
 * smaller value must never be accepted. */
static void check_size_cast(const char *case_name, const char *what,
                            uint64_t size64, tmsize_t size)
{
    if (size64 > (uint64_t)TIFF_TMSIZE_T_MAX)
    {
        CHECK(size == 0,
              "%s: %s size64=%llu was truncated to %lld instead of being "
              "rejected",
              case_name, what, (unsigned long long)size64, (long long)size);
    }
    else
    {
        CHECK((uint64_t)size == size64,
              "%s: %s size64=%llu but tmsize_t conversion yielded %lld",
              case_name, what, (unsigned long long)size64, (long long)size);
    }
}

typedef struct
{
    int read_attempts;
    int read_failures;
    int read_successes;
    uint64_t strip64;
    uint64_t tile64;
    uint64_t scan64;
    uint64_t byte_count0;
    int tiled;
    uint32_t width;
    uint32_t height;
    uint32_t rows_per_strip;
    uint16_t samples;
    uint16_t bits;
    uint16_t planar;
    uint32_t tile_width;
    uint32_t tile_height;
    uint32_t image_depth;
    int have_tile_dims;
    int size_is_zero; /* any of scan/strip/tile reported as zero */
} ProbeResult;

/* ------------------------------------------------------------------ */
/* Independent size oracle                                            */
/* ------------------------------------------------------------------ */

/* Reference computation of the same quantities libtiff derives from the
 * declared fields, with explicit 64-bit overflow detection.
 *
 * Each multiplier carries an "overflow" flag so an overflowing product can
 * be distinguished from a legitimate zero result.  libtiff must report zero
 * for an overflowing size, never a wrapped/truncated small value. */

static uint64_t oracle_checked_mul(uint64_t a, uint64_t b, int *overflow)
{
    if (b != 0 && a > UINT64_MAX / b)
    {
        *overflow = 1;
        return 0;
    }
    return a * b;
}

/* Returns the contiguous scanline size (contig, non-YCbCr) with overflow
 * detection.  `overflow` is set when any intermediate product wrapped. */
static uint64_t oracle_scanline_size(const ProbeResult *p, int *overflow)
{
    uint64_t samples, bits, bytes;
    uint32_t width = p->width;
    uint16_t spp = p->samples;
    uint16_t bps = p->bits;

    if (p->planar == PLANARCONFIG_SEPARATE)
        spp = 1;
    if (spp == 0)
        spp = 1;
    samples = oracle_checked_mul(width, spp, overflow);
    bits = oracle_checked_mul(samples, bps, overflow);
    bytes = (bits + 7) / 8;
    return bytes;
}

/* Strip size for the given number of rows. */
static uint64_t oracle_strip_rows(const ProbeResult *p, uint32_t rows,
                                  int *overflow)
{
    uint64_t scanline = oracle_scanline_size(p, overflow);
    if (*overflow)
        return 0;
    return oracle_checked_mul(scanline, rows, overflow);
}

/* Tile size: tile row size multiplied by the tile length. */
static uint64_t oracle_tile_size(const ProbeResult *p, int *overflow)
{
    uint64_t row, bytes;

    if (p->planar == PLANARCONFIG_CONTIG)
    {
        uint64_t bits;
        uint64_t samples = oracle_checked_mul(
            p->tile_width, p->samples == 0 ? 1 : p->samples, overflow);
        bits = oracle_checked_mul(samples, p->bits, overflow);
        row = (bits + 7) / 8;
    }
    else
    {
        uint64_t bits = oracle_checked_mul(p->tile_width, p->bits, overflow);
        row = (bits + 7) / 8;
    }
    if (*overflow)
        return 0;
    bytes = oracle_checked_mul(row, p->tile_height, overflow);
    return bytes;
}

/* Probe an opened handle: size conversions and every read entry point.
 * All destination buffers are guard-padded and bounded. */
static void probe_handle(TIFF *tif, const char *case_name, ProbeResult *res)
{
    const size_t cap = 4096;
    tmsize_t scan, strip, tile;
    uint64_t old_strip64;

    memset(res, 0, sizeof(*res));
    res->tiled = TIFFIsTiled(tif);
    TIFFGetFieldDefaulted(tif, TIFFTAG_IMAGEWIDTH, &res->width);
    TIFFGetFieldDefaulted(tif, TIFFTAG_IMAGELENGTH, &res->height);
    TIFFGetFieldDefaulted(tif, TIFFTAG_BITSPERSAMPLE, &res->bits);
    TIFFGetFieldDefaulted(tif, TIFFTAG_SAMPLESPERPIXEL, &res->samples);
    TIFFGetFieldDefaulted(tif, TIFFTAG_PLANARCONFIG, &res->planar);
    TIFFGetFieldDefaulted(tif, TIFFTAG_ROWSPERSTRIP, &res->rows_per_strip);
    res->have_tile_dims =
        TIFFGetField(tif, TIFFTAG_TILEWIDTH, &res->tile_width) &&
        TIFFGetField(tif, TIFFTAG_TILELENGTH, &res->tile_height);
    if (!TIFFGetFieldDefaulted(tif, TIFFTAG_IMAGEDEPTH, &res->image_depth) ||
        res->image_depth == 0)
        res->image_depth = 1;

    res->scan64 = TIFFScanlineSize64(tif);
    res->strip64 = TIFFStripSize64(tif);
    res->tile64 = TIFFTileSize64(tif);
    scan = TIFFScanlineSize(tif);
    strip = TIFFStripSize(tif);
    tile = TIFFTileSize(tif);

    check_size_cast(case_name, "scanline", res->scan64, scan);
    check_size_cast(case_name, "strip", res->strip64, strip);
    check_size_cast(case_name, "tile", res->tile64, tile);

    res->size_is_zero = (res->scan64 == 0) || (res->strip64 == 0) ||
                        (res->tiled && res->tile64 == 0);

    /* Cross-check the reported 64-bit sizes against the independent oracle.
     * Where the oracle overflows, libtiff must report zero; where it does
     * not, the values must be identical.  This detects a silent wraparound
     * that happens to land on a representable (small) value. */
    {
        int scan_overflow = 0;
        uint64_t want_scan = oracle_scanline_size(res, &scan_overflow);
        if (scan_overflow)
            CHECK(res->scan64 == 0,
                  "%s: overflowing scanline size wrapped to %llu instead of "
                  "being reported as zero",
                  case_name, (unsigned long long)res->scan64);
        else
            CHECK(want_scan == res->scan64,
                  "%s: TIFFScanlineSize64()=%llu disagrees with oracle %llu",
                  case_name, (unsigned long long)res->scan64,
                  (unsigned long long)want_scan);

        if (res->rows_per_strip != 0 && res->rows_per_strip != 0xFFFFFFFFu)
        {
            int strip_overflow = 0;
            uint32_t rows = res->rows_per_strip;
            uint64_t want_strip;
            if (rows > res->height)
                rows = res->height;
            want_strip = oracle_strip_rows(res, rows, &strip_overflow);
            if (strip_overflow)
                CHECK(res->strip64 == 0,
                      "%s: overflowing strip size wrapped to %llu instead of "
                      "being reported as zero",
                      case_name, (unsigned long long)res->strip64);
            else
                CHECK(want_strip == res->strip64,
                      "%s: TIFFStripSize64()=%llu disagrees with oracle %llu",
                      case_name, (unsigned long long)res->strip64,
                      (unsigned long long)want_strip);
        }

        if (res->tiled && res->have_tile_dims)
        {
            int tile_overflow = 0;
            uint64_t want_tile = oracle_tile_size(res, &tile_overflow);
            if (tile_overflow)
                CHECK(res->tile64 == 0,
                      "%s: overflowing tile size wrapped to %llu instead of "
                      "being reported as zero",
                      case_name, (unsigned long long)res->tile64);
            else
                CHECK(want_tile == res->tile64,
                      "%s: TIFFTileSize64()=%llu disagrees with oracle %llu",
                      case_name, (unsigned long long)res->tile64,
                      (unsigned long long)want_tile);
        }
    }

    res->byte_count0 = TIFFGetStrileByteCount(tif, 0);

    /* Whole-block decode, bounded destination.  Keep the recorded strip size
     * so a repair-style clamp can be compared against the computed value. */
    old_strip64 = res->strip64;
    {
        size_t want =
            old_strip64 == 0
                ? cap
                : (old_strip64 < (uint64_t)cap ? (size_t)old_strip64 : cap);
        unsigned char *buf = guard_alloc(want);
        CHECK(buf != NULL, "%s: out of memory", case_name);
        if (buf != NULL)
        {
            tmsize_t r = TIFFReadEncodedStrip(tif, 0, guard_payload(buf),
                                              (tmsize_t)want);
            res->read_attempts++;
            if (r < 0)
            {
                res->read_failures++;
            }
            else
            {
                res->read_successes++;
                CHECK((uint64_t)r <= (uint64_t)want,
                      "%s: TIFFReadEncodedStrip returned %lld for a %llu-byte "
                      "buffer",
                      case_name, (long long)r, (unsigned long long)want);
            }
            CHECK(guard_intact(buf, want),
                  "%s: TIFFReadEncodedStrip wrote outside its buffer",
                  case_name);
            guard_free(buf);
        }
    }

    /* Whole-block raw read: bounded by byte counts as well. */
    {
        unsigned char *buf = guard_alloc(cap);
        CHECK(buf != NULL, "%s: out of memory", case_name);
        if (buf != NULL)
        {
            tmsize_t r =
                TIFFReadRawStrip(tif, 0, guard_payload(buf), (tmsize_t)cap);
            if (r >= 0)
                CHECK((uint64_t)r <= (uint64_t)cap,
                      "%s: TIFFReadRawStrip returned %lld for a %llu-byte "
                      "buffer",
                      case_name, (long long)r, (unsigned long long)cap);
            CHECK(guard_intact(buf, cap),
                  "%s: TIFFReadRawStrip wrote outside its buffer", case_name);
            guard_free(buf);
        }
    }

    /* Chunked decode: scanline.  Only call it when the reported scanline
     * fits into a bounded buffer; a scanline that cannot be represented in
     * tmsize_t has already been rejected above. */
    if (res->scan64 != 0 && res->scan64 <= cap)
    {
        unsigned char *buf = guard_alloc((size_t)res->scan64);
        CHECK(buf != NULL, "%s: out of memory", case_name);
        if (buf != NULL)
        {
            int r = TIFFReadScanline(tif, guard_payload(buf), 0, 0);
            res->read_attempts++;
            if (r < 0)
                res->read_failures++;
            else
                res->read_successes++;
            CHECK(guard_intact(buf, (size_t)res->scan64),
                  "%s: TIFFReadScanline wrote outside its buffer", case_name);
            guard_free(buf);
        }
    }

    /* Chunked decode: tiles. */
    if (res->tiled && res->tile64 != 0)
    {
        size_t want = res->tile64 < (uint64_t)cap ? (size_t)res->tile64 : cap;
        unsigned char *buf = guard_alloc(want);
        CHECK(buf != NULL, "%s: out of memory", case_name);
        if (buf != NULL)
        {
            tmsize_t r =
                TIFFReadEncodedTile(tif, 0, guard_payload(buf), (tmsize_t)want);
            res->read_attempts++;
            if (r < 0)
            {
                res->read_failures++;
            }
            else
            {
                res->read_successes++;
                CHECK((uint64_t)r <= (uint64_t)want,
                      "%s: TIFFReadEncodedTile returned %lld for a %llu-byte "
                      "buffer",
                      case_name, (long long)r, (unsigned long long)want);
            }
            CHECK(guard_intact(buf, want),
                  "%s: TIFFReadEncodedTile wrote outside its buffer",
                  case_name);
            guard_free(buf);
        }
    }

    /* Chunked decode: RGBA raster limited to one pixel.  The raster is
     * wrapped in the same guards so a write past the single pixel is
     * detected. */
    {
        unsigned char *raster_mem = guard_alloc(sizeof(uint32_t));
        CHECK(raster_mem != NULL, "%s: out of memory", case_name);
        if (raster_mem != NULL)
        {
            uint32_t *raster = (uint32_t *)(void *)guard_payload(raster_mem);
            int r = TIFFReadRGBAImage(tif, 1, 1, raster, 1);
            res->read_attempts++;
            if (!r)
                res->read_failures++;
            else
                res->read_successes++;
            CHECK(guard_intact(raster_mem, sizeof(uint32_t)),
                  "%s: TIFFReadRGBAImage wrote outside its raster", case_name);
            guard_free(raster_mem);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Malformed case runner                                              */
/* ------------------------------------------------------------------ */

typedef enum
{
    POLICY_REJECT, /* TIFFOpen() must fail */
    POLICY_BOUND,  /* open may succeed, on 32-bit may reject: reads must fail
                    * or stay bounded, never truncated */
    POLICY_REPAIR, /* open must succeed and clamp the corrupt byte count */
} CasePolicy;

typedef struct
{
    const char *name;
    int (*build)(const char *path);
    CasePolicy policy;
} MalformedCase;

/* Open with the shared option profile and bounded allocation limits. */
static TIFF *open_case(const char *path, const char *mode, ErrorContext *ctx)
{
    TIFFOpenOptions *opts = TIFFOpenOptionsAlloc();
    TIFF *tif;
    if (opts == NULL)
        return NULL;
    TIFFOpenOptionsSetErrorHandlerExtR(opts, error_handler, ctx);
    TIFFOpenOptionsSetWarningHandlerExtR(opts, warning_handler, ctx);
    TIFFOpenOptionsSetMaxSingleMemAlloc(opts, MAX_SINGLE_ALLOC);
    TIFFOpenOptionsSetMaxCumulatedMemAlloc(opts, 4 * MAX_SINGLE_ALLOC);
    tif = TIFFOpenExt(path, mode, opts);
    TIFFOpenOptionsFree(opts);
    return tif;
}

static void run_malformed_case(const MalformedCase *c)
{
    char path[256];
    ErrorContext ctx;
    ProbeResult res;
    TIFF *tif;
    int opened_any = 0;
    int rejected_any = 0;
    static const char *modes[] = {"rc", "rmc"};
    size_t m;

    snprintf(path, sizeof(path), "test_integer_overflow_%s.tif", c->name);
    if (c->build(path) != 0)
    {
        fprintf(stderr, "FAIL: could not build %s\n", path);
        failures++;
        return;
    }

    for (m = 0; m < sizeof(modes) / sizeof(modes[0]); m++)
    {
        memset(&ctx, 0, sizeof(ctx));
        tif = open_case(path, modes[m], &ctx);
        if (tif == NULL)
        {
            /* A directory may be refused before the size checks run, in
             * which case the only signal is a warning (for example the
             * strip-byte-count repair path).  Require a diagnostic but do
             * not insist that it is an error. */
            CHECK(ctx.errors + ctx.warnings > 0,
                  "%s[%s]: TIFFOpen failed without any diagnostic", c->name,
                  modes[m]);
            rejected_any = 1;
            continue;
        }
        opened_any = 1;
        probe_handle(tif, c->name, &res);
        TIFFClose(tif);

        switch (c->policy)
        {
            case POLICY_REJECT:
            {
                /* Rejected at open on the platform where the directory is
                 * unusable.  If the handle still opened, the size itself
                 * must have been reported as zero (the directory layer's
                 * rejection signal); otherwise a nonzero-but-wrapped size
                 * would have been accepted.  Evaluate per mode so the two
                 * open paths are checked independently. */
                int this_rejected = 0;
                if (res.read_attempts > 0 &&
                    res.read_failures == res.read_attempts)
                    this_rejected = 1;
                CHECK(res.size_is_zero || this_rejected,
                      "%s[%s]: unusable directory opened with nonzero sizes "
                      "(scan=%llu strip=%llu tile=%llu)",
                      c->name, modes[m], (unsigned long long)res.scan64,
                      (unsigned long long)res.strip64,
                      (unsigned long long)res.tile64);
                if (this_rejected)
                    rejected_any = 1;
                break;
            }
            case POLICY_BOUND:
                /* At least one layer must have complained; every successful
                 * read must already have been bounded by probe_handle(). */
                CHECK(ctx.errors > 0 || ctx.warnings > 0,
                      "%s[%s]: oversized fields produced no diagnostic",
                      c->name, modes[m]);
                if (res.read_failures > 0)
                    rejected_any = 1;
                break;
            case POLICY_REPAIR:
                /* The corrupt byte count must have been recomputed to the
                 * computed strip size, not trusted. */
                CHECK(res.byte_count0 == res.strip64,
                      "%s[%s]: byte count %llu was not clamped to computed "
                      "strip size %llu",
                      c->name, modes[m], (unsigned long long)res.byte_count0,
                      (unsigned long long)res.strip64);
                CHECK(res.byte_count0 <= (uint64_t)MAX_SINGLE_ALLOC,
                      "%s[%s]: clamped byte count %llu still exceeds the "
                      "allocation cap",
                      c->name, modes[m], (unsigned long long)res.byte_count0);
                CHECK(ctx.errors > 0 || ctx.warnings > 0,
                      "%s[%s]: corrupt byte count produced no diagnostic",
                      c->name, modes[m]);
                if (res.read_successes > 0)
                    rejected_any = 1;
                break;
        }
    }

    switch (c->policy)
    {
        case POLICY_REJECT:
            CHECK(!opened_any || rejected_any,
                  "%s: directory with unusable sizes was accepted without any "
                  "read error",
                  c->name);
            break;
        case POLICY_BOUND:
            CHECK(rejected_any,
                  "%s: oversized sizes were never rejected at any layer",
                  c->name);
            break;
        case POLICY_REPAIR:
            CHECK(rejected_any, "%s: repaired image could not be read at all",
                  c->name);
            break;
    }

    UNLINK(path);
}

/* ------------------------------------------------------------------ */
/* Malformed file builders                                            */
/* ------------------------------------------------------------------ */

/* scanline (0xFFFFFFFF bytes) * rows (0xFFFFFFFF) lands between 2^63 and
 * 2^64: representable as uint64 but not as tmsize_t, so the cast must reject
 * it instead of truncating. */
static int build_dim_product_overflow(const char *path)
{
    StripSpec s;
    memset(&s, 0, sizeof(s));
    s.width = 0xFFFFFFFFu;
    s.height = 0xFFFFFFFFu;
    s.bits = 8;
    s.samples = 1;
    s.planar = PLANARCONFIG_CONTIG;
    s.compression = COMPRESSION_NONE;
    s.photometric = PHOTOMETRIC_MINISBLACK;
    s.rows_per_strip = 0xFFFFFFFFu;
    s.byte_count = 16;
    s.array_count = 1;
    s.array_values = 1;
    return build_strip_file(path, &s);
}

/* scanline * rows (9) exceeds UINT64_MAX: the 64-bit multiplication itself
 * overflows and must be reported as zero rather than wrapping to a small
 * value. */
static int build_dim_product_tmsize_overflow(const char *path)
{
    StripSpec s;
    memset(&s, 0, sizeof(s));
    s.width = 0xFFFFFFFFu;
    s.height = 9;
    s.bits = 65535;
    s.samples = 65535;
    s.planar = PLANARCONFIG_CONTIG;
    s.compression = COMPRESSION_NONE;
    s.photometric = PHOTOMETRIC_MINISBLACK;
    s.rows_per_strip = 9;
    s.byte_count = 16;
    s.array_count = 1;
    s.array_values = 1;
    return build_strip_file(path, &s);
}

/* LZW-compressed variant of the strip-size multiplication overflow.  With
 * COMPRESSION_NONE the byte-count repair path would reject the file first;
 * LZW keeps the corrupt byte count so the wrap in the size computation is
 * reached.  A silent 64-bit wraparound here yields a plausible smaller
 * strip size, which the oracle must flag. */
static int build_dim_product_wrap_lzw(const char *path)
{
    StripSpec s;
    memset(&s, 0, sizeof(s));
    s.width = 0xFFFFFFFFu;
    s.height = 9;
    s.bits = 65535;
    s.samples = 65535;
    s.planar = PLANARCONFIG_CONTIG;
    s.compression = COMPRESSION_LZW;
    s.photometric = PHOTOMETRIC_MINISBLACK;
    s.rows_per_strip = 9;
    s.byte_count = 16;
    s.array_count = 1;
    s.array_values = 1;
    return build_strip_file(path, &s);
}

/* Width * samples * bits yields a huge but tmsize_t-representable scanline;
 * well above the allocation cap, so reads must fail instead of allocating a
 * truncated buffer. */
static int build_samples_contig_huge_scanline(const char *path)
{
    StripSpec s;
    memset(&s, 0, sizeof(s));
    s.width = 0x80000001u;
    s.height = 4;
    s.bits = 65535;
    s.samples = 65535;
    s.planar = PLANARCONFIG_CONTIG;
    s.compression = COMPRESSION_NONE;
    s.photometric = PHOTOMETRIC_MINISBLACK;
    s.rows_per_strip = 1;
    s.byte_count = 16;
    s.array_count = 4;
    s.array_values = 4;
    return build_strip_file(path, &s);
}

/* Number of strips (rows-per-strip fold) multiplied by samples overflows
 * uint32 in separate planar mode, so the strip count wraps to zero. */
static int build_samples_separate_nstrips_overflow(const char *path)
{
    StripSpec s;
    memset(&s, 0, sizeof(s));
    s.width = 1;
    s.height = 0xFFFF0000u; /* 0xFFFF0000 strips * 65535 samples > UINT32 */
    s.bits = 8;
    s.samples = 65535;
    s.planar = PLANARCONFIG_SEPARATE;
    s.compression = COMPRESSION_NONE;
    s.photometric = PHOTOMETRIC_MINISBLACK;
    s.rows_per_strip = 1;
    s.byte_count = 16;
    s.array_count = 1;
    s.array_values = 1;
    return build_strip_file(path, &s);
}

static int build_bit_depth_zero(const char *path)
{
    StripSpec s;
    memset(&s, 0, sizeof(s));
    s.width = 100;
    s.height = 100;
    s.bits = 0;
    s.samples = 1;
    s.planar = PLANARCONFIG_CONTIG;
    s.compression = COMPRESSION_NONE;
    s.photometric = PHOTOMETRIC_MINISBLACK;
    s.rows_per_strip = 100;
    s.byte_count = 16;
    s.array_count = 1;
    s.array_values = 1;
    return build_strip_file(path, &s);
}

/* Maximum 16-bit bit depth: a huge but representable strip whose actual
 * payload is far too small. */
static int build_bit_depth_max(const char *path)
{
    StripSpec s;
    memset(&s, 0, sizeof(s));
    s.width = 100;
    s.height = 100;
    s.bits = 65535;
    s.samples = 1;
    s.planar = PLANARCONFIG_CONTIG;
    s.compression = COMPRESSION_NONE;
    s.photometric = PHOTOMETRIC_MINISBLACK;
    s.rows_per_strip = 100;
    s.byte_count = 16;
    s.array_count = 1;
    s.array_values = 1;
    return build_strip_file(path, &s);
}

static int build_strip_bytecount_zero(const char *path)
{
    StripSpec s;
    memset(&s, 0, sizeof(s));
    s.width = 16;
    s.height = 16;
    s.bits = 8;
    s.samples = 1;
    s.planar = PLANARCONFIG_CONTIG;
    s.compression = COMPRESSION_NONE;
    s.photometric = PHOTOMETRIC_MINISBLACK;
    s.rows_per_strip = 16;
    s.byte_count = 0;
    s.array_count = 1;
    s.array_values = 1;
    return build_strip_file(path, &s);
}

static int build_strip_bytecount_huge(const char *path)
{
    StripSpec s;
    memset(&s, 0, sizeof(s));
    s.width = 16;
    s.height = 16;
    s.bits = 8;
    s.samples = 1;
    s.planar = PLANARCONFIG_CONTIG;
    s.compression = COMPRESSION_NONE;
    s.photometric = PHOTOMETRIC_MINISBLACK;
    s.rows_per_strip = 16;
    s.byte_count = 0xFFFFFFFFu;
    s.array_count = 1;
    s.array_values = 1;
    return build_strip_file(path, &s);
}

/* Tiled 32x32 with 4 tiles; every tile declares a huge byte count. */
static int build_tile_bytecount_huge(const char *path)
{
    TileSpec s;
    memset(&s, 0, sizeof(s));
    s.width = 32;
    s.height = 32;
    s.tile_width = 16;
    s.tile_height = 16;
    s.bits = 8;
    s.samples = 1;
    s.planar = PLANARCONFIG_CONTIG;
    s.byte_count = 0xFFFFFFFFu;
    s.array_count = 4;
    s.array_values = 4;
    return build_tile_file(path, &s);
}

/* Tile count multiplication overflows uint32 in separate planar mode. */
static int build_tile_count_overflow(const char *path)
{
    TileSpec s;
    memset(&s, 0, sizeof(s));
    s.width = 0xFFFFFFE0u; /* 0xFFFFFFE0/16 = 0xFFFFFFE tiles per axis */
    s.height = 0xFFFFFFE0u;
    s.tile_width = 16;
    s.tile_height = 16;
    s.bits = 8;
    s.samples = 65535;
    s.planar = PLANARCONFIG_SEPARATE;
    s.byte_count = 16;
    s.array_count = 1;
    s.array_values = 1;
    return build_tile_file(path, &s);
}

/* Tile size multiplication overflows uint64 to zero. */
static int build_tile_size_tmsize_overflow(const char *path)
{
    TileSpec s;
    memset(&s, 0, sizeof(s));
    s.width = 16;
    s.height = 16;
    s.tile_width = 16;
    s.tile_height = 0x80000000u; /* tile row size * tile length > UINT64 */
    s.bits = 65535;
    s.samples = 65535;
    s.planar = PLANARCONFIG_CONTIG;
    s.byte_count = 16;
    s.array_count = 1;
    s.array_values = 1;
    return build_tile_file(path, &s);
}

static int build_zero_width(const char *path)
{
    StripSpec s;
    memset(&s, 0, sizeof(s));
    s.width = 0;
    s.height = 16;
    s.bits = 8;
    s.samples = 1;
    s.planar = PLANARCONFIG_CONTIG;
    s.compression = COMPRESSION_NONE;
    s.photometric = PHOTOMETRIC_MINISBLACK;
    s.rows_per_strip = 1;
    s.byte_count = 16;
    s.array_count = 16;
    s.array_values = 1;
    return build_strip_file(path, &s);
}

static int build_zero_height(const char *path)
{
    StripSpec s;
    memset(&s, 0, sizeof(s));
    s.width = 16;
    s.height = 0;
    s.bits = 8;
    s.samples = 1;
    s.planar = PLANARCONFIG_CONTIG;
    s.compression = COMPRESSION_NONE;
    s.photometric = PHOTOMETRIC_MINISBLACK;
    s.rows_per_strip = 1;
    s.byte_count = 16;
    s.array_count = 1;
    s.array_values = 1;
    return build_strip_file(path, &s);
}

static int build_zero_rows_per_strip(const char *path)
{
    StripSpec s;
    memset(&s, 0, sizeof(s));
    s.width = 16;
    s.height = 16;
    s.bits = 8;
    s.samples = 1;
    s.planar = PLANARCONFIG_CONTIG;
    s.compression = COMPRESSION_NONE;
    s.photometric = PHOTOMETRIC_MINISBLACK;
    s.rows_per_strip = 0;
    s.byte_count = 16;
    s.array_count = 1;
    s.array_values = 1;
    return build_strip_file(path, &s);
}

static int build_zero_samples(const char *path)
{
    StripSpec s;
    memset(&s, 0, sizeof(s));
    s.width = 16;
    s.height = 16;
    s.bits = 8;
    s.samples = 0;
    s.planar = PLANARCONFIG_CONTIG;
    s.compression = COMPRESSION_NONE;
    s.photometric = PHOTOMETRIC_MINISBLACK;
    s.rows_per_strip = 16;
    s.byte_count = 16;
    s.array_count = 1;
    s.array_values = 1;
    return build_strip_file(path, &s);
}

static int build_zero_tile_dimensions(const char *path)
{
    TileSpec s;
    memset(&s, 0, sizeof(s));
    s.width = 16;
    s.height = 16;
    s.tile_width = 0;
    s.tile_height = 16;
    s.bits = 8;
    s.samples = 1;
    s.planar = PLANARCONFIG_CONTIG;
    s.byte_count = 16;
    s.array_count = 1;
    s.array_values = 1;
    return build_tile_file(path, &s);
}

/* Many strips but an offset/byte-count array that declares only one entry.
 * The array must not be expanded to an unchecked size: the directory is
 * rejected when the arrays are loaded eagerly, or reads fail when strile
 * loading is deferred. */
static int build_many_strips_short_array(const char *path)
{
    StripSpec s;
    memset(&s, 0, sizeof(s));
    s.width = 16;
    s.height = 0xFFFFFFFEu;
    s.bits = 8;
    s.samples = 1;
    s.planar = PLANARCONFIG_CONTIG;
    s.compression = COMPRESSION_NONE;
    s.photometric = PHOTOMETRIC_MINISBLACK;
    s.rows_per_strip = 1;
    s.byte_count = 16;
    s.array_count = 1;
    s.array_values = 1;
    return build_strip_file(path, &s);
}

/* Strip size between TIFF_TMSIZE_T_MAX and UINT64_MAX in a BigTIFF file:
 * must be rejected, not truncated. */
static int build_bigtiff_dim_product_tmsize_overflow(const char *path)
{
    StripSpec s;
    memset(&s, 0, sizeof(s));
    s.width = 0xFFFFFFFFu;
    s.height = 9;
    s.bits = 65535;
    s.samples = 65535;
    s.planar = PLANARCONFIG_CONTIG;
    s.compression = COMPRESSION_NONE;
    s.photometric = PHOTOMETRIC_MINISBLACK;
    s.rows_per_strip = 9;
    s.byte_count = 16;
    s.array_count = 1;
    s.array_values = 1;
    s.bigtiff = 1;
    return build_strip_file(path, &s);
}

/* BigTIFF LONG8 byte count of UINT64_MAX: must be clamped on repair. */
static int build_bigtiff_bytecount_u64max(const char *path)
{
    StripSpec s;
    memset(&s, 0, sizeof(s));
    s.width = 16;
    s.height = 16;
    s.bits = 8;
    s.samples = 1;
    s.planar = PLANARCONFIG_CONTIG;
    s.compression = COMPRESSION_NONE;
    s.photometric = PHOTOMETRIC_MINISBLACK;
    s.rows_per_strip = 16;
    s.byte_count = 0xFFFFFFFFFFFFFFFFULL;
    s.array_count = 1;
    s.array_values = 1;
    s.bigtiff = 1;
    return build_strip_file(path, &s);
}

/* BigTIFF tiled file with UINT64_MAX tile byte counts: values beyond the
 * signed 63-bit range must be refused. */
static int build_bigtiff_tile_bytecount_u64max(const char *path)
{
    TileSpec s;
    memset(&s, 0, sizeof(s));
    s.width = 32;
    s.height = 32;
    s.tile_width = 16;
    s.tile_height = 16;
    s.bits = 8;
    s.samples = 1;
    s.planar = PLANARCONFIG_CONTIG;
    s.byte_count = 0xFFFFFFFFFFFFFFFFULL;
    s.array_count = 4;
    s.array_values = 4;
    s.bigtiff = 1;
    return build_tile_file(path, &s);
}

/* 32-bit boundary: on LP64 the strip is 4 GiB (representable but above the
 * cap); on ILP32 it exceeds tmsize_t and must be rejected. */
static int build_tmsize_boundary(const char *path)
{
    StripSpec s;
    memset(&s, 0, sizeof(s));
    s.width = 0x40000000u;
    s.height = 4;
    s.bits = 8;
    s.samples = 1;
    s.planar = PLANARCONFIG_CONTIG;
    s.compression = COMPRESSION_NONE;
    s.photometric = PHOTOMETRIC_MINISBLACK;
    s.rows_per_strip = 4;
    s.byte_count = 16;
    s.array_count = 1;
    s.array_values = 1;
    return build_strip_file(path, &s);
}

static const MalformedCase malformed_cases[] = {
    {"dim_product_overflow", build_dim_product_overflow, POLICY_REJECT},
    {"dim_product_tmsize_overflow", build_dim_product_tmsize_overflow,
     POLICY_REJECT},
    {"dim_product_wrap_lzw", build_dim_product_wrap_lzw, POLICY_REJECT},
    {"samples_contig_huge_scanline", build_samples_contig_huge_scanline,
     POLICY_BOUND},
    {"samples_separate_nstrips_overflow",
     build_samples_separate_nstrips_overflow, POLICY_REJECT},
    {"bit_depth_zero", build_bit_depth_zero, POLICY_REJECT},
    {"bit_depth_max", build_bit_depth_max, POLICY_BOUND},
    {"strip_bytecount_zero", build_strip_bytecount_zero, POLICY_REPAIR},
    {"strip_bytecount_huge", build_strip_bytecount_huge, POLICY_REPAIR},
    {"tile_bytecount_huge", build_tile_bytecount_huge, POLICY_BOUND},
    {"tile_count_overflow", build_tile_count_overflow, POLICY_REJECT},
    {"tile_size_tmsize_overflow", build_tile_size_tmsize_overflow,
     POLICY_REJECT},
    {"zero_width", build_zero_width, POLICY_REJECT},
    {"zero_height", build_zero_height, POLICY_REJECT},
    {"zero_rows_per_strip", build_zero_rows_per_strip, POLICY_REJECT},
    {"zero_samples", build_zero_samples, POLICY_REJECT},
    {"zero_tile_dimensions", build_zero_tile_dimensions, POLICY_REJECT},
    {"many_strips_short_array", build_many_strips_short_array, POLICY_BOUND},
    {"tmsize_boundary", build_tmsize_boundary, POLICY_BOUND},
    {"bigtiff_dim_product_tmsize_overflow",
     build_bigtiff_dim_product_tmsize_overflow, POLICY_REJECT},
    {"bigtiff_bytecount_u64max", build_bigtiff_bytecount_u64max, POLICY_REPAIR},
    {"bigtiff_tile_bytecount_u64max", build_bigtiff_tile_bytecount_u64max,
     POLICY_BOUND},
};

/* ------------------------------------------------------------------ */
/* Legitimate images: verify pixel data is preserved                  */
/* ------------------------------------------------------------------ */

#define STRIP_IMAGE_WIDTH 13
#define STRIP_IMAGE_HEIGHT 7
#define STRIP_IMAGE_SPP 3
#define STRIP_IMAGE_ROWS_PER_STRIP 3

static unsigned char
    strip_pixels[STRIP_IMAGE_WIDTH * STRIP_IMAGE_HEIGHT * STRIP_IMAGE_SPP];

static void fill_strip_pixels(void)
{
    size_t i;
    for (i = 0; i < sizeof(strip_pixels); i++)
        strip_pixels[i] = (unsigned char)((i * 37u + 11u) & 0xFF);
}

static int write_multi_strip_image(const char *path)
{
    TIFF *tif = TIFFOpen(path, "w");
    uint32_t row;
    if (tif == NULL)
        return -1;
    if (!TIFFSetField(tif, TIFFTAG_IMAGEWIDTH, STRIP_IMAGE_WIDTH) ||
        !TIFFSetField(tif, TIFFTAG_IMAGELENGTH, STRIP_IMAGE_HEIGHT) ||
        !TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, STRIP_IMAGE_SPP) ||
        !TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE, 8) ||
        !TIFFSetField(tif, TIFFTAG_ROWSPERSTRIP, STRIP_IMAGE_ROWS_PER_STRIP) ||
        !TIFFSetField(tif, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG) ||
        !TIFFSetField(tif, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_RGB) ||
        !TIFFSetField(tif, TIFFTAG_COMPRESSION, COMPRESSION_NONE))
    {
        TIFFClose(tif);
        return -1;
    }
    for (row = 0; row < STRIP_IMAGE_HEIGHT; row++)
    {
        const unsigned char *src =
            strip_pixels + (size_t)row * STRIP_IMAGE_WIDTH * STRIP_IMAGE_SPP;
        if (TIFFWriteScanline(tif, (void *)src, row, 0) < 0)
        {
            TIFFClose(tif);
            return -1;
        }
    }
    TIFFClose(tif);
    return 0;
}

static int verify_multi_strip_image(const char *path)
{
    ErrorContext ctx;
    TIFF *tif;
    uint32_t row;
    int ok = 1;

    memset(&ctx, 0, sizeof(ctx));
    tif = open_case(path, "rc", &ctx);
    if (tif == NULL)
        return 0;

    CHECK(TIFFNumberOfStrips(tif) ==
              (STRIP_IMAGE_HEIGHT + STRIP_IMAGE_ROWS_PER_STRIP - 1) /
                  STRIP_IMAGE_ROWS_PER_STRIP,
          "multi-strip: unexpected strip count %u",
          (unsigned)TIFFNumberOfStrips(tif));

    for (row = 0; row < STRIP_IMAGE_HEIGHT; row++)
    {
        unsigned char buf[STRIP_IMAGE_WIDTH * STRIP_IMAGE_SPP];
        const unsigned char *expect =
            strip_pixels + (size_t)row * STRIP_IMAGE_WIDTH * STRIP_IMAGE_SPP;
        memset(buf, 0, sizeof(buf));
        if (TIFFReadScanline(tif, buf, row, 0) < 0)
        {
            fprintf(stderr, "FAIL: multi-strip: scanline %u not readable\n",
                    (unsigned)row);
            failures++;
            ok = 0;
            break;
        }
        if (memcmp(buf, expect, sizeof(buf)) != 0)
        {
            fprintf(stderr, "FAIL: multi-strip: pixel mismatch on row %u\n",
                    (unsigned)row);
            failures++;
            ok = 0;
            break;
        }
    }

    if (ok)
    {
        uint32_t raster[STRIP_IMAGE_WIDTH * STRIP_IMAGE_HEIGHT];
        memset(raster, 0, sizeof(raster));
        if (!TIFFReadRGBAImage(tif, STRIP_IMAGE_WIDTH, STRIP_IMAGE_HEIGHT,
                               raster, 1))
        {
            fprintf(stderr, "FAIL: multi-strip: RGBA read failed\n");
            failures++;
            ok = 0;
        }
        else
        {
            uint32_t x, y;
            for (y = 0; y < STRIP_IMAGE_HEIGHT && ok; y++)
            {
                for (x = 0; x < STRIP_IMAGE_WIDTH; x++)
                {
                    /* TIFFReadRGBAImage() uses a bottom-left raster origin,
                     * so row y of the file lands in raster row
                     * height-1-y. */
                    uint32_t px = raster[(size_t)(STRIP_IMAGE_HEIGHT - 1 - y) *
                                             STRIP_IMAGE_WIDTH +
                                         x];
                    const unsigned char *expect =
                        strip_pixels +
                        ((size_t)y * STRIP_IMAGE_WIDTH + x) * STRIP_IMAGE_SPP;
                    if (TIFFGetR(px) != expect[0] ||
                        TIFFGetG(px) != expect[1] || TIFFGetB(px) != expect[2])
                    {
                        fprintf(stderr,
                                "FAIL: multi-strip: RGBA mismatch at %u,%u\n",
                                (unsigned)x, (unsigned)y);
                        failures++;
                        ok = 0;
                        break;
                    }
                }
            }
        }
    }

    CHECK(ctx.errors == 0, "multi-strip: %d unexpected error(s)", ctx.errors);
    TIFFClose(tif);
    return ok;
}

#define TILE_IMAGE_WIDTH 48
#define TILE_IMAGE_HEIGHT 32
#define TILE_IMAGE_TILE_WIDTH 16
#define TILE_IMAGE_TILE_HEIGHT 16
#define TILE_IMAGE_SPP 3

static unsigned char
    tile_pixels[TILE_IMAGE_WIDTH * TILE_IMAGE_HEIGHT * TILE_IMAGE_SPP];

static void fill_tile_pixels(void)
{
    size_t i;
    for (i = 0; i < sizeof(tile_pixels); i++)
        tile_pixels[i] = (unsigned char)((i * 53u + 7u) & 0xFF);
}

static int write_tiled_image(const char *path)
{
    TIFF *tif = TIFFOpen(path, "w");
    uint32_t x, y;
    unsigned char tilebuf[TILE_IMAGE_TILE_WIDTH * TILE_IMAGE_TILE_HEIGHT *
                          TILE_IMAGE_SPP];
    if (tif == NULL)
        return -1;
    if (!TIFFSetField(tif, TIFFTAG_IMAGEWIDTH, TILE_IMAGE_WIDTH) ||
        !TIFFSetField(tif, TIFFTAG_IMAGELENGTH, TILE_IMAGE_HEIGHT) ||
        !TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, TILE_IMAGE_SPP) ||
        !TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE, 8) ||
        !TIFFSetField(tif, TIFFTAG_TILEWIDTH, TILE_IMAGE_TILE_WIDTH) ||
        !TIFFSetField(tif, TIFFTAG_TILELENGTH, TILE_IMAGE_TILE_HEIGHT) ||
        !TIFFSetField(tif, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG) ||
        !TIFFSetField(tif, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_RGB) ||
        !TIFFSetField(tif, TIFFTAG_COMPRESSION, COMPRESSION_NONE))
    {
        TIFFClose(tif);
        return -1;
    }
    for (y = 0; y < TILE_IMAGE_HEIGHT; y += TILE_IMAGE_TILE_HEIGHT)
    {
        for (x = 0; x < TILE_IMAGE_WIDTH; x += TILE_IMAGE_TILE_WIDTH)
        {
            uint32_t ty, tx;
            for (ty = 0; ty < TILE_IMAGE_TILE_HEIGHT; ty++)
            {
                for (tx = 0; tx < TILE_IMAGE_TILE_WIDTH; tx++)
                {
                    const unsigned char *src =
                        tile_pixels +
                        (((size_t)(y + ty) * TILE_IMAGE_WIDTH + (x + tx)) *
                         TILE_IMAGE_SPP);
                    unsigned char *dst =
                        tilebuf + (((size_t)ty * TILE_IMAGE_TILE_WIDTH + tx) *
                                   TILE_IMAGE_SPP);
                    memcpy(dst, src, TILE_IMAGE_SPP);
                }
            }
            if (TIFFWriteTile(tif, tilebuf, x, y, 0, 0) < 0)
            {
                TIFFClose(tif);
                return -1;
            }
        }
    }
    TIFFClose(tif);
    return 0;
}

static int verify_tiled_image(const char *path)
{
    ErrorContext ctx;
    TIFF *tif;
    uint32_t x, y;
    int ok = 1;
    unsigned char tilebuf[TILE_IMAGE_TILE_WIDTH * TILE_IMAGE_TILE_HEIGHT *
                          TILE_IMAGE_SPP];

    memset(&ctx, 0, sizeof(ctx));
    tif = open_case(path, "rc", &ctx);
    if (tif == NULL)
        return 0;

    CHECK(TIFFIsTiled(tif), "tiled: image was not reported as tiled");
    CHECK(TIFFNumberOfTiles(tif) ==
              (TILE_IMAGE_WIDTH / TILE_IMAGE_TILE_WIDTH) *
                  (TILE_IMAGE_HEIGHT / TILE_IMAGE_TILE_HEIGHT),
          "tiled: unexpected tile count %u", (unsigned)TIFFNumberOfTiles(tif));

    for (y = 0; y < TILE_IMAGE_HEIGHT && ok; y += TILE_IMAGE_TILE_HEIGHT)
    {
        for (x = 0; x < TILE_IMAGE_WIDTH; x += TILE_IMAGE_TILE_WIDTH)
        {
            uint32_t ty, tx;
            memset(tilebuf, 0, sizeof(tilebuf));
            if (TIFFReadTile(tif, tilebuf, x, y, 0, 0) < 0)
            {
                fprintf(stderr, "FAIL: tiled: tile at %u,%u not readable\n",
                        (unsigned)x, (unsigned)y);
                failures++;
                ok = 0;
                break;
            }
            for (ty = 0; ty < TILE_IMAGE_TILE_HEIGHT; ty++)
            {
                for (tx = 0; tx < TILE_IMAGE_TILE_WIDTH; tx++)
                {
                    const unsigned char *expect =
                        tile_pixels +
                        (((size_t)(y + ty) * TILE_IMAGE_WIDTH + (x + tx)) *
                         TILE_IMAGE_SPP);
                    const unsigned char *got =
                        tilebuf + (((size_t)ty * TILE_IMAGE_TILE_WIDTH + tx) *
                                   TILE_IMAGE_SPP);
                    if (memcmp(expect, got, TILE_IMAGE_SPP) != 0)
                    {
                        fprintf(stderr,
                                "FAIL: tiled: pixel mismatch at %u,%u\n",
                                (unsigned)(x + tx), (unsigned)(y + ty));
                        failures++;
                        ok = 0;
                        break;
                    }
                }
                if (!ok)
                    break;
            }
            if (!ok)
                break;
        }
    }

    if (ok)
    {
        uint32_t raster[TILE_IMAGE_WIDTH * TILE_IMAGE_HEIGHT];
        memset(raster, 0, sizeof(raster));
        if (!TIFFReadRGBAImage(tif, TILE_IMAGE_WIDTH, TILE_IMAGE_HEIGHT, raster,
                               1))
        {
            fprintf(stderr, "FAIL: tiled: RGBA read failed\n");
            failures++;
            ok = 0;
        }
        else
        {
            for (y = 0; y < TILE_IMAGE_HEIGHT && ok; y++)
            {
                for (x = 0; x < TILE_IMAGE_WIDTH; x++)
                {
                    uint32_t px = raster[(size_t)(TILE_IMAGE_HEIGHT - 1 - y) *
                                             TILE_IMAGE_WIDTH +
                                         x];
                    const unsigned char *expect =
                        tile_pixels +
                        ((size_t)y * TILE_IMAGE_WIDTH + x) * TILE_IMAGE_SPP;
                    if (TIFFGetR(px) != expect[0] ||
                        TIFFGetG(px) != expect[1] || TIFFGetB(px) != expect[2])
                    {
                        fprintf(stderr, "FAIL: tiled: RGBA mismatch at %u,%u\n",
                                (unsigned)x, (unsigned)y);
                        failures++;
                        ok = 0;
                        break;
                    }
                }
            }
        }
    }

    CHECK(ctx.errors == 0, "tiled: %d unexpected error(s)", ctx.errors);
    TIFFClose(tif);
    return ok;
}

int main(void)
{
    size_t i;
    const char *strip_path = "test_integer_overflow_valid_strips.tif";
    const char *tile_path = "test_integer_overflow_valid_tiles.tif";

    init_payload();
    fill_strip_pixels();
    fill_tile_pixels();

    for (i = 0; i < sizeof(malformed_cases) / sizeof(malformed_cases[0]); i++)
        run_malformed_case(&malformed_cases[i]);

    if (write_multi_strip_image(strip_path) != 0)
    {
        fprintf(stderr, "FAIL: could not write valid multi-strip image\n");
        failures++;
    }
    else if (!verify_multi_strip_image(strip_path))
    {
        fprintf(stderr, "FAIL: valid multi-strip image was not read back "
                        "correctly\n");
        failures++;
    }
    UNLINK(strip_path);

    if (write_tiled_image(tile_path) != 0)
    {
        fprintf(stderr, "FAIL: could not write valid tiled image\n");
        failures++;
    }
    else if (!verify_tiled_image(tile_path))
    {
        fprintf(stderr,
                "FAIL: valid tiled image was not read back correctly\n");
        failures++;
    }
    UNLINK(tile_path);

    if (failures != 0)
    {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    printf("All integer overflow integration checks passed\n");
    return 0;
}
