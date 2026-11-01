/* GFOpenFont's OpenType layout: GDEF glyph classes, GSUB substitutions and
 * GPOS positioning, and the old 'kern' table.
 *
 * Copyright (c) 2026 AmiCradle Team. MIT licence (see LICENSE).
 *
 * Written from the OpenType specification: the common table formats
 * (ScriptList, FeatureList, LookupList, Coverage, ClassDef, Device), GDEF,
 * GSUB lookup types 1-8, GPOS lookup types 1-9, and 'kern' format 0. No
 * HarfBuzz code.
 *
 * Every read is checked against its table's length: a damaged table reads
 * as zeros past its end, which stops the walk, never the program. */
#include "gfopenfont/gfopenfont.h"
#include "of_layout.h"
#include "of_unicode.h"

#include <stdlib.h>
#include <string.h>

/* ---- reading a table ---- */

typedef struct Tab {
    const uint8_t *d;
    uint32_t len;
} Tab;

static uint32_t r16(const Tab *t, uint32_t off)
{
    if (off > t->len || t->len - off < 2)
        return 0;
    return of_u16(t->d + off);
}

static int32_t rs16(const Tab *t, uint32_t off)
{
    return (int16_t)r16(t, off);
}

static uint32_t r32(const Tab *t, uint32_t off)
{
    if (off > t->len || t->len - off < 4)
        return 0;
    return of_u32(t->d + off);
}

/* the coverage index of gid, or -1 */
static long coverage(const Tab *t, uint32_t off, uint32_t gid)
{
    uint32_t fmt, n, lo, hi;
    if (!off)
        return -1;
    fmt = r16(t, off);
    n = r16(t, off + 2);
    lo = 0;
    hi = n;
    if (fmt == 1) {
        while (lo < hi) {
            uint32_t mid = (lo + hi) / 2, g = r16(t, off + 4 + 2 * mid);
            if (g < gid)
                lo = mid + 1;
            else if (g > gid)
                hi = mid;
            else
                return (long)mid;
        }
    } else if (fmt == 2) {
        while (lo < hi) {
            uint32_t mid = (lo + hi) / 2, r = off + 4 + 6 * mid;
            uint32_t s = r16(t, r), e = r16(t, r + 2);
            if (gid < s)
                hi = mid;
            else if (gid > e)
                lo = mid + 1;
            else
                return (long)(r16(t, r + 4) + (gid - s));
        }
    }
    return -1;
}

static uint32_t class_of(const Tab *t, uint32_t off, uint32_t gid)
{
    uint32_t fmt;
    if (!off)
        return 0;
    fmt = r16(t, off);
    if (fmt == 1) {
        uint32_t start = r16(t, off + 2), n = r16(t, off + 4);
        if (gid >= start && gid - start < n)
            return r16(t, off + 6 + 2 * (gid - start));
    } else if (fmt == 2) {
        uint32_t n = r16(t, off + 2), lo = 0, hi = n;
        while (lo < hi) {
            uint32_t mid = (lo + hi) / 2, r = off + 4 + 6 * mid;
            uint32_t s = r16(t, r), e = r16(t, r + 2);
            if (gid < s)
                hi = mid;
            else if (gid > e)
                lo = mid + 1;
            else
                return r16(t, r + 4);
        }
    }
    return 0;
}

static Tab table_of(const OFFont *f, const OFTable *t)
{
    Tab r;
    r.d = f->data + t->off;
    r.len = t->len;
    return r;
}

/* ---- the buffer ---- */

int of_shbuf_reserve(OFShBuf *b, uint32_t more)
{
    if (b->failed)
        return 0;
    if (b->n + more > b->cap) {
        uint32_t cap = b->cap ? b->cap * 2 : 64;
        OFShGlyph *g;
        while (cap < b->n + more)
            cap *= 2;
        g = (OFShGlyph *)realloc(b->g, cap * sizeof(*g));
        if (!g) {
            b->failed = 1;
            return 0;
        }
        b->g = g;
        b->cap = cap;
    }
    return 1;
}

/* ---- GDEF ---- */

typedef struct Gdef {
    Tab t;
    uint32_t glyph_classes, mark_classes, mark_sets;
} Gdef;

static void gdef_open(const OFFont *f, Gdef *g)
{
    memset(g, 0, sizeof(*g));
    g->t = table_of(f, &f->gdef);
    if (!f->gdef.len)
        return;
    g->glyph_classes = r16(&g->t, 4);
    g->mark_classes = r16(&g->t, 10);
    if (r32(&g->t, 0) >= 0x00010002u)
        g->mark_sets = r16(&g->t, 12);
}

static int in_mark_set(const Gdef *g, uint32_t set, uint32_t gid)
{
    uint32_t off;
    if (!g->mark_sets || set >= r16(&g->t, g->mark_sets + 2))
        return 0;
    off = r32(&g->t, g->mark_sets + 4 + 4 * set);
    return off && coverage(&g->t, g->mark_sets + off, gid) >= 0;
}

void of_layout_classes(const OFFont *f, OFShBuf *b)
{
    Gdef g;
    uint32_t i;
    gdef_open(f, &g);
    for (i = 0; i < b->n; ++i) {
        OFShGlyph *x = &b->g[i];
        if (g.glyph_classes) {
            x->gclass = (uint8_t)class_of(&g.t, g.glyph_classes, x->gid);
        } else {
            /* no classes in the font: a mark is what Unicode calls one */
            x->gclass = (x->props & OF_SH_LIGATED) ? OF_CLASS_LIGATURE
                        : (of_uni_is_mark(x->cp) ? OF_CLASS_MARK : OF_CLASS_BASE);
        }
        x->mark_class = g.mark_classes ? (uint8_t)class_of(&g.t, g.mark_classes, x->gid) : 0;
    }
}

/* ---- scripts, features and lookups ---- */

#define MAX_LOOKUPS 512

typedef struct Planned {
    uint16_t index;
    uint32_t mask;
    uint8_t stage;
    uint16_t value;             /* the feature's value: an alternate's number */
} Planned;

typedef struct Plan {
    Planned l[MAX_LOOKUPS];
    uint32_t n;
} Plan;

typedef struct Want {
    uint32_t tag;
    uint32_t mask;
    uint8_t stage;
    uint16_t value;
} Want;

/* The features the shaper turns on for horizontal, left-to-right text:
 * the OpenType specification's registered default features for the
 * standard scripts, and the fraction features on their ranges. */
static const Want gsub_wants[] = {
    { OF_TAG('r', 'v', 'r', 'n'), OF_MASK_GLOBAL, 0, 1 },
    { OF_TAG('l', 't', 'r', 'a'), OF_MASK_GLOBAL, 1, 1 },
    { OF_TAG('l', 't', 'r', 'm'), OF_MASK_GLOBAL, 1, 1 },
    { OF_TAG('f', 'r', 'a', 'c'), OF_MASK_FRAC, 1, 1 },
    { OF_TAG('n', 'u', 'm', 'r'), OF_MASK_NUMR, 1, 1 },
    { OF_TAG('d', 'n', 'o', 'm'), OF_MASK_DNOM, 1, 1 },
    { OF_TAG('c', 'c', 'm', 'p'), OF_MASK_GLOBAL, 1, 1 },
    { OF_TAG('l', 'o', 'c', 'l'), OF_MASK_GLOBAL, 1, 1 },
    { OF_TAG('r', 'l', 'i', 'g'), OF_MASK_GLOBAL, 1, 1 },
    { OF_TAG('c', 'a', 'l', 't'), OF_MASK_GLOBAL, 1, 1 },
    { OF_TAG('c', 'l', 'i', 'g'), OF_MASK_GLOBAL, 1, 1 },
    { OF_TAG('l', 'i', 'g', 'a'), OF_MASK_GLOBAL, 1, 1 },
    { OF_TAG('r', 'c', 'l', 't'), OF_MASK_GLOBAL, 1, 1 },
};

/* Right to left, rtla and rtlm in place of ltra and ltrm. With joining
 * characters, the order the OpenType specification gives the joining
 * scripts: the forms each in a stage of their own, after ccmp and locl
 * and before rlig, then calt, then the rest. */
static const Want gsub_joining_wants[] = {
    { OF_TAG('r', 'v', 'r', 'n'), OF_MASK_GLOBAL, 0, 1 },
    { OF_TAG('f', 'r', 'a', 'c'), OF_MASK_FRAC, 1, 1 },
    { OF_TAG('n', 'u', 'm', 'r'), OF_MASK_NUMR, 1, 1 },
    { OF_TAG('d', 'n', 'o', 'm'), OF_MASK_DNOM, 1, 1 },
    { OF_TAG('c', 'c', 'm', 'p'), OF_MASK_GLOBAL, 1, 1 },
    { OF_TAG('l', 'o', 'c', 'l'), OF_MASK_GLOBAL, 1, 1 },
    { OF_TAG('i', 's', 'o', 'l'), OF_MASK_ISOL, 2, 1 },
    { OF_TAG('f', 'i', 'n', 'a'), OF_MASK_FINA, 3, 1 },
    { OF_TAG('m', 'e', 'd', 'i'), OF_MASK_MEDI, 4, 1 },
    { OF_TAG('i', 'n', 'i', 't'), OF_MASK_INIT, 5, 1 },
    { OF_TAG('r', 'l', 'i', 'g'), OF_MASK_GLOBAL, 6, 1 },
    { OF_TAG('c', 'a', 'l', 't'), OF_MASK_GLOBAL, 7, 1 },
    { OF_TAG('c', 'l', 'i', 'g'), OF_MASK_GLOBAL, 8, 1 },
    { OF_TAG('l', 'i', 'g', 'a'), OF_MASK_GLOBAL, 8, 1 },
    { OF_TAG('r', 'c', 'l', 't'), OF_MASK_GLOBAL, 8, 1 },
    { OF_TAG('m', 's', 'e', 't'), OF_MASK_GLOBAL, 8, 1 },
};

static const Want gpos_wants[] = {
    { OF_TAG('a', 'b', 'v', 'm'), OF_MASK_GLOBAL, 0, 1 },
    { OF_TAG('b', 'l', 'w', 'm'), OF_MASK_GLOBAL, 0, 1 },
    { OF_TAG('m', 'a', 'r', 'k'), OF_MASK_GLOBAL, 0, 1 },
    { OF_TAG('m', 'k', 'm', 'k'), OF_MASK_GLOBAL, 0, 1 },
    { OF_TAG('c', 'u', 'r', 's'), OF_MASK_GLOBAL, 0, 1 },
    { OF_TAG('d', 'i', 's', 't'), OF_MASK_GLOBAL, 0, 1 },
    { OF_TAG('k', 'e', 'r', 'n'), OF_MASK_GLOBAL, 0, 1 },
};

#define MAX_WANTS (sizeof gsub_joining_wants / sizeof gsub_joining_wants[0] + 2 + OF_MAX_FEATURES)

/* The features for spec: the shaper's own for its direction and joining,
 * then the program's on top (a value of 0 over the whole text takes one
 * of the shaper's away). Returns how many. */
static size_t make_wants(const OFLayoutSpec *spec, int gpos, Want *out)
{
    const Want *base;
    size_t nb, n = 0, i, k;
    uint8_t last_stage;
    if (gpos) {
        base = gpos_wants;
        nb = sizeof gpos_wants / sizeof gpos_wants[0];
    } else if (spec && spec->joining) {
        base = gsub_joining_wants;
        nb = sizeof gsub_joining_wants / sizeof gsub_joining_wants[0];
    } else {
        base = gsub_wants;
        nb = sizeof gsub_wants / sizeof gsub_wants[0];
    }
    for (i = 0; i < nb; ++i) {
        Want w = base[i];
        if (spec && spec->rtl && w.tag == OF_TAG('l', 't', 'r', 'a'))
            w.tag = OF_TAG('r', 't', 'l', 'a');
        if (spec && spec->rtl && w.tag == OF_TAG('l', 't', 'r', 'm'))
            w.tag = OF_TAG('r', 't', 'l', 'm');
        out[n++] = w;
    }
    if (!gpos && spec && spec->joining && spec->rtl) {
        Want a = { OF_TAG('r', 't', 'l', 'a'), OF_MASK_GLOBAL, 1, 1 };
        Want m = { OF_TAG('r', 't', 'l', 'm'), OF_MASK_GLOBAL, 1, 1 };
        out[n++] = a;
        out[n++] = m;
    } else if (!gpos && spec && spec->joining) {
        Want a = { OF_TAG('l', 't', 'r', 'a'), OF_MASK_GLOBAL, 1, 1 };
        Want m = { OF_TAG('l', 't', 'r', 'm'), OF_MASK_GLOBAL, 1, 1 };
        out[n++] = a;
        out[n++] = m;
    }
    last_stage = 0;
    for (i = 0; i < n; ++i)
        if (out[i].stage > last_stage)
            last_stage = out[i].stage;
    if (!spec)
        return n;
    for (k = 0; k < spec->nfeatures && k < OF_MAX_FEATURES; ++k) {
        const OFFeature *ft = &spec->features[k];
        uint32_t ranged = spec->ranged_mask[k];
        uint16_t value = (uint16_t)(ft->value > 0xFFFF ? 0xFFFF : ft->value);
        for (i = 0; i < n; ++i)
            if (out[i].tag == ft->tag)
                break;
        if (ranged) {
            /* on where its mask is, which the shaper set */
            if (i == n) {
                Want w = { ft->tag, ranged, last_stage, value ? value : 1 };
                out[n++] = w;
            } else {
                out[i].mask = ranged;
                if (value)
                    out[i].value = value;
            }
        } else if (!ft->value) {
            if (i < n) {
                memmove(&out[i], &out[i + 1], (n - i - 1) * sizeof(Want));
                n--;
            }
        } else if (i < n) {
            out[i].value = value;
        } else {
            Want w = { ft->tag, OF_MASK_GLOBAL, last_stage, value };
            out[n++] = w;
        }
    }
    return n;
}

int of_layout_default_feature(const OFLayoutSpec *spec, uint32_t tag)
{
    Want w[MAX_WANTS];
    OFLayoutSpec bare = *spec;
    size_t n, i;
    bare.nfeatures = 0;
    n = make_wants(&bare, 0, w);
    for (i = 0; i < n; ++i)
        if (w[i].tag == tag)
            return 1;
    n = make_wants(&bare, 1, w);
    for (i = 0; i < n; ++i)
        if (w[i].tag == tag)
            return 1;
    return 0;
}

/* the language system of the script for lang (its default when lang is 0
 * or it has none for it), or the default one of DFLT, dflt or latn when
 * the font has none for the script; 0 when none of them */
static uint32_t langsys(const Tab *t, uint32_t script_tag, uint32_t lang_tag)
{
    uint32_t sl = r16(t, 4), n = r16(t, sl), k, i, j;
    const uint32_t tries[4] = { script_tag, OF_TAG('D', 'F', 'L', 'T'), OF_TAG('d', 'f', 'l', 't'), OF_TAG('l', 'a', 't', 'n') };
    if (!sl)
        return 0;
    for (k = 0; k < 4; ++k)
        for (i = 0; i < n; ++i)
            if (r32(t, sl + 2 + 6 * i) == tries[k]) {
                uint32_t s = sl + r16(t, sl + 2 + 6 * i + 4);
                uint32_t def = r16(t, s), nl = r16(t, s + 2);
                if (lang_tag)
                    for (j = 0; j < nl; ++j)
                        if (r32(t, s + 4 + 6 * j) == lang_tag)
                            return s + r16(t, s + 4 + 6 * j + 4);
                return def ? s + def : 0;
            }
    return 0;
}

static void plan_add(Plan *p, uint32_t index, uint32_t mask, uint8_t stage, uint16_t value)
{
    uint32_t i;
    for (i = 0; i < p->n; ++i)
        if (p->l[i].index == index && p->l[i].stage == stage) {
            p->l[i].mask |= mask;
            return;
        }
    if (p->n < MAX_LOOKUPS) {
        p->l[p->n].index = (uint16_t)index;
        p->l[p->n].mask = mask;
        p->l[p->n].stage = stage;
        p->l[p->n].value = value;
        p->n++;
    }
}

static int planned_cmp(const void *a, const void *b)
{
    const Planned *x = (const Planned *)a, *y = (const Planned *)b;
    if (x->stage != y->stage)
        return x->stage - y->stage;
    return (int)x->index - (int)y->index;
}

/* the lookups of the wanted features, by stage then lookup index */
static void make_plan(const Tab *t, uint32_t script_tag, uint32_t lang_tag, const Want *w, size_t nw,
                      Plan *p, uint32_t only_tag)
{
    uint32_t ls = langsys(t, script_tag, lang_tag), fl = r16(t, 6), nf, req, i, k;
    p->n = 0;
    if (!ls || !fl)
        return;
    nf = r16(t, ls + 4);
    req = r16(t, ls + 2);
    for (i = 0; i <= nf; ++i) {
        uint32_t fi, rec, ft, tag, nl, j;
        const Want *want = NULL;
        Want required;
        if (i == nf) {
            if (req == 0xFFFF || only_tag)
                break;
            fi = req;
        } else {
            fi = r16(t, ls + 6 + 2 * i);
        }
        if (fi >= r16(t, fl))
            continue;
        rec = fl + 2 + 6 * fi;
        tag = r32(t, rec);
        ft = fl + r16(t, rec + 4);
        if (i == nf) {
            required.tag = tag;
            required.mask = OF_MASK_GLOBAL;
            required.stage = 1;
            required.value = 1;
            want = &required;
        } else {
            for (k = 0; k < nw; ++k)
                if (w[k].tag == tag && (!only_tag || tag == only_tag))
                    want = &w[k];
        }
        if (!want)
            continue;
        nl = r16(t, ft + 2);
        for (j = 0; j < nl; ++j)
            plan_add(p, r16(t, ft + 4 + 2 * j), want->mask, want->stage, want->value);
    }
    qsort(p->l, p->n, sizeof p->l[0], planned_cmp);
}

/* ---- applying lookups ---- */

#define MAX_CONTEXT 64
#define MAX_NESTING 8

typedef struct Ctx {
    const OFFont *f;
    Tab t;                      /* GSUB or GPOS */
    Gdef gdef;
    OFShBuf *b;
    int gpos;
    uint32_t mask;              /* the lookup's feature mask */
    uint32_t value;             /* its feature's value */
    int rtl;                    /* the run is right to left (still in logical order) */
    uint32_t flag;              /* its lookup flag */
    uint32_t mark_set;          /* its mark filtering set */
    int depth;
    uint32_t ppem;
    int32_t scale;              /* 16.16, font units to 26.6 */
} Ctx;

#define LF_RIGHT_TO_LEFT  0x0001
#define LF_IGNORE_BASE    0x0002
#define LF_IGNORE_LIG     0x0004
#define LF_IGNORE_MARKS   0x0008
#define LF_USE_MARK_SET   0x0010
#define LF_MARK_TYPE      0xFF00

/* font units to 26.6, rounded half up */
static int32_t units(const Ctx *c, int32_t v)
{
    int64_t n = (int64_t)v * c->scale + 32768;
    return (int32_t)(n >= 0 ? n >> 16 : -((-n + 65535) >> 16));
}

/* a 26.6 value with 16 more bits, rounded half up */
static int32_t round_fine(int64_t v)
{
    v += 32768;
    return (int32_t)(v >= 0 ? v >> 16 : -((-v + 65535) >> 16));
}

/* whether the lookup flag passes over glyph i */
static int skipped_by_flag(const Ctx *c, const OFShGlyph *x)
{
    if (x->gclass == OF_CLASS_BASE && (c->flag & LF_IGNORE_BASE))
        return 1;
    if (x->gclass == OF_CLASS_LIGATURE && (c->flag & LF_IGNORE_LIG))
        return 1;
    if (x->gclass == OF_CLASS_MARK) {
        if (c->flag & LF_IGNORE_MARKS)
            return 1;
        if ((c->flag & LF_USE_MARK_SET) && !in_mark_set(&c->gdef, c->mark_set, x->gid))
            return 1;
        if ((c->flag & LF_MARK_TYPE) && x->mark_class != (c->flag >> 8))
            return 1;
    }
    return 0;
}

/* whether matching passes over glyph i: the lookup flag's glyphs, and the
 * default ignorables, but in substitution not ZWNJ, which keeps letters
 * apart */
static int skipped(const Ctx *c, uint32_t i)
{
    const OFShGlyph *x = &c->b->g[i];
    if (skipped_by_flag(c, x))
        return 1;
    if (x->props & OF_SH_IGNORABLE) {
        if (!c->gpos && (x->props & OF_SH_ZWNJ))
            return 0;
        return 1;
    }
    return 0;
}

/* the next glyph matching may use after i, or -1 */
static long next_glyph(const Ctx *c, long i)
{
    for (++i; i < (long)c->b->n; ++i)
        if (!skipped(c, (uint32_t)i))
            return i;
    return -1;
}

static long prev_glyph(const Ctx *c, long i)
{
    for (--i; i >= 0; --i)
        if (!skipped(c, (uint32_t)i))
            return i;
    return -1;
}

/* breaking anywhere inside [from, to] (glyph indices) changes the shaping:
 * every glyph there whose cluster differs from the first's is marked */
void of_layout_unsafe(OFShBuf *b, long from, long to)
{
    uint32_t first;
    long i;
    if (from < 0 || to < from || (uint32_t)to >= b->n)
        return;
    /* whole clusters: the rest of the first's and the last's too */
    while (from > 0 && b->g[from - 1].cluster == b->g[from].cluster)
        from--;
    while ((uint32_t)to + 1 < b->n && b->g[to + 1].cluster == b->g[to].cluster)
        to++;
    first = b->g[from].cluster;
    for (i = from; i <= to; ++i)
        if (b->g[i].cluster < first)
            first = b->g[i].cluster;
    for (i = from; i <= to; ++i)
        if (b->g[i].cluster != first)
            b->g[i].props |= OF_SH_UNSAFE;
}

/* merges the clusters of glyphs [from, to] into the smallest, taking in
 * the rest of the first's and the last's clusters, so a cluster stays
 * whole */
static void merge_clusters(OFShBuf *b, uint32_t from, uint32_t to)
{
    uint32_t i, m;
    while (from > 0 && b->g[from - 1].cluster == b->g[from].cluster)
        from--;
    while (to + 1 < b->n && b->g[to + 1].cluster == b->g[to].cluster)
        to++;
    m = b->g[from].cluster;
    for (i = from; i <= to; ++i)
        if (b->g[i].cluster < m)
            m = b->g[i].cluster;
    for (i = from; i <= to; ++i)
        b->g[i].cluster = m;
}

static int apply_lookup(Ctx *c, uint32_t index, uint32_t i, uint32_t *next);

/* the input sequence of a contextual rule: count glyphs from i (the first
 * already matched), each tested by test(k, gid); their indices in pos */
typedef int (*Test)(const Ctx *c, const void *arg, uint32_t k, uint32_t gid);

static int match_input(const Ctx *c, uint32_t i, uint32_t count, Test test, const void *arg, uint32_t *pos)
{
    uint32_t k;
    long at = (long)i;
    pos[0] = i;
    for (k = 1; k < count; ++k) {
        at = next_glyph(c, at);
        if (at < 0 || !(c->b->g[at].mask & c->mask) || !test(c, arg, k, c->b->g[at].gid))
            return 0;
        pos[k] = (uint32_t)at;
    }
    return 1;
}

static int match_backtrack(const Ctx *c, uint32_t i, uint32_t count, Test test, const void *arg, long *start)
{
    uint32_t k;
    long at = (long)i;
    *start = at;
    for (k = 0; k < count; ++k) {
        at = prev_glyph(c, at);
        if (at < 0 || !test(c, arg, k, c->b->g[at].gid))
            return 0;
        *start = at;
    }
    return 1;
}

static int match_lookahead(const Ctx *c, uint32_t last, uint32_t count, Test test, const void *arg, long *end)
{
    uint32_t k;
    long at = (long)last;
    *end = at;
    for (k = 0; k < count; ++k) {
        at = next_glyph(c, at);
        if (at < 0 || !test(c, arg, k, c->b->g[at].gid))
            return 0;
        *end = at;
    }
    return 1;
}

/* tests: a list of glyph ids, of classes, or of coverages, at an offset */
typedef struct List {
    const Tab *t;
    uint32_t off;               /* the first entry */
    uint32_t base;              /* coverages: offsets from here; classes: the ClassDef */
    int skip_first;             /* the list leaves out the first glyph */
} List;

static int test_glyph(const Ctx *c, const void *arg, uint32_t k, uint32_t gid)
{
    const List *l = (const List *)arg;
    (void)c;
    return r16(l->t, l->off + 2 * (k - (uint32_t)l->skip_first)) == gid;
}

static int test_class(const Ctx *c, const void *arg, uint32_t k, uint32_t gid)
{
    const List *l = (const List *)arg;
    (void)c;
    return r16(l->t, l->off + 2 * (k - (uint32_t)l->skip_first)) == class_of(l->t, l->base, gid);
}

static int test_coverage(const Ctx *c, const void *arg, uint32_t k, uint32_t gid)
{
    const List *l = (const List *)arg;
    (void)c;
    return coverage(l->t, l->base + r16(l->t, l->off + 2 * (k - (uint32_t)l->skip_first)), gid) >= 0;
}

/* after a contextual match: its nested lookups, at their places in the
 * input, which may grow or shrink as they go */
static void apply_nested(Ctx *c, uint32_t *pos, uint32_t count, uint32_t recs, uint32_t nrec, uint32_t *next)
{
    uint32_t r, k;
    uint32_t mask = c->mask, flag = c->flag, mark_set = c->mark_set;
    long end = pos[count - 1];
    for (r = 0; r < nrec; ++r) {
        uint32_t seq = r16(&c->t, recs + 4 * r), li = r16(&c->t, recs + 4 * r + 2);
        uint32_t before = c->b->n, dummy;
        int32_t delta;
        if (seq >= count || c->depth >= MAX_NESTING)
            continue;
        c->depth++;
        apply_lookup(c, li, pos[seq], &dummy);
        c->depth--;
        c->mask = mask;         /* the nested lookup's own flag was its own */
        c->flag = flag;
        c->mark_set = mark_set;
        delta = (int32_t)c->b->n - (int32_t)before;
        if (delta) {
            for (k = seq + 1; k < count; ++k)
                pos[k] = (uint32_t)((int32_t)pos[k] + delta);
            end += delta;
        }
    }
    *next = (uint32_t)(end + 1);
    if (*next > c->b->n)
        *next = c->b->n;
}

/* ---- GSUB ---- */

static void set_glyph(Ctx *c, uint32_t i, uint32_t gid)
{
    OFShGlyph *x = &c->b->g[i];
    x->gid = gid;
    if (c->gdef.glyph_classes)
        x->gclass = (uint8_t)class_of(&c->gdef.t, c->gdef.glyph_classes, gid);
    x->mark_class = c->gdef.mark_classes ? (uint8_t)class_of(&c->gdef.t, c->gdef.mark_classes, gid) : 0;
}

static int gsub_single(Ctx *c, uint32_t st, uint32_t i)
{
    const Tab *t = &c->t;
    uint32_t fmt = r16(t, st), gid = c->b->g[i].gid;
    long k = coverage(t, st + r16(t, st + 2), gid);
    if (k < 0)
        return 0;
    if (fmt == 1)
        set_glyph(c, i, (gid + (uint32_t)rs16(t, st + 4)) & 0xFFFF);
    else if (fmt == 2 && (uint32_t)k < r16(t, st + 4))
        set_glyph(c, i, r16(t, st + 6 + 2 * k));
    else
        return 0;
    return 1;
}

static int gsub_multiple(Ctx *c, uint32_t st, uint32_t i, uint32_t *next)
{
    const Tab *t = &c->t;
    long k = coverage(t, st + r16(t, st + 2), c->b->g[i].gid);
    uint32_t seq, n, j;
    OFShGlyph x;
    if (r16(t, st) != 1 || k < 0 || (uint32_t)k >= r16(t, st + 4))
        return 0;
    seq = st + r16(t, st + 6 + 2 * k);
    n = r16(t, seq);
    x = c->b->g[i];
    if (n == 0) {
        /* an empty sequence takes the glyph out */
        memmove(&c->b->g[i], &c->b->g[i + 1], (c->b->n - i - 1) * sizeof(OFShGlyph));
        c->b->n--;
        *next = i;
        return 1;
    }
    if (!of_shbuf_reserve(c->b, n - 1))
        return 0;
    memmove(&c->b->g[i + n], &c->b->g[i + 1], (c->b->n - i - 1) * sizeof(OFShGlyph));
    c->b->n += n - 1;
    for (j = 0; j < n; ++j) {
        c->b->g[i + j] = x;
        set_glyph(c, i + j, r16(t, seq + 2 + 2 * j));
        if (n > 1) {
            c->b->g[i + j].props |= OF_SH_MULTIPLIED;
            /* the pieces of one character: marks find the right one */
            if (!x.lig_id)
                c->b->g[i + j].lig_comp = (uint8_t)(j + 1 > 255 ? 255 : j + 1);
        }
    }
    *next = i + n;
    return 1;
}

static int gsub_alternate(Ctx *c, uint32_t st, uint32_t i)
{
    const Tab *t = &c->t;
    long k = coverage(t, st + r16(t, st + 2), c->b->g[i].gid);
    uint32_t set;
    if (r16(t, st) != 1 || k < 0 || (uint32_t)k >= r16(t, st + 4))
        return 0;
    set = st + r16(t, st + 6 + 2 * k);
    if (!r16(t, set))
        return 0;
    /* the feature's value: 1 the first alternate, 2 the second */
    {
        uint32_t na = r16(t, set), v = c->value ? c->value : 1;
        if (v > na)
            return 0;
        set_glyph(c, i, r16(t, set + 2 * v));
    }
    return 1;
}

static int gsub_ligature(Ctx *c, uint32_t st, uint32_t i, uint32_t *next)
{
    const Tab *t = &c->t;
    long k = coverage(t, st + r16(t, st + 2), c->b->g[i].gid);
    uint32_t set, nl, l;
    if (r16(t, st) != 1 || k < 0 || (uint32_t)k >= r16(t, st + 4))
        return 0;
    set = st + r16(t, st + 6 + 2 * k);
    nl = r16(t, set);
    for (l = 0; l < nl; ++l) {
        uint32_t lig = set + r16(t, set + 2 + 2 * l);
        uint32_t count = r16(t, lig + 2), pos[MAX_CONTEXT], j, out, last;
        List list;
        uint8_t id, comp;
        if (!count || count > MAX_CONTEXT)
            continue;
        list.t = t;
        list.off = lig + 4;
        list.base = 0;
        list.skip_first = 1;
        if (!match_input(c, i, count, test_glyph, &list, pos))
            continue;
        last = pos[count - 1];
        /* the ligature takes the first glyph's place; the glyphs passed
         * over between its components (marks) stay, after it, each told
         * which component it followed */
        merge_clusters(c->b, i, last);
        if (++c->b->next_lig_id == 0)
            c->b->next_lig_id = 1;
        id = c->b->next_lig_id;
        set_glyph(c, i, r16(t, lig));
        c->b->g[i].props |= OF_SH_LIGATED;
        c->b->g[i].props &= (uint16_t)~OF_SH_MULTIPLIED;
        c->b->g[i].lig_id = id;
        c->b->g[i].lig_comp = 0;
        c->b->g[i].lig_count = (uint8_t)(count > 255 ? 255 : count);
        if (!c->gdef.glyph_classes)
            c->b->g[i].gclass = OF_CLASS_LIGATURE;
        out = i + 1;
        comp = 1;
        for (j = i + 1; j <= last; ++j) {
            uint32_t m;
            int component = 0;
            for (m = 1; m < count; ++m)
                if (pos[m] == j)
                    component = 1;
            if (component) {
                comp++;
                continue;
            }
            c->b->g[out] = c->b->g[j];
            if (c->b->g[out].gclass == OF_CLASS_MARK || (c->b->g[out].props & OF_SH_IGNORABLE)) {
                c->b->g[out].lig_id = id;
                c->b->g[out].lig_comp = comp;
            }
            out++;
        }
        memmove(&c->b->g[out], &c->b->g[last + 1], (c->b->n - last - 1) * sizeof(OFShGlyph));
        c->b->n -= (last + 1 - out);
        /* marks after the ligature that sat on its last component */
        *next = i + 1;
        return 1;
    }
    return 0;
}

/* contextual and chained contextual rules, GSUB 5/6 and GPOS 7/8 */
static int context(Ctx *c, uint32_t st, uint32_t i, uint32_t *next)
{
    const Tab *t = &c->t;
    uint32_t fmt = r16(t, st), gid = c->b->g[i].gid, pos[MAX_CONTEXT];
    if (fmt == 1 || fmt == 2) {
        long k = coverage(t, st + r16(t, st + 2), gid);
        uint32_t cdef = fmt == 2 ? st + r16(t, st + 4) : 0;
        uint32_t sets = fmt == 2 ? st + 6 : st + 4, set, nr, r, idx;
        if (k < 0)
            return 0;
        idx = fmt == 2 ? class_of(t, cdef, gid) : (uint32_t)k;
        if (idx >= r16(t, sets))
            return 0;
        set = r16(t, sets + 2 + 2 * idx);
        if (!set)
            return 0;
        set += st;
        nr = r16(t, set);
        for (r = 0; r < nr; ++r) {
            uint32_t rule = set + r16(t, set + 2 + 2 * r);
            uint32_t count = r16(t, rule), nrec = r16(t, rule + 2);
            List l;
            if (!count || count > MAX_CONTEXT)
                continue;
            l.t = t;
            l.off = rule + 4;
            l.base = cdef;
            l.skip_first = 1;
            if (!match_input(c, i, count, fmt == 2 ? test_class : test_glyph, &l, pos))
                continue;
            of_layout_unsafe(c->b, (long)i, (long)pos[count - 1]);
            apply_nested(c, pos, count, rule + 4 + 2 * (count - 1), nrec, next);
            return 1;
        }
        return 0;
    }
    if (fmt == 3) {
        uint32_t count = r16(t, st + 2), nrec = r16(t, st + 4);
        List l;
        if (!count || count > MAX_CONTEXT)
            return 0;
        l.t = t;
        l.off = st + 6;
        l.base = st;
        l.skip_first = 0;
        if (coverage(t, st + r16(t, st + 6), gid) < 0 || !match_input(c, i, count, test_coverage, &l, pos))
            return 0;
        of_layout_unsafe(c->b, (long)i, (long)pos[count - 1]);
        apply_nested(c, pos, count, st + 6 + 2 * count, nrec, next);
        return 1;
    }
    return 0;
}

static int chain_context(Ctx *c, uint32_t st, uint32_t i, uint32_t *next)
{
    const Tab *t = &c->t;
    uint32_t fmt = r16(t, st), gid = c->b->g[i].gid, pos[MAX_CONTEXT];
    long start, end;
    if (fmt == 1 || fmt == 2) {
        long k = coverage(t, st + r16(t, st + 2), gid);
        uint32_t bdef = 0, idef = 0, ldef = 0, sets, set, nr, r, idx;
        if (k < 0)
            return 0;
        if (fmt == 2) {
            bdef = st + r16(t, st + 4);
            idef = st + r16(t, st + 6);
            ldef = st + r16(t, st + 8);
            sets = st + 10;
            idx = class_of(t, idef, gid);
        } else {
            sets = st + 4;
            idx = (uint32_t)k;
        }
        if (idx >= r16(t, sets))
            return 0;
        set = r16(t, sets + 2 + 2 * idx);
        if (!set)
            return 0;
        set += st;
        nr = r16(t, set);
        for (r = 0; r < nr; ++r) {
            uint32_t rule = set + r16(t, set + 2 + 2 * r), at = rule;
            uint32_t nb = r16(t, at), ni, na, nrec;
            List bl, il, al;
            Test test = fmt == 2 ? test_class : test_glyph;
            bl.t = il.t = al.t = t;
            bl.off = at + 2;
            bl.base = bdef;
            bl.skip_first = 0;
            at += 2 + 2 * nb;
            ni = r16(t, at);
            il.off = at + 2;
            il.base = idef;
            il.skip_first = 1;
            if (!ni || ni > MAX_CONTEXT)
                continue;
            at += 2 + 2 * (ni - 1);
            na = r16(t, at);
            al.off = at + 2;
            al.base = ldef;
            al.skip_first = 0;
            at += 2 + 2 * na;
            nrec = r16(t, at);
            if (!match_input(c, i, ni, test, &il, pos) ||
                !match_backtrack(c, i, nb, test, &bl, &start) ||
                !match_lookahead(c, pos[ni - 1], na, test, &al, &end))
                continue;
            of_layout_unsafe(c->b, start, end);
            apply_nested(c, pos, ni, at + 2, nrec, next);
            return 1;
        }
        return 0;
    }
    if (fmt == 3) {
        uint32_t at = st + 2, nb = r16(t, at), ni, na, nrec;
        List bl, il, al;
        bl.t = il.t = al.t = t;
        bl.base = il.base = al.base = st;
        bl.off = at + 2;
        bl.skip_first = 0;
        at += 2 + 2 * nb;
        ni = r16(t, at);
        il.off = at + 2;
        il.skip_first = 0;
        if (!ni || ni > MAX_CONTEXT)
            return 0;
        at += 2 + 2 * ni;
        na = r16(t, at);
        al.off = at + 2;
        al.skip_first = 0;
        at += 2 + 2 * na;
        nrec = r16(t, at);
        if (!test_coverage(c, &il, 0, gid) || !match_input(c, i, ni, test_coverage, &il, pos) ||
            !match_backtrack(c, i, nb, test_coverage, &bl, &start) ||
            !match_lookahead(c, pos[ni - 1], na, test_coverage, &al, &end))
            return 0;
        of_layout_unsafe(c->b, start, end);
        apply_nested(c, pos, ni, at + 2, nrec, next);
        return 1;
    }
    return 0;
}

static int gsub_reverse(Ctx *c, uint32_t st, uint32_t i)
{
    const Tab *t = &c->t;
    uint32_t at = st + 4, nb, na, ng;
    long k = coverage(t, st + r16(t, st + 2), c->b->g[i].gid), start, end;
    List bl, al;
    if (r16(t, st) != 1 || k < 0)
        return 0;
    nb = r16(t, at);
    bl.t = al.t = t;
    bl.base = al.base = st;
    bl.off = at + 2;
    bl.skip_first = 0;
    at += 2 + 2 * nb;
    na = r16(t, at);
    al.off = at + 2;
    al.skip_first = 0;
    at += 2 + 2 * na;
    ng = r16(t, at);
    if ((uint32_t)k >= ng || !match_backtrack(c, i, nb, test_coverage, &bl, &start) ||
        !match_lookahead(c, i, na, test_coverage, &al, &end))
        return 0;
    of_layout_unsafe(c->b, start, end);
    set_glyph(c, i, r16(t, at + 2 + 2 * k));
    return 1;
}

/* ---- GPOS ---- */

static int32_t device_delta(const Ctx *c, uint32_t off)
{
    const Tab *t = &c->t;
    uint32_t start, end, fmt, bits, per, idx, word, shift, v;
    int32_t d;
    if (!off || !c->ppem)
        return 0;
    start = r16(t, off);
    end = r16(t, off + 2);
    fmt = r16(t, off + 4);
    if (fmt < 1 || fmt > 3 || c->ppem < start || c->ppem > end)
        return 0;               /* 0x8000 is a variation index: no variations here */
    bits = 1u << fmt;           /* 2, 4 or 8 bits a delta */
    per = 16 / bits;
    idx = c->ppem - start;
    word = r16(t, off + 6 + 2 * (idx / per));
    shift = 16 - bits * (idx % per + 1);
    v = (word >> shift) & ((1u << bits) - 1);
    d = (int32_t)v;
    if (v & (1u << (bits - 1)))
        d -= (int32_t)(1u << bits);
    return d * 64;              /* whole pixels in 26.6 */
}

static uint32_t value_size(uint32_t fmt)
{
    uint32_t n = 0;
    for (fmt &= 0xFF; fmt; fmt >>= 1)
        n += fmt & 1;
    return 2 * n;
}

/* adds a value record at off (relative to base for its devices) to glyph
 * i; whether it moved anything */
static int apply_value(Ctx *c, uint32_t base, uint32_t off, uint32_t fmt, uint32_t i)
{
    const Tab *t = &c->t;
    OFShGlyph *x = &c->b->g[i];
    uint32_t at = off;
    int32_t xo = x->xo, yo = x->yo, xa = x->xa, ya = x->ya;
    if (fmt & 0x01) { x->xo += units(c, rs16(t, at)); at += 2; }
    if (fmt & 0x02) { x->yo += units(c, rs16(t, at)); at += 2; }
    if (fmt & 0x04) { x->xa += units(c, rs16(t, at)); at += 2; }
    if (fmt & 0x08) { x->ya += units(c, rs16(t, at)); at += 2; }
    if (fmt & 0x10) { uint32_t d = r16(t, at); x->xo += d ? device_delta(c, base + d) : 0; at += 2; }
    if (fmt & 0x20) { uint32_t d = r16(t, at); x->yo += d ? device_delta(c, base + d) : 0; at += 2; }
    if (fmt & 0x40) { uint32_t d = r16(t, at); x->xa += d ? device_delta(c, base + d) : 0; at += 2; }
    if (fmt & 0x80) { uint32_t d = r16(t, at); x->ya += d ? device_delta(c, base + d) : 0; at += 2; }
    return xo != x->xo || yo != x->yo || xa != x->xa || ya != x->ya;
}

/* an anchor in 26.6 with 16 more bits: a mark's place is the difference
 * of two, rounded once */
static void anchor(const Ctx *c, uint32_t off, int64_t *x, int64_t *y)
{
    const Tab *t = &c->t;
    uint32_t fmt = r16(t, off);
    *x = (int64_t)rs16(t, off + 2) * c->scale;
    *y = (int64_t)rs16(t, off + 4) * c->scale;
    if (fmt == 3) {
        uint32_t dx = r16(t, off + 6), dy = r16(t, off + 8);
        if (dx)
            *x += (int64_t)device_delta(c, off + dx) * 65536;
        if (dy)
            *y += (int64_t)device_delta(c, off + dy) * 65536;
    }
    /* format 2's contour point is for hinted outlines: its x and y here */
}

static int gpos_single(Ctx *c, uint32_t st, uint32_t i)
{
    const Tab *t = &c->t;
    uint32_t fmt = r16(t, st), vf = r16(t, st + 4);
    long k = coverage(t, st + r16(t, st + 2), c->b->g[i].gid);
    if (k < 0)
        return 0;
    if (fmt == 1)
        apply_value(c, st, st + 6, vf, i);
    else if (fmt == 2 && (uint32_t)k < r16(t, st + 6))
        apply_value(c, st, st + 8 + value_size(vf) * (uint32_t)k, vf, i);
    else
        return 0;
    return 1;
}

static int gpos_pair(Ctx *c, uint32_t st, uint32_t i, uint32_t *next)
{
    const Tab *t = &c->t;
    uint32_t fmt = r16(t, st), vf1 = r16(t, st + 4), vf2 = r16(t, st + 6);
    uint32_t s1 = value_size(vf1), s2 = value_size(vf2), g2;
    long k = coverage(t, st + r16(t, st + 2), c->b->g[i].gid), j;
    if (k < 0)
        return 0;
    j = next_glyph(c, (long)i);
    if (j < 0)
        return 0;
    g2 = c->b->g[j].gid;
    if (fmt == 1) {
        uint32_t set, n, lo, hi, rec = 2 + s1 + s2;
        if ((uint32_t)k >= r16(t, st + 8))
            return 0;
        set = st + r16(t, st + 10 + 2 * k);
        n = r16(t, set);
        lo = 0;
        hi = n;
        while (lo < hi) {
            uint32_t mid = (lo + hi) / 2, r = set + 2 + rec * mid, g = r16(t, r);
            if (g < g2) {
                lo = mid + 1;
            } else if (g > g2) {
                hi = mid;
            } else {
                if (apply_value(c, set, r + 2, vf1, i) | apply_value(c, set, r + 2 + s1, vf2, (uint32_t)j))
                    of_layout_unsafe(c->b, (long)i, j);
                *next = s2 ? (uint32_t)j + 1 : (uint32_t)j;
                return 1;
            }
        }
        return 0;
    }
    if (fmt == 2) {
        uint32_t c1 = class_of(t, st + r16(t, st + 8), c->b->g[i].gid);
        uint32_t c2 = class_of(t, st + r16(t, st + 10), g2);
        uint32_t n1 = r16(t, st + 12), n2 = r16(t, st + 14), r;
        if (c1 >= n1 || c2 >= n2)
            return 0;
        r = st + 16 + (c1 * n2 + c2) * (s1 + s2);
        if (apply_value(c, st, r, vf1, i) | apply_value(c, st, r + s1, vf2, (uint32_t)j))
            of_layout_unsafe(c->b, (long)i, j);
        *next = s2 ? (uint32_t)j + 1 : (uint32_t)j;
        return 1;
    }
    return 0;
}

static int gpos_cursive(Ctx *c, uint32_t st, uint32_t i)
{
    const Tab *t = &c->t;
    uint32_t cov = st + r16(t, st + 2), n = r16(t, st + 4), exit_off, entry_off;
    long k = coverage(t, cov, c->b->g[i].gid), j, kj;
    int64_t ex, ey, nx, ny;
    if (r16(t, st) != 1 || k < 0 || (uint32_t)k >= n)
        return 0;
    exit_off = r16(t, st + 6 + 4 * k + 2);
    if (!exit_off)
        return 0;
    j = next_glyph(c, (long)i);
    if (j < 0)
        return 0;
    kj = coverage(t, cov, c->b->g[j].gid);
    if (kj < 0 || (uint32_t)kj >= n)
        return 0;
    entry_off = r16(t, st + 6 + 4 * kj);
    if (!entry_off)
        return 0;
    anchor(c, st + exit_off, &ex, &ey);
    anchor(c, st + entry_off, &nx, &ny);
    if (!c->rtl) {
        /* left to right: the first glyph's advance ends at its exit, the
         * next begins at its entry */
        c->b->g[i].xa = round_fine(ex) + c->b->g[i].xo;
        c->b->g[j].xa -= round_fine(nx) + c->b->g[j].xo;
        c->b->g[j].xo = 0;
    } else {
        /* right to left (logical order): the first glyph's advance begins
         * at its exit, the next ends at its entry */
        int32_t d = round_fine(ex) + c->b->g[i].xo;
        c->b->g[i].xa -= d;
        c->b->g[i].xo -= d;
        c->b->g[j].xa = round_fine(nx) + c->b->g[j].xo;
    }
    if (c->flag & LF_RIGHT_TO_LEFT) {
        /* the first hangs from the next vertically */
        c->b->g[i].yo = round_fine(ny - ey);
        c->b->g[i].attach_kind = OF_ATTACH_CURSIVE;
        c->b->g[i].attach_to = (int32_t)j;
    } else {
        /* the next hangs from the first vertically */
        c->b->g[j].yo = round_fine(ey - ny);
        c->b->g[j].attach_kind = OF_ATTACH_CURSIVE;
        c->b->g[j].attach_to = (int32_t)i;
    }
    of_layout_unsafe(c->b, (long)i, j);
    return 1;
}

/* the mark at i, its class and its anchor, from a MarkArray */
static int mark_record(Ctx *c, uint32_t marks, uint32_t mk, uint32_t *cls, int64_t *x, int64_t *y)
{
    const Tab *t = &c->t;
    if (mk >= r16(t, marks))
        return 0;
    *cls = r16(t, marks + 2 + 4 * mk);
    anchor(c, marks + r16(t, marks + 2 + 4 * mk + 2), x, y);
    return 1;
}

static void attach_mark(Ctx *c, uint32_t i, long base, int64_t bx, int64_t by, int64_t mx, int64_t my)
{
    OFShGlyph *m = &c->b->g[i];
    m->xo = round_fine(bx - mx);
    m->yo = round_fine(by - my);
    m->attach_kind = OF_ATTACH_MARK;
    m->attach_to = (int32_t)base;
    of_layout_unsafe(c->b, base, (long)i);
}

static int gpos_mark_base(Ctx *c, uint32_t st, uint32_t i)
{
    const Tab *t = &c->t;
    long mk = coverage(t, st + r16(t, st + 2), c->b->g[i].gid), j, bk;
    uint32_t ncls = r16(t, st + 6), marks = st + r16(t, st + 8), bases = st + r16(t, st + 10), cls, off;
    int64_t mx, my, bx, by;
    if (mk < 0)
        return 0;
    /* the base: back over marks */
    for (j = (long)i - 1; j >= 0; --j) {
        const OFShGlyph *x = &c->b->g[j];
        if (x->gclass == OF_CLASS_MARK || (x->props & OF_SH_IGNORABLE) || skipped_by_flag(c, x))
            continue;
        break;
    }
    if (j < 0)
        return 0;
    bk = coverage(t, st + r16(t, st + 4), c->b->g[j].gid);
    if (bk < 0 || (uint32_t)bk >= r16(t, bases) || !mark_record(c, marks, (uint32_t)mk, &cls, &mx, &my) || cls >= ncls)
        return 0;
    off = r16(t, bases + 2 + 2 * ((uint32_t)bk * ncls + cls));
    if (!off)
        return 0;
    anchor(c, bases + off, &bx, &by);
    attach_mark(c, i, j, bx, by, mx, my);
    return 1;
}

static int gpos_mark_lig(Ctx *c, uint32_t st, uint32_t i)
{
    const Tab *t = &c->t;
    long mk = coverage(t, st + r16(t, st + 2), c->b->g[i].gid), j, lk;
    uint32_t ncls = r16(t, st + 6), marks = st + r16(t, st + 8), ligs = st + r16(t, st + 10);
    uint32_t cls, att, ncomp, comp, off;
    int64_t mx, my, bx, by;
    if (mk < 0)
        return 0;
    for (j = (long)i - 1; j >= 0; --j) {
        const OFShGlyph *x = &c->b->g[j];
        if (x->gclass == OF_CLASS_MARK || (x->props & OF_SH_IGNORABLE) || skipped_by_flag(c, x))
            continue;
        break;
    }
    if (j < 0)
        return 0;
    lk = coverage(t, st + r16(t, st + 4), c->b->g[j].gid);
    if (lk < 0 || (uint32_t)lk >= r16(t, ligs) || !mark_record(c, marks, (uint32_t)mk, &cls, &mx, &my) || cls >= ncls)
        return 0;
    att = ligs + r16(t, ligs + 2 + 2 * lk);
    ncomp = r16(t, att);
    if (!ncomp)
        return 0;
    /* the component the mark followed, else the last */
    if (c->b->g[j].lig_id && c->b->g[i].lig_id == c->b->g[j].lig_id && c->b->g[i].lig_comp)
        comp = c->b->g[i].lig_comp < ncomp ? c->b->g[i].lig_comp - 1u : ncomp - 1;
    else
        comp = ncomp - 1;
    off = r16(t, att + 2 + 2 * (comp * ncls + cls));
    if (!off)
        return 0;
    anchor(c, att + off, &bx, &by);
    attach_mark(c, i, j, bx, by, mx, my);
    return 1;
}

static int gpos_mark_mark(Ctx *c, uint32_t st, uint32_t i)
{
    const Tab *t = &c->t;
    long mk = coverage(t, st + r16(t, st + 2), c->b->g[i].gid), j, k2;
    uint32_t ncls = r16(t, st + 6), m1 = st + r16(t, st + 8), m2 = st + r16(t, st + 10), cls, off;
    int64_t mx, my, bx, by;
    const OFShGlyph *a, *b;
    if (mk < 0)
        return 0;
    j = prev_glyph(c, (long)i);
    if (j < 0 || c->b->g[j].gclass != OF_CLASS_MARK)
        return 0;
    a = &c->b->g[i];
    b = &c->b->g[j];
    /* on the same base, or the same component of the same ligature */
    if (a->lig_id != b->lig_id || (a->lig_id && a->lig_comp != b->lig_comp))
        return 0;
    k2 = coverage(t, st + r16(t, st + 4), b->gid);
    if (k2 < 0 || (uint32_t)k2 >= r16(t, m2) || !mark_record(c, m1, (uint32_t)mk, &cls, &mx, &my) || cls >= ncls)
        return 0;
    off = r16(t, m2 + 2 + 2 * ((uint32_t)k2 * ncls + cls));
    if (!off)
        return 0;
    anchor(c, m2 + off, &bx, &by);
    attach_mark(c, i, j, bx, by, mx, my);
    return 1;
}

/* ---- one lookup at one glyph ---- */

static int apply_subtable(Ctx *c, uint32_t type, uint32_t st, uint32_t i, uint32_t *next)
{
    *next = i + 1;
    if (!c->gpos) {
        switch (type) {
        case 1: return gsub_single(c, st, i);
        case 2: return gsub_multiple(c, st, i, next);
        case 3: return gsub_alternate(c, st, i);
        case 4: return gsub_ligature(c, st, i, next);
        case 5: return context(c, st, i, next);
        case 6: return chain_context(c, st, i, next);
        case 8: return gsub_reverse(c, st, i);
        default: return 0;
        }
    }
    switch (type) {
    case 1: return gpos_single(c, st, i);
    case 2: return gpos_pair(c, st, i, next);
    case 3: return gpos_cursive(c, st, i);
    case 4: return gpos_mark_base(c, st, i);
    case 5: return gpos_mark_lig(c, st, i);
    case 6: return gpos_mark_mark(c, st, i);
    case 7: return context(c, st, i, next);
    case 8: return chain_context(c, st, i, next);
    default: return 0;
    }
}

/* the lookup's type and subtables, extensions followed */
static int lookup_at(const Ctx *c, uint32_t index, uint32_t *lk)
{
    uint32_t ll = r16(&c->t, 8);
    if (!ll || index >= r16(&c->t, ll))
        return 0;
    *lk = ll + r16(&c->t, ll + 2 + 2 * index);
    return 1;
}

static int apply_lookup(Ctx *c, uint32_t index, uint32_t i, uint32_t *next)
{
    const Tab *t = &c->t;
    uint32_t lk, type, n, s, ext = c->gpos ? 9 : 7;
    if (!lookup_at(c, index, &lk))
        return 0;
    type = r16(t, lk);
    c->flag = r16(t, lk + 2);
    n = r16(t, lk + 4);
    c->mark_set = (c->flag & LF_USE_MARK_SET) ? r16(t, lk + 6 + 2 * n) : 0;
    if (i >= c->b->n || skipped_by_flag(c, &c->b->g[i]))
        return 0;
    for (s = 0; s < n; ++s) {
        uint32_t st = lk + r16(t, lk + 6 + 2 * s), ty = type;
        if (ty == ext) {
            if (r16(t, st) != 1)
                continue;
            ty = r16(t, st + 2);
            st += r32(t, st + 4);
            if (ty == ext)
                continue;
        }
        if (apply_subtable(c, ty, st, i, next))
            return 1;
    }
    return 0;
}

/* a whole lookup over the run */
static void run_lookup(Ctx *c, uint32_t index, uint32_t mask, uint32_t value)
{
    uint32_t lk, type, i;
    if (!lookup_at(c, index, &lk))
        return;
    type = r16(&c->t, lk);
    if (type == (c->gpos ? 9u : 7u) && r16(&c->t, lk + 4))
        type = r16(&c->t, lk + r16(&c->t, lk + 6) + 2);
    c->mask = mask;
    c->value = value;
    if (!c->gpos && type == 8) {
        /* reverse chaining: from the end */
        for (i = c->b->n; i-- > 0;) {
            uint32_t next;
            if (c->b->g[i].mask & mask)
                apply_lookup(c, index, i, &next);
        }
        return;
    }
    for (i = 0; i < c->b->n && !c->b->failed;) {
        uint32_t next = i + 1;
        c->depth = 0;
        if ((c->b->g[i].mask & mask) && apply_lookup(c, index, i, &next))
            i = next > i ? next : i + 1;
        else
            i++;
    }
}

int of_layout_substitute(const OFFont *f, OFShBuf *b, const OFLayoutSpec *spec)
{
    Ctx c;
    Plan *p;
    Want w[MAX_WANTS];
    size_t nw;
    uint32_t k;
    if (!f->gsub.len)
        return OF_OK;
    p = (Plan *)malloc(sizeof(*p));
    if (!p)
        return OF_ERR_MEMORY;
    memset(&c, 0, sizeof c);
    c.f = f;
    c.t = table_of(f, &f->gsub);
    gdef_open(f, &c.gdef);
    c.b = b;
    c.rtl = spec->rtl;
    nw = make_wants(spec, 0, w);
    make_plan(&c.t, spec->script_tag, spec->lang_tag, w, nw, p, 0);
    for (k = 0; k < p->n; ++k)
        run_lookup(&c, p->l[k].index, p->l[k].mask, p->l[k].value);
    free(p);
    return b->failed ? OF_ERR_MEMORY : OF_OK;
}

static int has_feature(const OFFont *f, const OFTable *table, int gpos,
                       const OFLayoutSpec *spec, uint32_t feature)
{
    Tab t;
    Plan *p;
    Want w[MAX_WANTS];
    size_t nw;
    int has;
    if (!table->len)
        return 0;
    nw = make_wants(spec, gpos, w);
    p = (Plan *)malloc(sizeof(*p));
    if (!p)
        return 0;
    t = table_of(f, table);
    make_plan(&t, spec->script_tag, spec->lang_tag, w, nw, p, feature);
    has = p->n > 0;
    free(p);
    return has;
}

int of_layout_has_gpos_feature(const OFFont *f, const OFLayoutSpec *spec, uint32_t feature)
{
    return has_feature(f, &f->gpos, 1, spec, feature);
}

int of_layout_has_gsub_feature(const OFFont *f, const OFLayoutSpec *spec, uint32_t feature)
{
    return has_feature(f, &f->gsub, 0, spec, feature);
}

/* ---- 'kern' ---- */

static void kern_table(const OFFont *f, OFShBuf *b, int32_t scale)
{
    Tab t = table_of(f, &f->kern);
    uint32_t version = r16(&t, 0), ntab, at, k;
    int apple = 0;
    Ctx c;
    memset(&c, 0, sizeof c);
    c.scale = scale;
    if (version == 0) {
        ntab = r16(&t, 2);
        at = 4;
    } else if (r32(&t, 0) == 0x00010000u) {
        apple = 1;
        ntab = r32(&t, 4);
        at = 8;
    } else {
        return;
    }
    for (k = 0; k < ntab && k < 64; ++k) {
        uint32_t len, cov, fmt, horizontal, cross, sub = at, pairs, np, i;
        if (apple) {
            len = r32(&t, at);
            cov = r16(&t, at + 4);
            fmt = cov & 0xFF;
            horizontal = !(cov & 0x8000);
            cross = cov & 0x4000;
            pairs = sub + 8;
        } else {
            len = r16(&t, at + 2);
            cov = r16(&t, at + 4);
            fmt = cov >> 8;
            horizontal = cov & 1;
            cross = cov & 4;
            pairs = sub + 6;
            /* a format 0 subtable's length may overflow 16 bits: trust its
             * pair count */
            if (fmt == 0 && k == ntab - 1)
                len = t.len - at;
        }
        if (len < 6 || at + len > t.len)
            break;
        at += len;
        if (fmt != 0 || !horizontal || cross || (!apple && (cov & 2)))
            continue;
        np = r16(&t, pairs);
        for (i = 0; i + 1 < b->n; ++i) {
            uint32_t j = i + 1, key, lo = 0, hi = np;
            if (b->g[i].gclass == OF_CLASS_MARK || (b->g[i].props & OF_SH_IGNORABLE))
                continue;
            while (j < b->n && (b->g[j].gclass == OF_CLASS_MARK || (b->g[j].props & OF_SH_IGNORABLE)))
                j++;
            if (j >= b->n)
                break;
            key = (b->g[i].gid << 16) | b->g[j].gid;
            while (lo < hi) {
                uint32_t mid = (lo + hi) / 2, r = pairs + 8 + 6 * mid;
                uint32_t kk = (r16(&t, r) << 16) | r16(&t, r + 2);
                if (kk < key) {
                    lo = mid + 1;
                } else if (kk > key) {
                    hi = mid;
                } else {
                    b->g[i].xa += units(&c, rs16(&t, r + 4));
                    of_layout_unsafe(b, (long)i, (long)j);
                    break;
                }
            }
        }
    }
}

/* whether the program turned the feature off over the whole text */
static int turned_off(const OFLayoutSpec *spec, uint32_t tag)
{
    uint32_t k;
    int off = 0;
    for (k = 0; k < spec->nfeatures && k < OF_MAX_FEATURES; ++k)
        if (spec->features[k].tag == tag && !spec->ranged_mask[k])
            off = spec->features[k].value == 0;
    return off;
}

int of_layout_position(const OFFont *f, OFShBuf *b, const OFLayoutSpec *spec, uint32_t ppem)
{
    Ctx c;
    Plan *p;
    Want w[MAX_WANTS];
    size_t nw;
    uint32_t k, i;
    int has_kern = 0;
    memset(&c, 0, sizeof c);
    c.f = f;
    c.b = b;
    c.ppem = ppem;
    c.scale = of_scale_16_16(f->units_per_em, ppem);
    c.gpos = 1;
    c.rtl = spec->rtl;
    gdef_open(f, &c.gdef);
    if (f->gpos.len) {
        p = (Plan *)malloc(sizeof(*p));
        if (!p)
            return OF_ERR_MEMORY;
        c.t = table_of(f, &f->gpos);
        nw = make_wants(spec, 1, w);
        make_plan(&c.t, spec->script_tag, spec->lang_tag, w, nw, p, 0);
        for (k = 0; k < p->n; ++k)
            run_lookup(&c, p->l[k].index, p->l[k].mask, p->l[k].value);
        free(p);
        has_kern = of_layout_has_gpos_feature(f, spec, OF_TAG('k', 'e', 'r', 'n'));
    }
    if (!has_kern && f->kern.len && !turned_off(spec, OF_TAG('k', 'e', 'r', 'n')))
        kern_table(f, b, c.scale);
    /* marks take no room of their own */
    for (i = 0; i < b->n; ++i)
        if (b->g[i].gclass == OF_CLASS_MARK) {
            b->g[i].xa = 0;
            b->g[i].ya = 0;
        }
    /* a mark sits where its base's anchor is: left to right, back over
     * the advances between them; right to left (the run still in logical
     * order, to be reversed), forward over the advances after its base up
     * to itself */
    for (i = 0; i < b->n; ++i) {
        OFShGlyph *x = &b->g[i];
        if (x->attach_kind == OF_ATTACH_MARK && x->attach_to >= 0 && (uint32_t)x->attach_to < i) {
            uint32_t j = (uint32_t)x->attach_to, m;
            x->xo += b->g[j].xo;
            x->yo += b->g[j].yo;
            if (!spec->rtl)
                for (m = j; m < i; ++m)
                    x->xo -= b->g[m].xa;
            else
                for (m = j + 1; m <= i; ++m)
                    x->xo += b->g[m].xa;
        } else if (x->attach_kind == OF_ATTACH_CURSIVE && x->attach_to >= 0 && (uint32_t)x->attach_to < i) {
            x->yo += b->g[x->attach_to].yo;
        }
    }
    /* a cursive chain hung the other way (the lookup's right-to-left
     * flag): each from the one after it, from the end */
    for (i = b->n; i-- > 0;) {
        OFShGlyph *x = &b->g[i];
        if (x->attach_kind == OF_ATTACH_CURSIVE && x->attach_to > (int32_t)i && (uint32_t)x->attach_to < b->n)
            x->yo += b->g[x->attach_to].yo;
    }
    return b->failed ? OF_ERR_MEMORY : OF_OK;
}
