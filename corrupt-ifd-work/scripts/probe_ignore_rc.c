/*
 * What happens if a client ignores the 0 return of TIFFReadDirectory() and
 * keeps decoding from the handle? Demonstrates stale-state exposure.
 */
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <tiffio.h>
#include <tiffiop.h>

static int errs;
static void eh(void *t, const char *m, const char *f, va_list a)
{ (void)t;(void)m; errs++; fputs("  E ", stderr); vfprintf(stderr, f, a); fputc('\n', stderr); }
static void noop(void *t, const char *m, const char *f, va_list a)
{ (void)t;(void)m;(void)f;(void)a; }

int main(int argc, char **argv)
{
    if (argc < 2) return 2;
    TIFFSetErrorHandlerExt(eh);
    TIFFSetWarningHandlerExt(noop);

    TIFF *tif = TIFFOpen(argv[1], "r");
    if (!tif) { fprintf(stderr, "open failed errs=%d\n", errs); return 1; }

    int rc = TIFFReadDirectory(tif);   /* expected to fail */
    fprintf(stderr, "TIFFReadDirectory rc=%d (ignored)\n", rc);
    fprintf(stderr, "handle now: curdir=%d diroff=%llu nextdiroff=%llu\n",
            (int)tif->tif_curdir, (unsigned long long)tif->tif_diroff,
            (unsigned long long)tif->tif_nextdiroff);

    uint32_t w = 0, h = 0;
    TIFFGetField(tif, TIFFTAG_IMAGEWIDTH, &w);
    TIFFGetField(tif, TIFFTAG_IMAGELENGTH, &h);
    fprintf(stderr, "stale geometry exposed: %ux%u\n", w, h);
    if (w && h && w <= 64 && h <= 64)
    {
        uint32_t *raster = (uint32_t *)_TIFFmalloc((tmsize_t)w * h * 4);
        memset(raster, 0xCC, (tmsize_t)w * h * 4);
        int ok = TIFFReadRGBAImage(tif, w, h, raster, 0);
        fprintf(stderr,
                "TIFFReadRGBAImage after failed dir read -> %d "
                "px0=0x%08x (0xff0c0c0c would be stale dir0 data)\n",
                ok, raster[0]);
        _TIFFfree(raster);
    }
    else
    {
        fprintf(stderr, "decode not attempted (zero/huge dimension)\n");
    }
    TIFFClose(tif);
    return 0;
}
