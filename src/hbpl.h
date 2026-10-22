/*
 * hbpl.h - XPJL header and HBPL (host based printer language) records for
 * the Xerox Phaser 6000/6010 (XRM_HBPL / "rastertohbpl").
 */
#ifndef HBPL_H
#define HBPL_H

#include <stdint.h>
#include <stdio.h>

#include "xrsettings.h"

/* FX raster header as produced by the compressor stage */
enum {
    FX_WIDTH, FX_HEIGHT, FX_BPP, FX_BPL, FX_SIZE, FX_XRES, FX_YRES,
    FX_LANDSCAPE, FX_DUPLEX, FX_CSPACE, FX_COMPRESSION, FX_PAGES, FX_NFIELDS
};

typedef struct {
    int page_index; /* running page number written to the printer (from 1) */
    int pages;      /* pages written so far (PAGEINFO records) */
} hbpl_job;

void hbpl_job_begin(FILE *out, hbpl_job *j, const xr_settings *s, const char *user,
                    const char *title, int bits_per_pixel);
/* data: the compressed page (16 byte stream length table + streams) */
void hbpl_page(FILE *out, hbpl_job *j, const xr_settings *s, const int32_t *fx,
               const uint8_t *data);
/* pages: number of pages of one copy of the document */
void hbpl_job_end(FILE *out, hbpl_job *j, const xr_settings *s, int pages);

#endif
