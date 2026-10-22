/*
 * hbpl.c - XPJL job header/trailer and HBPL page records.
 * Byte-compatible with XRM_HBPL of the Xerox Phaser 6000/6010 macOS driver.
 */
#include "hbpl.h"

#include <netdb.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

/* ---- primitives ---------------------------------------------------------- */

static void put_u8(FILE *f, unsigned v) { fputc(v & 0xff, f); }

static void put_le(FILE *f, uint32_t v, int n)
{
    int i;
    for (i = 0; i < n; i++)
        fputc((v >> (8 * i)) & 0xff, f);
}

static int type_size(int type) { return type == 2 || type == 3 ? 2 : type >= 4 && type <= 6 ? 4 : 1; }

/* HBPLPutAttr_Numeric: id, 0xa0|type, value (little endian) */
static void attr_num(FILE *f, int id, int type, uint32_t v)
{
    put_u8(f, id);
    put_u8(f, 0xa0 | type);
    put_le(f, v, type_size(type));
}

/* HBPLPutAttr_XY: id, 0xc0|type, x, y */
static void attr_xy(FILE *f, int id, int type, uint32_t x, uint32_t y)
{
    put_u8(f, id);
    put_u8(f, 0xc0 | type);
    put_le(f, x, type_size(type));
    put_le(f, y, type_size(type));
}

static void uel(FILE *f) { fputs("\033%-12345X", f); }
static void pjl_attr(FILE *f, const char *k, const char *v) { fprintf(f, "@PJL SET JOBATTR=\"@%s=%s\"\n", k, v); }
static void pjl_opt(FILE *f, const char *k, const char *v) { fprintf(f, "@PJL SET %s=%s\n", k, v); }
static void pjl_comment(FILE *f, const char *s) { fprintf(f, "@PJL COMMENT %s\n", s); }

static void pjl_attr3(FILE *f, const char *k1, const char *k2, const char *k3, int v)
{
    char b[16];
    snprintf(b, sizeof(b), "%d", v);
    pjl_attr(f, k1, b);
    pjl_attr(f, k2, b);
    pjl_attr(f, k3, b);
}

/* Copies a UTF-8 string, truncated to at most max-1 bytes on a character boundary. */
static void utf8_trunc(char *dst, const char *src, size_t max)
{
    size_t n = strlen(src);
    if (n >= max) {
        n = max - 1;
        while (n > 0 && ((unsigned char)src[n] & 0xc0) == 0x80)
            n--;
    }
    memcpy(dst, src, n);
    dst[n] = 0;
}

/* fxround */
static uint32_t fxround(double d)
{
    uint32_t i = (uint32_t)(int64_t)d;
    return d - (double)i >= 0.5 ? i + 1 : i;
}

/* ---- XPJL job header (PutXPJLPrintHeader) ------------------------------- */

static const char *media_pjl(const char *m)
{
    static const char *const t[][2] = {
        {"Default", "OFF"}, {"Stationary", "NORMAL"}, {"StationaryBack", "NORMALREV"},
        {"FinePaper", "HIGHQUALITY"}, {"FinePaperBack", "HIGHQUALITYREV"},
        {"Envelope", "ENVELOPE"}, {"EnvelopeBack", "ENVELOPEREV"}, {"Postcard", "POSTCARD"},
        {"PostcardBack", "POSTCARDREV"}, {"ThickPaper", "THICK"}, {"ThickPaperBack", "THICKSIDE2"},
        {"ThickPaper2", "THICK2"}, {"ThickPaper2Back", "THICK2SIDE2"},
        {"SuperThickPaper", "SUPERTHICK"}, {"CoatingPaper1", "COATEDPAPER"},
        {"CoatingPaper1Back", "COATEDPAPERREV"}, {"CoatingPaper2", "COATEDPAPER2"},
        {"CoatingPaper2Back", "COATEDPAPER2REV"}, {"CoatingPaper3", "COATEDPAPER3"},
        {"Coat2", "COAT2"}, {"Coat3", "COAT3"}, {"LabelPaper", "LABEL"}, {"Recycled", "RECYCLE"},
        {"RecycledBack", "RECYCLEREV"}, {"Letterhead", "LETTERHEAD"},
        {"LetterheadBack", "LETTERHEADREV"}, {"Prepunched", "PREPUNCHED"},
        {"SpecialPaper", "SPECIAL"}, {"SpecialBack", "SPECIALREV"},
        {"Transparency", "TRANSPARENCY"}, {"LetterHead", "LETTERHEAD"}, {"ColoredPaper", "COLOR"},
        {"ColoredBack", "COLORREV"}, {"PrePunched", "PREPUNCHED"},
        {"PrePunchedBack", "PREPUNCHEDREV"}, {"PrePrinted", "PREPRINTED"},
        {"PrePrintedBack", "PREPRINTEDREV"}, {"User", "USER1"}, {"UserBack", "USER1REV"},
    };
    size_t i;
    for (i = 0; i < sizeof(t) / sizeof(t[0]); i++)
        if (!strncmp(t[i][0], m, 32))
            return t[i][1];
    fprintf(stderr, "WARNING: Unknown media keyword \"%s\".\n", m);
    return "OFF";
}

/* XPJLPutIPAddress: "I" + hex digits of each IPv4 byte, upper case */
static void put_ip(FILE *f)
{
    char host[256], ip[32] = "I", b[8];
    struct hostent *h;
    int i;

    if (gethostname(host, sizeof(host) - 1) || !(h = gethostbyname(host)) || h->h_length < 4)
        return;
    for (i = 0; i < 4; i++) {
        snprintf(b, sizeof(b), "%x", (unsigned char)h->h_addr_list[0][i]);
        strcat(ip, b);
    }
    for (i = 1; ip[i] && i < 15; i++)
        if (ip[i] >= 'a' && ip[i] <= 'f')
            ip[i] -= 0x20;
    pjl_attr(f, "HOAD", ip);
}

void hbpl_job_begin(FILE *out, hbpl_job *j, const xr_settings *s, const char *user,
                    const char *title, int bits_per_pixel)
{
    static const char *const aj[12] = {"AJCL", "AJCM", "AJCH", "AJML", "AJMM", "AJMH",
                                       "AJYL", "AJYM", "AJYH", "AJKL", "AJKM", "AJKH"};
    char buf[512], name[260];
    struct timeval tv;
    struct tm *tm;
    int i, color = s->color_mode == XR_COLORMODE_COLOR;

    j->page_index = 1;
    j->pages = 0;

    uel(out);
    fputs("@PJL JOB MODE=PRINTER\n", out);
    fputs("@PJL SET STRINGCODESET=UTF8\n", out);
    gettimeofday(&tv, NULL);
    tm = localtime(&tv.tv_sec);
    snprintf(buf, sizeof(buf), "DATE=%02d/%02d/%d", tm->tm_mon + 1, tm->tm_mday, tm->tm_year + 1900);
    pjl_comment(out, buf);
    snprintf(buf, sizeof(buf), "TIME=%02d:%02d:%02d", tm->tm_hour, tm->tm_min, tm->tm_sec);
    pjl_comment(out, buf);
    if (title) {
        utf8_trunc(name, title, 256);
        if (strlen(name) + 6 < 256) {
            snprintf(buf, sizeof(buf), "DNAME=%s", name);
            pjl_comment(out, buf);
        }
    }
    if (user) {
        utf8_trunc(name, user, 256);
        pjl_attr(out, "LUNA", name);
    }
    snprintf(buf, sizeof(buf), "%d", s->collate ? 1 : s->copies);
    pjl_opt(out, s->collate ? "QTY" : "COPIES", buf);
    pjl_attr(out, "TRCH", "OFF");     /* XPJLSetTraySet: tray from the printer */
    pjl_opt(out, "DUPLEX", "OFF");
    for (i = 0; i < 12; i++)
        if (s->density[i] != -1000) {
            snprintf(buf, sizeof(buf), "%d", s->density[i] + 3);
            pjl_attr(out, aj[i], buf);
        }
    if (s->media[0]) {
        pjl_attr(out, "APSP", "OFF");
        pjl_attr(out, "MSIP", media_pjl(s->media));
    } else
        pjl_attr(out, "MSIP", "OFF");
    pjl_opt(out, "RENDERMODE", color ? "COLOR" : "GRAYSCALE");
    pjl_opt(out, "ECONOMODE", s->toner_save ? "ON" : "OFF");
    pjl_attr(out, "IREC", "OFF");
    pjl_attr(out, "TRAP", "OFF");    /* trapping is not supported by this port */
    pjl_opt(out, "RET", "OFF");      /* edge enhancement: not supported */
    pjl_attr3(out, "TDFT", "GDFT", "IDFT", 0); /* always 0 on "Maglev" models */
    if (s->have_options) {
        if (s->number_up == 1)
            snprintf(buf, sizeof(buf), "1");
        else
            snprintf(buf, sizeof(buf), "N%d", s->number_up);
        if (s->number_up > 0)
            pjl_attr(out, "NLPP", buf);
    }
    put_ip(out);
    if (user) {
        utf8_trunc(name, user, 0x20);
        pjl_attr(out, "JOAU", name);
    }
    if (!gethostname(buf, 0x1f)) {
        buf[0x1f] = 0;
        pjl_attr(out, "CNAM", buf);
    }
    pjl_opt(out, "OUTBIN", "FACEDOWN");
    pjl_attr(out, "PODR", "NORMAL");
    pjl_opt(out, "SLIPSHEET", "OFF");
    pjl_opt(out, "PAPERDIRECTION", s->length <= s->width ? "LEF" : "SEF");
    pjl_opt(out, "RESOLUTION", "600");
    snprintf(buf, sizeof(buf), "%d", bits_per_pixel == 1 ? 1 : 8);
    pjl_opt(out, "BITSPERPIXEL", buf);
    pjl_attr(out, "DRDM", "RASTER");
    if (color) {
        pjl_attr3(out, "TCPR", "GCPR", "ICPR", s->dlut);
        pjl_attr3(out, "TUCR", "GUCR", "IUCR", s->ucr);
        i = s->screen2_mode ? s->screen2 : s->trc;
        pjl_attr3(out, "TTRC", "GTRC", "ITRC", i);
        i = s->screen2_mode ? s->screen2 : s->screen;
        pjl_attr3(out, "TSCR", "GSCR", "ISCR", i);
    } else {
        i = s->screen2_mode ? s->monoscreen2 : s->monotrc;
        pjl_attr3(out, "TTRC", "GTRC", "ITRC", i);
        i = s->screen2_mode ? s->monoscreen2 : s->monoscreen;
        pjl_attr3(out, "TSCR", "GSCR", "ISCR", i);
        pjl_attr(out, "PBLK", "OFF");
    }
    pjl_attr(out, "BANR", "DEVICE");
    if (s->doctype == XR_DOC_OFF && color)
        pjl_attr(out, "FCMS", "ON");
    pjl_opt(out, "IWAMANUALDUP", "OFF");
    pjl_opt(out, "IWAJAMRECOVERY", "AUTO");
    fputs("@PJL ENTER LANGUAGE=HBPL\n", out);

    /* BeginJob */
    put_u8(out, 'A');
    attr_num(out, 0x81, 1, 0);
    attr_num(out, 0x82, 2, 7); /* HBPL type = compression (alla5 + LPH) */
    attr_num(out, 0x83, 2, 1);
}

/* ---- pages (BeginPage / DrawChunkImage) --------------------------------- */

void hbpl_page(FILE *out, hbpl_job *j, const xr_settings *s, const int32_t *fx,
               const uint8_t *data)
{
    uint32_t W = fx[FX_WIDTH], H = fx[FX_HEIGHT], size = fx[FX_SIZE];
    float xres = (float)fx[FX_XRES], yres = (float)fx[FX_YRES];
    uint32_t pw, pl, iw, il, mx, my;
    int comp = fx[FX_COMPRESSION], cs = fx[FX_CSPACE];

    put_u8(out, 'C');
    attr_num(out, 0x91, 1, s->length <= s->width);
    attr_num(out, 0x92, 1, 1); /* input slot: automatic */
    attr_num(out, 0x93, 1, 1);
    attr_num(out, 0x94, 1, s->page_code);
    if (s->page_code == 0xff)
        attr_xy(out, 0x95, 2, fxround((double)(xres * s->width) / 72.0) & 0xffff,
                fxround((double)(xres * s->length) / 72.0) & 0xffff);
    else
        attr_xy(out, 0x95, 2, 0, 0);
    attr_num(out, 0x96, 1, 3);

    /* margins: zero when the raster covers the whole sheet */
    pw = fxround((double)(xres * s->width) / 72.0);
    pl = fxround((double)(yres * s->length) / 72.0);
    iw = fxround((double)(xres * (s->right - s->left)) / 72.0);
    il = fxround((double)((s->top - s->bottom) * yres) / 72.0);
    if (iw < W && (pw <= W || pw - W < W - iw))
        mx = 0;
    else
        mx = fxround((double)(xres * s->left) / 72.0);
    if (il < H && (pl <= H || pl - H < H - il))
        my = 0;
    else
        my = fxround((double)(yres * (s->length - s->top)) / 72.0);
    attr_xy(out, 0x97, 3, (int16_t)mx, (int16_t)my);

    attr_num(out, 0x98, 1, 0); /* simplex */
    attr_num(out, 0x99, 4, j->page_index++);
    attr_xy(out, 0x9a, 4, W, H);
    attr_num(out, 0x9b, 1, fx[FX_PAGES] == 0);
    attr_num(out, 0x9c, 1, comp == 6 || comp == 7 ? 1 : comp == 0 ? 0 : 1);
    attr_num(out, 0x9d, 1, cs == 0 || cs == 4 ? 9 : 0x8b);
    attr_num(out, 0x9e, 1, 2);
    attr_num(out, 0x9f, 1, 5);
    attr_num(out, 0xa0, 1, fx[FX_BPP] == 1 ? 1 : 8);
    attr_num(out, 0xa1, 1, fx[FX_XRES] == 300 ? 3 : 0);
    attr_xy(out, 0xa2, 4, (W + 7) & ~7u, H);

    put_u8(out, 'Q');
    /* DrawChunkImage for alla5 data */
    put_u8(out, 'R');
    attr_num(out, 0xa3, 1, 0);
    put_u8(out, 0xa4);
    put_u8(out, 0xb1);
    put_u8(out, 0xa4);
    put_le(out, size + 0x20, 4);
    put_le(out, 0x20, 4);
    put_le(out, cs == 0 || cs == 4 ? 0x108 : 0x10100, 4);
    put_le(out, 0x43210, 4);
    put_le(out, 0x42a1, 4);
    put_le(out, 0xff0000, 4);
    put_le(out, 0, 4);
    put_le(out, 0, 4);
    put_le(out, 0, 4);
    fwrite(data, 1, size, out);
    put_u8(out, 'S');
    put_u8(out, 'D');
    j->pages++;
}

/* ---- job trailer -------------------------------------------------------- */

static void jobinfo(FILE *f, const char *k, const char *v)
{
    fprintf(f, "@PJL COMMENT FXJOBINFO %s=%s\n", k, v);
}

void hbpl_job_end(FILE *out, hbpl_job *j, const xr_settings *s, int pages)
{
    char buf[256];
    float a = s->width, b = s->length, lo = a < b ? a : b, hi = a < b ? b : a;
    int i;

    put_u8(out, 'B');
    uel(out);
    jobinfo(out, "VERSION", "1.0.0");
    fputs("@PJL COMMENT FXJOBINFO BEGIN\n", out);
    jobinfo(out, "PDLTYPE", "PDL[HBPL]:VERSION[1.0.0]");
    jobinfo(out, "JOBTYPE", "PRINT");
    jobinfo(out, "JOBCOPIES", "1");
    snprintf(buf, sizeof(buf), "%d", s->collate ? 1 : s->copies);
    jobinfo(out, "PAGECOPIES", buf);
    jobinfo(out, "DUPLEXTYPE", "SIMPLEX");
    if (s->have_options) {
        snprintf(buf, sizeof(buf), "%d", s->number_up);
        jobinfo(out, "NUP", buf);
        jobinfo(out, "PAGEORDER", s->output_reverse ? "NTO1" : "1TON");
    }
    jobinfo(out, "TONERSAVE", s->toner_save ? "ON" : "OFF");
    snprintf(buf, sizeof(buf), "%d", s->collate ? pages * s->copies : pages);
    jobinfo(out, "PHYSICALPAGES", buf);
    fputs("@PJL COMMENT FXJOBINFO PAGEINFOBEGIN\n", out);
    for (i = 0; i < j->pages; i++) {
        snprintf(buf, sizeof(buf), "%s[%s%.1fX%.1f]:%sINTRAY[5]",
                 "SIZE", s->page_code == 0xff ? "*" : "",
                 (double)((lo * 25.4) / 72.0), (double)((hi * 25.4) / 72.0),
                 s->color_mode == XR_COLORMODE_COLOR ? "MODE[COLOR]:COLORS[0]:" : "MODE[MONOCHROME]:");
        jobinfo(out, "PAGEINFO", buf);
    }
    fputs("@PJL COMMENT FXJOBINFO PAGEINFOEND\n", out);
    fputs("@PJL COMMENT FXJOBINFO END\n", out);
    fputs("@PJL EOJ\n", out);
}
