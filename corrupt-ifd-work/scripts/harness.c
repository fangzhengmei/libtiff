/*
 * Corrupt-IFD test harness.
 *
 * For each file given on the command line:
 *   - opens it twice: once with default (possibly memory-mapped) I/O,
 *     once with streaming I/O (map proc disabled);
 *   - installs error/warning handlers that log every libtiff diagnostic;
 *   - walks the IFD chain via TIFFReadDirectory() and records each result;
 *   - attempts RGBA decoding of every directory and reports pixel output;
 *   - runs under ASan/UBSan so any out-of-bounds / truncation issue aborts.
 *
 * Build (see run_tests.sh):
 *   cc -g -O1 -fsanitize=address,undefined harness.c -I... -L... -ltiff
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#include <tiffio.h>
#include <tiffiop.h>

static int g_log_errors;
static int g_log_warnings;
static FILE *g_logfp;

static void error_handler(TIFF *t, const char *module, const char *fmt,
                          va_list ap)
{
    (void)t; (void)module;
    g_log_errors++;
    fprintf(g_logfp, "    [ERROR] ");
    vfprintf(g_logfp, fmt, ap);
    fputc('\n', g_logfp);
}

static void warn_handler(TIFF *t, const char *module, const char *fmt,
                         va_list ap)
{
    (void)t; (void)module;
    g_log_warnings++;
    fprintf(g_logfp, "    [WARN ] ");
    vfprintf(g_logfp, fmt, ap);
    fputc('\n', g_logfp);
}

/* Streaming variant: open with "rm" so TIFF_MAPPED is cleared at open time
 * (mmap happens inside TIFFOpen, so overriding the proc afterwards is too
 * late). No custom I/O procs are needed. */

static void decode_current_dir(TIFF *tif, int diridx)
{
    char emsg[1024] = {0};
    uint32_t w = 0, h = 0;
    TIFFGetField(tif, TIFFTAG_IMAGEWIDTH, &w);
    TIFFGetField(tif, TIFFTAG_IMAGELENGTH, &h);
    fprintf(g_logfp, "    dir %d: fields %ux%u\n", diridx, w, h);

    if (w == 0 || h == 0 || w > 64 || h > 64)
    {
        fprintf(g_logfp, "    dir %d: decode skipped (bad dimensions)\n",
                diridx);
        return;
    }

    uint32_t *raster = (uint32_t *)_TIFFmalloc((tmsize_t)w * h * sizeof(uint32_t));
    if (!raster)
    {
        fprintf(g_logfp, "    dir %d: raster alloc failed\n", diridx);
        return;
    }
    int rc = TIFFReadRGBAImage(tif, w, h, raster, 0);
    fprintf(g_logfp, "    dir %d: TIFFReadRGBAImage -> %d", diridx, rc);
    if (rc)
    {
        /* dump first four pixels as R,G,B,A */
        fprintf(g_logfp, " px0=0x%08x px1=0x%08x px2=0x%08x px3=0x%08x",
                raster[0], raster[1], raster[2], raster[3]);
    }
    fputc('\n', g_logfp);
    _TIFFfree(raster);
    (void)emsg;
}

static void exercise(const char *path, int use_mmap)
{
    g_log_errors = 0;
    g_log_warnings = 0;
    fprintf(g_logfp, "  [%s mode]\n", use_mmap ? "mmap " : "stream");

    /* Handlers must be installed before TIFFOpen: the first TIFFReadDirectory
     * call runs inside TIFFOpen itself (tif_open.c). */
    TIFFSetErrorHandlerExt(error_handler);
    TIFFSetWarningHandlerExt(warn_handler);

    TIFF *tif = TIFFOpen(path, use_mmap ? "r" : "rm");
    if (!tif)
    {
        fprintf(g_logfp, "    TIFFOpen FAILED (errors=%d warnings=%d) - "
                "first IFD could not be read, no directories available\n",
                g_log_errors, g_log_warnings);
        return;
    }
    fprintf(g_logfp, "    TIFFOpen OK; first IFD already read inside open\n");

    /* Directory 0 was read inside TIFFOpen. Decode it, then walk the rest of
     * the chain exactly like a sequential reader would. */
    int diridx = 0;
    decode_current_dir(tif, diridx);
    diridx++;

    while (1)
    {
        fprintf(g_logfp, "    -- TIFFReadDirectory #%d (nextdiroff=%llu)\n",
                diridx, (unsigned long long)tif->tif_nextdiroff);
        int before_e = g_log_errors, before_w = g_log_warnings;
        int ok = TIFFReadDirectory(tif);
        fprintf(g_logfp,
                "    TIFFReadDirectory #%d -> %d (new errors=%d warnings=%d)\n",
                diridx, ok, g_log_errors - before_e,
                g_log_warnings - before_w);
        if (!ok)
        {
            fprintf(g_logfp, "    chain stopped at dir %d; "
                    "TIFFLastDirectory=%d nextdiroff=%llu\n",
                    diridx, TIFFLastDirectory(tif),
                    (unsigned long long)tif->tif_nextdiroff);
            break;
        }
        decode_current_dir(tif, diridx);
        diridx++;
        if (TIFFLastDirectory(tif) || diridx > 8)
        {
            fprintf(g_logfp, "    end of chain (dirs walked=%d)\n", diridx);
            break;
        }
    }

    /* Also test random access via TIFFSetDirectory, which uses the hash maps
     * built from the chain walk. */
    int sd0 = TIFFSetDirectory(tif, 0);
    fprintf(g_logfp, "    TIFFSetDirectory(0) -> %d\n", sd0);
    if (sd0)
    {
        uint32_t w = 0;
        TIFFGetField(tif, TIFFTAG_IMAGEWIDTH, &w);
        fprintf(g_logfp, "    after SetDirectory(0): width=%u\n", w);
    }
    int sd1 = TIFFSetDirectory(tif, 1);
    fprintf(g_logfp, "    TIFFSetDirectory(1) -> %d\n", sd1);
    int sd2 = TIFFSetDirectory(tif, 2);
    fprintf(g_logfp, "    TIFFSetDirectory(2) -> %d\n", sd2);

    TIFFClose(tif);
    fprintf(g_logfp, "  TOTAL errors=%d warnings=%d\n",
            g_log_errors, g_log_warnings);
}

int main(int argc, char **argv)
{
    if (argc < 3)
    {
        fprintf(stderr, "usage: %s <logfile> <tif> [<tif> ...]\n", argv[0]);
        return 2;
    }
    g_logfp = fopen(argv[1], "w");
    if (!g_logfp)
    {
        perror("fopen log");
        return 2;
    }
    for (int i = 2; i < argc; i++)
    {
        fprintf(g_logfp, "==== %s ====\n", argv[i]);
        printf("testing %s\n", argv[i]);
        exercise(argv[i], 1);
        exercise(argv[i], 0);
        fputc('\n', g_logfp);
    }
    fclose(g_logfp);
    return 0;
}
