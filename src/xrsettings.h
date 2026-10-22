/*
 * xrsettings.h - job settings of the Xerox Phaser 6000/6010 HBPL driver,
 * resolved from the PPD and the CUPS job options.
 */
#ifndef XRSETTINGS_H
#define XRSETTINGS_H

#include "xrcolor.h"

#define XR_COLORMODE_COLOR 0x16
#define XR_COLORMODE_MONO 0x11

typedef struct {
    int color_mode;          /* XR_COLORMODE_COLOR or XR_COLORMODE_MONO */
    char media[64];          /* MediaType choice */
    enum xr_doctype doctype; /* XRDocumentType */
    int dlut, ucr, trc, screen, monotrc, monoscreen; /* from XRDocumentType */
    int screen2_mode;        /* XRScreen2: 0 Auto, 1 Fineness, 2 Standard, 3 Gradation */
    int screen2, monoscreen2;
    int turn_page;           /* XRTurnPage: rotate 180 degrees */
    int skip_blank;          /* XRSkip */
    int toner_save;          /* XRTonerSaver */
    int density[12];         /* XRColorDensity{Cyan,Magenta,Yellow,Black}{Low,Mid,High} */
    int collate;
    int copies;
    int have_options;        /* job options were supplied (NLPP/NUP/PAGEORDER lines) */
    int number_up;
    int output_reverse;
    /* page size (PostScript points) */
    int page_code;           /* HBPL paper code, 0xff = custom */
    float width, length, left, bottom, right, top;
    char dlut_path[1024];
} xr_settings;

/*
 * Reads the PPD named by $PPD (or ppd_path) and applies the job options.
 * Returns 0 on success, -1 on error (message in *err).
 */
int xr_settings_load(xr_settings *s, const char *ppd_path, int copies,
                     const char *options, const char **err);

#endif
