#!/usr/bin/env python3
"""Build a valid 2-IFD classic little-endian TIFF and emit corruption variants.

All output files are written to the samples directory passed as argv[1].
The generator only uses the Python standard library. Run with: python3 -I.

Layout of the base file (grayscale, 8-bit, uncompressed, 4x4 each):

  0..7    header: "II" 0x2A 0x00  IFD0 offset
  IFD0    8 entries (96 bytes) + next-IFD pointer
  IFD1    8 entries (96 bytes) + zero pointer
  pixels  two 16-byte strips
"""
import os
import struct
import sys

# --- TIFF constants -------------------------------------------------------
BYTE, ASCII, SHORT, LONG, RATIONAL = 1, 2, 3, 4, 5
UNDEFINED = 7
IMAGEWIDTH, IMAGELENGTH = 256, 257
BITSPERSAMPLE, COMPRESSION, PHOTOMETRIC = 258, 259, 262
STRIPOFFSETS, SAMPLESPERPIXEL = 273, 277
ROWSPERSTRIP, STRIPBYTECOUNTS = 278, 279


def entry(tag, typ, count, value_bytes_or_int):
    """One 12-byte classic IFD entry."""
    if isinstance(value_bytes_or_int, int):
        vb = struct.pack("<I", value_bytes_or_int)
    else:
        vb = value_bytes_or_int
    assert len(vb) == 4
    return struct.pack("<HHI", tag, typ, count) + vb


def short_inline(v):
    return struct.pack("<HH", v, 0)


def build_base():
    """Return (bytearray, dict of named offsets).

    Two 9-entry classic IFDs, each describing a 4x4 8-bit grayscale image.
    """
    n_entries = 9
    ifd_size = 2 + n_entries * 12 + 4      # 114
    ifd0_off = 8
    ifd1_off = ifd0_off + ifd_size         # 122
    pix0_off = ifd1_off + ifd_size         # 236
    pix1_off = pix0_off + 16               # 252
    total = pix1_off + 16                  # 268

    b = bytearray(total)
    # Header
    b[0:2] = b"II"
    struct.pack_into("<H", b, 2, 42)
    struct.pack_into("<I", b, 4, ifd0_off)

    info = {
        "ifd0_off": ifd0_off,
        "ifd1_off": ifd1_off,
        "pix0_off": pix0_off,
        "pix1_off": pix1_off,
        "n_entries": n_entries,
        "ifd_size": ifd_size,
    }

    def write_ifd(off, width, length, pix_off, next_off):
        struct.pack_into("<H", b, off, n_entries)
        e = off + 2
        entries = [
            entry(IMAGEWIDTH, SHORT, 1, short_inline(width)),
            entry(IMAGELENGTH, SHORT, 1, short_inline(length)),
            entry(BITSPERSAMPLE, SHORT, 1, short_inline(8)),
            entry(COMPRESSION, SHORT, 1, short_inline(1)),
            entry(PHOTOMETRIC, SHORT, 1, short_inline(1)),
            entry(STRIPOFFSETS, LONG, 1, pix_off),
            entry(SAMPLESPERPIXEL, SHORT, 1, short_inline(1)),
            entry(ROWSPERSTRIP, SHORT, 1, short_inline(length)),
            entry(STRIPBYTECOUNTS, LONG, 1, width * length),
        ]
        for ent in entries:
            b[e:e + 12] = ent
            e += 12
        struct.pack_into("<I", b, e, next_off)
        info[f"nextptr_off_{off}"] = e
        # record entry offsets for later patching
        info.setdefault("entry_offsets", {})
        for i, tag in enumerate([IMAGEWIDTH, IMAGELENGTH, BITSPERSAMPLE,
                                 COMPRESSION, PHOTOMETRIC, STRIPOFFSETS,
                                 SAMPLESPERPIXEL, ROWSPERSTRIP,
                                 STRIPBYTECOUNTS]):
            info["entry_offsets"].setdefault(tag, []).append(off + 2 + i * 12)

    write_ifd(ifd0_off, 4, 4, pix0_off, ifd1_off)
    write_ifd(ifd1_off, 4, 4, pix1_off, 0)

    # Pixel data: IFD0 gradient 0..15, IFD1 constant 128.
    b[pix0_off:pix0_off + 16] = bytes(range(16))
    b[pix1_off:pix1_off + 16] = bytes([128] * 16)
    return b, info


def patch32(buf, off, val):
    struct.pack_into("<I", buf, off, val)


def patch16(buf, off, val):
    struct.pack_into("<H", buf, off, val)


def main():
    outdir = sys.argv[1]
    os.makedirs(outdir, exist_ok=True)
    base, info = build_base()
    i0, i1 = info["ifd0_off"], info["ifd1_off"]
    eoff = info["entry_offsets"]
    next0 = info[f"nextptr_off_{i0}"]
    next1 = info[f"nextptr_off_{i1}"]

    def save(name, buf):
        p = os.path.join(outdir, name)
        with open(p, "wb") as f:
            f.write(buf)
        print(f"wrote {p} ({len(buf)} bytes)")

    # 0. legal multi-directory baseline
    save("valid_multi.tif", bytearray(base))

    # --- corrupt first IFD ------------------------------------------------
    # A. short directory: count says 1 but full 9-entry body + next ptr remain
    buf = bytearray(base)
    patch16(buf, i0, 1)
    save("ifd0_short_count.tif", buf)

    # B. overflow count: classic 16-bit count set beyond 4096 sanity limit
    buf = bytearray(base)
    patch16(buf, i0, 5000)
    save("ifd0_overflow_count.tif", buf)

    # C. unknown field type on ImageWidth (type 99 does not exist)
    buf = bytearray(base)
    patch16(buf, eoff[IMAGEWIDTH][0] + 2, 99)
    save("ifd0_unknown_type.tif", buf)

    # D. entry offset pointing past EOF (StripOffsets -> bogus 0xFFFFFFF0)
    buf = bytearray(base)
    patch32(buf, eoff[STRIPOFFSETS][0] + 8, 0xFFFFFFF0)
    save("ifd0_entry_oob.tif", buf)

    # E. first IFD loops back to itself
    buf = bytearray(base)
    patch32(buf, next0, i0)
    save("ifd0_self_loop.tif", buf)

    # F. truncated file right after the IFD0 count word (short directory body)
    buf = bytearray(base[:i0 + 2])
    save("ifd0_truncated.tif", buf)

    # --- corrupt middle IFD (IFD1) ----------------------------------------
    # G. IFD1 short count
    buf = bytearray(base)
    patch16(buf, i1, 1)
    save("ifd1_short_count.tif", buf)

    # H. IFD1 count beyond sanity limit
    buf = bytearray(base)
    patch16(buf, i1, 6000)
    save("ifd1_overflow_count.tif", buf)

    # I. IFD1 unknown type on ImageLength
    buf = bytearray(base)
    patch16(buf, eoff[IMAGELENGTH][1] + 2, 100)
    save("ifd1_unknown_type.tif", buf)

    # J. IFD1 StripOffsets OOB
    buf = bytearray(base)
    patch32(buf, eoff[STRIPOFFSETS][1] + 8, 0xFFFFFFF0)
    save("ifd1_entry_oob.tif", buf)

    # K. IFD1 truncated: keep header+IFD0 intact, cut inside IFD1 body
    buf = bytearray(base[:i1 + 10])
    save("ifd1_truncated.tif", buf)

    # --- corrupt tail pointer ---------------------------------------------
    # L. IFD1 next pointer points back to IFD0 (2-node cycle)
    buf = bytearray(base)
    patch32(buf, next1, i0)
    save("tail_loop_to_ifd0.tif", buf)

    # M. tail pointer to an offset past EOF
    buf = bytearray(base)
    patch32(buf, next1, len(base) + 64)
    save("tail_ptr_past_eof.tif", buf)

    # N. tail pointer points into the middle of the pixel data (not an IFD:
    #    the word there will fail the 4096 count sanity check)
    buf = bytearray(base)
    patch32(buf, next1, info["pix0_off"] + 4)
    save("tail_ptr_into_pixels.tif", buf)

    # O. extra IFD loop where IFD1 points at itself
    buf = bytearray(base)
    patch32(buf, next1, i1)
    save("tail_self_loop.tif", buf)

    # --- three-IFD chain: IFD0 good, IFD1 corrupt, IFD2 good --------------
    # Append a third full IFD and a 16-byte strip at EOF.
    def build_three_ifd(corrupt_kind):
        b3 = bytearray(base)
        ifd2_off = len(b3)                 # new IFD right after current EOF
        pix2_off = ifd2_off + info["ifd_size"]
        b3.extend(b"\0" * (info["ifd_size"] + 16))
        # splice IFD1's next pointer to the new IFD2
        patch32(b3, next1, ifd2_off)
        # write IFD2 as a valid 9-entry directory
        struct.pack_into("<H", b3, ifd2_off, info["n_entries"])
        tags9 = [IMAGEWIDTH, IMAGELENGTH, BITSPERSAMPLE, COMPRESSION,
                 PHOTOMETRIC, STRIPOFFSETS, SAMPLESPERPIXEL, ROWSPERSTRIP,
                 STRIPBYTECOUNTS]
        vals = [short_inline(4), short_inline(4), short_inline(8),
                short_inline(1), short_inline(1),
                struct.pack("<I", pix2_off), short_inline(1),
                short_inline(4), struct.pack("<I", 16)]
        e = ifd2_off + 2
        for tag, val in zip(tags9, vals):
            b3[e:e + 12] = entry(tag,
                                 LONG if tag in (STRIPOFFSETS,
                                                 STRIPBYTECOUNTS) else SHORT,
                                 1, val)
            e += 12
        struct.pack_into("<I", b3, e, 0)
        b3[pix2_off:pix2_off + 16] = bytes([200] * 16)
        # now corrupt IFD1
        if corrupt_kind == "short":
            patch16(b3, i1, 1)
        elif corrupt_kind == "unknown_type":
            patch16(b3, eoff[IMAGELENGTH][1] + 2, 100)
        elif corrupt_kind == "overflow":
            patch16(b3, i1, 6000)
        return b3, ifd2_off

    b3, ifd2_off = build_three_ifd("short")
    save("three_ifd_middle_short.tif", b3)
    b3, _ = build_three_ifd("unknown_type")
    save("three_ifd_middle_unknowntype.tif", b3)
    b3, _ = build_three_ifd("overflow")
    save("three_ifd_middle_overflow.tif", b3)

    # Sanity variant: all three IFDs intact, proves IFD2 itself is readable.
    b3, _ = build_three_ifd("short")
    patch16(b3, i1, info["n_entries"])   # restore IFD1 count
    save("three_ifd_all_valid.tif", b3)

    print("layout info:", info)


if __name__ == "__main__":
    main()
