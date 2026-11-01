/* GFOpenFont's CFF reader: the outlines of an OpenType font with a 'CFF '
 * table, plain or CID-keyed, through a Type 2 charstring interpreter.
 *
 * Copyright (c) 2026 AmigaChrome Team. MIT licence (see LICENSE).
 *
 * From Adobe's published specifications: "The Compact Font Format
 * Specification" (Technical Note #5176) and "The Type 2 Charstring Format"
 * (Technical Note #5177). Hints are read past and not used: GFOpenFont draws
 * without hinting. Numbers are 16.16 fixed point throughout. */
#include "gfopenfont/gfopenfont.h"
#include "of_font.h"

#include <stdlib.h>
#include <string.h>

typedef struct Index {
    const uint8_t *data;        /* the INDEX's start */
    uint32_t count;
    uint8_t off_size;
    const uint8_t *offsets;     /* count + 1 of them */
    const uint8_t *objects;     /* offsets count from objects - 1 */
    uint32_t total;             /* bytes the INDEX takes */
} Index;

typedef struct FontDict {
    Index subrs;                /* local subroutines */
    int32_t subr_bias;
    int64_t mat_num, mat_den;   /* this FD's FontMatrix's first entry (CID fonts; 1 when none) */
} FontDict;

struct OFCff {
    const uint8_t *data;
    uint32_t size;
    Index charstrings;
    Index gsubrs;
    int32_t gsubr_bias;
    int cid;
    uint32_t nfds;
    FontDict *fds;
    const uint8_t *fdselect;
    uint32_t fdselect_len;
    uint32_t nglyphs;
    int64_t mat_num, mat_den;   /* the Top DICT's FontMatrix's first entry (0.001 when none) */
    uint32_t units_per_em;      /* the font's (head), which the outlines are given in */
};

static int read_index(const uint8_t *base, uint32_t size, uint32_t at, Index *ix)
{
    uint32_t count, off_size, last, i;
    memset(ix, 0, sizeof(*ix));
    if (at > size || size - at < 2)
        return 0;
    count = of_u16(base + at);
    ix->data = base + at;
    ix->count = count;
    if (!count) {
        ix->total = 2;
        return 1;
    }
    if (size - at < 3)
        return 0;
    off_size = base[at + 2];
    if (off_size < 1 || off_size > 4)
        return 0;
    if ((uint64_t)(count + 1) * off_size > size - at - 3)
        return 0;
    ix->off_size = (uint8_t)off_size;
    ix->offsets = base + at + 3;
    ix->objects = ix->offsets + (count + 1) * off_size - 1;
    last = 0;
    for (i = 0; i < off_size; ++i)
        last = (last << 8) | ix->offsets[count * off_size + i];
    if (last < 1 || (uint64_t)(ix->objects - base) + last > size)
        return 0;
    ix->total = (uint32_t)(ix->objects - (base + at)) + last;
    return 1;
}

static uint32_t index_offset(const Index *ix, uint32_t i)
{
    uint32_t v = 0, k;
    const uint8_t *p = ix->offsets + i * ix->off_size;
    for (k = 0; k < ix->off_size; ++k)
        v = (v << 8) | p[k];
    return v;
}

/* object i: its bytes and length; 0 when out of range or inconsistent */
static int index_get(const Index *ix, uint32_t i, const uint8_t **p, uint32_t *len)
{
    uint32_t a, b;
    if (i >= ix->count)
        return 0;
    a = index_offset(ix, i);
    b = index_offset(ix, i + 1);
    if (a < 1 || b < a || b - 1 > ix->total)
        return 0;
    *p = ix->objects + a;
    *len = b - a;
    return 1;
}

static int32_t bias_of(uint32_t count)
{
    return count < 1240 ? 107 : (count < 33900 ? 1131 : 32768);
}

/* ---- DICTs ---- */

#define MAX_OPERANDS 48

typedef struct Operand {
    int64_t num, den;           /* a rational: integers have den 1 */
} Operand;

typedef struct Dict {
    const uint8_t *p, *end;
} Dict;

/* A real number's nibbles (CFF 5176, table 5) as a rational. */
static const uint8_t *read_real(const uint8_t *p, const uint8_t *end, Operand *o)
{
    int64_t mant = 0, den = 1;
    int frac = 0, neg = 0, eneg = 0, in_exp = 0, digits = 0;
    int32_t exp = 0;
    int done = 0;
    while (p < end && !done) {
        uint8_t b = *p++;
        int k;
        for (k = 0; k < 2 && !done; ++k) {
            uint8_t n = k ? (b & 0x0F) : (b >> 4);
            if (n <= 9) {
                if (in_exp)
                    exp = exp < 1000 ? exp * 10 + n : exp;
                else if (digits < 15) {
                    mant = mant * 10 + n;
                    if (frac)
                        den *= 10;
                    if (mant)
                        digits++;
                }
            } else if (n == 0xA) {
                frac = 1;
            } else if (n == 0xB) {
                in_exp = 1;
            } else if (n == 0xC) {
                in_exp = 1;
                eneg = 1;
            } else if (n == 0xE) {
                neg = 1;
            } else if (n == 0xF) {
                done = 1;
            }
        }
    }
    while (exp-- > 0) {
        if (eneg) {
            if (den < (int64_t)1 << 50)
                den *= 10;
        } else if (mant < (int64_t)1 << 50) {
            mant *= 10;
        }
    }
    o->num = neg ? -mant : mant;
    o->den = den;
    return p;
}

/* The next operator, with its operands in ops[0 .. *n); -1 at the end. Two-
 * byte operators are 1200 + the second byte. */
static int dict_next(Dict *d, Operand *ops, int *n)
{
    *n = 0;
    while (d->p < d->end) {
        uint8_t b0 = *d->p;
        if (b0 <= 21) {
            d->p++;
            if (b0 == 12) {
                if (d->p >= d->end)
                    return -1;
                return 1200 + *d->p++;
            }
            return b0;
        }
        if (*n >= MAX_OPERANDS)
            return -1;
        if (b0 == 30) {
            d->p = read_real(d->p + 1, d->end, &ops[*n]);
            (*n)++;
            continue;
        }
        ops[*n].den = 1;
        if (b0 >= 32 && b0 <= 246) {
            ops[*n].num = (int32_t)b0 - 139;
            d->p += 1;
        } else if (b0 >= 247 && b0 <= 250) {
            if (d->end - d->p < 2)
                return -1;
            ops[*n].num = ((int32_t)b0 - 247) * 256 + d->p[1] + 108;
            d->p += 2;
        } else if (b0 >= 251 && b0 <= 254) {
            if (d->end - d->p < 2)
                return -1;
            ops[*n].num = -((int32_t)b0 - 251) * 256 - d->p[1] - 108;
            d->p += 2;
        } else if (b0 == 28) {
            if (d->end - d->p < 3)
                return -1;
            ops[*n].num = of_s16(d->p + 1);
            d->p += 3;
        } else if (b0 == 29) {
            if (d->end - d->p < 5)
                return -1;
            ops[*n].num = (int32_t)of_u32(d->p + 1);
            d->p += 5;
        } else {
            return -1;              /* reserved */
        }
        (*n)++;
    }
    return -1;
}

static int32_t op_int(const Operand *o)
{
    return (int32_t)(o->den == 1 ? o->num : o->num / o->den);
}

/* Reads a Private DICT for its local subroutines. */
static int read_private(const OFCff *c, uint32_t size, uint32_t off, FontDict *fd)
{
    Dict d;
    Operand ops[MAX_OPERANDS];
    int n, op;
    uint32_t subrs = 0;
    memset(&fd->subrs, 0, sizeof fd->subrs);
    fd->subr_bias = 0;
    if (off > c->size || size > c->size - off)
        return 0;
    d.p = c->data + off;
    d.end = d.p + size;
    while ((op = dict_next(&d, ops, &n)) >= 0) {
        if (op == 19 && n >= 1)
            subrs = (uint32_t)op_int(&ops[0]);
    }
    if (subrs) {
        if (!read_index(c->data, c->size, off + subrs, &fd->subrs))
            return 0;
        fd->subr_bias = bias_of(fd->subrs.count);
    }
    return 1;
}

/* A FontMatrix's first entry, kept as a rational (a zero one is ignored). */
static void matrix_entry(const Operand *a, int64_t *num, int64_t *den)
{
    if (a->num == 0 || a->den == 0)
        return;
    *num = a->num;
    *den = a->den;
}

int of_cff_open(OFCff **out, const uint8_t *data, uint32_t size, uint32_t units_per_em)
{
    OFCff *c;
    Index names, tops, strings;
    const uint8_t *top;
    uint32_t top_len, at, charstrings = 0, priv_off = 0, priv_size = 0, fdarray = 0, fdselect = 0;
    Dict d;
    Operand ops[MAX_OPERANDS];
    int n, op;

    *out = NULL;
    if (size < 4 || data[0] != 1)
        return OF_ERR_FONT;         /* CFF version 1 only: CFF2 is variable fonts */
    c = (OFCff *)calloc(1, sizeof(*c));
    if (!c)
        return OF_ERR_MEMORY;
    c->data = data;
    c->size = size;
    c->mat_num = 1;
    c->mat_den = 1000;
    c->units_per_em = units_per_em;
    at = data[2];
    if (!read_index(data, size, at, &names) || !names.count)
        goto bad;
    at += names.total;
    if (!read_index(data, size, at, &tops) || !tops.count)
        goto bad;
    at += tops.total;
    if (!read_index(data, size, at, &strings))
        goto bad;
    at += strings.total;
    if (!read_index(data, size, at, &c->gsubrs))
        goto bad;
    c->gsubr_bias = bias_of(c->gsubrs.count);

    /* the first font's Top DICT (an OpenType CFF table holds one) */
    if (!index_get(&tops, 0, &top, &top_len))
        goto bad;
    d.p = top;
    d.end = top + top_len;
    while ((op = dict_next(&d, ops, &n)) >= 0) {
        switch (op) {
        case 17: if (n >= 1) charstrings = (uint32_t)op_int(&ops[0]); break;
        case 18: if (n >= 2) { priv_size = (uint32_t)op_int(&ops[0]); priv_off = (uint32_t)op_int(&ops[1]); } break;
        case 1206: if (n >= 1 && op_int(&ops[0]) != 2) goto bad; break;   /* CharstringType */
        case 1207: if (n >= 1) matrix_entry(&ops[0], &c->mat_num, &c->mat_den); break;
        case 1230: c->cid = 1; break;                                    /* ROS */
        case 1236: if (n >= 1) fdarray = (uint32_t)op_int(&ops[0]); break;
        case 1237: if (n >= 1) fdselect = (uint32_t)op_int(&ops[0]); break;
        default: break;
        }
    }
    if (!charstrings || !read_index(data, size, charstrings, &c->charstrings) || !c->charstrings.count)
        goto bad;
    c->nglyphs = c->charstrings.count;

    if (c->cid) {
        Index fds;
        uint32_t i;
        if (!fdarray || !fdselect || !read_index(data, size, fdarray, &fds) || !fds.count)
            goto bad;
        c->nfds = fds.count;
        c->fds = (FontDict *)calloc(c->nfds, sizeof(*c->fds));
        if (!c->fds) {
            of_cff_close(c);
            return OF_ERR_MEMORY;
        }
        for (i = 0; i < fds.count; ++i) {
            const uint8_t *fp;
            uint32_t fl, ps = 0, po = 0;
            c->fds[i].mat_num = c->fds[i].mat_den = 1;
            if (!index_get(&fds, i, &fp, &fl))
                goto bad;
            d.p = fp;
            d.end = fp + fl;
            while ((op = dict_next(&d, ops, &n)) >= 0) {
                if (op == 18 && n >= 2) {
                    ps = (uint32_t)op_int(&ops[0]);
                    po = (uint32_t)op_int(&ops[1]);
                } else if (op == 1207 && n >= 1) {
                    /* an FD's matrix multiplies the top's */
                    matrix_entry(&ops[0], &c->fds[i].mat_num, &c->fds[i].mat_den);
                }
            }
            if (ps && !read_private(c, ps, po, &c->fds[i]))
                goto bad;
        }
        if (fdselect >= size)
            goto bad;
        c->fdselect = data + fdselect;
        c->fdselect_len = size - fdselect;
    } else {
        c->nfds = 1;
        c->fds = (FontDict *)calloc(1, sizeof(*c->fds));
        if (!c->fds) {
            of_cff_close(c);
            return OF_ERR_MEMORY;
        }
        c->fds[0].mat_num = c->fds[0].mat_den = 1;
        if (priv_size && !read_private(c, priv_size, priv_off, &c->fds[0]))
            goto bad;
    }
    *out = c;
    return OF_OK;
bad:
    of_cff_close(c);
    return OF_ERR_FONT;
}

void of_cff_close(OFCff *c)
{
    if (!c)
        return;
    free(c->fds);
    free(c);
}

static int fd_of(const OFCff *c, uint32_t glyph)
{
    const uint8_t *s = c->fdselect;
    if (!c->cid)
        return 0;
    if (c->fdselect_len < 1)
        return -1;
    if (s[0] == 0) {
        if (glyph + 1 >= c->fdselect_len)
            return -1;
        return s[1 + glyph] < c->nfds ? s[1 + glyph] : -1;
    }
    if (s[0] == 3) {
        uint32_t nr, i;
        if (c->fdselect_len < 5)
            return -1;
        nr = of_u16(s + 1);
        if (3 + 3 * (uint64_t)nr + 2 > c->fdselect_len)
            return -1;
        for (i = 0; i < nr; ++i) {
            uint32_t first = of_u16(s + 3 + 3 * i);
            uint32_t next = of_u16(s + 3 + 3 * (i + 1));    /* the next range's first, or the sentinel */
            if (glyph >= first && glyph < next)
                return s[3 + 3 * i + 2] < c->nfds ? s[3 + 3 * i + 2] : -1;
        }
    }
    return -1;
}

/* ---- the Type 2 interpreter ---- */

#define STACK 48
#define MAX_CALLS 10

typedef struct T2 {
    const OFCff *c;
    const FontDict *fd;
    OFPath *path;
    int32_t st[STACK];          /* 16.16 */
    int n;
    int32_t x, y;               /* the current point, 16.16 */
    int open;                   /* a contour has begun */
    int nstems;
    int width_done;             /* the optional width has been taken (or not) */
    int32_t transient[32];
    int depth;
    int ended;
    int err;
} T2;

static void move_to(T2 *t, int32_t dx, int32_t dy)
{
    if (t->open)
        of_path_close(t->path);
    t->x += dx;
    t->y += dy;
    of_path_point(t->path, t->x, t->y, OF_PT_ON);
    t->open = 1;
}

static void line_to(T2 *t, int32_t dx, int32_t dy)
{
    if (!t->open)
        move_to(t, 0, 0);
    t->x += dx;
    t->y += dy;
    of_path_point(t->path, t->x, t->y, OF_PT_ON);
}

static void curve_to(T2 *t, int32_t dx1, int32_t dy1, int32_t dx2, int32_t dy2, int32_t dx3, int32_t dy3)
{
    if (!t->open)
        move_to(t, 0, 0);
    t->x += dx1;
    t->y += dy1;
    of_path_point(t->path, t->x, t->y, OF_PT_CUBIC);
    t->x += dx2;
    t->y += dy2;
    of_path_point(t->path, t->x, t->y, OF_PT_CUBIC);
    t->x += dx3;
    t->y += dy3;
    of_path_point(t->path, t->x, t->y, OF_PT_ON);
}

/* The width, when an operator's argument count says it is there: it is
 * the first argument of the first stack-clearing operator. */
static int take_width(T2 *t, int extra)
{
    if (t->width_done)
        return 0;
    t->width_done = 1;
    return extra ? 1 : 0;
}

static void stems(T2 *t)
{
    int first = take_width(t, t->n % 2);
    t->nstems += (t->n - first) / 2;
    t->n = 0;
}

static int32_t fx_mul(int32_t a, int32_t b)
{
    int64_t v = (int64_t)a * b;
    return (int32_t)(v >= 0 ? (v + 32768) >> 16 : -((-v + 32768) >> 16));
}

static int32_t fx_div(int32_t a, int32_t b)
{
    int64_t v;
    if (!b)
        return 0;
    v = ((int64_t)a << 16) / b;
    return (int32_t)v;
}

static int32_t fx_sqrt(int32_t a)
{
    /* the square root of a 16.16 number: of (a << 16) as an integer */
    uint64_t v, r = 0, bit = (uint64_t)1 << 62;
    if (a <= 0)
        return 0;
    v = (uint64_t)a << 16;
    while (bit > v)
        bit >>= 2;
    while (bit) {
        if (v >= r + bit) {
            v -= r + bit;
            r = (r >> 1) + bit;
        } else {
            r >>= 1;
        }
        bit >>= 2;
    }
    return (int32_t)r;
}

static int run(T2 *t, const uint8_t *p, uint32_t len);

static int call(T2 *t, const Index *ix, int32_t bias)
{
    const uint8_t *s;
    uint32_t sl;
    int32_t i;
    if (t->n < 1 || t->depth >= MAX_CALLS)
        return 0;
    i = (t->st[--t->n] >> 16) + bias;
    if (i < 0 || !index_get(ix, (uint32_t)i, &s, &sl))
        return 0;
    t->depth++;
    if (!run(t, s, sl))
        return 0;
    t->depth--;
    return 1;
}

#define NEED(k) do { if (t->n < (k)) return 0; } while (0)

static int run(T2 *t, const uint8_t *p, uint32_t len)
{
    const uint8_t *end = p + len;
    int32_t *s = t->st;
    while (p < end && !t->ended) {
        uint8_t b0 = *p++;
        int i;
        if (b0 >= 32 || b0 == 28 || b0 == 255) {
            int32_t v;
            if (t->n >= STACK)
                return 0;
            if (b0 >= 32 && b0 <= 246) {
                v = ((int32_t)b0 - 139) * 65536;
            } else if (b0 >= 247 && b0 <= 250) {
                if (p >= end)
                    return 0;
                v = (((int32_t)b0 - 247) * 256 + *p++ + 108) * 65536;
            } else if (b0 >= 251 && b0 <= 254) {
                if (p >= end)
                    return 0;
                v = (-((int32_t)b0 - 251) * 256 - *p++ - 108) * 65536;
            } else if (b0 == 28) {
                if (end - p < 2)
                    return 0;
                v = (int32_t)of_s16(p) * 65536;
                p += 2;
            } else {
                if (end - p < 4)
                    return 0;
                v = (int32_t)of_u32(p);    /* 16.16 */
                p += 4;
            }
            s[t->n++] = v;
            continue;
        }
        switch (b0) {
        case 1:     /* hstem */
        case 3:     /* vstem */
        case 18:    /* hstemhm */
        case 23:    /* vstemhm */
            stems(t);
            break;
        case 19:    /* hintmask */
        case 20:    /* cntrmask */
            if (t->n)
                stems(t);           /* the vstems before a mask are implied */
            t->width_done = 1;
            p += (t->nstems + 7) / 8;
            if (p > end)
                return 0;
            t->n = 0;
            break;
        case 21:    /* rmoveto */
            i = take_width(t, t->n > 2);
            NEED(i + 2);
            move_to(t, s[i], s[i + 1]);
            t->n = 0;
            break;
        case 22:    /* hmoveto */
            i = take_width(t, t->n > 1);
            NEED(i + 1);
            move_to(t, s[i], 0);
            t->n = 0;
            break;
        case 4:     /* vmoveto */
            i = take_width(t, t->n > 1);
            NEED(i + 1);
            move_to(t, 0, s[i]);
            t->n = 0;
            break;
        case 5:     /* rlineto */
            for (i = 0; i + 1 < t->n; i += 2)
                line_to(t, s[i], s[i + 1]);
            t->n = 0;
            break;
        case 6:     /* hlineto */
        case 7:     /* vlineto */
            for (i = 0; i < t->n; ++i) {
                int horizontal = ((i & 1) == 0) == (b0 == 6);
                if (horizontal)
                    line_to(t, s[i], 0);
                else
                    line_to(t, 0, s[i]);
            }
            t->n = 0;
            break;
        case 8:     /* rrcurveto */
            for (i = 0; i + 5 < t->n; i += 6)
                curve_to(t, s[i], s[i + 1], s[i + 2], s[i + 3], s[i + 4], s[i + 5]);
            t->n = 0;
            break;
        case 24:    /* rcurveline */
            for (i = 0; i + 5 < t->n - 2; i += 6)
                curve_to(t, s[i], s[i + 1], s[i + 2], s[i + 3], s[i + 4], s[i + 5]);
            if (i + 1 < t->n)
                line_to(t, s[i], s[i + 1]);
            t->n = 0;
            break;
        case 25:    /* rlinecurve */
            for (i = 0; i + 1 < t->n - 6; i += 2)
                line_to(t, s[i], s[i + 1]);
            if (i + 5 < t->n)
                curve_to(t, s[i], s[i + 1], s[i + 2], s[i + 3], s[i + 4], s[i + 5]);
            t->n = 0;
            break;
        case 26:    /* vvcurveto */
            i = 0;
            {
                int32_t dx1 = 0;
                if (t->n & 1)
                    dx1 = s[i++];
                for (; i + 3 < t->n; i += 4) {
                    curve_to(t, dx1, s[i], s[i + 1], s[i + 2], 0, s[i + 3]);
                    dx1 = 0;
                }
            }
            t->n = 0;
            break;
        case 27:    /* hhcurveto */
            i = 0;
            {
                int32_t dy1 = 0;
                if (t->n & 1)
                    dy1 = s[i++];
                for (; i + 3 < t->n; i += 4) {
                    curve_to(t, s[i], dy1, s[i + 1], s[i + 2], s[i + 3], 0);
                    dy1 = 0;
                }
            }
            t->n = 0;
            break;
        case 30:    /* vhcurveto */
        case 31:    /* hvcurveto */
            {
                int horizontal = b0 == 31;
                for (i = 0; i + 3 < t->n; i += 4) {
                    int last = i + 4 >= t->n - 1;
                    int32_t extra = (last && i + 4 == t->n - 1) ? s[i + 4] : 0;
                    if (horizontal)
                        curve_to(t, s[i], 0, s[i + 1], s[i + 2], extra, s[i + 3]);
                    else
                        curve_to(t, 0, s[i], s[i + 1], s[i + 2], s[i + 3], extra);
                    horizontal = !horizontal;
                }
            }
            t->n = 0;
            break;
        case 10:    /* callsubr */
            if (!call(t, &t->fd->subrs, t->fd->subr_bias))
                return 0;
            break;
        case 29:    /* callgsubr */
            if (!call(t, &t->c->gsubrs, t->c->gsubr_bias))
                return 0;
            break;
        case 11:    /* return */
            return 1;
        case 14:    /* endchar */
            take_width(t, t->n == 1 || t->n == 5);
            if (t->open)
                of_path_close(t->path);
            t->open = 0;
            t->ended = 1;
            t->n = 0;
            return 1;
        case 12: {
            uint8_t b1;
            if (p >= end)
                return 0;
            b1 = *p++;
            switch (b1) {
            case 35:    /* flex: two curves */
                NEED(13);
                curve_to(t, s[0], s[1], s[2], s[3], s[4], s[5]);
                curve_to(t, s[6], s[7], s[8], s[9], s[10], s[11]);
                t->n = 0;
                break;
            case 34: {  /* hflex */
                int32_t y0 = t->y;
                NEED(7);
                curve_to(t, s[0], 0, s[1], s[2], s[3], 0);
                curve_to(t, s[4], 0, s[5], y0 - (t->y + 0), s[6], 0);
                t->n = 0;
                break;
            }
            case 36: {  /* hflex1 */
                int32_t y0 = t->y;
                NEED(9);
                curve_to(t, s[0], s[1], s[2], s[3], s[4], 0);
                curve_to(t, s[5], 0, s[6], s[7], s[8], y0 - (t->y + s[7]));
                t->n = 0;
                break;
            }
            case 37: {  /* flex1 */
                int32_t x0 = t->x, y0 = t->y, dx, dy, ax, ay;
                NEED(11);
                dx = s[0] + s[2] + s[4] + s[6] + s[8];
                dy = s[1] + s[3] + s[5] + s[7] + s[9];
                curve_to(t, s[0], s[1], s[2], s[3], s[4], s[5]);
                ax = dx < 0 ? -dx : dx;
                ay = dy < 0 ? -dy : dy;
                if (ax > ay) {
                    int32_t x2 = t->x + s[6] + s[8];
                    curve_to(t, s[6], s[7], s[8], s[9], s[10], y0 - (t->y + s[7] + s[9]));
                    (void)x2;
                } else {
                    curve_to(t, s[6], s[7], s[8], s[9], x0 - (t->x + s[6] + s[8]), s[10]);
                }
                t->n = 0;
                break;
            }
            /* arithmetic, rarely used in fonts but defined */
            case 3:  NEED(2); s[t->n - 2] = (s[t->n - 2] && s[t->n - 1]) ? 65536 : 0; t->n--; break;   /* and */
            case 4:  NEED(2); s[t->n - 2] = (s[t->n - 2] || s[t->n - 1]) ? 65536 : 0; t->n--; break;   /* or */
            case 5:  NEED(1); s[t->n - 1] = s[t->n - 1] ? 0 : 65536; break;                           /* not */
            case 9:  NEED(1); if (s[t->n - 1] < 0) s[t->n - 1] = -s[t->n - 1]; break;                 /* abs */
            case 10: NEED(2); s[t->n - 2] += s[t->n - 1]; t->n--; break;                              /* add */
            case 11: NEED(2); s[t->n - 2] -= s[t->n - 1]; t->n--; break;                              /* sub */
            case 12: NEED(2); s[t->n - 2] = fx_div(s[t->n - 2], s[t->n - 1]); t->n--; break;          /* div */
            case 14: NEED(1); s[t->n - 1] = -s[t->n - 1]; break;                                      /* neg */
            case 15: NEED(2); s[t->n - 2] = s[t->n - 2] == s[t->n - 1] ? 65536 : 0; t->n--; break;    /* eq */
            case 18: NEED(1); t->n--; break;                                                          /* drop */
            case 20: NEED(2); { int32_t k = s[t->n - 1] >> 16; if (k >= 0 && k < 32) t->transient[k] = s[t->n - 2]; } t->n -= 2; break; /* put */
            case 21: NEED(1); { int32_t k = s[t->n - 1] >> 16; s[t->n - 1] = (k >= 0 && k < 32) ? t->transient[k] : 0; } break;        /* get */
            case 22: NEED(4); s[t->n - 4] = s[t->n - 2] <= s[t->n - 1] ? s[t->n - 4] : s[t->n - 3]; t->n -= 3; break;                  /* ifelse */
            case 23: if (t->n >= STACK) return 0; s[t->n++] = 32768; break;                          /* random: a fixed value */
            case 24: NEED(2); s[t->n - 2] = fx_mul(s[t->n - 2], s[t->n - 1]); t->n--; break;          /* mul */
            case 26: NEED(1); s[t->n - 1] = fx_sqrt(s[t->n - 1]); break;                              /* sqrt */
            case 27: NEED(1); if (t->n >= STACK) return 0; s[t->n] = s[t->n - 1]; t->n++; break;     /* dup */
            case 28: NEED(2); { int32_t v = s[t->n - 1]; s[t->n - 1] = s[t->n - 2]; s[t->n - 2] = v; } break; /* exch */
            case 29: NEED(1); {                                                                      /* index */
                int32_t k = s[t->n - 1] >> 16;
                if (k < 0) k = 0;
                s[t->n - 1] = (k < t->n - 1) ? s[t->n - 2 - k] : 0;
                break;
            }
            case 30: NEED(2); {                                                                      /* roll */
                int32_t nn = s[t->n - 2] >> 16, jj = s[t->n - 1] >> 16, a, b2;
                t->n -= 2;
                if (nn > 0 && nn <= t->n) {
                    int32_t tmp[STACK];
                    jj %= nn;
                    if (jj < 0)
                        jj += nn;
                    for (a = 0; a < nn; ++a) {
                        b2 = (a + jj) % nn;
                        tmp[b2] = s[t->n - nn + a];
                    }
                    for (a = 0; a < nn; ++a)
                        s[t->n - nn + a] = tmp[a];
                }
                break;
            }
            default:
                t->n = 0;           /* unknown: clear, as the spec asks */
                break;
            }
            break;
        }
        default:
            t->n = 0;               /* reserved operators clear the stack */
            break;
        }
        if (t->path->failed)
            return 0;
    }
    return 1;
}

int of_cff_outline(const OFCff *c, uint32_t glyph, OFPath *path)
{
    const uint8_t *cs;
    uint32_t len, i, first = path->npoints;
    int fd;
    T2 t;
    int64_t num, den;
    if (glyph >= c->nglyphs || !index_get(&c->charstrings, glyph, &cs, &len))
        return OF_ERR_FONT;
    fd = fd_of(c, glyph);
    if (fd < 0)
        return OF_ERR_FONT;
    memset(&t, 0, sizeof t);
    t.c = c;
    t.fd = &c->fds[fd];
    t.path = path;
    if (!run(&t, cs, len))
        return path->failed ? OF_ERR_MEMORY : OF_ERR_FONT;
    if (t.open)
        of_path_close(path);
    /* charstring units to the font's: (top matrix * FD matrix) * units_per_em,
     * 1 for the usual 0.001 and 1000 units per em */
    num = c->mat_num * c->fds[fd].mat_num * (int64_t)c->units_per_em;
    den = c->mat_den * c->fds[fd].mat_den;
    if (num != den && den > 0 && num > 0) {
        for (i = first; i < path->npoints; ++i) {
            path->x[i] = (int32_t)((int64_t)path->x[i] * num / den);
            path->y[i] = (int32_t)((int64_t)path->y[i] * num / den);
        }
    }
    return path->failed ? OF_ERR_MEMORY : OF_OK;
}
