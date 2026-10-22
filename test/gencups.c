/*
 * gencups - deterministic multi-page CUPS raster (RGB, 8 bit, 600 dpi) for
 * end-to-end tests of rastertoxrhbpl.
 * usage: gencups WIDTH HEIGHT PAGES SEED > file.ras
 * Page 1: shapes and gradients, page 2: noisy photo, page 3: blank.
 */
#include <cups/raster.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t st;
static uint32_t rnd(void)
{
    st ^= st << 13;
    st ^= st >> 17;
    st ^= st << 5;
    return st;
}

int main(int argc, char **argv)
{
    cups_raster_t *r;
    cups_page_header2_t h;
    int w, ht, pages, pg, x, y;
    uint8_t *line;

    if (argc != 5)
        return 2;
    w = atoi(argv[1]);
    ht = atoi(argv[2]);
    pages = atoi(argv[3]);
    st = (uint32_t)atoi(argv[4]) * 2654435761u + 1;
    line = malloc(3 * (size_t)w);
    r = cupsRasterOpen(1, CUPS_RASTER_WRITE);
    for (pg = 0; pg < pages; pg++) {
        memset(&h, 0, sizeof(h));
        h.HWResolution[0] = h.HWResolution[1] = 600;
        h.PageSize[0] = (w * 72 + 599) / 600;
        h.PageSize[1] = (ht * 72 + 599) / 600;
        h.cupsWidth = w;
        h.cupsHeight = ht;
        h.cupsBitsPerColor = 8;
        h.cupsBitsPerPixel = 24;
        h.cupsBytesPerLine = 3 * w;
        h.cupsColorOrder = CUPS_ORDER_CHUNKED;
        h.cupsColorSpace = CUPS_CSPACE_RGB;
        h.NumCopies = 1;
        cupsRasterWriteHeader2(r, &h);
        for (y = 0; y < ht; y++) {
            for (x = 0; x < w; x++) {
                uint8_t *p = line + 3 * x;
                switch (pg % 3) {
                case 0:
                    if (x > w / 10 && x < w / 3 && y > ht / 8 && y < ht / 4)
                        p[0] = 255, p[1] = 0, p[2] = 0;
                    else if ((x - w / 2) * (x - w / 2) + (y - ht / 2) * (y - ht / 2) < w * w / 25)
                        p[0] = x * 255 / w, p[1] = y * 255 / ht, p[2] = 128;
                    else if ((x + 2 * y) % 97 < 3)
                        p[0] = p[1] = p[2] = 0;
                    else
                        p[0] = p[1] = p[2] = 255;
                    break;
                case 1:
                    p[0] = (uint8_t)(x * 255 / w + rnd() % 11);
                    p[1] = (uint8_t)(y * 255 / ht + rnd() % 11);
                    p[2] = (uint8_t)((x + y) / 7 + rnd() % 5);
                    break;
                default:
                    p[0] = p[1] = p[2] = 255;
                }
            }
            cupsRasterWritePixels(r, line, 3 * w);
        }
    }
    cupsRasterClose(r);
    free(line);
    return 0;
}
