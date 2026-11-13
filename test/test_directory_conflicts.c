/*
 * Safe handling of conflicting and malformed main-IFD tags.
 */

#include "tif_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif

#include "tiffio.h"

typedef struct
{
    uint16_t tag;
    uint16_t type;
    uint64_t count;
    uint64_t value;
} RawEntry;

static void put16(FILE *fp, uint16_t value)
{
    unsigned char b[2] = {value & 255U, value >> 8};
    if (fwrite(b, 1, 2, fp) != 2)
        exit(1);
}

static void put32(FILE *fp, uint32_t value)
{
    unsigned char b[4] = {value & 255U, (value >> 8) & 255U,
                          (value >> 16) & 255U, value >> 24};
    if (fwrite(b, 1, 4, fp) != 4)
        exit(1);
}

static void put64le(FILE *fp, uint64_t value)
{
    unsigned char b[8];
    int i;
    for (i = 0; i < 8; i++)
        b[i] = (unsigned char)(value >> (i * 8));
    if (fwrite(b, 1, 8, fp) != 8)
        exit(1);
}

static int entry_compare(const void *a, const void *b)
{
    const RawEntry *ea = a;
    const RawEntry *eb = b;
    if (ea->tag < eb->tag)
        return -1;
    if (ea->tag > eb->tag)
        return 1;
    return 0;
}

static void write_ifd(FILE *fp, int big_tiff, const RawEntry *entries,
                      size_t entry_count, uint64_t next_ifd,
                      uint64_t external_offset, const void *external_data,
                      size_t external_size)
{
    RawEntry sorted[32];
    RawEntry conflicted[32];
    size_t i;
    int has_conflicting_duplicate = 0;

    if (entry_count > sizeof(sorted) / sizeof(sorted[0]))
        exit(1);
    memcpy(sorted, entries, entry_count * sizeof(sorted[0]));
    qsort(sorted, entry_count, sizeof(sorted[0]), entry_compare);
    for (i = 1; i < entry_count; i++)
    {
        if (sorted[i - 1].tag == sorted[i].tag &&
            (sorted[i - 1].type != sorted[i].type ||
             sorted[i - 1].count != sorted[i].count ||
             sorted[i - 1].value != sorted[i].value))
        {
            has_conflicting_duplicate = 1;
        }
    }

    if (has_conflicting_duplicate)
    {
        memcpy(conflicted, entries, entry_count * sizeof(conflicted[0]));
        /* Keep the malformed input order; only assert that it is unsorted. */
        for (i = 1; i < entry_count; i++)
        {
            if (conflicted[i - 1].tag > conflicted[i].tag)
                break;
        }
        if (i == entry_count)
            exit(1);
    }

    if (!big_tiff)
    {
        const RawEntry *output = has_conflicting_duplicate ? conflicted
                                                            : sorted;
        put16(fp, (uint16_t)entry_count);
        for (i = 0; i < entry_count; i++)
        {
            put16(fp, output[i].tag);
            put16(fp, output[i].type);
            put32(fp, (uint32_t)output[i].count);
            put32(fp, (uint32_t)output[i].value);
        }
        put32(fp, (uint32_t)next_ifd);
    }
    else
    {
        const RawEntry *output = has_conflicting_duplicate ? conflicted
                                                            : sorted;
        put64le(fp, entry_count);
        for (i = 0; i < entry_count; i++)
        {
            put16(fp, output[i].tag);
            put16(fp, output[i].type);
            put64le(fp, output[i].count);
            put64le(fp, output[i].value);
        }
        put64le(fp, next_ifd);
    }

    if (external_data && external_size)
    {
        if (ftello(fp) != (off_t)external_offset)
            exit(1);
        if (fwrite(external_data, 1, external_size, fp) != external_size)
            exit(1);
    }
}

static void write_header(FILE *fp, int big_tiff, uint64_t first_ifd)
{
    put16(fp, 0x4949);
    put16(fp, big_tiff ? 43 : 42);
    if (!big_tiff)
    {
        put32(fp, (uint32_t)first_ifd);
    }
    else
    {
        put16(fp, 8);
        put16(fp, 0);
        put64le(fp, first_ifd);
    }
}

static void basic_entries(RawEntry *entries, size_t *count,
                          uint32_t pixel_offset)
{
    const RawEntry base[] = {
        {TIFFTAG_IMAGEWIDTH, TIFF_LONG, 1, 1},
        {TIFFTAG_IMAGELENGTH, TIFF_LONG, 1, 1},
        {TIFFTAG_BITSPERSAMPLE, TIFF_SHORT, 1, 8},
        {TIFFTAG_COMPRESSION, TIFF_SHORT, 1, 1},
        {TIFFTAG_PHOTOMETRIC, TIFF_SHORT, 1, 1},
        {TIFFTAG_STRIPOFFSETS, TIFF_LONG, 1, pixel_offset},
        {TIFFTAG_SAMPLESPERPIXEL, TIFF_SHORT, 1, 1},
        {TIFFTAG_ROWSPERSTRIP, TIFF_LONG, 1, 1},
        {TIFFTAG_STRIPBYTECOUNTS, TIFF_LONG, 1, 1},
        {TIFFTAG_PLANARCONFIG, TIFF_SHORT, 1, 1},
    };
    *count = sizeof(base) / sizeof(base[0]);
    memcpy(entries, base, sizeof(base));
}

static void set_entry_value(RawEntry *entries, size_t count, uint16_t tag,
                            uint64_t value)
{
    size_t i;
    for (i = 0; i < count; i++)
    {
        if (entries[i].tag == tag)
        {
            entries[i].value = value;
            return;
        }
    }
    exit(1);
}

static int create_file(const char *name, int big_tiff, int duplicate_short,
                       int conflicting_width, int bad_type,
                       int duplicate_long8, int conflicting_locations,
                       int bad_signed)
{
    FILE *fp = fopen(name, "wb");
    RawEntry entries[32];
    size_t count = 0;
    uint32_t ifd0_entry_count;
    uint32_t ifd0_size;
    uint32_t pixel_offset;
    uint32_t ifd1_entry_count;
    uint32_t ifd1_offset;
    uint32_t ifd1_size;
    const unsigned char pixel = 0x5a;
    const char page[] = "valid-next-ifd";
    uint32_t page_offset;

    if (!fp)
        return 0;

    ifd0_entry_count = count;
    ifd0_size = big_tiff ? 8 + ifd0_entry_count * 20 + 8
                         : 2 + ifd0_entry_count * 12 + 4;
    pixel_offset = (big_tiff ? 16 : 8) + ifd0_size;
    basic_entries(entries, &count, pixel_offset);
    ifd0_entry_count = count;

    if (duplicate_short)
    {
        entries[count++] = (RawEntry){TIFFTAG_IMAGELENGTH, TIFF_SHORT, 1, 1};
        ifd0_entry_count += 1;
    }
    if (conflicting_width)
    {
        entries[count++] = (RawEntry){TIFFTAG_IMAGEWIDTH, TIFF_LONG, 1, 2};
        ifd0_entry_count += 1;
    }
    if (bad_type)
    {
        entries[count++] = (RawEntry){TIFFTAG_STRIPOFFSETS, TIFF_DOUBLE, 1, 0};
        ifd0_entry_count += 1;
    }
    if (duplicate_long8)
    {
        entries[count++] = (RawEntry){TIFFTAG_IMAGELENGTH, TIFF_LONG8, 1, 1};
        ifd0_entry_count += 1;
    }
    if (conflicting_locations)
    {
        entries[count++] = (RawEntry){TIFFTAG_TILEWIDTH, TIFF_LONG, 1, 1};
        entries[count++] = (RawEntry){TIFFTAG_TILELENGTH, TIFF_LONG, 1, 1};
        entries[count++] = (RawEntry){TIFFTAG_TILEOFFSETS, TIFF_LONG, 1,
                                      pixel_offset};
        entries[count++] = (RawEntry){TIFFTAG_TILEBYTECOUNTS, TIFF_LONG, 1,
                                      1};
        ifd0_entry_count += 4;
    }
    if (bad_signed)
    {
        set_entry_value(entries, count, TIFFTAG_IMAGEWIDTH, 1);
        entries[0].type = TIFF_SLONG;
    }

    ifd0_size = big_tiff ? 8 + ifd0_entry_count * 20 + 8
                         : 2 + ifd0_entry_count * 12 + 4;
    pixel_offset = (big_tiff ? 16 : 8) + ifd0_size;
    set_entry_value(entries, count, TIFFTAG_STRIPOFFSETS, pixel_offset);

    ifd1_entry_count = 11; /* The ten basic tags plus PageName. */
    ifd1_size = big_tiff ? 8 + ifd1_entry_count * 20 + 8
                         : 2 + ifd1_entry_count * 12 + 4;
    ifd1_offset = pixel_offset + 1;
    page_offset = ifd1_offset + ifd1_size;

    write_header(fp, big_tiff, big_tiff ? 16 : 8);
    write_ifd(fp, big_tiff, entries, count, ifd1_offset, 0, NULL, 0);
    if (fwrite(&pixel, 1, 1, fp) != 1)
        goto fail;

    basic_entries(entries, &count, pixel_offset);
    entries[count++] = (RawEntry){TIFFTAG_PAGENAME, TIFF_ASCII, sizeof(page),
                                  page_offset};
    write_ifd(fp, big_tiff, entries, count, 0, page_offset, page,
              sizeof(page));
    fclose(fp);
    return 1;

fail:
    fclose(fp);
    return 0;
}

static int expect_pixel(TIFF *tif)
{
    unsigned char pixel = 0;
    if (TIFFReadScanline(tif, &pixel, 0, 0) != 1 || pixel != 0x5a)
    {
        fprintf(stderr, "Unexpected pixel content\n");
        return 0;
    }
    return 1;
}

static int run_case(const char *filename, int should_succeed,
                    int check_pixel)
{
    TIFF *tif = TIFFOpen(filename, "rh");
    unsigned char pixel = 0;

    if (!tif)
    {
        fprintf(stderr, "Could not open %s\n", filename);
        return 0;
    }

    if (should_succeed)
    {
        if (!TIFFReadDirectory(tif))
        {
            fprintf(stderr, "Initial IFD could not be read\n");
            goto fail;
        }
        if (check_pixel && !expect_pixel(tif))
            goto fail;
    }
    else
    {
        if (TIFFReadDirectory(tif))
        {
            fprintf(stderr, "Malformed IFD was unexpectedly accepted\n");
            goto fail;
        }
        if (TIFFReadScanline(tif, &pixel, 0, 0) == 1)
        {
            fprintf(stderr,
                    "Malformed IFD unexpectedly delivered pixels (mode=%d)\n",
                    should_succeed);
            goto fail;
        }
        if (!TIFFReadDirectory(tif))
        {
            fprintf(stderr, "Could not continue to next IFD after failure\n");
            goto fail;
        }
        if (!expect_pixel(tif))
            goto fail;
    }

    TIFFClose(tif);
    return 1;

fail:
    TIFFClose(tif);
    return 0;
}

int main(void)
{
    static const char *classic_valid = "directory-conflict-classic-valid.tif";
    static const char *bigtiff_valid = "directory-conflict-bigtiff-valid.tif";
    static const char *classic_width = "directory-conflict-classic-width.tif";
    static const char *bigtiff_width = "directory-conflict-bigtiff-width.tif";
    static const char *classic_type = "directory-conflict-classic-type.tif";
    static const char *bigtiff_type = "directory-conflict-bigtiff-type.tif";
    static const char *locations = "directory-conflict-locations.tif";
    static const char *bigtiff_long8 =
        "directory-conflict-bigtiff-long8.tif";
    static const char *signed_type = "directory-conflict-signed.tif";
    int result = 1;

    if (!create_file(classic_valid, 0, 1, 0, 0, 0, 0, 0) ||
        !run_case(classic_valid, 1, 1))
    {
        result = 0;
    }

    if (!create_file(bigtiff_valid, 1, 1, 0, 0, 0, 0, 0) ||
        !run_case(bigtiff_valid, 1, 1))
    {
        result = 0;
    }

    if (!create_file(bigtiff_long8, 1, 0, 0, 0, 1, 0, 0) ||
        !run_case(bigtiff_long8, 1, 1))
    {
        result = 0;
    }

    if (!create_file(classic_width, 0, 0, 1, 0, 0, 0, 0) ||
        !run_case(classic_width, 0, 0))
    {
        result = 0;
    }

    if (!create_file(bigtiff_width, 1, 0, 1, 0, 0, 0, 0) ||
        !run_case(bigtiff_width, 0, 0))
    {
        result = 0;
    }

    if (!create_file(classic_type, 0, 0, 0, 1, 0, 0, 0) ||
        !run_case(classic_type, 0, 0))
    {
        result = 0;
    }

    if (!create_file(bigtiff_type, 1, 0, 0, 1, 0, 0, 0) ||
        !run_case(bigtiff_type, 0, 0))
    {
        result = 0;
    }

    if (!create_file(locations, 0, 0, 0, 0, 0, 1, 0) ||
        !run_case(locations, 0, 0))
    {
        result = 0;
    }

    if (!create_file(signed_type, 0, 0, 0, 0, 0, 0, 1) ||
        !run_case(signed_type, 0, 0))
    {
        result = 0;
    }

    unlink(classic_valid);
    unlink(bigtiff_valid);
    unlink(bigtiff_long8);
    unlink(classic_width);
    unlink(bigtiff_width);
    unlink(classic_type);
    unlink(bigtiff_type);
    unlink(locations);
    unlink(signed_type);
    return result ? 0 : 1;
}
