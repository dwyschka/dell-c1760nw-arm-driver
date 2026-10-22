/*
 * gen - deterministic FX raster test images for the regression tests.
 * usage: gen rgb|cmyk|mono KIND WIDTH HEIGHT SEED > file
 *   KIND: zero, white, rand, sparse, blocks, grad, photo
 * Writes the 48 byte FX raster header followed by the pixels.
 */
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
    int32_t h[12];
    int w, ht, bpp, x, y, c;
    const char *kind;
    uint8_t *px, pal[4][4];

    if (argc != 6)
        return 2;
    bpp = !strcmp(argv[1], "rgb") ? 3 : 4;
    kind = argv[2];
    w = atoi(argv[3]);
    ht = atoi(argv[4]);
    st = (uint32_t)atoi(argv[5]) * 2654435761u + 1;
    px = calloc((size_t)w * ht, bpp);
    for (c = 0; c < 16; c++)
        pal[c / 4][c % 4] = rnd();
    for (y = 0; y < ht; y++)
        for (x = 0; x < w; x++) {
            uint8_t *p = px + ((size_t)y * w + x) * bpp;
            for (c = 0; c < bpp; c++) {
                uint8_t v = 0;
                if (!strcmp(kind, "white")) v = bpp == 3 ? 255 : 0;
                else if (!strcmp(kind, "rand")) v = rnd();
                else if (!strcmp(kind, "sparse")) v = rnd() % 23 == 0 ? (uint8_t)rnd() : 0;
                else if (!strcmp(kind, "blocks"))
                    v = rnd() % 40 ? pal[(x / 7 + y / 3) % 4][c] : (uint8_t)rnd();
                else if (!strcmp(kind, "grad")) v = (uint8_t)((x * 3 + y * 5 + c * 40) / 4);
                else if (!strcmp(kind, "photo"))
                    v = (uint8_t)((x * (c + 1) + y * (3 - c % 3)) / 3 + rnd() % 9);
                if (!strcmp(argv[1], "mono") && c != 3)
                    v = 0;
                p[c] = v;
            }
        }
    h[0] = w;
    h[1] = ht;
    h[2] = bpp * 8;
    h[3] = w * bpp;
    h[4] = w * ht * bpp;
    h[5] = h[6] = 600;
    h[7] = h[8] = 0;
    h[9] = bpp == 3 ? 1 : 3;
    h[10] = 0;
    h[11] = 1;
    fwrite(h, 4, 12, stdout); /* test hosts are little endian */
    fwrite(px, bpp, (size_t)w * ht, stdout);
    free(px);
    return 0;
}
