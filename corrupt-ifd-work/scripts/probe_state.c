/*
 * Dirty-state probe: after TIFFReadDirectory() fails on a corrupted middle
 * IFD, inspect what state remains in the TIFF handle and whether a subsequent
 * valid directory can still be reached.
 *
 * Usage: probe_state <file> <good-dir-index>
 *   opens the file, walks until the first failure, then dumps handle state
 *   and attempts TIFFSetDirectory() to known-good indices.
 */
#include <stdio.h>
#include <stdarg.h>
#include <tiffio.h>
#include <tiffiop.h>

static int errs, warns;
static void eh(void *t_, const char *m, const char *f, va_list a)
{ (void)t_;(void)m; errs++; fputs("  E ", stderr); vfprintf(stderr, f, a); fputc('\n', stderr); }
static void wh(void *t_, const char *m, const char *f, va_list a)
{ (void)t_;(void)m; warns++; fputs("  W ", stderr); vfprintf(stderr, f, a); fputc('\n', stderr); }

static void dump_state(TIFF *tif, const char *tag)
{
    uint32_t w = 0xDEAD, h = 0xBEEF;
    TIFFGetField(tif, TIFFTAG_IMAGEWIDTH, &w);
    TIFFGetField(tif, TIFFTAG_IMAGELENGTH, &h);
    fprintf(stderr,
            "[%s] curdir=%d diroff=%llu nextdiroff=%llu lastdir=%d "
            "width=%u height=%u nstrips=%u\n",
            tag, (int)tif->tif_curdir,
            (unsigned long long)tif->tif_diroff,
            (unsigned long long)tif->tif_nextdiroff,
            TIFFLastDirectory(tif), w, h, tif->tif_dir.td_nstrips);
}

int main(int argc, char **argv)
{
    if (argc < 2) return 2;
    TIFFSetErrorHandlerExt(eh);
    TIFFSetWarningHandlerExt(wh);

    TIFF *tif = TIFFOpen(argv[1], "r");
    if (!tif)
    {
        fprintf(stderr, "open failed: errs=%d warns=%d\n", errs, warns);
        return 1;
    }
    dump_state(tif, "after open (dir 0 loaded)");

    int idx = 1;
    for (;;)
    {
        int ok = TIFFReadDirectory(tif);
        fprintf(stderr, "TIFFReadDirectory #%d -> %d\n", idx, ok);
        if (!ok)
        {
            dump_state(tif, "after FAILED read");
            break;
        }
        dump_state(tif, "after successful read");
        if (TIFFLastDirectory(tif) || ++idx > 6) break;
    }

    /* Can we go back to a directory that was read successfully before? */
    fprintf(stderr, "retry TIFFSetDirectory(0): %d\n", TIFFSetDirectory(tif, 0));
    dump_state(tif, "after SetDirectory(0)");
    fprintf(stderr, "retry TIFFSetDirectory(1): %d\n", TIFFSetDirectory(tif, 1));
    dump_state(tif, "after SetDirectory(1)");

    TIFFClose(tif);
    fprintf(stderr, "done errs=%d warns=%d\n", errs, warns);
    return 0;
}
