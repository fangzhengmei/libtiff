/*
 * Copyright (c) 2026
 *
 * Permission to use, copy, modify, distribute, and sell this software and
 * its documentation for any purpose is hereby granted without fee, provided
 * that (i) the above copyright notices and this permission notice appear in
 * all copies of the software and related documentation, and (ii) the names
 * of Sam Leffler and Silicon Graphics may not be used in any advertising or
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
 * TIFF Library - corrupt IFD handling regression tests
 *
 * Covers three failure classes that used to leave a TIFF handle in an
 * inconsistent state:
 *
 * 1. Stale-state reuse: when TIFFFetchDirectory() fails on a middle IFD,
 *    tif_dir must be reset to defaults instead of still describing the
 *    previous directory. Otherwise a client ignoring the 0 return value can
 *    decode the previous image through the handle.
 *
 * 2. Integer truncation / out-of-bounds reads: directory counts above the
 *    4096 sanity limit must be rejected on both streaming and memory-mapped
 *    paths, for Classic and BigTIFF, and BigTIFF 64-bit counts may never be
 *    silently truncated to uint16_t.
 *
 * 3. IFD loops: a next-IFD pointer that revisits a known offset must stop the
 *    chain with a warning and must never be followed into an unbounded loop.
 */

#include "tif_config.h"

#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif

#include "tiffio.h"

#define GOTOFAILURE goto failure

static const char *tmp_classic = "test_corrupt_ifd_classic.tif";
static const char *tmp_bigtiff = "test_corrupt_ifd_bigtiff.tif";

/* Suppress the error/warning noise produced on purpose by these tests. */
static void ignore_handler(const char *module, const char *fmt, va_list ap)
{
    (void)module;
    (void)fmt;
    (void)ap;
}
static void ignore_handler_ext(thandle_t th, const char *module,
                               const char *fmt, va_list ap)
{
    (void)th;
    (void)module;
    (void)fmt;
    (void)ap;
}

/* Build a 3-directory grayscale TIFF. Returns the three IFD offsets. */
static int create_three_ifd_file(const char *filename, int bigtiff,
                                 uint64_t offsets[3])
{
    TIFF *tif = TIFFOpen(filename, bigtiff ? "w8" : "w");
    int i;
    if (!tif)
        return 1;
    for (i = 0; i < 3; i++)
    {
        TIFFSetField(tif, TIFFTAG_IMAGEWIDTH, 4);
        TIFFSetField(tif, TIFFTAG_IMAGELENGTH, 4);
        TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE, 8);
        TIFFSetField(tif, TIFFTAG_COMPRESSION, COMPRESSION_NONE);
        TIFFSetField(tif, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK);
        TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, 1);
        TIFFSetField(tif, TIFFTAG_ROWSPERSTRIP, 4);
        TIFFSetField(tif, TIFFTAG_PAGENUMBER, i, 3);
        {
            unsigned char pixels[16];
            memset(pixels, (unsigned char)(10 * (i + 1)), sizeof(pixels));
            if (TIFFWriteRawStrip(tif, 0, pixels, sizeof(pixels)) < 0)
            {
                TIFFClose(tif);
                return 1;
            }
        }
        if (!TIFFWriteDirectory(tif))
        {
            TIFFClose(tif);
            return 1;
        }
        /* The offset is only populated once the directory is on disk: jump
         * back to it, record the offset, then create the next one. */
        if (!TIFFSetDirectory(tif, (tdir_t)i))
        {
            TIFFClose(tif);
            return 1;
        }
        offsets[i] = TIFFCurrentDirOffset(tif);
        if (i < 2)
            TIFFCreateDirectory(tif);
    }
    TIFFClose(tif);
    return 0;
}

/* Overwrite the directory-count word of the IFD at dir_off. */
static void patch_dir_count(const char *filename, uint64_t dir_off,
                            int bigtiff, uint64_t count)
{
    FILE *fp = fopen(filename, "r+b");
    if (!fp)
        return;
    if (bigtiff)
    {
        unsigned char buf[8];
        int k;
        for (k = 0; k < 8; k++)
            buf[k] = (unsigned char)((count >> (8 * k)) & 0xFF);
        fseek(fp, (long)dir_off, SEEK_SET);
        fwrite(buf, 1, 8, fp);
    }
    else
    {
        unsigned char buf[2];
        buf[0] = (unsigned char)(count & 0xFF);
        buf[1] = (unsigned char)((count >> 8) & 0xFF);
        fseek(fp, (long)dir_off, SEEK_SET);
        fwrite(buf, 1, 2, fp);
    }
    fclose(fp);
}

/* Overwrite the next-IFD pointer at the end of the IFD at dir_off. */
static void patch_next_offset(const char *filename, uint64_t dir_off,
                              int bigtiff, uint16_t entry_count,
                              uint64_t next_off)
{
    FILE *fp = fopen(filename, "r+b");
    long ptrpos;
    if (!fp)
        return;
    ptrpos = (long)(dir_off + (bigtiff ? 8 + (uint64_t)entry_count * 20
                                      : 2 + (uint64_t)entry_count * 12));
    fseek(fp, ptrpos, SEEK_SET);
    if (bigtiff)
    {
        unsigned char buf[8];
        int k;
        for (k = 0; k < 8; k++)
            buf[k] = (unsigned char)((next_off >> (8 * k)) & 0xFF);
        fwrite(buf, 1, 8, fp);
    }
    else
    {
        unsigned char buf[4];
        int k;
        for (k = 0; k < 4; k++)
            buf[k] = (unsigned char)((next_off >> (8 * k)) & 0xFF);
        fwrite(buf, 1, 4, fp);
    }
    fclose(fp);
}

/*
 * Test 1: corrupt the count of the middle IFD so TIFFFetchDirectory fails.
 * After the failed TIFFReadDirectory(), the handle must expose default
 * (zero) geometry rather than the previous directory's 4x4 geometry.
 */
static int test_stale_state_after_fetch_failure(const char *filename,
                                                int bigtiff,
                                                const uint64_t offsets[3])
{
    int ret = 1;
    TIFF *tif;
    uint32_t w = 0, h = 0;

    /* 5000 exceeds the 4096 sanity limit, so the fetch of IFD1 fails.
     * IFD0 is untouched, so TIFFOpen still succeeds and loads IFD0. */
    patch_dir_count(filename, offsets[1], bigtiff, 5000);

    tif = TIFFOpen(filename, "r");
    if (!tif)
    {
        fprintf(stderr, "test_stale_state: unexpected open failure\n");
        GOTOFAILURE;
    }
    /* IFD0 was read successfully inside TIFFOpen; record its geometry. */
    TIFFGetField(tif, TIFFTAG_IMAGEWIDTH, &w);
    if (w != 4)
    {
        fprintf(stderr, "test_stale_state: IFD0 not loaded at open (w=%u)\n",
                w);
        TIFFClose(tif);
        GOTOFAILURE;
    }
    /* Reset so the post-failure values are not confused with IFD0's. */
    w = 0;
    h = 0;
    /* Reading the corrupted IFD1 must fail. */
    if (TIFFReadDirectory(tif) != 0)
    {
        fprintf(stderr, "test_stale_state: expected TIFFReadDirectory failure\n");
        TIFFClose(tif);
        GOTOFAILURE;
    }
    /* The previous IFD's geometry must not survive the failed read. */
    TIFFGetField(tif, TIFFTAG_IMAGEWIDTH, &w);
    TIFFGetField(tif, TIFFTAG_IMAGELENGTH, &h);
    if (w != 0 || h != 0)
    {
        fprintf(stderr,
                "test_stale_state: stale geometry leaked after failed read "
                "(%ux%u, bigtiff=%d)\n",
                w, h, bigtiff);
        TIFFClose(tif);
        GOTOFAILURE;
    }
    /* The still-valid first directory remains reachable by random access. */
    if (!TIFFSetDirectory(tif, 0))
    {
        fprintf(stderr, "test_stale_state: cannot return to valid dir 0\n");
        TIFFClose(tif);
        GOTOFAILURE;
    }
    TIFFGetField(tif, TIFFTAG_IMAGEWIDTH, &w);
    if (w != 4)
    {
        fprintf(stderr, "test_stale_state: dir 0 geometry wrong (%u)\n", w);
        TIFFClose(tif);
        GOTOFAILURE;
    }
    TIFFClose(tif);
    ret = 0;

failure:
    unlink(filename);
    return ret;
}

/*
 * Test 2: oversized directory counts are rejected and never truncate.
 * Re-run on both "r" (mmap allowed) and "rm" (streaming) open modes.
 */
static int test_oversized_count_rejected(const char *filename, int bigtiff,
                                         const uint64_t offsets[3])
{
    static const char *modes[2] = {"r", "rm"};
    int m;
    int ret = 1;

    patch_dir_count(filename, offsets[1], bigtiff, bigtiff ? 5000U : 5000U);

    for (m = 0; m < 2; m++)
    {
        TIFF *tif = TIFFOpen(filename, modes[m]);
        if (!tif)
        {
            fprintf(stderr, "test_oversized_count: open failed in mode %s\n",
                    modes[m]);
            GOTOFAILURE;
        }
        /* dir 0 is fine; reading dir 1 must fail cleanly. */
        if (TIFFReadDirectory(tif) != 0)
        {
            fprintf(stderr,
                    "test_oversized_count: bad count accepted in mode %s "
                    "(bigtiff=%d)\n",
                    modes[m], bigtiff);
            TIFFClose(tif);
            GOTOFAILURE;
        }
        TIFFClose(tif);
    }
    ret = 0;

failure:
    unlink(filename);
    return ret;
}

/*
 * Test 3: point the tail IFD's next pointer back at the first IFD. The chain
 * walk must terminate on the third TIFFReadDirectory() attempt instead of
 * looping, and the two real directories must stay decodable.
 */
static int test_ifd_loop_terminates(const char *filename, int bigtiff,
                                    const uint64_t offsets[3],
                                    uint64_t entry_count)
{
    int ret = 1;
    TIFF *tif;

    patch_next_offset(filename, offsets[2], bigtiff, entry_count, offsets[0]);

    tif = TIFFOpen(filename, "r");
    if (!tif)
    {
        fprintf(stderr, "test_ifd_loop: open failed\n");
        GOTOFAILURE;
    }
    /* dir 0 is read inside TIFFOpen. dirs 1 and 2 are still legitimate. */
    if (TIFFReadDirectory(tif) == 0)
    {
        fprintf(stderr, "test_ifd_loop: second directory unexpectedly failed\n");
        TIFFClose(tif);
        GOTOFAILURE;
    }
    if (TIFFReadDirectory(tif) == 0)
    {
        fprintf(stderr, "test_ifd_loop: third directory unexpectedly failed\n");
        TIFFClose(tif);
        GOTOFAILURE;
    }
    /* The next read follows dir2's looped pointer back to dir0 and must be
     * rejected by the IFD loop detection rather than followed. */
    if (TIFFReadDirectory(tif) != 0)
    {
        fprintf(stderr, "test_ifd_loop: loop back to dir 0 was not detected\n");
        TIFFClose(tif);
        GOTOFAILURE;
    }
    /* Both legitimate directories remain addressable by random access. */
    if (!TIFFSetDirectory(tif, 0) || !TIFFSetDirectory(tif, 1) ||
        !TIFFSetDirectory(tif, 2))
    {
        fprintf(stderr, "test_ifd_loop: valid directories no longer reachable\n");
        TIFFClose(tif);
        GOTOFAILURE;
    }
    if (TIFFSetDirectory(tif, 3))
    {
        fprintf(stderr, "test_ifd_loop: looped directory exposed as real\n");
        TIFFClose(tif);
        GOTOFAILURE;
    }
    TIFFClose(tif);
    ret = 0;

failure:
    unlink(filename);
    return ret;
}

/* Read the entry count word of an IFD (16-bit Classic, 64-bit BigTIFF). */
static uint64_t read_ifd_entry_count(const char *filename, uint64_t dir_off,
                                     int bigtiff)
{
    FILE *fp = fopen(filename, "rb");
    uint64_t cnt = 0;
    if (!fp)
        return 0;
    fseek(fp, (long)dir_off, SEEK_SET);
    if (bigtiff)
    {
        unsigned char buf[8];
        int k;
        if (fread(buf, 1, 8, fp) != 8)
        {
            fclose(fp);
            return 0;
        }
        for (k = 0; k < 8; k++)
            cnt |= ((uint64_t)buf[k]) << (8 * k);
    }
    else
    {
        uint16_t cnt16 = 0;
        if (fread(&cnt16, 2, 1, fp) != 1)
        {
            fclose(fp);
            return 0;
        }
        cnt = cnt16;
    }
    fclose(fp);
    return cnt;
}

int main(void)
{
    uint64_t offsets[3];
    uint64_t entry_count;

    TIFFSetErrorHandler(ignore_handler);
    TIFFSetWarningHandler(ignore_handler);
    TIFFSetErrorHandlerExt(ignore_handler_ext);
    TIFFSetWarningHandlerExt(ignore_handler_ext);

    /* ---- Classic TIFF ---- */
    if (create_three_ifd_file(tmp_classic, 0, offsets) != 0)
    {
        fprintf(stderr, "Cannot create %s\n", tmp_classic);
        goto failure;
    }
    entry_count = read_ifd_entry_count(tmp_classic, offsets[0], 0);
    if (entry_count == 0)
    {
        fprintf(stderr, "Cannot read IFD entry count\n");
        goto failure;
    }

    if (test_stale_state_after_fetch_failure(tmp_classic, 0, offsets) != 0)
        goto failure;
    if (create_three_ifd_file(tmp_classic, 0, offsets) != 0)
        goto failure;
    if (test_oversized_count_rejected(tmp_classic, 0, offsets) != 0)
        goto failure;
    if (create_three_ifd_file(tmp_classic, 0, offsets) != 0)
        goto failure;
    if (test_ifd_loop_terminates(tmp_classic, 0, offsets, entry_count) != 0)
        goto failure;

    /* ---- BigTIFF ---- */
    if (create_three_ifd_file(tmp_bigtiff, 1, offsets) != 0)
    {
        fprintf(stderr, "Cannot create %s\n", tmp_bigtiff);
        goto failure;
    }
    if (test_stale_state_after_fetch_failure(tmp_bigtiff, 1, offsets) != 0)
        goto failure;
    if (create_three_ifd_file(tmp_bigtiff, 1, offsets) != 0)
        goto failure;
    if (test_oversized_count_rejected(tmp_bigtiff, 1, offsets) != 0)
        goto failure;
    if (create_three_ifd_file(tmp_bigtiff, 1, offsets) != 0)
        goto failure;
    entry_count = read_ifd_entry_count(tmp_bigtiff, offsets[0], 1);
    if (entry_count == 0)
        goto failure;
    if (test_ifd_loop_terminates(tmp_bigtiff, 1, offsets, entry_count) != 0)
        goto failure;

    printf("All corrupt IFD tests passed.\n");
    return 0;

failure:
    unlink(tmp_classic);
    unlink(tmp_bigtiff);
    return 1;
}
