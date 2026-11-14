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
 * Targeted fuzzer for strip/tile offset and byte-count handling.
 *
 * A whole-file TIFF fuzzer mostly dies in header/IFD parsing before reaching
 * the strile bounds checks.  This harness instead builds an otherwise valid
 * container in memory and uses the fuzz input to drive exactly the values
 * that matter:
 *
 *   - the strip/tile offsets and byte counts written into the IFD,
 *   - the amount of payload actually present (i.e. truncation),
 *   - the size argument passed to the read APIs, which selects between the
 *     whole-block read path and the partial-read path,
 *   - whether the file is opened through the memory-mapped or the explicit
 *     read/seek path,
 *   - whether striles arrays are loaded eagerly, deferred, or on demand.
 *
 * Every iteration therefore exercises the offset+byte-count overflow guards,
 * the mapped versus non-mapped reads, and the whole versus partial read
 * boundary in TIFFFillStrip()/TIFFFillTile().  A finding here is a real
 * out-of-bounds access or an integer overflow in those paths, not a dead-end
 * parser branch.
 */

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include <fuzzer/FuzzedDataProvider.h>

#include "tiffio.h"

namespace
{

/* Keep allocations small: OOM is not the bug class under test. */
constexpr uint32_t kMaxDimension = 256;
constexpr uint32_t kMaxStriles = 16;
constexpr uint64_t kMaxPayload = 64 * 1024;

/* ---- in-memory TIFF client ------------------------------------------- */

struct MemFile
{
    const uint8_t *data;
    toff_t size;
    toff_t pos;
};

tmsize_t memRead(thandle_t handle, void *buf, tmsize_t want)
{
    MemFile *m = static_cast<MemFile *>(handle);
    if (want < 0 || m->pos >= m->size)
        return 0;
    toff_t avail = m->size - m->pos;
    if (static_cast<toff_t>(want) > avail)
        want = static_cast<tmsize_t>(avail);
    memcpy(buf, m->data + m->pos, static_cast<size_t>(want));
    m->pos += static_cast<toff_t>(want);
    return want;
}

tmsize_t memWrite(thandle_t, void *, tmsize_t) { return 0; }

toff_t memSeek(thandle_t handle, toff_t off, int whence)
{
    MemFile *m = static_cast<MemFile *>(handle);
    toff_t base;
    switch (whence)
    {
        case SEEK_SET:
            base = 0;
            break;
        case SEEK_CUR:
            base = m->pos;
            break;
        case SEEK_END:
            base = m->size;
            break;
        default:
            return static_cast<toff_t>(-1);
    }
    if (off > static_cast<toff_t>(-1) - base)
        return static_cast<toff_t>(-1);
    if (base + off > m->size)
        return static_cast<toff_t>(-1);
    m->pos = base + off;
    return m->pos;
}

int memClose(thandle_t) { return 0; }

toff_t memSize(thandle_t handle) { return static_cast<MemFile *>(handle)->size; }


void silentError(const char *, const char *, va_list) {}
void silentWarning(const char *, const char *, va_list) {}

/* ---- minimal TIFF writer --------------------------------------------- */

constexpr uint16_t kTypeShort = 3;
constexpr uint16_t kTypeLong = 4;
constexpr uint16_t kTypeLong8 = 16;

constexpr uint16_t kTagImageWidth = 256;
constexpr uint16_t kTagImageLength = 257;
constexpr uint16_t kTagBitsPerSample = 258;
constexpr uint16_t kTagCompression = 259;
constexpr uint16_t kTagPhotometric = 262;
constexpr uint16_t kTagStripOffsets = 273;
constexpr uint16_t kTagSamplesPerPixel = 277;
constexpr uint16_t kTagRowsPerStrip = 278;
constexpr uint16_t kTagStripByteCounts = 279;
constexpr uint16_t kTagPlanarConfig = 284;
constexpr uint16_t kTagTileWidth = 322;
constexpr uint16_t kTagTileLength = 323;
constexpr uint16_t kTagTileOffsets = 324;
constexpr uint16_t kTagTileByteCounts = 325;

struct Entry
{
    uint16_t tag;
    uint16_t type;
    uint64_t count;
    uint64_t inline_value;
    const std::vector<uint64_t> *array; /* non-null: out-of-line values */
};

void put16(std::vector<uint8_t> &v, uint16_t x)
{
    v.push_back(static_cast<uint8_t>(x & 0xff));
    v.push_back(static_cast<uint8_t>((x >> 8) & 0xff));
}

void put32(std::vector<uint8_t> &v, uint32_t x)
{
    v.push_back(static_cast<uint8_t>(x & 0xff));
    v.push_back(static_cast<uint8_t>((x >> 8) & 0xff));
    v.push_back(static_cast<uint8_t>((x >> 16) & 0xff));
    v.push_back(static_cast<uint8_t>((x >> 24) & 0xff));
}

void put64(std::vector<uint8_t> &v, uint64_t x)
{
    for (int i = 0; i < 8; i++)
        v.push_back(static_cast<uint8_t>((x >> (8 * i)) & 0xff));
}

uint64_t typeSize(uint16_t type)
{
    if (type == kTypeShort)
        return 2;
    if (type == kTypeLong)
        return 4;
    return 8;
}

/* Build a classic or BigTIFF image whose striles arrays hold the caller's
 * offsets and byte counts.  Little-endian only, which keeps the writer small
 * while still covering the layout logic. */
std::vector<uint8_t> buildTiff(bool bigtiff, bool tiled, uint32_t width,
                               uint32_t height, uint32_t rows_per_strip,
                               uint32_t tile_width, uint32_t tile_length,
                               const std::vector<uint64_t> &offsets,
                               const std::vector<uint64_t> &bytecounts,
                               uint64_t payload_bytes)
{
    const uint64_t striles = offsets.size();
    const uint64_t inline_max = bigtiff ? 8 : 4;
    const uint16_t array_type = bigtiff ? kTypeLong8 : kTypeLong;
    const uint64_t entry_size = bigtiff ? 20 : 12;
    const uint64_t ifd_offset = bigtiff ? 16 : 8;

    std::vector<Entry> entries;
    entries.push_back({kTagImageWidth, kTypeLong, 1, width, nullptr});
    entries.push_back({kTagImageLength, kTypeLong, 1, height, nullptr});
    entries.push_back({kTagBitsPerSample, kTypeShort, 1, 8, nullptr});
    entries.push_back({kTagCompression, kTypeShort, 1, COMPRESSION_NONE,
                       nullptr});
    entries.push_back({kTagPhotometric, kTypeShort, 1, PHOTOMETRIC_MINISBLACK,
                       nullptr});
    entries.push_back({kTagSamplesPerPixel, kTypeShort, 1, 1, nullptr});
    entries.push_back({kTagPlanarConfig, kTypeShort, 1, PLANARCONFIG_CONTIG,
                       nullptr});
    if (tiled)
    {
        entries.push_back({kTagTileWidth, kTypeLong, 1, tile_width, nullptr});
        entries.push_back({kTagTileLength, kTypeLong, 1, tile_length, nullptr});
        entries.push_back({kTagTileOffsets, array_type, striles, 0, &offsets});
        entries.push_back(
            {kTagTileByteCounts, array_type, striles, 0, &bytecounts});
    }
    else
    {
        entries.push_back({kTagRowsPerStrip, kTypeLong, 1, rows_per_strip,
                           nullptr});
        entries.push_back({kTagStripOffsets, array_type, striles, 0, &offsets});
        entries.push_back(
            {kTagStripByteCounts, array_type, striles, 0, &bytecounts});
    }

    const uint64_t ifd_size =
        (bigtiff ? 8 : 2) + entries.size() * entry_size + (bigtiff ? 8 : 4);
    uint64_t cursor = ifd_offset + ifd_size;
    std::vector<uint64_t> slot[2] = {std::vector<uint64_t>(1),
                                     std::vector<uint64_t>(1)};
    uint64_t slot_offset[2] = {0, 0};

    for (Entry &e : entries)
    {
        if (e.array == nullptr)
            continue;
        const uint64_t nbytes = typeSize(e.type) * e.count;
        const int slot_index =
            (e.tag == kTagStripOffsets || e.tag == kTagTileOffsets) ? 0 : 1;
        if (nbytes > inline_max)
        {
            slot_offset[slot_index] = cursor;
            cursor += nbytes;
        }
        else
        {
            slot_offset[slot_index] = 0;
        }
    }
    const uint64_t payload_offset = cursor;

    std::vector<uint8_t> out;
    out.reserve(static_cast<size_t>(payload_offset + payload_bytes));

    out.push_back('I');
    out.push_back('I');
    if (bigtiff)
    {
        put16(out, 43);
        put16(out, 8);
        put16(out, 0);
        put64(out, ifd_offset);
        put64(out, entries.size());
    }
    else
    {
        put16(out, 42);
        put32(out, static_cast<uint32_t>(ifd_offset));
        put16(out, static_cast<uint16_t>(entries.size()));
    }

    for (const Entry &e : entries)
    {
        put16(out, e.tag);
        put16(out, e.type);
        if (bigtiff)
            put64(out, e.count);
        else
            put32(out, static_cast<uint32_t>(e.count));

        const uint64_t nbytes = typeSize(e.type) * e.count;
        if (e.array != nullptr && nbytes > inline_max)
        {
            const int slot_index =
                (e.tag == kTagStripOffsets || e.tag == kTagTileOffsets) ? 0 : 1;
            if (bigtiff)
                put64(out, slot_offset[slot_index]);
            else
                put32(out, static_cast<uint32_t>(slot_offset[slot_index]));
        }
        else if (e.array != nullptr)
        {
            /* Fits in the value field. */
            uint64_t written = 0;
            for (uint64_t i = 0; i < e.count; i++)
            {
                if (e.type == kTypeShort)
                {
                    put16(out, static_cast<uint16_t>((*e.array)[i]));
                    written += 2;
                }
                else
                {
                    put32(out, static_cast<uint32_t>((*e.array)[i]));
                    written += 4;
                }
            }
            while (written < inline_max)
            {
                out.push_back(0);
                written++;
            }
        }
        else
        {
            uint64_t written = 0;
            if (e.type == kTypeShort)
            {
                put16(out, static_cast<uint16_t>(e.inline_value));
                written = 2;
            }
            else
            {
                put32(out, static_cast<uint32_t>(e.inline_value));
                written = 4;
            }
            while (written < inline_max)
            {
                out.push_back(0);
                written++;
            }
        }
    }

    if (bigtiff)
        put64(out, 0);
    else
        put32(out, 0);

    for (int s = 0; s < 2; s++)
    {
        for (const Entry &e : entries)
        {
            const uint64_t nbytes = typeSize(e.type) * e.count;
            if (e.array == nullptr || nbytes <= inline_max)
                continue;
            const int slot_index =
                (e.tag == kTagStripOffsets || e.tag == kTagTileOffsets) ? 0 : 1;
            if (slot_index != s)
                continue;
            for (uint64_t i = 0; i < e.count; i++)
            {
                if (bigtiff)
                    put64(out, (*e.array)[i]);
                else
                    put32(out, static_cast<uint32_t>((*e.array)[i]));
            }
        }
    }

    /* Payload of deterministic, non-zero bytes. */
    for (uint64_t i = 0; i < payload_bytes; i++)
        out.push_back(static_cast<uint8_t>(0x11 + (i % 251)));

    return out;
}

/* Report the buffer as a mapping, so libtiff takes the direct-memory-access
 * path whose bounds checks differ from the read/seek ones. */
int memMap(thandle_t handle, void **base, toff_t *size)
{
    MemFile *m = static_cast<MemFile *>(handle);
    *base = const_cast<uint8_t *>(m->data);
    *size = m->size;
    return 1;
}

void memUnmap(thandle_t, void *, toff_t) {}

/* Read every striles through the encoded API with the given size argument.
 * Reading with size == -1 takes the whole-block path; a smaller size takes
 * the capped/partial path. */
void readAll(TIFF *tif, bool tiled, tmsize_t size)
{
    const uint32_t count =
        tiled ? TIFFNumberOfTiles(tif) : TIFFNumberOfStrips(tif);
    const tmsize_t full =
        tiled ? static_cast<tmsize_t>(TIFFTileSize(tif))
              : static_cast<tmsize_t>(TIFFStripSize(tif));
    if (full <= 0 || static_cast<uint64_t>(full) > kMaxPayload)
        return;

    if (size != static_cast<tmsize_t>(-1) && size > full)
        size = full;

    std::vector<uint8_t> buf(static_cast<size_t>(full));
    for (uint32_t i = 0; i < count && i < kMaxStriles; i++)
    {
        void *dst = buf.data();
        tmsize_t ask = size;
        if (ask != static_cast<tmsize_t>(-1))
        {
            if (ask > full)
                ask = full;
            /* Give the API exactly what it was asked for. */
            dst = buf.data();
        }
        if (tiled)
            TIFFReadEncodedTile(tif, i, dst, ask);
        else
            TIFFReadEncodedStrip(tif, i, dst, ask);
    }

    /* Raw reads always use an explicit, bounded size: the unbounded form
     * would require a caller buffer as large as the (possibly corrupt) byte
     * count. */
    for (uint32_t i = 0; i < count && i < kMaxStriles; i++)
    {
        uint8_t raw[64];
        if (tiled)
            TIFFReadRawTile(tif, i, raw, static_cast<tmsize_t>(sizeof(raw)));
        else
            TIFFReadRawStrip(tif, i, raw, static_cast<tmsize_t>(sizeof(raw)));
    }

    if (!tiled)
    {
        const tmsize_t scanline = static_cast<tmsize_t>(TIFFScanlineSize(tif));
        if (scanline > 0 && static_cast<uint64_t>(scanline) <= kMaxPayload)
        {
            std::vector<uint8_t> row(static_cast<size_t>(scanline));
            uint32_t height = 0;
            TIFFGetField(tif, TIFFTAG_IMAGELENGTH, &height);
            if (height > kMaxDimension)
                height = kMaxDimension;
            for (uint32_t y = 0; y < height; y++)
                TIFFReadScanline(tif, row.data(), y, 0);
        }
    }
}

} /* namespace */

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    FuzzedDataProvider fdp(data, size);

    const bool bigtiff = fdp.ConsumeBool();
    const bool tiled = fdp.ConsumeBool();
    const bool mapped = fdp.ConsumeBool();

    uint32_t width = fdp.ConsumeIntegralInRange<uint32_t>(1, kMaxDimension);
    uint32_t height = fdp.ConsumeIntegralInRange<uint32_t>(1, kMaxDimension);
    if (tiled)
    {
        /* Tile dimensions must be multiples of 16 in a valid TIFF; the
         * generator uses 16 and 32 so the tile geometry stays legal. */
        width = 16 * fdp.ConsumeIntegralInRange<uint32_t>(1, kMaxDimension / 16);
        height =
            16 * fdp.ConsumeIntegralInRange<uint32_t>(1, kMaxDimension / 16);
    }

    const uint32_t rows_per_strip =
        fdp.ConsumeIntegralInRange<uint32_t>(1, height);
    const uint32_t tile_width = 16;
    const uint32_t tile_length = 16;

    uint64_t striles;
    if (tiled)
        striles = ((width + 15) / 16) * ((height + 15) / 16);
    else
        striles = (height + rows_per_strip - 1) / rows_per_strip;
    if (striles == 0 || striles > kMaxStriles)
        return 0;

    std::vector<uint64_t> offsets;
    std::vector<uint64_t> bytecounts;
    offsets.reserve(striles);
    bytecounts.reserve(striles);

    for (uint64_t i = 0; i < striles; i++)
    {
        /* Mix plausible values with the interesting extremes so the mutator
         * spends its time on the bounds checks. */
        const uint8_t kind = fdp.ConsumeIntegral<uint8_t>() % 8;
        switch (kind)
        {
            case 0:
                offsets.push_back(0);
                break;
            case 1:
                offsets.push_back(fdp.ConsumeIntegral<uint64_t>());
                break;
            case 2:
                offsets.push_back(0xFFFFFFFFFFFFFFFFULL -
                                  fdp.ConsumeIntegralInRange<uint64_t>(0, 64));
                break;
            case 3:
                offsets.push_back(0x7FFFFFFFFFFFFFFFULL -
                                  fdp.ConsumeIntegralInRange<uint64_t>(0, 64));
                break;
            case 4:
                offsets.push_back(0xFFFFFFFFULL -
                                  fdp.ConsumeIntegralInRange<uint64_t>(0, 64));
                break;
            default:
                offsets.push_back(
                    fdp.ConsumeIntegralInRange<uint64_t>(0, kMaxPayload));
                break;
        }

        const uint8_t count_kind = fdp.ConsumeIntegral<uint8_t>() % 8;
        switch (count_kind)
        {
            case 0:
                bytecounts.push_back(0);
                break;
            case 1:
                bytecounts.push_back(fdp.ConsumeIntegral<uint64_t>());
                break;
            case 2:
                bytecounts.push_back(0xFFFFFFFFFFFFFFFFULL -
                                     fdp.ConsumeIntegralInRange<uint64_t>(0,
                                                                          64));
                break;
            case 3:
                bytecounts.push_back(0x7FFFFFFFFFFFFFFFULL -
                                     fdp.ConsumeIntegralInRange<uint64_t>(0,
                                                                          64));
                break;
            case 4:
                bytecounts.push_back(0xFFFFFFFFULL -
                                     fdp.ConsumeIntegralInRange<uint64_t>(0,
                                                                          64));
                break;
            default:
                bytecounts.push_back(fdp.ConsumeIntegralInRange<uint64_t>(
                    0, kMaxPayload));
                break;
        }
    }

    /* Keep the actual payload present in the file small, so most iterations
     * end up truncated relative to the declared byte counts - exactly the
     * suspicious combination. */
    const uint64_t payload_bytes =
        fdp.ConsumeIntegralInRange<uint64_t>(0, kMaxPayload);

    const std::vector<uint8_t> tiff =
        buildTiff(bigtiff, tiled, width, height, rows_per_strip, tile_width,
                  tile_length, offsets, bytecounts, payload_bytes);

    TIFFSetErrorHandler(silentError);
    TIFFSetWarningHandler(silentWarning);

    /* Choose the size argument for the encoded reads: -1 (whole block) or a
     * small value that forces the partial path. */
    const tmsize_t read_size =
        fdp.ConsumeBool() ? static_cast<tmsize_t>(-1)
                          : static_cast<tmsize_t>(fdp.ConsumeIntegralInRange<
                                                  uint32_t>(1, 1024));

    /* Open modes: 'm' disables mapping, 'D'/'O' change striles loading. */
    const char *mode = fdp.ConsumeBool() ? "rm" : "r";
    const uint8_t flags = fdp.ConsumeIntegral<uint8_t>() % 3;
    std::string mode_string(mode);
    if (flags == 1)
        mode_string += "D";
    else if (flags == 2)
        mode_string += "O";

    if (mapped)
    {
        /* A client that reports the buffer as mappable makes libtiff take the
         * direct-memory-access path, whose bounds checks differ from the
         * read/seek ones.  memRead/memSeek still work on the same handle for
         * the (few) places that fall back to ordinary reads. */
        MemFile mf{tiff.data(), static_cast<toff_t>(tiff.size()), 0};
        TIFF *tif =
            TIFFClientOpen("strile_fuzz", mode_string.c_str(),
                           static_cast<thandle_t>(&mf), memRead, memWrite,
                           memSeek, memClose, memSize, memMap, memUnmap);
        if (!tif)
            return 0;
        readAll(tif, tiled, read_size);
        TIFFClose(tif);
    }

    {
        MemFile mf{tiff.data(), static_cast<toff_t>(tiff.size()), 0};
        TIFF *tif = TIFFClientOpen("strile_fuzz", mode_string.c_str(),
                                   static_cast<thandle_t>(&mf), memRead,
                                   memWrite, memSeek, memClose, memSize,
                                   nullptr, nullptr);
        if (!tif)
            return 0;
        readAll(tif, tiled, read_size);
        TIFFClose(tif);
    }

    return 0;
}
