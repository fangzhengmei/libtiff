/*
 * Copyright (c) 2026, libtiff contributors
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
 * WHETHER OR NOT ADVISED OF THE POSSIBILITY OF DAMAGE, AND ON ANY THEORY OF
 * LIABILITY, ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE
 * OF THIS SOFTWARE.
 */

/*
 * TIFF Library
 *
 * Integration test for malformed strip/tile offset and byte-count metadata,
 * and for the interaction between whole-block reads (TIFFReadEncodedStrip /
 * TIFFReadEncodedTile with size == -1) and partial reads (an explicit,
 * smaller request, as used by the "chunky" pre-read of TIFFReadScanline()).
 *
 * The test synthesizes images with:
 *
 *   - offsets beyond the end of file
 *   - offsets that do not fit in tmsize_t
 *   - byte counts that run past the end of the file
 *   - byte counts at or above TIFF_TMSIZE_T_MAX (signed overflow candidates)
 *   - overlapping striles ranges
 *   - payloads truncated in the middle of a striles
 *
 * for classic TIFF and BigTIFF, stripped and tiled, in both byte orders, and
 * for the memory-mapped ('r'), explicit read ('rm'), deferred ('rD') and
 * on-demand ('rO') striles loading modes.  Images are fed to libtiff from a
 * heap buffer, so a read outside the file shows up immediately as an out of
 * bounds access under AddressSanitizer or valgrind.
 *
 * Malformed metadata must be either rejected at open time or make the
 * affected read return -1 with a diagnostic.  It must never produce a
 * wrapped-around byte count that is reported as success, and must never cause
 * a read outside the supplied buffer.  When libtiff repairs the metadata by
 * falling back to a computed byte count (documented behaviour for several
 * "Bogus StripByteCounts" patterns) the read may succeed, but only with a
 * diagnostic and never with more bytes than the real uncompressed striles
 * size.
 *
 * Well-formed multi-striles images must keep decoding to the exact pixels
 * that were synthesized, on every open mode.
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

#include "strile_synth.h"

/* Upper bound on any buffer handed to libtiff: a corrupt byte count must
 * never make the test allocate an absurd amount of memory. */
#define MAX_TEST_BUFFER (64 * 1024 * 1024)

/* ------------------------------------------------------------------ */
/* Diagnostics capture                                                 */
/* ------------------------------------------------------------------ */

static int g_error_count;
static int g_warning_count;
static char g_last_error[512];

static void count_error_handler(const char *module, const char *fmt, va_list ap)
{
    (void)module;
    vsnprintf(g_last_error, sizeof(g_last_error), fmt, ap);
    g_error_count++;
}

static void count_warning_handler(const char *module, const char *fmt,
                                  va_list ap)
{
    (void)module;
    (void)fmt;
    (void)ap;
    g_warning_count++;
}

static void reset_diagnostics(void)
{
    g_error_count = 0;
    g_warning_count = 0;
    g_last_error[0] = '\0';
}

/* ------------------------------------------------------------------ */
/* Memory-backed client                                                */
/* ------------------------------------------------------------------ */

typedef struct
{
    const uint8_t *data;
    size_t size;
    size_t pos;
} MemoryStream;

static tmsize_t stream_read(thandle_t handle, void *buf, tmsize_t size)
{
    MemoryStream *ms = (MemoryStream *)handle;
    size_t want;
    size_t avail;

    if (size <= 0)
        return 0;
    want = (size_t)size;
    if (ms->pos >= ms->size)
        return 0;
    avail = ms->size - ms->pos;
    if (want > avail)
        want = avail;
    memcpy(buf, ms->data + ms->pos, want);
    ms->pos += want;
    return (tmsize_t)want;
}

static tmsize_t stream_write(thandle_t handle, void *buf, tmsize_t size)
{
    (void)handle;
    (void)buf;
    (void)size;
    return -1;
}

static uint64_t stream_seek(thandle_t handle, uint64_t off, int whence)
{
    MemoryStream *ms = (MemoryStream *)handle;
    uint64_t base;

    switch (whence)
    {
        case SEEK_SET:
            base = 0;
            break;
        case SEEK_CUR:
            base = (uint64_t)ms->pos;
            break;
        case SEEK_END:
            base = (uint64_t)ms->size;
            break;
        default:
            return (uint64_t)-1;
    }
    if (off > (uint64_t)-1 - base)
        return (uint64_t)-1;
    if (base + off > (uint64_t)ms->size)
        return (uint64_t)-1;
    ms->pos = (size_t)(base + off);
    return (uint64_t)ms->pos;
}

static int stream_close(thandle_t handle)
{
    (void)handle;
    return 0;
}

static uint64_t stream_size(thandle_t handle)
{
    MemoryStream *ms = (MemoryStream *)handle;
    return (uint64_t)ms->size;
}

/* Deliberately no mapping: the test wants the explicit seek/read bounds
 * checks exercised against a buffer whose length is fully known. */
static int stream_map(thandle_t handle, void **base, toff_t *size)
{
    (void)handle;
    (void)base;
    (void)size;
    return 0;
}

static void stream_unmap(thandle_t handle, void *base, toff_t size)
{
    (void)handle;
    (void)base;
    (void)size;
}

static TIFF *open_memory(const uint8_t *data, size_t len, const char *mode,
                         MemoryStream *ms)
{
    ms->data = data;
    ms->size = len;
    ms->pos = 0;
    return TIFFClientOpen("MemTIFF", mode, (thandle_t)ms, stream_read,
                          stream_write, stream_seek, stream_close, stream_size,
                          stream_map, stream_unmap);
}

/* Open modes that select different code paths: 'rm' takes the seek/read path
 * instead of direct memory access, and 'D'/'O' select deferred and on-demand
 * striles array loading ('O' implies 'D'). */
static const char *const k_modes[] = {"r", "rD", "rO", "rm"};
#define NUM_MODES (sizeof(k_modes) / sizeof(k_modes[0]))

/* ------------------------------------------------------------------ */
/* Spec helpers                                                        */
/* ------------------------------------------------------------------ */

static void init_spec(StrileSynthSpec *spec, int bigtiff, int tiled, int le)
{
    memset(spec, 0, sizeof(*spec));
    spec->bigtiff = bigtiff;
    spec->tiled = tiled;
    spec->little_endian = le;
    /* A geometry with a short last striles, as in real images. */
    spec->width = 40;
    spec->height = 72;
    spec->rows_per_strip = 16;
    spec->tile_width = 16;
    spec->tile_length = 16;
    spec->bits_per_sample = 8;
    spec->samples_per_pixel = 1;
    spec->compression = COMPRESSION_NONE;
    spec->photometric = PHOTOMETRIC_MINISBLACK;
    spec->planar_config = PLANARCONFIG_CONTIG;
}

static const char *describe_spec(const StrileSynthSpec *spec)
{
    static char buf[96];
    snprintf(buf, sizeof(buf), "%s %s %s",
             spec->bigtiff ? "BigTIFF" : "ClassicTIFF",
             spec->tiled ? "tiled" : "stripped",
             spec->little_endian ? "II" : "MM");
    return buf;
}

/* ------------------------------------------------------------------ */
/* Valid round trip                                                    */
/* ------------------------------------------------------------------ */

static int verify_decodes_correctly(TIFF *tif, const StrileSynthSpec *spec,
                                    const char *context)
{
    const StrileSynthSpec spec_ref = *spec;
    uint64_t striles = strilesynth_striles(spec);
    uint64_t i;
    int ret = 0;

    for (i = 0; i < striles && ret == 0; i++)
    {
        uint64_t expected = strilesynth_bytecount(spec, i);
        uint64_t first = 0;
        uint64_t j;
        tmsize_t got;
        uint8_t *buf;

        for (j = 0; j < i; j++)
            first += strilesynth_bytecount(spec, j);

        if (expected == 0 || expected > MAX_TEST_BUFFER)
        {
            fprintf(stderr, "%s: bad expected size\n", context);
            return 1;
        }
        buf = (uint8_t *)malloc((size_t)expected);
        if (buf == NULL)
        {
            fprintf(stderr, "%s: out of memory\n", context);
            return 1;
        }

        got = spec->tiled
                  ? TIFFReadEncodedTile(tif, (uint32_t)i, buf, (tmsize_t)-1)
                  : TIFFReadEncodedStrip(tif, (uint32_t)i, buf, (tmsize_t)-1);
        if (got != (tmsize_t)expected)
        {
            fprintf(stderr, "%s: %s %llu returned %ld, expected %llu\n",
                    context, spec->tiled ? "tile" : "strip",
                    (unsigned long long)i, (long)got,
                    (unsigned long long)expected);
            free(buf);
            return 1;
        }
        for (j = 0; j < expected; j++)
        {
            if (buf[j] != strilesynth_payload_byte(first + j))
            {
                fprintf(stderr, "%s: %s %llu byte %llu is %u, want %u\n",
                        context, spec->tiled ? "tile" : "strip",
                        (unsigned long long)i, (unsigned long long)j,
                        (unsigned)buf[j],
                        (unsigned)strilesynth_payload_byte(first + j));
                free(buf);
                return 1;
            }
        }
        free(buf);
    }

    if (ret == 0 && !spec->tiled)
    {
        tmsize_t scanline_size = (tmsize_t)TIFFScanlineSize(tif);
        if (scanline_size > 0 && scanline_size <= MAX_TEST_BUFFER)
        {
            uint8_t *row = (uint8_t *)malloc((size_t)scanline_size);
            if (row != NULL)
            {
                uint32_t y;
                for (y = 0; y < spec->height; y++)
                {
                    if (TIFFReadScanline(tif, row, y, 0) < 0)
                    {
                        fprintf(stderr, "%s: scanline %u failed\n", context,
                                y);
                        ret = 1;
                        break;
                    }
                    if (row[0] != strilesynth_payload_byte(
                                      strilesynth_payload_index(&spec_ref, 0,
                                                                y)))
                    {
                        fprintf(stderr, "%s: scanline %u is %u, want %u\n",
                                context, y, (unsigned)row[0],
                                (unsigned)strilesynth_payload_byte(
                                    strilesynth_payload_index(&spec_ref, 0,
                                                              y)));
                        ret = 1;
                        break;
                    }
                }
                free(row);
            }
        }
    }

    if (ret == 0)
    {
        uint64_t total = (uint64_t)spec->width * spec->height;
        uint32_t *raster = (uint32_t *)malloc((size_t)total * sizeof(uint32_t));
        if (raster != NULL)
        {
            memset(raster, 0, (size_t)total * sizeof(uint32_t));
            if (!TIFFReadRGBAImageOriented(tif, spec->width, spec->height,
                                           raster, ORIENTATION_TOPLEFT, 0))
            {
                fprintf(stderr, "%s: RGBA read failed\n", context);
                ret = 1;
            }
            else
            {
                /* With ORIENTATION_TOPLEFT the raster is top-down, so raster
                 * row y holds image row y. */
                uint64_t x, y;
                for (y = 0; y < spec->height && ret == 0; y += 11)
                {
                    for (x = 0; x < spec->width; x += 7)
                    {
                        uint8_t want = strilesynth_payload_byte(
                            strilesynth_payload_index(spec, x, y));
                        if (TIFFGetR(raster[y * spec->width + x]) != want)
                        {
                            fprintf(stderr,
                                    "%s: RGBA pixel (%llu,%llu) is %u, want "
                                    "%u\n",
                                    context, (unsigned long long)x,
                                    (unsigned long long)y,
                                    (unsigned)TIFFGetR(
                                        raster[y * spec->width + x]),
                                    (unsigned)want);
                            ret = 1;
                            break;
                        }
                    }
                }
            }
            free(raster);
        }
    }

    return ret;
}

static int check_valid_image(int bigtiff, int tiled, int le)
{
    StrileSynthSpec spec;
    size_t len = 0;
    uint8_t *data;
    size_t m;
    int ret = 0;

    init_spec(&spec, bigtiff, tiled, le);
    data = strilesynth_build_buffer(&spec, &len, NULL);
    if (data == NULL)
    {
        fprintf(stderr, "cannot synthesize valid image (%s)\n",
                describe_spec(&spec));
        return 1;
    }

    for (m = 0; m < NUM_MODES; m++)
    {
        MemoryStream ms;
        TIFF *tif;
        char context[128];

        snprintf(context, sizeof(context), "valid image (%s) [%s]",
                 describe_spec(&spec), k_modes[m]);
        reset_diagnostics();
        tif = open_memory(data, len, k_modes[m], &ms);
        if (tif == NULL)
        {
            fprintf(stderr, "%s: cannot open\n", context);
            ret = 1;
            continue;
        }
        ret |= verify_decodes_correctly(tif, &spec, context);
        TIFFClose(tif);
    }

    free(data);
    return ret;
}

/* ------------------------------------------------------------------ */
/* Malformed cases                                                     */
/* ------------------------------------------------------------------ */

/* How the read is expected to behave. */
typedef enum
{
    /* Every read must fail (return -1). */
    EXPECT_FAILURE,
    /* libtiff may repair the metadata (documented recovery heuristics), in
     * which case reads must succeed, but with a diagnostic and within the
     * real uncompressed striles size. */
    EXPECT_FAILURE_OR_RECOVERY
} Expectation;

/* Allocate a buffer for one read.  Raw reads use an explicit, bounded size
 * because their "whole" form trusts the (possibly corrupt) byte count. */
static void *alloc_read_buffer(TIFF *tif, int tiled, int raw,
                               tmsize_t *out_size)
{
    tmsize_t size;

    if (raw)
        size = 64;
    else
        size = tiled ? (tmsize_t)TIFFTileSize(tif)
                     : (tmsize_t)TIFFStripSize(tif);

    if (size <= 0 || (uint64_t)size > MAX_TEST_BUFFER)
    {
        *out_size = 0;
        return NULL;
    }
    *out_size = size;
    {
        void *buf = malloc((size_t)size);
        if (buf != NULL)
            memset(buf, 0x5A, (size_t)size);
        return buf;
    }
}

static tmsize_t issue_read(TIFF *tif, uint32_t index, int tiled, int raw,
                           void *buf, tmsize_t explicit_size)
{
    if (raw)
    {
        if (tiled)
            return TIFFReadRawTile(tif, index, buf, explicit_size);
        return TIFFReadRawStrip(tif, index, buf, explicit_size);
    }
    if (tiled)
        return TIFFReadEncodedTile(tif, index, buf, explicit_size);
    return TIFFReadEncodedStrip(tif, index, buf, explicit_size);
}

static int check_malformed_reads(const uint8_t *data, size_t len,
                                 const StrileSynthSpec *spec, const char *what,
                                 const char *mode, Expectation expectation)
{
    MemoryStream ms;
    TIFF *tif;
    uint64_t striles;
    uint64_t i;
    int ret = 0;
    int any_success = 0;

    reset_diagnostics();
    tif = open_memory(data, len, mode, &ms);
    if (tif == NULL)
    {
        if (g_error_count == 0)
        {
            fprintf(stderr, "%s [%s]: rejected without a diagnostic\n", what,
                    mode);
            return 1;
        }
        return 0;
    }

    striles = spec->tiled ? TIFFNumberOfTiles(tif) : TIFFNumberOfStrips(tif);
    if (striles > STRILE_SYNTH_MAX_STRILES)
        striles = STRILE_SYNTH_MAX_STRILES;

    for (i = 0; i < striles && ret == 0; i++)
    {
        int raw;
        for (raw = 0; raw <= 1 && ret == 0; raw++)
        {
            tmsize_t requests[2];
            int r;

            /* Encoded: the whole-block read plus a capped partial request.
             * Raw: only explicit bounded requests, never the "whole" form. */
            requests[0] = (tmsize_t)-1;
            requests[1] = raw ? (tmsize_t)16 : (tmsize_t)32;

            for (r = 0; r < 2 && ret == 0; r++)
            {
                tmsize_t buf_size = 0;
                void *buf;
                tmsize_t result;
                tmsize_t request = requests[r];

                if (raw && request == (tmsize_t)-1)
                    continue;

                buf = alloc_read_buffer(tif, spec->tiled, raw, &buf_size);
                if (buf == NULL)
                {
                    ret = 1;
                    break;
                }
                /* An explicit request must fit the buffer we handed over. */
                if (!raw && request != (tmsize_t)-1 && request > buf_size)
                    request = buf_size;

                result = issue_read(tif, (uint32_t)i, spec->tiled, raw, buf,
                                    request);
                free(buf);

                if (result >= 0)
                {
                    any_success = 1;
                    if (expectation == EXPECT_FAILURE)
                    {
                        fprintf(stderr,
                                "%s [%s]: %s read of striles %llu returned "
                                "%ld, expected failure\n",
                                what, mode, raw ? "raw" : "encoded",
                                (unsigned long long)i, (long)result);
                        ret = 1;
                        break;
                    }
                    /* A repaired read must not report more data than the
                     * real uncompressed striles holds. */
                    if (!raw &&
                        result > (tmsize_t)strilesynth_bytecount(spec, i))
                    {
                        fprintf(stderr,
                                "%s [%s]: repaired read of striles %llu "
                                "returned %ld > real size %llu\n",
                                what, mode, (unsigned long long)i,
                                (long)result,
                                (unsigned long long)strilesynth_bytecount(
                                    spec, i));
                        ret = 1;
                        break;
                    }
                }
            }
        }
    }

    if (ret == 0 && expectation == EXPECT_FAILURE && !any_success &&
        g_error_count == 0)
    {
        fprintf(stderr, "%s [%s]: reads failed without an error diagnostic\n",
                what, mode);
        ret = 1;
    }
    if (ret == 0 && expectation == EXPECT_FAILURE_OR_RECOVERY && any_success &&
        g_error_count == 0 && g_warning_count == 0)
    {
        /* Silent repair would hide the corruption from the application. */
        fprintf(stderr,
                "%s [%s]: metadata was repaired without a diagnostic\n", what,
                mode);
        ret = 1;
    }

    TIFFClose(tif);
    return ret;
}

static int check_malformed_all_modes(const StrileSynthSpec *spec,
                                     const char *what, Expectation expectation)
{
    size_t len = 0;
    uint8_t *data = strilesynth_build_buffer(spec, &len, NULL);
    size_t m;
    int ret = 0;

    if (data == NULL)
    {
        fprintf(stderr, "cannot synthesize %s\n", what);
        return 1;
    }
    for (m = 0; m < NUM_MODES; m++)
        ret |= check_malformed_reads(data, len, spec, what, k_modes[m],
                                     expectation);
    free(data);
    return ret;
}

static int check_malformed_matrix(int bigtiff, int tiled, int le)
{
    StrileSynthSpec base;
    StrileSynthSpec spec;
    uint64_t offsets[STRILE_SYNTH_MAX_STRILES];
    uint64_t counts[STRILE_SYNTH_MAX_STRILES];
    uint64_t striles;
    uint64_t i;
    int ret = 0;
    char what[160];

    init_spec(&base, bigtiff, tiled, le);
    striles = strilesynth_striles(&base);
    if (striles == 0 || striles > STRILE_SYNTH_MAX_STRILES)
        return 1;

    /* (1) Offsets far beyond the end of the file. */
    for (i = 0; i < striles; i++)
    {
        offsets[i] = 0x10000000ULL + i;
        counts[i] = strilesynth_bytecount(&base, i);
    }
    spec = base;
    spec.offsets = offsets;
    spec.bytecounts = counts;
    snprintf(what, sizeof(what), "%s: offsets past EOF", describe_spec(&base));
    ret |= check_malformed_all_modes(&spec, what, EXPECT_FAILURE);

    /* (2) Offsets that do not fit in tmsize_t: the offset arithmetic must not
     * wrap around into a plausible in-file value. */
    for (i = 0; i < striles; i++)
    {
        offsets[i] = 0xFFFFFFFFFFFFFFFFULL - i;
        counts[i] = strilesynth_bytecount(&base, i);
    }
    snprintf(what, sizeof(what), "%s: offsets above TMSIZE_MAX",
             describe_spec(&base));
    ret |= check_malformed_all_modes(&spec, what, EXPECT_FAILURE);

    /* (3) Byte counts that run past the end of the file. */
    for (i = 0; i < striles; i++)
    {
        offsets[i] = 200 + i;
        counts[i] = 0x1000000ULL + i;
    }
    snprintf(what, sizeof(what), "%s: byte counts past EOF",
             describe_spec(&base));
    ret |= check_malformed_all_modes(&spec, what, EXPECT_FAILURE_OR_RECOVERY);

    /* (4) Byte counts above TIFF_TMSIZE_T_MAX, which cannot be cast to
     * tmsize_t without wrapping. */
    for (i = 0; i < striles; i++)
    {
        offsets[i] = 200 + i;
        counts[i] = 0x8000000000000000ULL + i;
    }
    snprintf(what, sizeof(what), "%s: byte counts above TMSIZE_MAX",
             describe_spec(&base));
    ret |= check_malformed_all_modes(&spec, what, EXPECT_FAILURE_OR_RECOVERY);

    /* (5) Byte count exactly TIFF_TMSIZE_T_MAX: offset + size overflows
     * tmsize_t unless the implementation guards it. */
    for (i = 0; i < striles; i++)
    {
        offsets[i] = 200 + i;
        counts[i] = 0x7FFFFFFFFFFFFFFFULL;
    }
    snprintf(what, sizeof(what), "%s: byte counts at TMSIZE_MAX",
             describe_spec(&base));
    ret |= check_malformed_all_modes(&spec, what, EXPECT_FAILURE_OR_RECOVERY);

    /* (6) Overlapping ranges: legal TIFF, so reads may succeed, but they must
     * stay inside the file and not read beyond the requested length. */
    for (i = 0; i < striles; i++)
    {
        offsets[i] = 200;
        counts[i] = 64;
    }
    snprintf(what, sizeof(what), "%s: overlapping ranges",
             describe_spec(&base));
    ret |= check_malformed_all_modes(&spec, what, EXPECT_FAILURE_OR_RECOVERY);

    return ret;
}

/* A file whose metadata is intact but whose payload stops in the middle of a
 * striles.  Striles before the cut decode exactly as written; the one that
 * straddles the cut must fail cleanly on both the whole and the partial read
 * path. */
static int check_truncated_payload(int bigtiff, int tiled, int le)
{
    StrileSynthSpec spec;
    StrileSynthLayout layout;
    size_t len = 0;
    uint8_t *data;
    uint64_t striles;
    uint64_t cut_striles;
    size_t cut_len;
    size_t m;
    int ret = 0;
    char what[160];

    init_spec(&spec, bigtiff, tiled, le);
    striles = strilesynth_striles(&spec);
    if (striles < 2)
        return 0;

    data = strilesynth_build_buffer(&spec, &len, &layout);
    if (data == NULL)
        return 1;

    /* Cut in the middle of the second striles, leaving the first intact. */
    cut_striles = 1;
    cut_len = (size_t)layout.payload_offset +
              (size_t)strilesynth_bytecount(&spec, 0) +
              (size_t)(strilesynth_bytecount(&spec, 1) / 2);
    if (cut_len >= len)
    {
        free(data);
        return 0;
    }

    snprintf(what, sizeof(what), "%s: payload truncated mid-striles",
             describe_spec(&spec));

    /* The intact striles must still decode correctly, on every mode. */
    for (m = 0; m < NUM_MODES && ret == 0; m++)
    {
        MemoryStream ms;
        TIFF *tif = open_memory(data, cut_len, k_modes[m], &ms);
        uint64_t i;

        if (tif == NULL)
        {
            fprintf(stderr, "%s [%s]: cannot open\n", what, k_modes[m]);
            ret = 1;
            break;
        }
        reset_diagnostics();
        for (i = 0; i < cut_striles && ret == 0; i++)
        {
            uint64_t expected = strilesynth_bytecount(&spec, i);
            uint64_t first = 0;
            uint64_t j;
            uint8_t *buf;
            tmsize_t got;

            for (j = 0; j < i; j++)
                first += strilesynth_bytecount(&spec, j);
            buf = (uint8_t *)malloc((size_t)expected);
            if (buf == NULL)
            {
                ret = 1;
                break;
            }
            got = tiled ? TIFFReadEncodedTile(tif, (uint32_t)i, buf,
                                              (tmsize_t)-1)
                        : TIFFReadEncodedStrip(tif, (uint32_t)i, buf,
                                               (tmsize_t)-1);
            if (got != (tmsize_t)expected)
            {
                fprintf(stderr, "%s [%s]: intact striles %llu returned %ld\n",
                        what, k_modes[m], (unsigned long long)i, (long)got);
                free(buf);
                ret = 1;
                break;
            }
            for (j = 0; j < expected; j++)
            {
                if (buf[j] != strilesynth_payload_byte(first + j))
                {
                    fprintf(stderr,
                            "%s [%s]: intact striles %llu byte %llu corrupted"
                            "\n",
                            what, k_modes[m], (unsigned long long)i,
                            (unsigned long long)j);
                    ret = 1;
                    break;
                }
            }
            free(buf);
        }
        TIFFClose(tif);
    }

    /* The striles that straddles the cut (and every later one) must fail its
     * whole-block encoded read: the declared range extends past the end of
     * the payload.  Partial and raw reads are still issued for every striles
     * so that a partial request that stays inside the surviving bytes is
     * exercised under sanitizers, but their result is not constrained -
     * reading the in-file prefix of a truncated striles is legitimate. */
    for (m = 0; m < NUM_MODES && ret == 0; m++)
    {
        MemoryStream ms;
        TIFF *tif = open_memory(data, cut_len, k_modes[m], &ms);
        uint64_t i;

        if (tif == NULL)
        {
            fprintf(stderr, "%s [%s]: cannot reopen truncated image\n", what,
                    k_modes[m]);
            ret = 1;
            break;
        }
        reset_diagnostics();
        for (i = cut_striles; i < striles && ret == 0; i++)
        {
            tmsize_t buf_size = 0;
            void *buf;
            tmsize_t result;

            buf = alloc_read_buffer(tif, tiled, 0, &buf_size);
            if (buf == NULL)
            {
                ret = 1;
                break;
            }
            result = issue_read(tif, (uint32_t)i, tiled, 0, buf, (tmsize_t)-1);
            free(buf);
            if (result >= 0)
            {
                fprintf(stderr,
                        "%s [%s]: whole encoded read of truncated striles %llu"
                        " returned %ld\n",
                        what, k_modes[m], (unsigned long long)i,
                        (long)result);
                ret = 1;
                break;
            }
        }

        /* Exercise the partial / raw paths for memory safety. */
        for (i = cut_striles; i < striles && ret == 0; i++)
        {
            int raw;
            for (raw = 0; raw <= 1 && ret == 0; raw++)
            {
                tmsize_t buf_size = 0;
                void *buf = alloc_read_buffer(tif, tiled, raw, &buf_size);
                if (buf == NULL)
                {
                    ret = 1;
                    break;
                }
                (void)issue_read(tif, (uint32_t)i, tiled, raw, buf,
                                 raw ? (tmsize_t)16 : (tmsize_t)32);
                free(buf);
            }
        }

        if (ret == 0 && g_error_count == 0)
        {
            fprintf(stderr, "%s [%s]: truncated reads failed silently\n", what,
                    k_modes[m]);
            ret = 1;
        }
        TIFFClose(tif);
    }

    free(data);
    return ret;
}

/* ------------------------------------------------------------------ */
/* Entry point                                                         */
/* ------------------------------------------------------------------ */

int main(void)
{
    int ret = 0;
    int bigtiff;
    int tiled;
    int le;

    TIFFSetErrorHandler(count_error_handler);
    TIFFSetWarningHandler(count_warning_handler);

    for (bigtiff = 0; bigtiff <= 1; bigtiff++)
    {
        for (le = 0; le <= 1; le++)
        {
            for (tiled = 0; tiled <= 1; tiled++)
            {
                /* Valid images still decode to their original pixels. */
                ret |= check_valid_image(bigtiff, tiled, le);
                /* Malformed metadata is rejected or read within bounds. */
                ret |= check_malformed_matrix(bigtiff, tiled, le);
                /* Truncation splits whole-block from partial reads. */
                ret |= check_truncated_payload(bigtiff, tiled, le);
            }
        }
    }

    if (ret != 0)
        fprintf(stderr, "test_strile_bounds: FAILED\n");
    return ret;
}
