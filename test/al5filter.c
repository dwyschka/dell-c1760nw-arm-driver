/*
 * al5filter - drop-in replacement for the XRM_AL5CB stage, used to compare
 * the reimplementation against the original filter.  Reads FX raster pages
 * (48 byte header + data) on stdin, writes compressed pages to stdout.
 * usage: al5filter [--mono] [--no-lph]
 */
#include "../src/alla5.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void put32(FILE *f, int32_t v) { fwrite(&v, 4, 1, f); }

int main(int argc, char **argv)
{
    int32_t h[12];
    int i, lph = 1, force_mono = 0;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--no-lph"))
            lph = 0;
        else if (!strcmp(argv[i], "--mono"))
            force_mono = 1;
    }
    while (fread(h, 4, 12, stdin) == 12) {
        int w = h[0], ht = h[1], bpl = h[3], color = !force_mono;
        uint8_t *line = malloc(bpl), *mono = malloc(w);
        a5_enc *e = a5_create(w, color, lph);
        int32_t lens[5] = {0};
        int order[5] = {0, 4, 1, 2, 3}, n, y, x;

        for (y = 0; y < ht; y++) {
            if (fread(line, 1, bpl, stdin) != (size_t)bpl)
                return 1;
            if (color)
                a5_encode_line(e, line);
            else {
                for (x = 0; x < w; x++)
                    mono[x] = line[4 * x + 3];
                a5_encode_line(e, mono);
            }
        }
        a5_finish(e);
        n = a5_nstreams(e);
        for (i = 0; i < n; i++) {
            lens[i] = (int32_t)a5_stream_get(e, i)->len;
            if (i && !lens[i])
                lens[i] = 1; /* empty stream is written as a single 'A' */
        }
        if (lph)
            h[0] += 8;
        h[4] = 16;
        for (i = 0; i < n; i++)
            h[4] += lens[i];
        h[9] = color ? 3 : 0;
        h[10] = lph ? 7 : 6;
        fwrite(h, 4, 12, stdout);
        put32(stdout, lens[0]);
        if (color) {
            put32(stdout, lens[4]);
            put32(stdout, lens[1]);
            put32(stdout, lens[2]);
        } else {
            put32(stdout, lens[1]);
            put32(stdout, 0);
            put32(stdout, 0);
        }
        for (i = 0; i < n; i++) {
            int s = color ? order[i] : i;
            const a5_stream *st = a5_stream_get(e, s);
            if (st->len)
                fwrite(st->data, 1, st->len, stdout);
            else
                fputc('A', stdout);
        }
        a5_destroy(e);
        free(line);
        free(mono);
    }
    return 0;
}
