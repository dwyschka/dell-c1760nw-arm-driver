/*
 * xrpage.c - page pipeline of the Phaser 6000/6010 filter chain.
 */
#include "xrpage.h"

#include <stdlib.h>
#include <string.h>

#include "alla5.h"

struct xr_page {
    const xr_settings *s;
    xr_color *cc;
    a5_enc *enc;
    int width, height, xres, yres, color;
    int blank;        /* no non-white pixel seen yet */
    uint8_t *cmyk, *k;
};

xr_page *xr_page_begin(const xr_settings *s, xr_color *cc, int width, int height, int xres,
                       int yres)
{
    xr_page *p = calloc(1, sizeof(*p));
    if (!p)
        return NULL;
    p->s = s;
    p->cc = cc;
    p->width = width;
    p->height = height;
    p->xres = xres;
    p->yres = yres;
    p->color = s->color_mode == XR_COLORMODE_COLOR;
    p->blank = 1;
    p->cmyk = malloc(4 * (size_t)width);
    p->k = malloc(width);
    p->enc = a5_create(width, p->color, 1);
    if (!p->cmyk || !p->k || !p->enc) {
        a5_destroy(p->enc);
        free(p->cmyk);
        free(p->k);
        free(p);
        return NULL;
    }
    return p;
}

void xr_page_line(xr_page *p, const uint8_t *rgb)
{
    int i, n = 3 * p->width;

    if (p->blank) /* XRM_SBP: a page is blank when every byte is 0xff */
        for (i = 0; i < n; i++)
            if (rgb[i] != 0xff) {
                p->blank = 0;
                break;
            }
    xr_color_line(p->cc, rgb, p->cmyk, p->width);
    if (p->s->toner_save) /* XRM_TRC */
        for (i = 0; i < 4 * p->width; i++)
            p->cmyk[i] = (uint8_t)(int)((float)p->cmyk[i] * 0.7f);
    if (p->color)
        a5_encode_line(p->enc, p->cmyk);
    else {
        for (i = 0; i < p->width; i++)
            p->k[i] = p->cmyk[4 * i + 3];
        a5_encode_line(p->enc, p->k);
    }
}

int xr_page_end(xr_page *p, int32_t fx[FX_NFIELDS], uint8_t **data)
{
    static const int color_order[5] = {0, 4, 1, 2, 3};
    int32_t lens[5] = {0};
    int i, n, keep = !(p->s->skip_blank && p->blank);
    size_t total = 16, off;
    uint8_t *d = NULL;

    if (keep) {
        a5_finish(p->enc);
        n = a5_nstreams(p->enc);
        for (i = 0; i < n; i++) {
            lens[i] = (int32_t)a5_stream_get(p->enc, i)->len;
            if (i && !lens[i])
                lens[i] = 1; /* an empty stream is sent as a single 'A' */
            total += lens[i];
        }
        d = malloc(total);
        if (!d) {
            keep = -1;
            goto out;
        }
        /* stream length table (little endian): 0, 4, 1, 2 (colour) or 0, 1, 0, 0 (mono) */
        {
            int32_t t[4] = {lens[0], p->color ? lens[4] : lens[1], p->color ? lens[1] : 0,
                            p->color ? lens[2] : 0};
            int k;
            for (k = 0; k < 16; k++)
                d[k] = (uint8_t)((uint32_t)t[k / 4] >> (8 * (k % 4)));
        }
        off = 16;
        for (i = 0; i < n; i++) {
            const a5_stream *st = a5_stream_get(p->enc, p->color ? color_order[i] : i);
            if (st->len)
                memcpy(d + off, st->data, st->len);
            else
                d[off] = 'A';
            off += lens[p->color ? color_order[i] : i];
        }
        fx[FX_WIDTH] = p->width + 8;
        fx[FX_HEIGHT] = p->height;
        fx[FX_BPP] = 32;
        fx[FX_BPL] = 4 * p->width;
        fx[FX_SIZE] = (int32_t)total;
        fx[FX_XRES] = p->xres;
        fx[FX_YRES] = p->yres;
        fx[FX_LANDSCAPE] = p->height < p->width;
        fx[FX_DUPLEX] = 0;
        fx[FX_CSPACE] = p->color ? 3 : 0;
        fx[FX_COMPRESSION] = 7;
        fx[FX_PAGES] = 1;
        *data = d;
    }
out:
    a5_destroy(p->enc);
    free(p->cmyk);
    free(p->k);
    free(p);
    return keep;
}
