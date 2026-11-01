/*
 * Glyph runs drawn through GFOpenGPU, with a glyph cache (of_gpu.h).
 *
 * The cache: atlas pages of A8 coverage (OFG_PAGE square), filled shelf by
 * shelf; a glyph's cell is found by (the face's source fingerprint, pixel
 * size, glyph, quarter-pixel x phase, anti-aliasing) in an open-addressed
 * table. A glyph not there is filled into a free cell by PATH_FILL (its
 * outline at the pixel size, y flipped, moved by its phase) in the same
 * batch as the run, before its first use. When the pages or the table are
 * full the cache is emptied (after the batch is drawn) and filled again.
 *
 * A run with no clip mask: one COMPOSITE_PD a glyph, the colour through its
 * cell (OGPU_PD_SOLID with the page as mask), inside the clip rectangle.
 * With a clip mask (COMPOSITE_PD takes one mask): the glyphs' coverage is
 * added up in an A8 layer first (ADD, as pixman_composite_glyphs does),
 * multiplied by the clip mask (IN_REVERSE), then the colour composited
 * through the layer. An operator acts on each glyph's cell (or the layer's
 * box), so operators that change pixels a source of 0 doesn't (SRC, IN,
 * CLEAR...) only reach that far: text is drawn with OVER.
 * A glyph x position goes to the nearest quarter pixel, y to the nearest
 * pixel. Under a matrix, or with OF_GPU_NOCACHE, each glyph's outline is
 * filled straight onto the target, under the clip mask if there is one.
 * MIT, Copyright (c) 2026 AmiCradle Team.
 * SPDX-License-Identifier: MIT
 */
#include "of_gpu.h"

#include <stdlib.h>
#include <string.h>

#include <opengpu/stream.h>

/* stream v1.8's flag (gfopengpu.library 0.23), for headers from before it */
#ifndef OGPU_PATH_ALIASED
#define OGPU_PATH_ALIASED 0x4000
#endif

#define OFG_PAGE        256         /* an atlas page's side */
#define OFG_PAGES       8           /* 512 KB of coverage at most */
#define OFG_TABLE       4096        /* cells, a power of two */
#define OFG_STREAM      4096        /* stream words a batch */
#define OFG_SCRATCH     (64 * 1024) /* path words a batch */
#define OFG_WORK_ROWS   16

/* slots: 0 the target, 1 the clip mask, 2 the layer, 3.. the pages */
#define SLOT_TARGET     0
#define SLOT_MASK       1
#define SLOT_LAYER      2
#define SLOT_PAGE0      3

typedef struct Cell {
    uint64_t fp;
    uint32_t size, glyph;
    uint8_t phase, aliased, used, page;
    int16_t x, y, w, h;             /* in its page */
    int16_t left, top;              /* from the pen (rounded, phase applied) to the cell's corner */
} Cell;

typedef struct Page {
    uint8_t *mem;
    uint32_t addr;
    int shelf_x, shelf_y, shelf_h;
} Page;

struct OFGpu {
    of_gpu_alloc_fn alloc;
    of_gpu_free_fn release;
    of_gpu_submit_fn submit;
    void *user;
    int aliased_ok;
    /* the batch */
    uint8_t *stream;
    long words;
    int drawn;
    uint32_t *scratch;              /* path words, at scratch_addr */
    uint32_t scratch_addr;
    long scratch_used;
    uint8_t *work;
    uint32_t work_addr;
    uint32_t work_bytes;
    uint32_t work_width;            /* the widest target the work memory takes */
    int clip_sent;
    int cur;                        /* the slot drawn into, -1 none yet */
    uint32_t bound;                 /* pages (bit n: page n) and the layer (bit 31) bound in the batch */
    /* the cache */
    Page page[OFG_PAGES];
    int pages;
    Cell cell[OFG_TABLE];
    int cells;
    /* outline buffers */
    uint8_t *verbs;
    int32_t *points;
    size_t verb_cap, point_cap;
    OFGpuStats stats;
    long gpu_error;                 /* the last batch's, from submit */
};

/* ---- the stream ------------------------------------------------------------------------ */

static void put(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static uint32_t sw(uint32_t v)            /* a scratch word in the Amiga's order */
{
    uint32_t r;
    put((uint8_t *)&r, v);
    return r;
}

static void emit(OFGpu *g, unsigned op, int n, const uint32_t *a)
{
    uint8_t *p = g->stream + g->words * 4;
    int i;
    put(p, OGPU_HDR(op, n + 1));
    for (i = 0; i < n; i++)
        put(p + 4 + i * 4, a[i]);
    g->words += 1 + n;
}

static uint32_t xy(long x, long y) { return OGPU_XY((unsigned long)x, (unsigned long)y); }

static void surface(OFGpu *g, int slot, uint32_t addr, long bpr, int w, int h, uint32_t fmt)
{
    uint32_t a[5];
    a[0] = (uint32_t)slot; a[1] = addr; a[2] = (uint32_t)bpr; a[3] = xy(w, h); a[4] = fmt;
    emit(g, OGPU_OP_SURFACE, 5, a);
}

static void target(OFGpu *g, int slot)
{
    uint32_t a[1];
    a[0] = (uint32_t)slot;
    emit(g, OGPU_OP_TARGET, 1, a);
    g->clip_sent = 0;
    g->cur = slot;
}

static void clip(OFGpu *g, long x, long y, long w, long h)
{
    uint32_t a[2];
    a[0] = xy(x, y);
    a[1] = xy(w, h);
    emit(g, OGPU_OP_CLIP, 2, a);
}

static void cache_empty(OFGpu *g);

/* The batch drawn; 0, or OF_ERR_GPU with GFOpenGPU's error kept. */
static long flush(OFGpu *g)
{
    long r = 0;
    if (g->drawn && g->submit) {
        g->gpu_error = g->submit(g->user, g->stream, g->words);
        r = g->gpu_error ? OF_ERR_GPU : 0;
    }
    g->words = 0;
    g->drawn = 0;
    g->scratch_used = 0;
    g->cur = -1;
    g->bound = 0;
    if (r)                          /* the batch's glyph fills may not have run */
        cache_empty(g);
    return r;
}

static void bind_page(OFGpu *g, int pg)
{
    if (!(g->bound & (1u << pg))) {
        surface(g, SLOT_PAGE0 + pg, g->page[pg].addr, OFG_PAGE, OFG_PAGE, OFG_PAGE, OGPU_FMT_A8);
        g->bound |= 1u << pg;
    }
}

static void bind_layer(OFGpu *g, uint32_t addr, long bpr, long w, long h)
{
    if (!(g->bound & 0x80000000u)) {
        surface(g, SLOT_LAYER, addr, bpr, (int)w, (int)h, OGPU_FMT_A8);
        g->bound |= 0x80000000u;
    }
}

/* Draw into slot, inside x, y, w, h: TARGET and CLIP only when they change. */
static void into(OFGpu *g, int slot, long x, long y, long w, long h)
{
    if (g->cur != slot || !g->clip_sent) {
        target(g, slot);
        clip(g, x, y, w, h);
        g->clip_sent = 1;
    }
}

/* ---- the cache ------------------------------------------------------------------------- */

static uint32_t hash(uint64_t fp, uint32_t size, uint32_t glyph, int phase, int aliased)
{
    uint64_t h = fp ^ ((uint64_t)size << 40) ^ ((uint64_t)glyph * 0x9E3779B97F4A7C15ULL) ^ ((uint64_t)phase << 20)
        ^ ((uint64_t)aliased << 23);
    h ^= h >> 29;
    h *= 0xBF58476D1CE4E5B9ULL;
    h ^= h >> 32;
    return (uint32_t)h & (OFG_TABLE - 1);
}

static void cache_empty(OFGpu *g)
{
    int i;
    for (i = 0; i < g->pages; i++) {
        memset(g->page[i].mem, 0, OFG_PAGE * OFG_PAGE);
        g->page[i].shelf_x = g->page[i].shelf_y = g->page[i].shelf_h = 0;
    }
    memset(g->cell, 0, sizeof g->cell);
    g->cells = 0;
    g->stats.resets++;
}

/* Room for a w x h cell (with a pixel between cells): its page and corner. */
static int place(OFGpu *g, int w, int h, int *pg, int *x, int *y)
{
    int i;
    if (w + 1 > OFG_PAGE || h + 1 > OFG_PAGE)
        return 0;
    for (i = 0; i <= g->pages && i < OFG_PAGES; i++) {
        Page *p = &g->page[i];
        if (i == g->pages) {
            uint32_t addr = 0;
            void *m = g->alloc(g->user, OFG_PAGE * OFG_PAGE, &addr);
            if (!m)
                return 0;
            p->mem = (uint8_t *)m;
            p->addr = addr;
            memset(p->mem, 0, OFG_PAGE * OFG_PAGE);
            p->shelf_x = p->shelf_y = p->shelf_h = 0;
            g->pages++;
        }
        if (p->shelf_x + w + 1 > OFG_PAGE) {    /* the next shelf */
            p->shelf_y += p->shelf_h;
            p->shelf_x = 0;
            p->shelf_h = 0;
        }
        if (p->shelf_y + h + 1 > OFG_PAGE)
            continue;
        *pg = i;
        *x = p->shelf_x;
        *y = p->shelf_y;
        p->shelf_x += w + 1;
        if (h + 1 > p->shelf_h)
            p->shelf_h = h + 1;
        return 1;
    }
    return 0;
}

/* The glyph's outline at the face's pixel size, 26.6, y up; 0 when it has none. */
static int outline(OFGpu *g, OFFace *face, uint32_t glyph, size_t *nv, size_t *np)
{
    int r = of_face_glyph_outline(face, glyph, g->verbs, g->verb_cap, g->points, g->point_cap, nv, np);
    if (r == OF_ERR_BUFFER) {
        size_t vc = *nv + 16, pc = *np + 16;      /* verbs, points (x, y pairs) */
        uint8_t *v = (uint8_t *)realloc(g->verbs, vc);
        int32_t *p;
        if (!v)
            return OF_ERR_MEMORY;
        g->verbs = v;
        g->verb_cap = vc;
        p = (int32_t *)realloc(g->points, pc * 2 * sizeof(int32_t));
        if (!p)
            return OF_ERR_MEMORY;
        g->points = p;
        g->point_cap = pc;
        r = of_face_glyph_outline(face, glyph, g->verbs, g->verb_cap, g->points, g->point_cap, nv, np);
    }
    return r;
}

/* The outline's path words in scratch: each point (x, y) at 26.6, y up, as
 * x * mx + y * my... through m (16.16 2 x 2 on 24.8: x' = m0 x + m1 y, y' =
 * m2 x + m3 y with y already down) and moved by (ox, oy) (24.8). 0 when
 * scratch is full. */
static int path_words(OFGpu *g, size_t nv, const int32_t *m, long ox, long oy, uint32_t *addr, long *words)
{
    size_t i, k = 0;
    long start = g->scratch_used;
    for (i = 0; i < nv; i++) {
        int v = g->verbs[i], np = v == OF_PATH_QUAD ? 2 : v == OF_PATH_CUBIC ? 3 : v == OF_PATH_CLOSE ? 0 : 1, j;
        if (g->scratch_used + 1 + 2 * np > OFG_SCRATCH / 4) {
            g->scratch_used = start;
            return 0;
        }
        g->scratch[g->scratch_used++] = sw((uint32_t)(v == OF_PATH_MOVE ? OGPU_PATH_MOVE : v == OF_PATH_LINE ? OGPU_PATH_LINE
            : v == OF_PATH_QUAD ? OGPU_PATH_QUAD : v == OF_PATH_CUBIC ? OGPU_PATH_CUBIC : OGPU_PATH_CLOSE));
        for (j = 0; j < np; j++, k += 2) {
            int64_t x = (int64_t)g->points[k] * 4, y = -(int64_t)g->points[k + 1] * 4, X, Y;
            if (m) {
                X = (m[0] * x + m[1] * y) / 65536;
                Y = (m[2] * x + m[3] * y) / 65536;
            } else {
                X = x;
                Y = y;
            }
            g->scratch[g->scratch_used++] = sw((uint32_t)(int32_t)(X + ox));
            g->scratch[g->scratch_used++] = sw((uint32_t)(int32_t)(Y + oy));
        }
    }
    *addr = g->scratch_addr + (uint32_t)start * 4;
    *words = g->scratch_used - start;
    return 1;
}

/* The 24.8 box of the outline's points (26.6, y up) moved by dx (24.8). */
static void outline_box(OFGpu *g, size_t np, long dx, long *b)
{
    size_t i;
    b[0] = b[1] = 0x7FFFFFFFL;
    b[2] = b[3] = -0x7FFFFFFFL;
    for (i = 0; i < np; i++) {
        long x = (long)g->points[2 * i] * 4 + dx, y = -(long)g->points[2 * i + 1] * 4;
        if (x < b[0]) b[0] = x;
        if (x > b[2]) b[2] = x;
        if (y < b[1]) b[1] = y;
        if (y > b[3]) b[3] = y;
    }
}

static long fdiv256(long v) { return v >= 0 ? v / 256 : -((-v + 255) / 256); }

/* PATH_FILL with the given words, matrix identity. */
static void path_fill(OFGpu *g, uint32_t flags, uint32_t paint, int mslot, long mx, long my, uint32_t addr, long words)
{
    uint32_t a[15];
    a[0] = flags; a[1] = paint; a[2] = 0; a[3] = (uint32_t)mslot; a[4] = xy(mx, my);
    a[5] = addr; a[6] = (uint32_t)words;
    a[7] = 0x10000; a[8] = 0; a[9] = 0; a[10] = 0; a[11] = 0x10000; a[12] = 0;
    a[13] = g->work_addr; a[14] = g->work_bytes;
    emit(g, OGPU_OP_PATH_FILL, 15, a);
    g->drawn++;
}

/* The glyph's cell, filled now if it is new; 0 for none (no outline, or no room). */
static const Cell *cell_of(OFGpu *g, OFFace *face, uint64_t fp, uint32_t size, uint32_t glyph, int phase, int aliased,
    int *empty)
{
    uint32_t h = hash(fp, size, glyph, phase, aliased), addr;
    size_t nv = 0, np = 0;
    long b[4], words;
    int i, pg, x, y, w, ht;
    Cell *c;
    *empty = 0;
    for (i = 0; i < OFG_TABLE; i++) {
        c = &g->cell[(h + (uint32_t)i) & (OFG_TABLE - 1)];
        if (!c->used)
            break;
        if (c->fp == fp && c->size == size && c->glyph == glyph && c->phase == phase && c->aliased == aliased) {
            g->stats.hits++;
            if (!c->w) *empty = 1;
            return c->w ? c : 0;
        }
    }
    if (g->cells * 4 >= OFG_TABLE * 3 || i == OFG_TABLE)
        return 0;                       /* the caller empties the cache and asks again */
    if (outline(g, face, glyph, &nv, &np) != OF_OK)
        return 0;
    c->used = 1;
    c->fp = fp; c->size = size; c->glyph = glyph; c->phase = (uint8_t)phase; c->aliased = (uint8_t)aliased;
    c->w = c->h = 0;
    g->cells++;
    if (!nv || !np) {
        *empty = 1;
        return 0;
    }
    outline_box(g, np, (long)phase * 64, b);
    b[0] = fdiv256(b[0]);
    b[1] = fdiv256(b[1]);
    b[2] = fdiv256(b[2] + 255);
    b[3] = fdiv256(b[3] + 255);
    w = (int)(b[2] - b[0]);
    ht = (int)(b[3] - b[1]);
    if (w <= 0 || ht <= 0) {
        *empty = 1;
        return 0;
    }
    if (!place(g, w, ht, &pg, &x, &y)) {
        c->used = 0;
        g->cells--;
        return 0;
    }
    if (!path_words(g, nv, 0, ((long)x - b[0]) * 256 + phase * 64, ((long)y - b[1]) * 256, &addr, &words)) {
        c->used = 0;
        g->cells--;
        return 0;
    }
    c->page = (uint8_t)pg;
    c->x = (int16_t)x; c->y = (int16_t)y; c->w = (int16_t)w; c->h = (int16_t)ht;
    c->left = (int16_t)b[0];
    c->top = (int16_t)b[1];
    /* filled into the page, inside its cell */
    bind_page(g, pg);
    target(g, SLOT_PAGE0 + pg);
    clip(g, x, y, w, ht);
    path_fill(g, OGPU_PD_OVER | (aliased ? OGPU_PATH_ALIASED : 0), 0xFF000000UL, 0, 0, 0, addr, words);
    g->stats.rasterised++;
    return c;
}

/* ---- drawing --------------------------------------------------------------------------- */

OFGpu *of_gpu_create(of_gpu_alloc_fn alloc, of_gpu_free_fn release, of_gpu_submit_fn submit, void *user, int aliased_ok)
{
    OFGpu *g = (OFGpu *)calloc(1, sizeof *g);
    uint32_t a;
    if (!g)
        return NULL;
    g->alloc = alloc;
    g->release = release;
    g->submit = submit;
    g->user = user;
    g->aliased_ok = aliased_ok;
    g->cur = -1;
    g->stream = (uint8_t *)malloc(OFG_STREAM * 4);
    g->scratch = (uint32_t *)alloc(user, OFG_SCRATCH, &a);
    g->scratch_addr = a;
    if (!g->stream || !g->scratch) {
        of_gpu_destroy(g);
        return NULL;
    }
    return g;
}

void of_gpu_destroy(OFGpu *g)
{
    int i;
    if (!g)
        return;
    flush(g);
    for (i = 0; i < g->pages; i++)
        g->release(g->user, g->page[i].mem);
    if (g->scratch)
        g->release(g->user, g->scratch);
    if (g->work)
        g->release(g->user, g->work);
    free(g->stream);
    free(g->verbs);
    free(g->points);
    free(g);
}

void of_gpu_stats(const OFGpu *g, OFGpuStats *s)
{
    *s = g->stats;
}

long of_gpu_error(const OFGpu *g)
{
    return g->gpu_error;
}

/* Work memory for PATH_FILL on a target width wide. */
static int ensure_work(OFGpu *g, uint32_t width)
{
    uint32_t a, bytes;
    if (width < OFG_PAGE)
        width = OFG_PAGE;
    if (g->work && g->work_width >= width)
        return 1;
    if (g->work) {
        flush(g);
        g->release(g->user, g->work);
        g->work = NULL;
    }
    bytes = 32 * (width + 4) * OFG_WORK_ROWS;
    g->work = (uint8_t *)g->alloc(g->user, bytes, &a);
    if (!g->work)
        return 0;
    g->work_addr = a;
    g->work_bytes = bytes;
    g->work_width = width;
    return 1;
}

/* The batch's head: the target and the clip mask's slot, the target's clip. */
static void head(OFGpu *g, const OFGpuTarget *t)
{
    if (g->words)
        return;
    surface(g, SLOT_TARGET, t->address, t->bpr, t->width, t->height, t->format);
    if (t->mask_address)
        surface(g, SLOT_MASK, t->mask_address, t->mask_bpr, t->mask_w, t->mask_h, OGPU_FMT_A8);
    target(g, SLOT_TARGET);
}

/* Room for a glyph's commands (a page fill and a draw: about 60 words). */
static long room(OFGpu *g, const OFGpuTarget *t)
{
    long r = 0;
    if (g->words + 64 > OFG_STREAM || g->scratch_used + 2048 > OFG_SCRATCH / 4)
        r = flush(g);
    head(g, t);
    return r;
}

static int clip_box(const OFGpuTarget *t, long *c)
{
    c[0] = t->clip_x < 0 ? 0 : t->clip_x;
    c[1] = t->clip_y < 0 ? 0 : t->clip_y;
    c[2] = (long)t->clip_x + t->clip_w > t->width ? t->width : (long)t->clip_x + t->clip_w;
    c[3] = (long)t->clip_y + t->clip_h > t->height ? t->height : (long)t->clip_y + t->clip_h;
    if (t->mask_address) {          /* nothing outside the mask */
        if (t->mask_x > c[0]) c[0] = t->mask_x;
        if (t->mask_y > c[1]) c[1] = t->mask_y;
        if ((long)t->mask_x + t->mask_w < c[2]) c[2] = (long)t->mask_x + t->mask_w;
        if ((long)t->mask_y + t->mask_h < c[3]) c[3] = (long)t->mask_y + t->mask_h;
    }
    return c[2] > c[0] && c[3] > c[1];
}

/* Under a matrix, or past the cache: each outline filled onto the target. */
static int draw_direct(OFGpu *g, OFFace *face, const OFGpuTarget *t, const OFGpuRun *r, const long *c)
{
    static const int32_t identity[4] = { 0x10000, 0, 0, 0x10000 };
    uint32_t i, addr;
    int drawn = 0;
    long words, e;
    for (i = 0; i < r->count; i++) {
        size_t nv = 0, np = 0;
        uint32_t flags = (r->op & 255) | (r->flags & OF_GPU_ALIASED ? OGPU_PATH_ALIASED : 0)
            | (t->mask_address ? OGPU_PATH_MASK : 0);
        if (outline(g, face, r->glyphs[i], &nv, &np) != OF_OK)
            continue;
        drawn++;
        if (!nv)
            continue;
        if ((e = room(g, t)) != 0)
            return (int)e;
        into(g, SLOT_TARGET, c[0], c[1], c[2] - c[0], c[3] - c[1]);
        if (!path_words(g, nv, r->matrix ? r->matrix : identity, r->positions[2 * i], r->positions[2 * i + 1], &addr, &words)) {
            if ((e = flush(g)) != 0)
                return (int)e;
            head(g, t);
            into(g, SLOT_TARGET, c[0], c[1], c[2] - c[0], c[3] - c[1]);
            if (!path_words(g, nv, r->matrix ? r->matrix : identity, r->positions[2 * i], r->positions[2 * i + 1], &addr, &words))
                return OF_ERR_BUFFER;
        }
        path_fill(g, flags, r->color, t->mask_address ? SLOT_MASK : 0, t->mask_x, t->mask_y, addr, words);
        g->stats.direct++;
    }
    e = flush(g);
    return e ? (int)e : drawn;
}

int of_gpu_draw(OFGpu *g, OFFace *face, const OFGpuTarget *t, const OFGpuRun *r)
{
    uint64_t fp;
    long c[4], lx = 0, ly = 0, lw = 0, lh = 0, lbpr = 0, e = 0;
    uint32_t i, layer_addr = 0, size_was;
    OFFaceInfo info;
    uint8_t *layer = NULL;
    int aliased = (r->flags & OF_GPU_ALIASED) != 0, drawn = 0, masked = t->mask_address != 0;
    uint32_t a[7];
    if (!g || !face || !t || !r || (r->count && (!r->glyphs || !r->positions)) || !r->pixel_size)
        return OF_ERR_ARGUMENT;
    if (aliased && !g->aliased_ok)
        return OF_ERR_ARGUMENT;
    if (!r->count || !clip_box(t, c))
        return (int)r->count;
    if (!ensure_work(g, t->width > t->mask_w ? t->width : (uint32_t)t->mask_w))
        return OF_ERR_MEMORY;
    /* the run's size, the face's own put back after */
    size_was = of_face_info(face, &info) == OF_OK ? info.pixel_size : 0;
    if (of_face_set_pixel_size(face, r->pixel_size) != OF_OK)
        return OF_ERR_FONT;
    if (r->matrix || (r->flags & OF_GPU_NOCACHE)) {
        drawn = draw_direct(g, face, t, r, c);
        if (size_was)
            of_face_set_pixel_size(face, size_was);
        return drawn;
    }
    fp = of_face_source_fingerprint(face);
    if (masked) {                       /* an A8 layer over the clip's box */
        lx = c[0]; ly = c[1]; lw = c[2] - c[0]; lh = c[3] - c[1];
        lbpr = (lw + 3) & ~3L;
        layer = (uint8_t *)g->alloc(g->user, (size_t)lbpr * (size_t)lh, &layer_addr);
        if (!layer) {
            e = OF_ERR_MEMORY;
            goto done;
        }
    }
    flush(g);
    head(g, t);
    if (masked) {
        bind_layer(g, layer_addr, lbpr, lw, lh);
        into(g, SLOT_LAYER, 0, 0, lw, lh);
        a[0] = xy(0, 0); a[1] = xy(lw, lh); a[2] = 0;
        emit(g, OGPU_OP_FILL, 3, a);
        g->drawn++;
    }
    for (i = 0; i < r->count; i++) {
        long x = r->positions[2 * i] + 32, y = r->positions[2 * i + 1] + 128, px, py;
        int phase = (int)((x >> 6) & 3), empty;
        const Cell *cl;
        if ((e = room(g, t)) != 0)
            goto done;
        cl = cell_of(g, face, fp, r->pixel_size, r->glyphs[i], phase, aliased, &empty);
        if (!cl && !empty) {            /* no room: the cache emptied, after what uses it is drawn */
            if ((e = flush(g)) != 0)
                goto done;
            cache_empty(g);
            head(g, t);
            cl = cell_of(g, face, fp, r->pixel_size, r->glyphs[i], phase, aliased, &empty);
        }
        drawn++;
        if (!cl)
            continue;
        px = (x >> 8) + cl->left;
        py = (y >> 8) + cl->top;
        bind_page(g, cl->page);
        if (masked) {                   /* the glyph's coverage into the layer */
            bind_layer(g, layer_addr, lbpr, lw, lh);
            into(g, SLOT_LAYER, 0, 0, lw, lh);
            a[0] = OGPU_PD_ADD | OGPU_PD_SOLID | OGPU_PD_MASK;
            a[1] = 0xFF000000UL;
            px -= lx;
            py -= ly;
        } else {                        /* the colour through the glyph's cell */
            into(g, SLOT_TARGET, c[0], c[1], c[2] - c[0], c[3] - c[1]);
            a[0] = (r->op & 255) | OGPU_PD_SOLID | OGPU_PD_MASK;
            a[1] = r->color;
        }
        a[2] = 0;
        a[3] = (uint32_t)(SLOT_PAGE0 + cl->page);
        a[4] = xy(cl->x, cl->y);
        a[5] = xy(px, py);
        a[6] = xy(cl->w, cl->h);
        emit(g, OGPU_OP_COMPOSITE_PD, 7, a);
        g->drawn++;
    }
    if (masked) {                       /* the layer times the clip mask, then the colour through it */
        if ((e = room(g, t)) != 0)
            goto done;
        bind_layer(g, layer_addr, lbpr, lw, lh);
        into(g, SLOT_LAYER, 0, 0, lw, lh);
        a[0] = OGPU_PD_IN_REVERSE;
        a[1] = SLOT_MASK;
        a[2] = xy(lx - t->mask_x, ly - t->mask_y);
        a[3] = 0;
        a[4] = 0;
        a[5] = xy(0, 0);
        a[6] = xy(lw, lh);
        emit(g, OGPU_OP_COMPOSITE_PD, 7, a);
        into(g, SLOT_TARGET, c[0], c[1], c[2] - c[0], c[3] - c[1]);
        a[0] = (r->op & 255) | OGPU_PD_SOLID | OGPU_PD_MASK;
        a[1] = r->color;
        a[2] = 0;
        a[3] = SLOT_LAYER;
        a[4] = xy(0, 0);
        a[5] = xy(lx, ly);
        a[6] = xy(lw, lh);
        emit(g, OGPU_OP_COMPOSITE_PD, 7, a);
        g->drawn++;
    }
    e = flush(g);
done:
    if (layer) {
        flush(g);
        g->release(g->user, layer);
    }
    if (size_was)
        of_face_set_pixel_size(face, size_was);
    return e ? (int)e : drawn;
}
