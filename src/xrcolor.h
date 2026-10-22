/*
 * xrcolor.h - RGB to CMYK conversion of the Xerox host-based filter (XRM_CC).
 */
#ifndef XRCOLOR_H
#define XRCOLOR_H

#include <stdint.h>

enum xr_doctype {
    XR_DOC_NORMAL = 0,
    XR_DOC_PHOTO,
    XR_DOC_PRESENTATION,
    XR_DOC_WEB,
    XR_DOC_POP,
    XR_DOC_OFF /* no colour matching: plain UCR */
};

typedef struct xr_color xr_color;

/*
 * dlut_path: the "Xerox Phaser 6010.dlut" file from the original driver
 * package (only needed for colour output with doctype != XR_DOC_OFF).
 * mono: produce black-only output (CMY = 0).
 * Returns NULL on error and sets *err to a static message.
 */
xr_color *xr_color_create(const char *dlut_path, enum xr_doctype doc, int mono,
                          const char **err);
/* rgb: width * 3 bytes (R,G,B); cmyk: width * 4 bytes (C,M,Y,K) */
void xr_color_line(xr_color *c, const uint8_t *rgb, uint8_t *cmyk, int width);
void xr_color_destroy(xr_color *c);

enum xr_doctype xr_doctype_from_name(const char *name);

#endif
