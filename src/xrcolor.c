/*
 * xrcolor.c - reimplementation of XRM_CC ("colorconv").
 *
 * Colour pipeline per non-white pixel:
 *   1. CST: sRGB gamma 2.2 linearisation, 3x3 matrix (XYZ->sRGB x sRGB->XYZ,
 *      i.e. almost identity), re-encoding with the sRGB transfer curve.
 *      All tables are generated from the public sRGB definitions; they match
 *      the tables in the original binary exactly.
 *   2. Tetrahedral interpolation in a 17x17x17 RGB->CMYK table taken from the
 *      A_DLUT_PRINT section of the printer's .dlut file.
 * Monochrome uses K = 255 - luma, CMY = 0.
 *
 * NOTE: build with -ffp-contract=off so the matrix is computed with the same
 * rounding as the original (no fused multiply-add).
 */
#include "xrcolor.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LUT_N 17
#define LUT_ENTRIES (LUT_N * LUT_N * LUT_N)

struct xr_color {
    int mono;
    int cst;            /* colour matching enabled (doctype != Off) */
    uint16_t gamma[256];
    uint8_t trc[4096];
    int coef[3][3];
    uint8_t lut[LUT_ENTRIES][4]; /* C,M,Y,K per grid node */
    int have_lut;
    /* one entry cache, like the original */
    uint32_t last_rgb;
    uint8_t last_out[4];
};

static uint32_t be32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static unsigned be16(const uint8_t *p) { return (unsigned)p[0] << 8 | p[1]; }

/* sub-table of the A_DLUT_PRINT section used for each XRDocumentType */
static const int doc_table_index[] = {1, 4, 5, 7, 9, 21};

/* cms_Init + cms_getADD + get_dlutParam */
static int load_dlut(xr_color *c, const char *path, enum xr_doctype doc, const char **err)
{
    FILE *f = fopen(path, "rb");
    uint8_t *d = NULL;
    long size;
    int i, n, ok = 0;

    if (!f) {
        *err = "cannot open DLUT file";
        return -1;
    }
    if (fseek(f, 0, SEEK_END) || (size = ftell(f)) < 0x1c || fseek(f, 0, SEEK_SET) ||
        !(d = malloc(size)) || fread(d, 1, size, f) != (size_t)size) {
        *err = "cannot read DLUT file";
        goto out;
    }
    *err = "unsupported DLUT file";
    n = d[0x1b];
    for (i = 0; i < n; i++) {
        const uint8_t *e = d + 0x1c + 0x1c * i;
        uint32_t off, cnt1, cnt2, j, k;
        const uint8_t *blk, *p2;
        long sub = -1;

        if (e + 0x1c > d + size)
            goto out;
        if (memcmp(e, "A_DLUT_PRINT_________V01", 24))
            continue;
        off = be32(e + 24);
        if (off + 0x1c > (uint32_t)size)
            goto out;
        blk = d + off;
        cnt1 = be32(blk + 0x18);
        if (off + 0x1c + 4ull * cnt1 + 4 > (uint64_t)size)
            goto out;
        p2 = blk + 0x1c + 4 * cnt1;
        cnt2 = be32(p2);
        k = doc_table_index[doc];
        if (k >= cnt2 || p2 + 6 + 2 * k > d + size)
            goto out;
        j = be16(p2 + 4 + 2 * k);
        if (j >= cnt1)
            goto out;
        sub = off + be32(blk + 0x1c + 4 * j);
        if (sub + 0x308 + 4L * LUT_ENTRIES > size)
            goto out;
        memcpy(c->lut, d + sub + 0x308, sizeof(c->lut));
        c->have_lut = 1;
        ok = 1;
        break;
    }
    if (!ok && i == n)
        *err = "no A_DLUT_PRINT section in DLUT file";
out:
    free(d);
    fclose(f);
    return ok ? 0 : -1;
}

static double srgb_encode(double v)
{
    return v <= 0.0031308 ? 12.92 * v : 1.055 * pow(v, 1 / 2.4) - 0.055;
}

/* cil_Create_CST_Param_WpGm(6500K, gamma 2.2) */
static void init_cst(xr_color *c)
{
    static const double xyz2srgb[3][3] = {
        {3.241, -1.5374, -0.4986}, {-0.9692, 1.876, 0.0416}, {0.0556, -0.204, 1.057}};
    static const double srgb2xyz[3][3] = {
        {0.4124, 0.3576, 0.1805}, {0.2126, 0.7152, 0.0722}, {0.0193, 0.1192, 0.9505}};
    int i, j;

    for (i = 0; i < 256; i++)
        c->gamma[i] = (uint16_t)floor(65535.0 * pow(i / 255.0, 2.2) + 0.5);
    for (i = 0; i < 4096; i++) {
        double v = floor(255.0 * srgb_encode(i / 4095.0) + 0.5);
        c->trc[i] = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
    }
    /* volatile temporaries keep every product rounded to double, so no
       compiler can contract them into fused multiply-adds (e.g. on aarch64) */
    for (i = 0; i < 3; i++)
        for (j = 0; j < 3; j++) {
            volatile double p0 = xyz2srgb[i][0] * srgb2xyz[0][j];
            volatile double p1 = xyz2srgb[i][1] * srgb2xyz[1][j];
            volatile double p2 = xyz2srgb[i][2] * srgb2xyz[2][j];
            volatile double s = p0 + 0.0;
            s = p1 + s;
            s = p2 + s;
            c->coef[i][j] = (int)(s * 65536.0);
        }
}

static uint32_t fnv1a(uint32_t h, const void *p, size_t n)
{
    const uint8_t *b = p;
    while (n--)
        h = (h ^ *b++) * 0x01000193u;
    return h;
}

/*
 * The CST tables are computed with pow(); verify that this platform's libm
 * reproduced exactly the tables of the original driver.
 */
static int check_cst(const xr_color *c)
{
    uint8_t g[512], k[36];
    int i;
    for (i = 0; i < 256; i++) {
        g[2 * i] = c->gamma[i] & 0xff;
        g[2 * i + 1] = c->gamma[i] >> 8;
    }
    for (i = 0; i < 36; i++)
        k[i] = (uint8_t)((uint32_t)c->coef[i / 12][(i / 4) % 3] >> (8 * (i % 4)));
    return fnv1a(0x811c9dc5u, g, sizeof(g)) == 0xf4e9427cu &&
           fnv1a(0x811c9dc5u, c->trc, sizeof(c->trc)) == 0xe6d50448u &&
           fnv1a(0x811c9dc5u, k, sizeof(k)) == 0x51c6e074u;
}

xr_color *xr_color_create(const char *dlut_path, enum xr_doctype doc, int mono,
                          const char **err)
{
    xr_color *c = calloc(1, sizeof(*c));
    if (!c) {
        *err = "out of memory";
        return NULL;
    }
    c->mono = mono;
    c->cst = doc != XR_DOC_OFF;
    c->last_rgb = 0xffffffff; /* no valid cached pixel */
    init_cst(c);
    if (!check_cst(c))
        fputs("WARNING: colour tables differ from the original driver (libm rounding)\n", stderr);
    if (!mono && c->cst && load_dlut(c, dlut_path, doc, err)) {
        free(c);
        return NULL;
    }
    return c;
}

void xr_color_destroy(xr_color *c) { free(c); }

enum xr_doctype xr_doctype_from_name(const char *n)
{
    if (!n) return XR_DOC_NORMAL;
    if (!strcmp(n, "Photo")) return XR_DOC_PHOTO;
    if (!strcmp(n, "Presentation")) return XR_DOC_PRESENTATION;
    if (!strcmp(n, "Web")) return XR_DOC_WEB;
    if (!strcmp(n, "POP")) return XR_DOC_POP;
    if (!strcmp(n, "Off")) return XR_DOC_OFF;
    return XR_DOC_NORMAL;
}

static int clamp12(int64_t v) { return v <= 0 ? 0 : v > 4095 ? 4095 : (int)v; }

/* cil_CST_Conv_sBGR_Pixel */
static void cst(const xr_color *c, uint8_t *p)
{
    int64_t r = c->gamma[p[0]], g = c->gamma[p[1]], b = c->gamma[p[2]];
    int64_t o[3];
    int i;
    for (i = 0; i < 3; i++)
        o[i] = ((c->coef[i][0] >> 2) * r + (c->coef[i][1] >> 2) * g +
                (c->coef[i][2] >> 2) * b) >> 18;
    for (i = 0; i < 3; i++)
        p[i] = c->trc[clamp12(o[i])];
}

/* DlutCMYKProc: tetrahedral interpolation, axes (B, G, R) */
static void dlut(const xr_color *c, int b, int g, int r, uint8_t *out)
{
    int i1 = b >> 4, i2 = g >> 4, i3 = r >> 4;
    int f1 = b == 255 ? 16 : b & 15, f2 = g == 255 ? 16 : g & 15, f3 = r == 255 ? 16 : r & 15;
    int base = i1 * 289 + i2 * 17 + i3;
    int s1 = 289, s2 = 17, s3 = 1;
    int fa, fb, fc, da, db, dc, k;

    /* order the axes by descending fraction */
    if (f1 >= f2 && f2 >= f3)      { fa = f1; fb = f2; fc = f3; da = s1; db = s2; dc = s3; }
    else if (f1 >= f3 && f3 >= f2) { fa = f1; fb = f3; fc = f2; da = s1; db = s3; dc = s2; }
    else if (f2 >= f1 && f1 >= f3) { fa = f2; fb = f1; fc = f3; da = s2; db = s1; dc = s3; }
    else if (f2 >= f3 && f3 >= f1) { fa = f2; fb = f3; fc = f1; da = s2; db = s3; dc = s1; }
    else if (f3 >= f1 && f1 >= f2) { fa = f3; fb = f1; fc = f2; da = s3; db = s1; dc = s2; }
    else                           { fa = f3; fb = f2; fc = f1; da = s3; db = s2; dc = s1; }

    for (k = 0; k < 4; k++) {
        unsigned v = (16 - fa) * c->lut[base][k] + (fa - fb) * c->lut[base + da][k] +
                     (fb - fc) * c->lut[base + da + db][k] +
                     fc * c->lut[base + da + db + dc][k];
        v >>= 4;
        out[k] = v > 255 ? 255 : (uint8_t)v;
    }
}

static void convert(const xr_color *c, const uint8_t *in, uint8_t *out)
{
    uint8_t p[3] = {in[0], in[1], in[2]};
    int white = p[0] == 255 && p[1] == 255 && p[2] == 255;

    if (!white && c->cst)
        cst(c, p);
    if (c->mono) {
        unsigned y = (p[1] * 0x970au + p[0] * 0x4ccdu + p[2] * 0x1c29u) >> 16;
        out[0] = out[1] = out[2] = 0;
        out[3] = (uint8_t)(255 - y);
        return;
    }
    if (p[0] == 255 && p[1] == 255 && p[2] == 255) {
        memset(out, 0, 4);
        return;
    }
    if (!c->cst) {
        int k = p[0];
        if (p[1] > k) k = p[1];
        if (p[2] > k) k = p[2];
        k = 255 - k;
        out[0] = (uint8_t)(255 - p[0] - k);
        out[1] = (uint8_t)(255 - k - p[1]);
        out[2] = (uint8_t)(255 - k - p[2]);
        out[3] = (uint8_t)k;
        return;
    }
    dlut(c, p[2], p[1], p[0], out);
}

void xr_color_line(xr_color *c, const uint8_t *rgb, uint8_t *cmyk, int width)
{
    int x;
    for (x = 0; x < width; x++, rgb += 3, cmyk += 4) {
        uint32_t key = (uint32_t)rgb[0] | (uint32_t)rgb[1] << 8 | (uint32_t)rgb[2] << 16;
        if (key != c->last_rgb) {
            convert(c, rgb, c->last_out);
            c->last_rgb = key;
        }
        memcpy(cmyk, c->last_out, 4);
    }
}
