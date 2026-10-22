/*
 * xrsettings.c - PPD/option handling (GetPrintSetting, GetColorSetting,
 * GetScreen2Setting, ... of the original filters).
 */
/* macOS SDK only; newer Linux CUPS headers turn this into "unavailable" */
#ifdef __APPLE__
#define _CUPS_NO_DEPRECATED 0
#endif
#include "xrsettings.h"

#include <cups/cups.h>
#include <cups/ppd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif

static const char *density_opts[12] = {
    "XRColorDensityCyanLow",     "XRColorDensityCyanMid",     "XRColorDensityCyanHigh",
    "XRColorDensityMagentaLow",  "XRColorDensityMagentaMid",  "XRColorDensityMagentaHigh",
    "XRColorDensityYellowLow",   "XRColorDensityYellowMid",   "XRColorDensityYellowHigh",
    "XRColorDensityBlackLow",    "XRColorDensityBlackMid",    "XRColorDensityBlackHigh",
};

static const char *marked(ppd_file_t *ppd, const char *opt)
{
    ppd_choice_t *c = ppdFindMarkedChoice(ppd, opt);
    return c ? c->choice : NULL;
}

static int is_true(ppd_file_t *ppd, const char *opt)
{
    const char *v = marked(ppd, opt);
    return v && !strcmp(v, "True");
}

/* FXParseMarkedCode: "<</NAME value/NAME value>>setpagedevice" */
static int code_value(ppd_file_t *ppd, const char *opt, const char *name)
{
    ppd_choice_t *c = ppdFindMarkedChoice(ppd, opt);
    const char *p;
    size_t n = strlen(name);

    if (!c || !c->code)
        return 0;
    for (p = strchr(c->code, '/'); p; p = strchr(p + 1, '/'))
        if (!strncmp(p + 1, name, n) && p[1 + n] == ' ')
            return atoi(p + 1 + n);
    return 0;
}

/* GetPageSizeSetting */
static int page_code(const char *n)
{
    static const struct { const char *name; int code; } t[] = {
        {"A4", 2}, {"A4Rotated", 2}, {"A3", 5}, {"A5", 0xf}, {"B4", 10}, {"B5", 0xb},
        {"B5Rotated", 0xb}, {"Letter", 0}, {"LetterRotated", 0}, {"FanFoldGermanLegal", 0xcd},
        {"Legal", 1}, {"Executive", 3}, {"Statement", 0xcc}, {"Tabloid", 4}, {"Env10", 6},
        {"EnvMonarch", 7}, {"XREnvMonarchRotated", 7}, {"EnvC5", 8}, {"EnvDL", 9},
        {"XREnvDLRotated", 9}, {"XRPostcard", 0xd}, {"XRPostcardRotated", 0xd},
        {"XREnvYou2Rotated", 0xe0}, {"XREnvYou3Rotated", 0xe1}, {"XREnvYou2", 0xe0},
        {"XREnvYou3", 0xe1}, {"XREnvYou4", 0xe2}, {"XREnvYou6", 0xe7}, {"XREnvYouChou3", 0xe3},
        {"XRReplyPostcard", 0xe}, {"EnvChou3", 0xe4}, {"EnvChou4", 0xe5}, {"EnvKaku3", 0xe8},
        {"Custom", 0xff}, {"EnvC6", 0xe0}, {"EnvC4", 0xe6}, {"XR-MEX-OFFICIO", 0xce},
        {"XR16-KAI-GCO", 0xd3}, {"XR16-KAI-GCORotated", 0xd3}, {"XR8-KAI-GCO", 0xd4},
    };
    size_t i;
    for (i = 0; i < sizeof(t) / sizeof(t[0]); i++)
        if (!strcmp(n, t[i].name))
            return t[i].code;
    return 2;
}

int xr_settings_load(xr_settings *s, const char *ppd_path, int copies,
                     const char *options, const char **err)
{
    ppd_file_t *ppd;
    ppd_attr_t *attr;
    ppd_size_t *size;
    cups_option_t *opts = NULL;
    int nopts = 0, i;
    const char *v;

    memset(s, 0, sizeof(*s));
    if (!ppd_path)
        ppd_path = getenv("PPD");
    if (!ppd_path || !(ppd = ppdOpenFile(ppd_path))) {
        *err = "cannot open PPD file";
        return -1;
    }
    ppdMarkDefaults(ppd);
    if (options)
        nopts = cupsParseOptions(options, 0, &opts);
    if (nopts)
        cupsMarkOptions(ppd, nopts, opts);

    s->have_options = nopts > 0;
    s->number_up = 1;
    if ((v = cupsGetOption("number-up", nopts, opts)))
        s->number_up = atoi(v);
    if ((v = cupsGetOption("OutputOrder", nopts, opts)))
        s->output_reverse = !strcmp(v, "Reverse");

    s->copies = copies < 1 ? 1 : copies > 999 ? 999 : copies;
    v = marked(ppd, "Collate");
    s->collate = v ? !strcmp(v, "True") : 1;

    v = marked(ppd, "XRColorMode");
    s->color_mode = (v && !strcmp(v, "Color")) ? XR_COLORMODE_COLOR : XR_COLORMODE_MONO;

    v = marked(ppd, "MediaType");
    snprintf(s->media, sizeof(s->media), "%s", v ? v : "");

    v = marked(ppd, "XRDocumentType");
    s->doctype = xr_doctype_from_name(v);
    s->dlut = code_value(ppd, "XRDocumentType", "XRDLUT");
    s->ucr = code_value(ppd, "XRDocumentType", "XRUCR");
    s->trc = code_value(ppd, "XRDocumentType", "XRTRC");
    s->screen = code_value(ppd, "XRDocumentType", "XRSCREEN");
    s->monotrc = code_value(ppd, "XRDocumentType", "XRMONOTRC");
    s->monoscreen = code_value(ppd, "XRDocumentType", "XRMONOSCREEN");

    v = marked(ppd, "XRScreen2");
    s->screen2_mode = !v ? 0 : !strcmp(v, "Fineness") ? 1 : !strcmp(v, "Standard") ? 2
                    : !strcmp(v, "Gradation") ? 3 : 0;
    s->screen2 = code_value(ppd, "XRScreen2", "XRSCREEN2");
    s->monoscreen2 = code_value(ppd, "XRScreen2", "XRMONOSCREEN2");

    s->turn_page = is_true(ppd, "XRTurnPage");
    s->skip_blank = is_true(ppd, "XRSkip");
    s->toner_save = is_true(ppd, "XRTonerSaver");

    for (i = 0; i < 12; i++) {
        v = marked(ppd, density_opts[i]);
        s->density[i] = v ? (int)strtol(v, NULL, 10) : -1000;
    }

    v = marked(ppd, "PageSize");
    if (!v)
        v = marked(ppd, "PageRegion");
    if (!v || !(size = ppdPageSize(ppd, v))) {
        *err = "no PageSize selected";
        goto fail;
    }
    s->page_code = page_code(v);
    s->width = size->width;
    s->length = size->length;
    s->left = size->left;
    s->bottom = size->bottom;
    s->right = size->right;
    s->top = size->top;

    if ((attr = ppdFindAttr(ppd, "XRDlutFilePath", NULL)) && attr->value)
        snprintf(s->dlut_path, sizeof(s->dlut_path), "%s", attr->value);

    cupsFreeOptions(nopts, opts);
    ppdClose(ppd);
    return 0;
fail:
    cupsFreeOptions(nopts, opts);
    ppdClose(ppd);
    return -1;
}
