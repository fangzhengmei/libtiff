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
 * Implementation of the malformed-strile TIFF synthesizer (see
 * strile_synth.h).
 */

#include "tif_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tiffio.h"

#include "strile_synth.h"

#define SYNTH_TYPE_SHORT 3
#define SYNTH_TYPE_LONG 4
#define SYNTH_TYPE_LONG8 16

#define SYNTH_TAG_IMAGEWIDTH 256
#define SYNTH_TAG_IMAGELENGTH 257
#define SYNTH_TAG_BITSPERSAMPLE 258
#define SYNTH_TAG_COMPRESSION 259
#define SYNTH_TAG_PHOTOMETRIC 262
#define SYNTH_TAG_STRIPOFFSETS 273
#define SYNTH_TAG_SAMPLESPERPIXEL 277
#define SYNTH_TAG_ROWSPERSTRIP 278
#define SYNTH_TAG_STRIPBYTECOUNTS 279
#define SYNTH_TAG_PLANARCONFIG 284
#define SYNTH_TAG_TILEWIDTH 322
#define SYNTH_TAG_TILELENGTH 323
#define SYNTH_TAG_TILEOFFSETS 324
#define SYNTH_TAG_TILEBYTECOUNTS 325

#define SYNTH_MAX_ENTRIES 16
#define SYNTH_MAX_STRILES 64

typedef struct
{
    uint16_t tag;
    uint16_t type;
    uint64_t count;
    const uint64_t *values;
    int scalar;     /* the value field holds a single scalar */
    int array_slot; /* >= 0 when the values live in an array slot */
} SynthEntry;

typedef struct
{
    uint8_t *data;
    size_t size;
    size_t capacity;
} SynthBuffer;

static uint64_t synth_typesize(uint16_t type)
{
    if (type == SYNTH_TYPE_SHORT)
        return 2;
    if (type == SYNTH_TYPE_LONG)
        return 4;
    return 8;
}

static int synth_reserve(SynthBuffer *buf, size_t extra)
{
    size_t needed = buf->size + extra;
    size_t capacity;
    uint8_t *data;

    if (needed <= buf->capacity)
        return 1;

    capacity = buf->capacity ? buf->capacity : 1024;
    while (capacity < needed)
    {
        if (capacity > ((size_t)-1) / 2)
            return 0;
        capacity *= 2;
    }
    data = (uint8_t *)realloc(buf->data, capacity);
    if (data == NULL)
        return 0;
    buf->data = data;
    buf->capacity = capacity;
    return 1;
}

static int synth_put(SynthBuffer *buf, const void *bytes, size_t n)
{
    if (!synth_reserve(buf, n))
        return 0;
    memcpy(buf->data + buf->size, bytes, n);
    buf->size += n;
    return 1;
}

static int synth_put_u16(SynthBuffer *buf, int le, uint16_t v)
{
    unsigned char b[2];
    if (le)
    {
        b[0] = (unsigned char)(v & 0xff);
        b[1] = (unsigned char)((v >> 8) & 0xff);
    }
    else
    {
        b[0] = (unsigned char)((v >> 8) & 0xff);
        b[1] = (unsigned char)(v & 0xff);
    }
    return synth_put(buf, b, sizeof(b));
}

static int synth_put_u32(SynthBuffer *buf, int le, uint32_t v)
{
    unsigned char b[4];
    int i;
    for (i = 0; i < 4; i++)
    {
        int shift = le ? 8 * i : 8 * (3 - i);
        b[i] = (unsigned char)((v >> shift) & 0xff);
    }
    return synth_put(buf, b, sizeof(b));
}

static int synth_put_u64(SynthBuffer *buf, int le, uint64_t v)
{
    unsigned char b[8];
    int i;
    for (i = 0; i < 8; i++)
    {
        int shift = le ? 8 * i : 8 * (7 - i);
        b[i] = (unsigned char)((v >> shift) & 0xff);
    }
    return synth_put(buf, b, sizeof(b));
}

static int synth_put_value(SynthBuffer *buf, int le, uint16_t type, uint64_t v)
{
    if (type == SYNTH_TYPE_SHORT)
        return synth_put_u16(buf, le, (uint16_t)v);
    if (type == SYNTH_TYPE_LONG)
        return synth_put_u32(buf, le, (uint32_t)v);
    return synth_put_u64(buf, le, v);
}

static int synth_entry_cmp(const void *pa, const void *pb)
{
    const SynthEntry *a = (const SynthEntry *)pa;
    const SynthEntry *b = (const SynthEntry *)pb;
    if (a->tag < b->tag)
        return -1;
    if (a->tag > b->tag)
        return 1;
    return 0;
}

uint64_t strilesynth_striles(const StrileSynthSpec *spec)
{
    uint64_t width = spec->width;
    uint64_t height = spec->height;

    if (spec->tiled)
    {
        uint64_t tw = spec->tile_width ? spec->tile_width : 1;
        uint64_t th = spec->tile_length ? spec->tile_length : 1;
        return ((width + tw - 1) / tw) * ((height + th - 1) / th);
    }

    if (spec->rows_per_strip == 0)
        return 0;
    if (spec->rows_per_strip >= height)
        return 1;
    return (height + spec->rows_per_strip - 1) / spec->rows_per_strip;
}

uint64_t strilesynth_bytecount(const StrileSynthSpec *spec, uint64_t index)
{
    uint64_t striles = strilesynth_striles(spec);
    uint64_t bits =
        (uint64_t)spec->bits_per_sample * (uint64_t)spec->samples_per_pixel;
    uint64_t rowbytes;
    uint64_t rows;
    uint64_t first_row;

    if (index >= striles)
        return 0;

    if (spec->tiled)
    {
        rowbytes = ((uint64_t)spec->tile_width * bits + 7) / 8;
        return rowbytes * (uint64_t)spec->tile_length;
    }

    rows = spec->rows_per_strip;
    first_row = index * rows;
    rowbytes = ((uint64_t)spec->width * bits + 7) / 8;
    if (first_row >= spec->height)
        return 0;
    if (rows > spec->height - first_row)
        rows = spec->height - first_row;
    return rowbytes * rows;
}

uint8_t strilesynth_payload_byte(uint64_t payload_index)
{
    return (uint8_t)(0x10 + (payload_index % 251));
}

uint64_t strilesynth_payload_index(const StrileSynthSpec *spec, uint64_t x,
                                   uint64_t y)
{
    uint64_t bits;
    uint64_t first = 0;
    uint64_t striles;
    uint64_t index;
    uint64_t local;
    uint64_t i;

    if (x >= spec->width || y >= spec->height)
        return (uint64_t)-1;

    /* Only the 8-bit, single-sample layouts used by the tests are supported. */
    bits = (uint64_t)spec->bits_per_sample * spec->samples_per_pixel;
    if (bits != 8 || spec->planar_config != PLANARCONFIG_CONTIG)
        return (uint64_t)-1;

    if (spec->tiled)
    {
        uint64_t across =
            (spec->width + spec->tile_width - 1) / spec->tile_width;
        uint64_t tx = x / spec->tile_width;
        uint64_t ty = y / spec->tile_length;
        uint64_t rowbytes = (uint64_t)spec->tile_width * bits / 8;

        index = ty * across + tx;
        local = (y % spec->tile_length) * rowbytes + (x % spec->tile_width);
    }
    else
    {
        uint64_t rowbytes = (uint64_t)spec->width * bits / 8;

        index = y / spec->rows_per_strip;
        local = (y % spec->rows_per_strip) * rowbytes + x;
    }

    striles = strilesynth_striles(spec);
    if (index >= striles)
        return (uint64_t)-1;
    for (i = 0; i < index; i++)
        first += strilesynth_bytecount(spec, i);
    return first + local;
}

uint8_t *strilesynth_build_buffer(const StrileSynthSpec *spec, size_t *out_len,
                                  StrileSynthLayout *layout)
{
    const int big = spec->bigtiff;
    const int le = spec->little_endian;
    const uint64_t striles = strilesynth_striles(spec);
    const uint64_t entry_size = big ? 20 : 12;
    const uint64_t inline_max = big ? 8 : 4;
    const uint16_t array_type = big ? SYNTH_TYPE_LONG8 : SYNTH_TYPE_LONG;

    SynthEntry entries[SYNTH_MAX_ENTRIES];
    uint64_t scalars[SYNTH_MAX_ENTRIES];
    uint64_t auto_offsets[SYNTH_MAX_STRILES];
    uint64_t auto_counts[SYNTH_MAX_STRILES];
    uint64_t payload_lens[SYNTH_MAX_STRILES];
    uint64_t slot_offsets[2];
    SynthBuffer buf;
    uint64_t ifd_offset;
    uint64_t ifd_size;
    uint64_t arrays_offset;
    uint64_t payload_offset;
    uint64_t payload_len = 0;
    uint64_t i;
    int nentries = 0;
    int k;

    memset(&buf, 0, sizeof(buf));
    memset(entries, 0, sizeof(entries));
    memset(scalars, 0, sizeof(scalars));

    if (striles == 0 || striles > SYNTH_MAX_STRILES)
        return NULL;

    /* Assemble the IFD entries. */
#define SYNTH_ADD(tag_, type_, count_, values_, is_scalar, slot)              \
    do                                                                        \
    {                                                                         \
        entries[nentries].tag = (tag_);                                       \
        entries[nentries].type = (type_);                                     \
        entries[nentries].count = (count_);                                   \
        entries[nentries].values = (values_);                                 \
        entries[nentries].scalar = (is_scalar);                               \
        entries[nentries].array_slot = (slot);                                \
        nentries++;                                                           \
    } while (0)

    scalars[0] = spec->width;
    SYNTH_ADD(SYNTH_TAG_IMAGEWIDTH, SYNTH_TYPE_LONG, 1, &scalars[0], 1, -1);
    scalars[1] = spec->height;
    SYNTH_ADD(SYNTH_TAG_IMAGELENGTH, SYNTH_TYPE_LONG, 1, &scalars[1], 1, -1);
    scalars[2] = spec->bits_per_sample;
    SYNTH_ADD(SYNTH_TAG_BITSPERSAMPLE, SYNTH_TYPE_SHORT, 1, &scalars[2], 1, -1);
    scalars[3] = spec->compression;
    SYNTH_ADD(SYNTH_TAG_COMPRESSION, SYNTH_TYPE_SHORT, 1, &scalars[3], 1, -1);
    scalars[4] = spec->photometric;
    SYNTH_ADD(SYNTH_TAG_PHOTOMETRIC, SYNTH_TYPE_SHORT, 1, &scalars[4], 1, -1);
    if (spec->tiled)
    {
        scalars[5] = spec->tile_width;
        SYNTH_ADD(SYNTH_TAG_TILEWIDTH, SYNTH_TYPE_LONG, 1, &scalars[5], 1, -1);
        scalars[6] = spec->tile_length;
        SYNTH_ADD(SYNTH_TAG_TILELENGTH, SYNTH_TYPE_LONG, 1, &scalars[6], 1, -1);
    }
    else
    {
        scalars[5] = spec->rows_per_strip;
        SYNTH_ADD(SYNTH_TAG_ROWSPERSTRIP, SYNTH_TYPE_LONG, 1, &scalars[5], 1,
                  -1);
    }
    scalars[7] = spec->samples_per_pixel;
    SYNTH_ADD(SYNTH_TAG_SAMPLESPERPIXEL, SYNTH_TYPE_SHORT, 1, &scalars[7], 1,
              -1);
    scalars[8] = spec->planar_config;
    SYNTH_ADD(SYNTH_TAG_PLANARCONFIG, SYNTH_TYPE_SHORT, 1, &scalars[8], 1, -1);
    SYNTH_ADD(spec->tiled ? SYNTH_TAG_TILEOFFSETS : SYNTH_TAG_STRIPOFFSETS,
              array_type, striles, auto_offsets, 0, 0);
    SYNTH_ADD(spec->tiled ? SYNTH_TAG_TILEBYTECOUNTS : SYNTH_TAG_STRIPBYTECOUNTS,
              array_type, striles, auto_counts, 0, 1);

    /* The TIFF specification requires entries sorted by tag. */
    qsort(entries, (size_t)nentries, sizeof(entries[0]), synth_entry_cmp);

    /* Lay out the file: header, IFD, out-of-line arrays, payload. */
    ifd_offset = big ? 16 : 8;
    ifd_size = (big ? 8 : 2) + (uint64_t)nentries * entry_size + (big ? 8 : 4);
    arrays_offset = ifd_offset + ifd_size;

    slot_offsets[0] = arrays_offset;
    slot_offsets[1] = arrays_offset;

    {
        /* Arrays shrink to the value field when they fit; count the room they
         * actually need. */
        uint64_t cursor = arrays_offset;
        uint64_t sizes[2] = {0, 0};
        int e;

        for (e = 0; e < nentries; e++)
        {
            uint64_t nbytes =
                synth_typesize(entries[e].type) * entries[e].count;
            if (entries[e].array_slot < 0)
                continue;
            if (nbytes > inline_max)
            {
                slot_offsets[entries[e].array_slot] = cursor;
                sizes[entries[e].array_slot] = nbytes;
                cursor += nbytes;
            }
            else
            {
                slot_offsets[entries[e].array_slot] = 0;
                sizes[entries[e].array_slot] = 0;
            }
        }
        payload_offset = cursor;
        (void)sizes;
    }

    /* Automatic striles layout: contiguous, each sized to its data. */
    for (i = 0; i < striles; i++)
        payload_lens[i] = strilesynth_bytecount(spec, i);

    for (i = 0; i < striles; i++)
    {
        uint64_t off = payload_offset;
        uint64_t j;
        for (j = 0; j < i; j++)
            off += payload_lens[j];
        auto_offsets[i] = spec->offsets ? spec->offsets[i] : off;
        auto_counts[i] =
            spec->bytecounts ? spec->bytecounts[i] : payload_lens[i];
    }

    for (i = 0; i < striles; i++)
        payload_len += payload_lens[i];
    payload_len += spec->trailing_bytes;

    /* Header. */
    if (!synth_put(&buf, le ? "II" : "MM", 2))
        goto error;
    if (big)
    {
        if (!synth_put_u16(&buf, le, 43) || !synth_put_u16(&buf, le, 8) ||
            !synth_put_u16(&buf, le, 0) ||
            !synth_put_u64(&buf, le, ifd_offset) ||
            !synth_put_u64(&buf, le, (uint64_t)nentries))
            goto error;
    }
    else
    {
        if (!synth_put_u16(&buf, le, 42) ||
            !synth_put_u32(&buf, le, (uint32_t)ifd_offset) ||
            !synth_put_u16(&buf, le, (uint16_t)nentries))
            goto error;
    }

    /* IFD entries plus their inline values. */
    for (k = 0; k < nentries; k++)
    {
        uint64_t nbytes = synth_typesize(entries[k].type) * entries[k].count;

        if (!synth_put_u16(&buf, le, entries[k].tag) ||
            !synth_put_u16(&buf, le, entries[k].type))
            goto error;
        if (big)
        {
            if (!synth_put_u64(&buf, le, entries[k].count))
                goto error;
        }
        else
        {
            if (!synth_put_u32(&buf, le, (uint32_t)entries[k].count))
                goto error;
        }

        if (entries[k].array_slot >= 0 && nbytes > inline_max)
        {
            if (big)
            {
                if (!synth_put_u64(&buf, le,
                                   slot_offsets[entries[k].array_slot]))
                    goto error;
            }
            else
            {
                if (!synth_put_u32(
                        &buf, le,
                        (uint32_t)slot_offsets[entries[k].array_slot]))
                    goto error;
            }
        }
        else if (entries[k].scalar)
        {
            uint64_t written = 0;
            if (!synth_put_value(&buf, le, entries[k].type,
                                 entries[k].values[0]))
                goto error;
            written += synth_typesize(entries[k].type);
            while (written < inline_max)
            {
                if (!synth_put(&buf, "\0", 1))
                    goto error;
                written++;
            }
        }
        else
        {
            uint64_t v;
            uint64_t written = 0;
            for (v = 0; v < entries[k].count; v++)
            {
                if (!synth_put_value(&buf, le, entries[k].type,
                                     entries[k].values[v]))
                    goto error;
                written += synth_typesize(entries[k].type);
            }
            while (written < inline_max)
            {
                if (!synth_put(&buf, "\0", 1))
                    goto error;
                written++;
            }
        }
    }

    if (big)
    {
        if (!synth_put_u64(&buf, le, 0))
            goto error;
    }
    else
    {
        if (!synth_put_u32(&buf, le, 0))
            goto error;
    }

    /* Out-of-line arrays, slot 0 (offsets) then slot 1 (byte counts). */
    for (k = 0; k < 2; k++)
    {
        int e;
        for (e = 0; e < nentries; e++)
        {
            uint64_t nbytes =
                synth_typesize(entries[e].type) * entries[e].count;
            uint64_t v;
            if (entries[e].array_slot != k || nbytes <= inline_max)
                continue;
            for (v = 0; v < entries[e].count; v++)
            {
                if (!synth_put_value(&buf, le, entries[e].type,
                                     entries[e].values[v]))
                    goto error;
            }
        }
    }

    /* Deterministic payload, then optional trailing padding. */
    {
        uint64_t pos;
        for (pos = 0; pos < payload_len; pos++)
        {
            uint8_t b = (pos < payload_len - spec->trailing_bytes)
                            ? strilesynth_payload_byte(pos)
                            : (uint8_t)(0xE0 + (pos % 17));
            if (!synth_put(&buf, &b, 1))
                goto error;
        }
    }

    if (buf.size != (size_t)(payload_offset + payload_len))
    {
        /* The layout arithmetic must always match what was emitted. */
        goto error;
    }

    if (layout)
    {
        layout->file_size = (uint64_t)buf.size;
        layout->payload_offset = payload_offset;
        layout->striles = striles;
    }

    *out_len = buf.size;
    return buf.data;

error:
    free(buf.data);
    return NULL;
}

int64_t strilesynth_write_file(const char *path, const StrileSynthSpec *spec,
                               StrileSynthLayout *layout)
{
    size_t len = 0;
    uint8_t *data = strilesynth_build_buffer(spec, &len, layout);
    FILE *f;

    if (data == NULL)
        return -1;

    f = fopen(path, "wb");
    if (f == NULL)
    {
        free(data);
        return -1;
    }
    if (len != 0 && fwrite(data, 1, len, f) != len)
    {
        fclose(f);
        free(data);
        return -1;
    }
    if (fclose(f) != 0)
    {
        free(data);
        return -1;
    }
    free(data);
    return (int64_t)len;
}
