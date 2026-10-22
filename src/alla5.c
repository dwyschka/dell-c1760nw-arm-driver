/*
 * alla5.c - reimplementation of the "alla5" (ALLA, penta stream) encoder.
 *
 * Model (alla::LrleEnc / alla::Predictor):
 *   Each line is coded as a sequence of symbols.  Five predictors are kept in
 *   a move-to-front list; a run symbol says "the next N pixels equal what
 *   predictor k predicts", an error symbol codes one pixel as the byte-wise
 *   difference to its left neighbour.  A run that reaches the end of a line
 *   stays open and continues into the next line.
 *
 * Entropy coding (alla::HuffmanEnc): static prefix codes built from the
 * cw_* group tables; stream 0 receives order/run/marker codes, streams 1..4
 * receive one error code per colour component.
 */
#include "alla5.h"

#include <stdlib.h>
#include <string.h>

/* ---- code tables (alla::cw_*) ------------------------------------------ */

typedef struct {
    int min, max, base, prefix_len, sign, extra, prefix;
} a5_group;

static const a5_group cw_marker_ver1[] = {
    {0, 0, 0, 1, 0, 0, 0}, {1, 1, 1, 3, 0, 0, 6},
    {2, 2, 2, 3, 0, 0, 7}, {3, 7, 3, 2, 0, 0, 2},
};
static const a5_group cw_order_graphics[] = {
    {5, 5, 5, 2, 0, 0, 2},  {0, 0, 0, 1, 0, 0, 0},  {1, 1, 1, 3, 0, 0, 6},
    {2, 2, 2, 5, 0, 0, 28}, {3, 3, 3, 5, 0, 0, 29}, {4, 4, 4, 5, 0, 0, 30},
    {6, 6, 6, 5, 0, 0, 31},
};
static const a5_group cw_order_image[] = {
    {5, 5, 5, 1, 0, 0, 0},  {0, 0, 0, 2, 0, 0, 2},  {1, 1, 1, 3, 0, 0, 6},
    {2, 2, 2, 5, 0, 0, 28}, {3, 3, 3, 5, 0, 0, 29}, {4, 4, 4, 5, 0, 0, 30},
    {6, 6, 6, 5, 0, 0, 31},
};
static const a5_group cw_run_graphics[] = {
    {1, 1, 1, 2, 0, 0, 0},          {2, 2, 2, 3, 0, 0, 2},
    {3, 3, 3, 3, 0, 0, 3},          {4, 5, 4, 3, 0, 1, 4},
    {6, 9, 6, 3, 0, 2, 5},          {10, 25, 10, 3, 0, 4, 6},
    {26, 33, 26, 6, 0, 3, 56},      {34, 41, 34, 6, 0, 3, 57},
    {42, 49, 42, 6, 0, 3, 58},      {50, 65, 50, 6, 0, 4, 59},
    {66, 97, 66, 6, 0, 5, 60},      {98, 161, 98, 6, 0, 6, 61},
    {162, 673, 162, 6, 0, 9, 62},   {674, 17057, 674, 6, 0, 14, 63},
};
static const a5_group cw_error_graphics[] = {
    {0, 0, 0, 3, 0, 0, 0},       {1, 1, 1, 3, 0, 0, 1},
    {-1, -1, 1, 3, 0, 0, 2},     {-3, 3, 2, 3, 1, 1, 3},
    {-7, 7, 4, 3, 1, 2, 4},      {-15, 15, 8, 3, 1, 3, 5},
    {-47, 47, 16, 3, 1, 5, 6},   {-79, 79, 48, 4, 1, 5, 14},
    {-128, 128, 80, 4, 1, 6, 15},
};

#define NGROUPS(t) ((int)(sizeof(t) / sizeof((t)[0])))

/* alla::HuffmanEnc::write_code / set_direct_code_internal */
static void code_for(const a5_group *t, int n, int v, uint32_t *code, int *len)
{
    int i, av = v < 0 ? -v : v;
    for (i = 0; i < n; i++)
        if (v >= t[i].min && v <= t[i].max)
            break;
    if (i == n) { /* cannot happen with the value ranges used here */
        *code = 0;
        *len = 0;
        return;
    }
    const a5_group *g = &t[i];
    int ab = g->base < 0 ? -g->base : g->base;
    if (!g->sign) {
        *code = ((uint32_t)g->prefix << g->extra) + av - ab;
        *len = g->prefix_len + g->extra;
    } else {
        *code = ((uint32_t)g->prefix << (g->extra + 1)) +
                ((uint32_t)(v < 0) << g->extra) + av - ab;
        *len = g->prefix_len + 1 + g->extra;
    }
}

/* ---- bit output (alla::OutCodeBuffer) --------------------------------- */

static void put_byte(a5_stream *s, uint8_t b)
{
    if (s->len == s->cap) {
        s->cap = s->cap ? s->cap * 2 : 65536;
        s->data = realloc(s->data, s->cap);
        if (!s->data)
            abort();
    }
    s->data[s->len++] = b;
}

static void put_bits(a5_stream *s, int n, uint32_t code)
{
    s->acc = (s->acc << n) | (code & ((n < 32) ? ((1u << n) - 1) : 0xffffffffu));
    s->nacc += n;
    while (s->nacc >= 8) {
        s->nacc -= 8;
        put_byte(s, (uint8_t)(s->acc >> s->nacc));
    }
}

/* fill_rest_bit + flush_buffer: pad with one bits to a byte boundary */
static void flush_bits(a5_stream *s)
{
    if (s->nacc)
        put_bits(s, 8 - s->nacc, 0xff);
}

/* ---- encoder state ----------------------------------------------------- */

#define A5_LIMIT 17057 /* maximum run length (Info::normalize) */
#define A5_MARGIN 8    /* pixels of padding kept around each line */

enum { PT_LINE = 1, PT_CONST = 2 };

typedef struct {
    int name; /* alla::prediction value */
    int type; /* Referer::Type */
} a5_pred;

struct a5_enc {
    int color, lph, in_width;
    int width;          /* coding width in pixels (multiple of 8) */
    uint32_t outfield;  /* value of every pixel outside the line */
    uint32_t *bufa, *bufb;
    uint32_t *cur, *prev;
    a5_pred preds[5];
    int slot[5];        /* move-to-front list of predictor indices */
    int shift;          /* order shift: 0 or -1 */
    int sym_kind, sym_len; /* last / pending symbol */
    uint32_t sym_err;
    const a5_group *order_tab;
    int dht_count;
    int nstreams;
    a5_stream st[5];
};

a5_enc *a5_create(int width, int color, int lph)
{
    static const int order_color[5] = {0x01, 0x81, 0x80, 0x82, 0x83};
    static const int order_mono[5] = {0x01, 0x81, 0x80, 0x82, 0x08};
    a5_enc *e = calloc(1, sizeof(*e));
    int i;

    if (!e)
        return NULL;
    e->color = color;
    e->lph = lph;
    e->in_width = width;
    e->width = ((lph ? 8 : 0) + 7 + width) & ~7;
    e->outfield = color ? 0xffffffffu : 0;
    e->bufa = malloc(sizeof(uint32_t) * (e->width + 2 * A5_MARGIN));
    e->bufb = malloc(sizeof(uint32_t) * (e->width + 2 * A5_MARGIN));
    if (!e->bufa || !e->bufb) {
        a5_destroy(e);
        return NULL;
    }
    for (i = 0; i < e->width + 2 * A5_MARGIN; i++)
        e->bufa[i] = e->bufb[i] = e->outfield;
    e->cur = e->bufa + A5_MARGIN;
    e->prev = e->bufb + A5_MARGIN;

    /* Lrle::apply_initial_order */
    for (i = 0; i < 5; i++) {
        e->preds[i].name = color ? order_color[i] : order_mono[i];
        e->preds[i].type = e->preds[i].name == 1 ? PT_CONST : PT_LINE;
        e->slot[i] = i;
    }
    e->shift = 0;
    e->sym_kind = 6; /* LrleEnc::init_last_symbol */
    e->sym_len = 2;
    e->order_tab = cw_order_graphics;
    e->dht_count = 0;
    e->nstreams = color ? 5 : 2;
    return e;
}

void a5_destroy(a5_enc *e)
{
    int i;
    if (!e)
        return;
    for (i = 0; i < 5; i++)
        free(e->st[i].data);
    free(e->bufa);
    free(e->bufb);
    free(e);
}

const a5_stream *a5_stream_get(const a5_enc *e, int i) { return &e->st[i]; }
int a5_nstreams(const a5_enc *e) { return e->nstreams; }

/* Pointer to the reference line of a PT_LINE predictor. */
static const uint32_t *pred_ref(const a5_enc *e, const a5_pred *p)
{
    if (p->name >= 0x80)
        return e->prev + (p->name - 0x81);
    return e->cur - p->name;
}

/* alla::Predictor::look_run */
static int look_run(const a5_enc *e, int pi, int pos)
{
    const a5_pred *p = &e->preds[pi];
    const uint32_t *cur = e->cur;
    int i = pos;

    if (p->type == PT_CONST) {
        uint32_t v = cur[pos - p->name];
        while (i < e->width && cur[i] == v)
            i++;
    } else {
        const uint32_t *ref = pred_ref(e, p);
        while (i < e->width && cur[i] == ref[i])
            i++;
    }
    return i - pos;
}

static void mtf(a5_enc *e, int idx)
{
    int v = e->slot[idx];
    for (; idx > 0; idx--)
        e->slot[idx] = e->slot[idx - 1];
    e->slot[0] = v;
}

/* alla::Predictor::look_error with the left-neighbour referer */
static uint32_t look_error(const a5_enc *e, int pos)
{
    uint32_t c = e->cur[pos], l = e->cur[pos - 1], r = 0;
    int k;
    for (k = 0; k < 32; k += 8)
        r |= (uint32_t)(uint8_t)((c >> k) - (l >> k)) << k;
    return r;
}

/* alla::LrleEnc::look_longest_run<T> */
static void look_longest_run(a5_enc *e, int pos)
{
    int best, idx, r, k;

    r = look_run(e, e->slot[0], pos);
    if (r >= 1) {
        if (r >= A5_LIMIT) {
            e->sym_kind = e->shift;
            e->sym_len = A5_LIMIT;
            return;
        }
        idx = 0;
        best = r;
    } else {
        idx = 5;
        best = 0;
    }
    for (k = 1; k < 5; k++) {
        r = look_run(e, e->slot[k], pos);
        if (r > best) {
            idx = k;
            best = r;
            if (r >= A5_LIMIT) {
                mtf(e, k);
                e->sym_kind = e->shift + k;
                e->sym_len = A5_LIMIT;
                return;
            }
        }
    }
    if (idx == 5) {
        e->sym_kind = 5;
        e->sym_err = look_error(e, pos);
        e->shift = 0;
        return;
    }
    mtf(e, idx);
    e->sym_kind = idx + e->shift;
    e->sym_len = best;
}

/* Lrle::examine_order_shift(0) */
static void examine_order_shift(a5_enc *e)
{
    e->shift = (e->sym_len == A5_LIMIT || e->preds[e->slot[0]].name == 0xf0) ? 0 : -1;
}

/* ---- Huffman layer ----------------------------------------------------- */

static void put_order(a5_enc *e, int kind)
{
    uint32_t c;
    int n;
    code_for(e->order_tab, NGROUPS(cw_order_graphics), kind, &c, &n);
    put_bits(&e->st[0], n, c);
}

static void encode_run(a5_enc *e, int kind, int len)
{
    uint32_t c;
    int n;
    e->dht_count += (kind == 0);
    put_order(e, kind);
    code_for(cw_run_graphics, NGROUPS(cw_run_graphics), len, &c, &n);
    put_bits(&e->st[0], n, c);
}

static void encode_order(a5_enc *e, int kind)
{
    if (kind == 5)
        e->dht_count--;
    put_order(e, kind);
}

static void encode_error(a5_enc *e, uint32_t err)
{
    int k, ncomp = e->color ? 4 : 1;
    for (k = 0; k < ncomp; k++) {
        uint32_t c;
        int n;
        code_for(cw_error_graphics, NGROUPS(cw_error_graphics),
                 (int8_t)(err >> (8 * k)), &c, &n);
        put_bits(&e->st[1 + k], n, c);
    }
}

static void encode_marker(a5_enc *e, int m)
{
    uint32_t c;
    int n;
    put_order(e, 6);
    code_for(cw_marker_ver1, NGROUPS(cw_marker_ver1), m, &c, &n);
    put_bits(&e->st[0], n, c);
}

/* alla::HuffmanEnc::examine_dht: adaptive switch of the order table */
static void examine_dht(a5_enc *e)
{
    const a5_group *nt = NULL;
    if (e->dht_count > 8 && e->order_tab == cw_order_image)
        nt = cw_order_graphics;
    else if (-e->dht_count > 8 && e->order_tab == cw_order_graphics)
        nt = cw_order_image;
    if (nt) {
        encode_marker(e, 0);
        e->order_tab = nt;
    }
    e->dht_count = 0;
}

/* ---- line coding (alla::CoreEnc::encode_line_internal<T>) -------------- */

static void load_line(a5_enc *e, const uint8_t *line)
{
    uint32_t *t;
    int i, x = 0;

    /* InImageBuffer::swap */
    t = e->prev;
    e->prev = e->cur;
    e->cur = t;

    if (e->lph) {
        e->cur[x++] = e->color ? 0xffffffffu : 0xff;
        for (i = 0; i < 7; i++)
            e->cur[x++] = 0;
    }
    if (e->color) {
        for (i = 0; i < e->in_width; i++, line += 4)
            e->cur[x++] = (uint32_t)line[0] | (uint32_t)line[1] << 8 |
                          (uint32_t)line[2] << 16 | (uint32_t)line[3] << 24;
    } else {
        for (i = 0; i < e->in_width; i++)
            e->cur[x++] = line[i];
    }
    while (x < e->width)
        e->cur[x++] = 0;
    /* pixels beyond the line read as the outfield (remove_sentinel) */
    e->cur[e->width] = e->outfield;
}

void a5_encode_line(a5_enc *e, const uint8_t *line)
{
    int W = e->width, pos = 0;

    load_line(e, line);

    if (e->sym_kind <= 4) {
        /* the run left open at the end of the previous line continues */
        int old = e->sym_len;
        int nl = look_run(e, e->slot[0], 0) + old;
        if (nl > A5_LIMIT)
            nl = A5_LIMIT;
        e->sym_len = nl;
        if (W + old == nl)
            goto done;
        examine_order_shift(e);
        encode_run(e, e->sym_kind, nl);
        pos = nl - old;
    }
    for (;;) {
        look_longest_run(e, pos);
        if (e->sym_kind > 4) {
            pos += (e->sym_kind == 5);
            encode_order(e, e->sym_kind);
            encode_error(e, e->sym_err);
            if (pos == W)
                break;
            continue;
        }
        pos += e->sym_len;
        if (pos == W)
            break; /* run stays open */
        encode_run(e, e->sym_kind, e->sym_len);
        examine_order_shift(e);
    }
done:
    examine_dht(e);
}

void a5_finish(a5_enc *e)
{
    int i, any = 0;
    for (i = 0; i < e->nstreams; i++)
        any |= e->st[i].len || e->st[i].nacc;
    if (!any)
        return;
    /* CoreEnc::encode_marker_terminating_run(EOI) */
    if (e->sym_kind <= 4) {
        encode_run(e, e->sym_kind, e->sym_len);
        e->shift = 0;
    }
    e->sym_kind = 6;
    e->sym_len = 2;
    encode_marker(e, 2);
    for (i = 0; i < e->nstreams; i++)
        flush_bits(&e->st[i]);
}
