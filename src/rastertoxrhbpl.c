/*
 * rastertoxrhbpl - CUPS filter for the Xerox Phaser 6000/6010 (HBPL).
 *
 * Replaces the macOS filter chain XRM_MF -> SP, FXR, SBP, PR, CC, TRC,
 * AL5CB, HBPL of the original Xerox driver with a portable implementation.
 *
 * usage: rastertoxrhbpl job-id user title copies options [file]
 */
#include <cups/cups.h>
#include <cups/raster.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "hbpl.h"
#include "xrcolor.h"
#include "xrpage.h"
#include "xrsettings.h"

#ifndef DEFAULT_DLUT
#define DEFAULT_DLUT "/usr/share/xerox-phaser6000/Xerox Phaser 6010.dlut"
#endif

static volatile sig_atomic_t canceled;

static void on_term(int sig)
{
    (void)sig;
    canceled = 1;
}

/* Converts one raster line to 8 bit RGB. Returns -1 for unsupported data. */
static int to_rgb(const cups_page_header2_t *h, const uint8_t *in, uint8_t *rgb)
{
    unsigned x, w = h->cupsWidth;

    if (h->cupsBitsPerColor != 8 || h->cupsColorOrder != CUPS_ORDER_CHUNKED)
        return -1;
    switch (h->cupsColorSpace) {
    case CUPS_CSPACE_RGB:
    case CUPS_CSPACE_SRGB:
    case CUPS_CSPACE_ADOBERGB:
        if (h->cupsBitsPerPixel != 24)
            return -1;
        memcpy(rgb, in, 3 * (size_t)w);
        return 0;
    case CUPS_CSPACE_W:
    case CUPS_CSPACE_SW:
        for (x = 0; x < w; x++)
            rgb[3 * x] = rgb[3 * x + 1] = rgb[3 * x + 2] = in[x];
        return 0;
    case CUPS_CSPACE_K:
        for (x = 0; x < w; x++)
            rgb[3 * x] = rgb[3 * x + 1] = rgb[3 * x + 2] = (uint8_t)~in[x];
        return 0;
    default:
        return -1;
    }
}

static void reverse_pixels(uint8_t *rgb, unsigned w)
{
    unsigned a, b;
    for (a = 0, b = w - 1; a < b; a++, b--) {
        uint8_t t[3];
        memcpy(t, rgb + 3 * a, 3);
        memcpy(rgb + 3 * a, rgb + 3 * b, 3);
        memcpy(rgb + 3 * b, t, 3);
    }
}

/* spooled, compressed page */
typedef struct {
    int32_t fx[FX_NFIELDS];
    long offset;
} spool_page;

int main(int argc, char **argv)
{
    xr_settings s;
    xr_color *cc;
    cups_raster_t *ras;
    cups_page_header2_t h;
    hbpl_job job;
    spool_page *pages = NULL;
    FILE *spool, *rot = NULL;
    const char *err = NULL;
    uint8_t *line = NULL, *rgb = NULL;
    int fd = 0, npages = 0, copy, i, rc = 1;

    setbuf(stderr, NULL);
    signal(SIGTERM, on_term);
    if (argc < 6 || argc > 7) {
        fputs("Usage: rastertoxrhbpl job-id user title copies options [file]\n", stderr);
        return 1;
    }
    if (argc == 7 && (fd = open(argv[6], O_RDONLY)) < 0) {
        fprintf(stderr, "ERROR: Unable to open raster file: %s\n", strerror(errno));
        return 1;
    }
    if (xr_settings_load(&s, NULL, atoi(argv[4]), argv[5], &err)) {
        fprintf(stderr, "ERROR: %s\n", err);
        return 1;
    }
    if (!s.dlut_path[0])
        snprintf(s.dlut_path, sizeof(s.dlut_path), "%s", DEFAULT_DLUT);
    cc = xr_color_create(s.dlut_path, s.doctype, s.color_mode != XR_COLORMODE_COLOR, &err);
    if (!cc) {
        fprintf(stderr, "ERROR: %s (%s)\n", err, s.dlut_path);
        return 1;
    }
    if (!(spool = tmpfile())) {
        fputs("ERROR: Unable to create spool file\n", stderr);
        return 1;
    }
    ras = cupsRasterOpen(fd, CUPS_RASTER_READ);

    while (!canceled && cupsRasterReadHeader2(ras, &h)) {
        xr_page *p;
        int32_t fx[FX_NFIELDS];
        uint8_t *data;
        unsigned y;
        int keep;

        fprintf(stderr, "INFO: Processing page %d\n", npages + 1);
        line = realloc(line, h.cupsBytesPerLine);
        rgb = realloc(rgb, 3 * (size_t)h.cupsWidth);
        if (!line || !rgb) {
            fputs("ERROR: Out of memory\n", stderr);
            goto out;
        }
        p = xr_page_begin(&s, cc, h.cupsWidth, h.cupsHeight, h.HWResolution[0],
                          h.HWResolution[1]);
        if (!p) {
            fputs("ERROR: Out of memory\n", stderr);
            goto out;
        }
        if (s.turn_page && !(rot = tmpfile())) {
            fputs("ERROR: Unable to create temporary file\n", stderr);
            goto out;
        }
        for (y = 0; y < h.cupsHeight; y++) {
            if (cupsRasterReadPixels(ras, line, h.cupsBytesPerLine) != h.cupsBytesPerLine) {
                fputs("ERROR: Short raster data\n", stderr);
                goto out;
            }
            if (to_rgb(&h, line, rgb)) {
                fprintf(stderr, "ERROR: Unsupported raster format (colorspace %d, %d bit)\n",
                        h.cupsColorSpace, h.cupsBitsPerPixel);
                goto out;
            }
            if (rot)
                fwrite(rgb, 3, h.cupsWidth, rot);
            else
                xr_page_line(p, rgb);
        }
        if (rot) { /* XRM_PR: rotate by 180 degrees */
            for (y = h.cupsHeight; y-- > 0;) {
                if (fseek(rot, (long)y * 3 * h.cupsWidth, SEEK_SET) ||
                    fread(rgb, 3, h.cupsWidth, rot) != h.cupsWidth) {
                    fputs("ERROR: Unable to read temporary file\n", stderr);
                    goto out;
                }
                reverse_pixels(rgb, h.cupsWidth);
                xr_page_line(p, rgb);
            }
            fclose(rot);
            rot = NULL;
        }
        keep = xr_page_end(p, fx, &data);
        if (keep < 0) {
            fputs("ERROR: Out of memory\n", stderr);
            goto out;
        }
        if (!keep) {
            fprintf(stderr, "INFO: Skipping blank page %d\n", npages + 1);
            continue;
        }
        pages = realloc(pages, sizeof(*pages) * (npages + 1));
        if (!pages) {
            fputs("ERROR: Out of memory\n", stderr);
            free(data);
            goto out;
        }
        memcpy(pages[npages].fx, fx, sizeof(fx));
        pages[npages].offset = ftell(spool);
        if (fwrite(data, 1, fx[FX_SIZE], spool) != (size_t)fx[FX_SIZE]) {
            fputs("ERROR: Unable to write spool file\n", stderr);
            free(data);
            goto out;
        }
        free(data);
        npages++;
    }
    if (canceled)
        goto out;

    if (npages) {
        hbpl_job_begin(stdout, &job, &s, argv[2], argv[3], 32);
        for (copy = 0; copy < (s.collate ? s.copies : 1); copy++)
            for (i = 0; i < npages; i++) {
                uint8_t *data = malloc(pages[i].fx[FX_SIZE]);
                if (!data || fseek(spool, pages[i].offset, SEEK_SET) ||
                    fread(data, 1, pages[i].fx[FX_SIZE], spool) != (size_t)pages[i].fx[FX_SIZE]) {
                    fputs("ERROR: Unable to read spool file\n", stderr);
                    free(data);
                    goto out;
                }
                hbpl_page(stdout, &job, &s, pages[i].fx, data);
                free(data);
                fprintf(stderr, "PAGE: %d %d\n", job.pages, s.collate ? 1 : s.copies);
            }
        hbpl_job_end(stdout, &job, &s, npages);
    }
    fflush(stdout);
    rc = 0;
out:
    if (rot)
        fclose(rot);
    cupsRasterClose(ras);
    fclose(spool);
    xr_color_destroy(cc);
    free(pages);
    free(line);
    free(rgb);
    if (fd)
        close(fd);
    return rc;
}
