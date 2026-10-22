/*
 * xrpage.h - per page processing: RGB lines -> CMYK (XRM_CC) -> toner saver
 * curve (XRM_TRC) -> alla5 compression (XRM_AL5CB).
 */
#ifndef XRPAGE_H
#define XRPAGE_H

#include <stddef.h>
#include <stdint.h>

#include "hbpl.h"
#include "xrcolor.h"
#include "xrsettings.h"

typedef struct xr_page xr_page;

xr_page *xr_page_begin(const xr_settings *s, xr_color *cc, int width, int height, int xres,
                       int yres);
/* rgb: width * 3 bytes, lines in printing order */
void xr_page_line(xr_page *p, const uint8_t *rgb);
/*
 * Finishes the page.  Returns 1 and fills fx/data/len (data: malloc'ed
 * stream table + streams, fx[FX_SIZE] bytes), or 0 if the page is blank and
 * blank pages are skipped.
 */
int xr_page_end(xr_page *p, int32_t fx[FX_NFIELDS], uint8_t **data);

#endif
