/* ccfilter - drop-in replacement for the XRM_CC stage (RGB FX raster in,
 * CMYK FX raster out), used to compare against the original.
 * usage: ccfilter dlut doctype mono */
#include "../src/xrcolor.h"
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
    int32_t h[12];
    const char *err = NULL;
    if (argc < 4) return 2;
    xr_color *c = xr_color_create(argv[1], xr_doctype_from_name(argv[2]), atoi(argv[3]), &err);
    if (!c) { fprintf(stderr, "%s\n", err); return 1; }
    while (fread(h, 4, 12, stdin) == 12) {
        int w = h[0], ht = h[1], bpl = h[3], y;
        uint8_t *in = malloc(bpl), *out = malloc(4 * w);
        h[2] = 32; h[3] = 4 * w; h[4] = 4 * w * ht; h[9] = 3;
        fwrite(h, 4, 12, stdout);
        for (y = 0; y < ht; y++) {
            if (fread(in, 1, bpl, stdin) != (size_t)bpl) return 1;
            xr_color_line(c, in, out, w);
            fwrite(out, 1, 4 * w, stdout);
        }
        free(in); free(out);
    }
    xr_color_destroy(c);
    return 0;
}
