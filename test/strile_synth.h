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
 * Helper to synthesize deliberately malformed TIFF images whose strip/tile
 * offsets and byte counts are fully caller-controlled, plus a deterministic
 * payload so that well-formed images can be checked pixel by pixel.
 *
 * The generator produces a minimal, sorted IFD for a single-block image
 * (uncompressed by default), either stripped or tiled, classic TIFF or
 * BigTIFF, in either byte order.  Strip/tile offset and byte count arrays may
 * be replaced by caller-supplied values, which makes it possible to express
 * offsets beyond the end of the file, hugely oversized byte counts,
 * overlapping ranges and truncated payloads.
 *
 * Images are built into a heap buffer so that callers can either write them
 * to disk or serve them to libtiff from memory (via TIFFClientOpen), and can
 * simulate truncation simply by using a shorter buffer.
 */

#ifndef STRILE_SYNTH_H
#define STRILE_SYNTH_H

#include <stddef.h>
#include <stdint.h>

/* Layout selectors. */
#define STRILE_SYNTH_STRIPS 0
#define STRILE_SYNTH_TILES 1

/* Maximum number of strips/tiles the generator will emit. */
#define STRILE_SYNTH_MAX_STRILES 64

typedef struct StrileSynthSpec
{
    int bigtiff;       /* 0: classic TIFF, 1: BigTIFF */
    int tiled;         /* STRILE_SYNTH_STRIPS or STRILE_SYNTH_TILES */
    int little_endian; /* 1: "II", 0: "MM" */

    uint32_t width;
    uint32_t height;

    /* Stripped layout only. */
    uint32_t rows_per_strip;

    /* Tiled layout only. */
    uint32_t tile_width;
    uint32_t tile_length;

    uint16_t bits_per_sample;
    uint16_t samples_per_pixel;
    uint16_t compression;
    uint16_t photometric;
    uint16_t planar_config;

    /* Optional replacement arrays.  NULL selects a valid, contiguous layout,
     * in which case each entry is exactly as large as the uncompressed data.
     * Non-NULL arrays must hold strilesynth_striles(spec) entries. */
    const uint64_t *offsets;
    const uint64_t *bytecounts;

    /* Extra payload bytes appended after the striles' data.  Lets a test
     * place data after the last striles so that truncation can fall between
     * two striles. */
    uint64_t trailing_bytes;
} StrileSynthSpec;

typedef struct StrileSynthLayout
{
    uint64_t file_size;      /* total size of the generated image */
    uint64_t payload_offset; /* file offset of the first payload byte */
    uint64_t striles;        /* number of strips / tiles */
} StrileSynthLayout;

/* Number of strips/tiles implied by the geometry. */
uint64_t strilesynth_striles(const StrileSynthSpec *spec);

/* Uncompressed byte count of strip/tile index under the automatic layout.
 * Returns 0 for an out-of-range index. */
uint64_t strilesynth_bytecount(const StrileSynthSpec *spec, uint64_t index);

/* Deterministic payload byte for an index inside the payload area. */
uint8_t strilesynth_payload_byte(uint64_t payload_index);

/* Payload index holding pixel (x, y), accounting for either layout.  Returns
 * ((uint64_t)-1) when the coordinate is outside the image. */
uint64_t strilesynth_payload_index(const StrileSynthSpec *spec, uint64_t x,
                                   uint64_t y);

/* Build the image into a heap buffer.  Returns the buffer (caller frees) and
 * stores its length, or returns NULL on error.  layout may be NULL. */
uint8_t *strilesynth_build_buffer(const StrileSynthSpec *spec, size_t *out_len,
                                  StrileSynthLayout *layout);

/* Build the image and write it to path.  Returns the file size, or -1. */
int64_t strilesynth_write_file(const char *path,
                               const StrileSynthSpec *spec,
                               StrileSynthLayout *layout);

#endif /* STRILE_SYNTH_H */
