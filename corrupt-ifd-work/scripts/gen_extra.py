#!/usr/bin/env python3
"""Extra edge samples: unknown-tag/unknown-type and a minimal BigTIFF.

Run: python3 -I gen_extra.py <outdir>
Standard library only.
"""
import os
import struct
import sys

SHORT, LONG, LONG8 = 3, 4, 16
IMAGEWIDTH, IMAGELENGTH = 256, 257
BITSPERSAMPLE, COMPRESSION, PHOTOMETRIC = 258, 259, 262
STRIPOFFSETS, SAMPLESPERPIXEL = 273, 277
ROWSPERSTRIP, STRIPBYTECOUNTS = 278, 279


def entry12(tag, typ, count, val4):
    return struct.pack("<HHI", tag, typ, count) + val4


def short_inline(v):
    return struct.pack("<HH", v, 0)


def main():
    outdir = sys.argv[1]
    os.makedirs(outdir, exist_ok=True)

    # ---- 1. classic TIFF, unknown tag 65000 carrying unknown type 99 -------
    n = 10
    ifd_size = 2 + n * 12 + 4
    ifd0 = 8
    pix = ifd0 + ifd_size
    total = pix + 16
    b = bytearray(total)
    b[0:2] = b"II"
    struct.pack_into("<H", b, 2, 42)
    struct.pack_into("<I", b, 4, ifd0)
    struct.pack_into("<H", b, ifd0, n)
    entries = [
        entry12(IMAGEWIDTH, SHORT, 1, short_inline(4)),
        entry12(IMAGELENGTH, SHORT, 1, short_inline(4)),
        entry12(BITSPERSAMPLE, SHORT, 1, short_inline(8)),
        entry12(COMPRESSION, SHORT, 1, short_inline(1)),
        entry12(PHOTOMETRIC, SHORT, 1, short_inline(1)),
        entry12(STRIPOFFSETS, LONG, 1, struct.pack("<I", pix)),
        entry12(SAMPLESPERPIXEL, SHORT, 1, short_inline(1)),
        entry12(ROWSPERSTRIP, SHORT, 1, short_inline(4)),
        entry12(STRIPBYTECOUNTS, LONG, 1, struct.pack("<I", 16)),
        # unknown tag, unknown type 99, count 1, inline junk value
        entry12(65000, 99, 1, struct.pack("<I", 0x11223344)),
    ]
    e = ifd0 + 2
    for ent in entries:
        b[e:e + 12] = ent
        e += 12
    struct.pack_into("<I", b, e, 0)
    b[pix:pix + 16] = bytes(range(50, 66))
    with open(os.path.join(outdir, "unknown_tag_unknown_type.tif"), "wb") as f:
        f.write(b)

    # ---- 2. minimal valid BigTIFF, 4x4 grayscale --------------------------
    # Header: "II", magic 43 (u16), offsetsize 8 (u16), unused 0 (u16),
    # diroff (u64) = 16 bytes.
    hdr = struct.pack("<HHHHQ", 0x4949, 43, 8, 0, 16)
    assert len(hdr) == 16
    bn = 9
    ifd_size_b = 8 + bn * 20 + 8
    ifd0b = 16
    pixb = ifd0b + ifd_size_b
    totalb = pixb + 16
    b = bytearray(totalb)
    b[0:16] = hdr
    struct.pack_into("<Q", b, ifd0b, bn)

    def entry20(tag, typ, count, val8):
        return struct.pack("<HHQ", tag, typ, count) + val8

    ents = [
        entry20(IMAGEWIDTH, SHORT, 1, struct.pack("<HH", 4, 0) + b"\0" * 4),
        entry20(IMAGELENGTH, SHORT, 1, struct.pack("<HH", 4, 0) + b"\0" * 4),
        entry20(BITSPERSAMPLE, SHORT, 1, struct.pack("<HH", 8, 0) + b"\0" * 4),
        entry20(COMPRESSION, SHORT, 1, struct.pack("<HH", 1, 0) + b"\0" * 4),
        entry20(PHOTOMETRIC, SHORT, 1, struct.pack("<HH", 1, 0) + b"\0" * 4),
        entry20(STRIPOFFSETS, LONG8, 1, struct.pack("<Q", pixb)),
        entry20(SAMPLESPERPIXEL, SHORT, 1, struct.pack("<HH", 1, 0) + b"\0" * 4),
        entry20(ROWSPERSTRIP, SHORT, 1, struct.pack("<HH", 4, 0) + b"\0" * 4),
        entry20(STRIPBYTECOUNTS, LONG8, 1, struct.pack("<Q", 16)),
    ]
    e = ifd0b + 8
    for ent in ents:
        assert len(ent) == 20
        b[e:e + 20] = ent
        e += 20
    struct.pack_into("<Q", b, e, 0)
    b[pixb:pixb + 16] = bytes([77] * 16)
    with open(os.path.join(outdir, "bigtiff_valid.tif"), "wb") as f:
        f.write(b)

    # ---- 3. BigTIFF with a count above 4096 (sanity rejection) -----------
    b2 = bytearray(b)
    struct.pack_into("<Q", b2, ifd0b, 5000)
    with open(os.path.join(outdir, "bigtiff_overflow_count.tif"), "wb") as f:
        f.write(b2)

    # ---- 4. BigTIFF next-IFD pointer past EOF ----------------------------
    b3 = bytearray(b)
    struct.pack_into("<Q", b3, ifd0b + 8 + bn * 20, totalb + 100)
    with open(os.path.join(outdir, "bigtiff_tail_past_eof.tif"), "wb") as f:
        f.write(b3)

    print("wrote 4 extra samples to", outdir)


if __name__ == "__main__":
    main()
