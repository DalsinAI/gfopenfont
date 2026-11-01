/*
 * Glyph runs drawn through GFOpenGPU (src/of_gpu.c, what gfopenfont.library's
 * OF_DrawGlyphsA does) on openamigartg's CPU core, against cairo:
 *  - cairo filling the same outlines at the same (quarter-pixel) positions:
 *    away from the glyphs' edges the pixels must be the same, at the edges
 *    (where the two rasterisers each anti-alias) within a bound;
 *  - the cache against OF_GPU_NOCACHE (each outline filled onto the target):
 *    the same pixels, as GFOpenGPU composites a cell's coverage as PATH_FILL
 *    does;
 *  - a clip mask (cairo: the glyphs' coverage times the mask, the colour
 *    through it), anti-aliasing off, a matrix, a translucent colour, an
 *    XRGB32 target;
 *  - the cache's counts: a run drawn again is all hits; enough sizes empty
 *    the atlas, and what is drawn after is still right.
 *   test_gpu FONT.ttf        (needs cairo, and openamigartg's include and
 *                             library/opengpu/ogpu_core.c)
 * MIT, Copyright (c) 2026 AmigaChrome Team.
 * SPDX-License-Identifier: MIT
 */
#include <cairo.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "of_gpu.h"
#include "ogpu_core.h"
#include <opengpu/stream.h>

#define ARENA       (1L << 23)
#define W           200
#define H           60
#define BPR         (W * 4)
#define TARGET      0x1000UL
#define MASK        0x40000UL
#define MASK_X      20
#define MASK_Y      6
#define MASK_W      150
#define MASK_H      48
#define HEAP        0x80000UL

static unsigned char arena[ARENA];
static struct ogpu_core core;
static unsigned long heapTop = HEAP;
static int live, failures, checks, submits;
static cairo_surface_t *cs;
static OFFace *face;

static ogpu_u8 *map(void *user, ogpu_u32 address, ogpu_u32 length)
{
    (void)user;
    if (address >= ARENA || length > (ogpu_u32)ARENA - address)
        return 0;
    return arena + address;
}

static long submit(void *user, const uint8_t *stream, long words)
{
    (void)user;
    submits++;
    core.last_error = 0;
    ogpu_core_run(&core, stream, words);
    return core.last_error;
}

/* A bump allocator in the arena (what the library does with AllocVec). */
static void *alloc(void *user, size_t bytes, uint32_t *address)
{
    (void)user;
    if (heapTop + bytes > ARENA)
        return 0;
    *address = (uint32_t)heapTop;
    heapTop += (bytes + 15) & ~(size_t)15;
    live++;
    return arena + *address;
}

static void release(void *user, void *block)
{
    (void)user;
    (void)block;
    live--;
}

static void check(int ok, const char *what)
{
    checks++;
    if (!ok) {
        printf("FAIL %s\n", what);
        failures++;
    }
}

static unsigned long background(int x, int y)
{
    unsigned a = (x * 7 + y * 13) % 3 ? 255 : (unsigned)(128 + (x * 5 + y) % 120);
    unsigned r = (unsigned)(x * 255 / W), g = (unsigned)(y * 255 / H), b = (unsigned)((x + y) * 3 % 256);
    r = r * a / 255; g = g * a / 255; b = b * a / 255;
    return (unsigned long)a << 24 | (unsigned long)r << 16 | (unsigned long)g << 8 | b;
}

/* Both pictures set to the background; xrgb: alpha 255. */
static void begin(int xrgb)
{
    int x, y, stride;
    unsigned int *c;
    if (!cs)
        cs = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, W, H);
    cairo_surface_flush(cs);
    c = (unsigned int *)cairo_image_surface_get_data(cs);
    stride = cairo_image_surface_get_stride(cs) / 4;
    for (y = 0; y < H; y++)
        for (x = 0; x < W; x++) {
            unsigned long v = background(x, y);
            unsigned char *p = arena + TARGET + y * BPR + x * 4;
            if (xrgb)
                v |= 0xFF000000UL;
            p[0] = (unsigned char)(v >> 24); p[1] = (unsigned char)(v >> 16); p[2] = (unsigned char)(v >> 8); p[3] = (unsigned char)v;
            c[y * stride + x] = (unsigned int)v;
        }
    cairo_surface_mark_dirty(cs);
}

static unsigned long ours(int x, int y)
{
    const unsigned char *p = arena + TARGET + y * BPR + x * 4;
    return (unsigned long)p[0] << 24 | (unsigned long)p[1] << 16 | (unsigned long)p[2] << 8 | p[3];
}

static int diff(unsigned long a, unsigned long b)
{
    int i, m = 0;
    for (i = 0; i < 32; i += 8) {
        int d = abs((int)((a >> i) & 255) - (int)((b >> i) & 255));
        if (d > m)
            m = d;
    }
    return m;
}

/* Ours against cairo's: away from edges (a pixel whose 3 x 3 neighbourhood
 * is one colour in both pictures, not on the picture's border, where a
 * glyph cut by it has an edge) the same; edge pixels within bound. */
static void compare(const char *what, int xrgb, int bound)
{
    const unsigned int *c;
    int x, y, n = 0, inner = 0, worst = 0, stride;
    char msg[200];
    cairo_surface_flush(cs);
    c = (const unsigned int *)cairo_image_surface_get_data(cs);
    stride = cairo_image_surface_get_stride(cs) / 4;
    for (y = 0; y < H; y++)
        for (x = 0; x < W; x++) {
            unsigned long a = ours(x, y), b = c[y * stride + x];
            int d, dx, dy, flat = x > 0 && y > 0 && x < W - 1 && y < H - 1;     /* the picture's border: an edge */
            if (xrgb) {
                a &= 0xFFFFFF;
                b &= 0xFFFFFF;
            }
            d = diff(a, b);
            if (!d)
                continue;
            n++;
            if (d > worst)
                worst = d;
            for (dy = -1; dy <= 1; dy++)
                for (dx = -1; dx <= 1; dx++) {
                    int X = x + dx, Y = y + dy;
                    if (X < 0 || Y < 0 || X >= W || Y >= H)
                        continue;
                    /* cairo's picture against the background: changed pixels near */
                    if (diff(c[Y * stride + X] & (xrgb ? 0xFFFFFFu : 0xFFFFFFFFu),
                            background(X, Y) & (xrgb ? 0xFFFFFFUL : 0xFFFFFFFFUL)) != 0
                        || diff(ours(X, Y) & (xrgb ? 0xFFFFFFUL : 0xFFFFFFFFUL),
                            background(X, Y) & (xrgb ? 0xFFFFFFUL : 0xFFFFFFFFUL)) != 0) {
                        unsigned long v = c[Y * stride + X];
                        if (diff(v, c[y * stride + x]) || diff(ours(X, Y), a))
                            flat = 0;
                    }
                }
            if (flat) {
                inner++;
                if (getenv("GPU_DEBUG"))
                    printf("    inner x %d y %d: ours %08lx cairo %08x\n", x, y, a, (unsigned)b);
            }
        }
    printf("  %-44s %5d pixels differ (largest %3d, %d away from edges)\n", what, n, worst, inner);
    snprintf(msg, sizeof msg, "%s: no pixel away from the edges differs (%d)", what, inner);
    check(inner == 0, msg);
    snprintf(msg, sizeof msg, "%s: edge pixels within %d (%d)", what, bound, worst);
    check(worst <= bound, msg);
}

static void same_as_saved(const char *what, const unsigned char *saved)
{
    char msg[200];
    int n = 0, i, worst = 0;
    for (i = 0; i < W * H * 4; i++)
        if (saved[i] != arena[TARGET + i]) {
            n++;
            if (abs(saved[i] - arena[TARGET + i]) > worst)
                worst = abs(saved[i] - arena[TARGET + i]);
        }
    if (getenv("GPU_DEBUG"))
        for (i = 0; i < W * H * 4; i++)
            if (saved[i] != arena[TARGET + i])
                printf("    x %d y %d byte %d: %d %d (bg %08lx)\n", (i / 4) % W, i / 4 / W, i & 3, saved[i], arena[TARGET + i],
                    background((i / 4) % W, i / 4 / W));
    snprintf(msg, sizeof msg, "%s (%d bytes differ, largest %d)", what, n, worst);
    check(n == 0, msg);
}

/* ---- the run ---------------------------------------------------------------------------- */

#define MAXG 64
static uint32_t glyphs[MAXG];
static int32_t pos[2 * MAXG];
static uint32_t count;

/* text from (x, y) (24.8) at size, advances as the face gives them. */
static void layout(const char *text, uint32_t size, long x, long y)
{
    count = 0;
    of_face_set_pixel_size(face, size);
    for (; *text && count < MAXG; text++) {
        uint32_t g = of_face_char_index(face, (unsigned char)*text);
        glyphs[count] = g;
        pos[2 * count] = (int32_t)x;
        pos[2 * count + 1] = (int32_t)y;
        x += of_face_glyph_advance_px(face, g) * 4;     /* 26.6 to 24.8 */
        count++;
    }
}

/* cairo: each glyph's outline at the position GFOpenGPU puts it (x to the
 * nearest quarter pixel, y to the nearest pixel; exact under a matrix). */
static void cairo_glyphs(cairo_t *cr, uint32_t size, const double *m, int rounded)
{
    static uint8_t verbs[4096];
    static int32_t pts[8192];
    uint32_t i;
    of_face_set_pixel_size(face, size);
    for (i = 0; i < count; i++) {
        size_t nv = 0, np = 0, v, k = 0;
        double ox = pos[2 * i] / 256.0, oy = pos[2 * i + 1] / 256.0;
        if (rounded) {
            ox = floor((pos[2 * i] + 32) / 64.0) / 4.0;
            oy = floor((pos[2 * i + 1] + 128) / 256.0);
        }
        if (of_face_glyph_outline(face, glyphs[i], verbs, sizeof verbs, pts, sizeof pts / 8, &nv, &np) != OF_OK)
            continue;
        for (v = 0; v < nv; v++) {
            double q[6];
            int j, n = verbs[v] == OF_PATH_QUAD ? 2 : verbs[v] == OF_PATH_CUBIC ? 3 : verbs[v] == OF_PATH_CLOSE ? 0 : 1;
            for (j = 0; j < n; j++, k += 2) {
                double x = pts[k] / 64.0, y = -pts[k + 1] / 64.0;
                if (m) {
                    double X = m[0] * x + m[1] * y, Y = m[2] * x + m[3] * y;
                    x = X;
                    y = Y;
                }
                q[2 * j] = x + ox;
                q[2 * j + 1] = y + oy;
            }
            switch (verbs[v]) {
            case OF_PATH_MOVE: cairo_move_to(cr, q[0], q[1]); break;
            case OF_PATH_LINE: cairo_line_to(cr, q[0], q[1]); break;
            case OF_PATH_QUAD: {
                double x0, y0;
                cairo_get_current_point(cr, &x0, &y0);
                cairo_curve_to(cr, x0 + 2.0 / 3 * (q[0] - x0), y0 + 2.0 / 3 * (q[1] - y0),
                    q[2] + 2.0 / 3 * (q[0] - q[2]), q[3] + 2.0 / 3 * (q[1] - q[3]), q[2], q[3]);
                break;
            }
            case OF_PATH_CUBIC: cairo_curve_to(cr, q[0], q[1], q[2], q[3], q[4], q[5]); break;
            default: cairo_close_path(cr); break;
            }
        }
    }
}

static void set_colour(cairo_t *cr, unsigned long c)
{
    double a = (c >> 24) / 255.0;
    if (a == 0) {
        cairo_set_source_rgba(cr, 0, 0, 0, 0);
        return;
    }
    cairo_set_source_rgba(cr, ((c >> 16) & 255) / 255.0 / a, ((c >> 8) & 255) / 255.0 / a, (c & 255) / 255.0 / a, a);
}

static OFGpuTarget target_of(int xrgb, int masked)
{
    OFGpuTarget t;
    memset(&t, 0, sizeof t);
    t.address = TARGET;
    t.bpr = BPR;
    t.width = W;
    t.height = H;
    t.format = xrgb ? OGPU_FMT_XRGB32 : OGPU_FMT_ARGB32;
    t.clip_x = 0; t.clip_y = 0; t.clip_w = W; t.clip_h = H;
    if (masked) {
        t.mask_address = MASK;
        t.mask_bpr = MASK_W;
        t.mask_x = MASK_X; t.mask_y = MASK_Y; t.mask_w = MASK_W; t.mask_h = MASK_H;
    }
    return t;
}

static int draw(OFGpu *g, uint32_t size, unsigned long colour, const int32_t *m, uint32_t flags, int xrgb, int masked)
{
    OFGpuTarget t = target_of(xrgb, masked);
    OFGpuRun r;
    memset(&r, 0, sizeof r);
    r.glyphs = glyphs;
    r.positions = pos;
    r.count = count;
    r.pixel_size = size;
    r.color = (uint32_t)colour;
    r.op = OGPU_PD_OVER;
    r.matrix = m;
    r.flags = flags;
    return of_gpu_draw(g, face, &t, &r);
}

/* cairo's side of one run: plain, or (masked) the glyphs' coverage times the mask, the colour through it. */
static void cairo_run(uint32_t size, unsigned long colour, const double *m, int aliased, int masked, int rounded)
{
    cairo_t *cr = cairo_create(cs);
    if (aliased)
        cairo_set_antialias(cr, CAIRO_ANTIALIAS_NONE);
    if (!masked) {
        cairo_glyphs(cr, size, m, rounded);
        set_colour(cr, colour);
        cairo_fill(cr);
    } else {
        cairo_surface_t *cov = cairo_image_surface_create(CAIRO_FORMAT_A8, W, H), *mk;
        cairo_t *c2 = cairo_create(cov);
        int x, y;
        unsigned char *d;
        if (aliased)
            cairo_set_antialias(c2, CAIRO_ANTIALIAS_NONE);
        cairo_glyphs(c2, size, m, rounded);
        cairo_set_source_rgba(c2, 0, 0, 0, 1);
        cairo_fill(c2);
        /* times the mask */
        mk = cairo_image_surface_create(CAIRO_FORMAT_A8, MASK_W, MASK_H);
        cairo_surface_flush(mk);
        d = cairo_image_surface_get_data(mk);
        for (y = 0; y < MASK_H; y++)
            for (x = 0; x < MASK_W; x++)
                d[y * cairo_image_surface_get_stride(mk) + x] = arena[MASK + y * MASK_W + x];
        cairo_surface_mark_dirty(mk);
        cairo_set_operator(c2, CAIRO_OPERATOR_DEST_IN);
        cairo_set_source_surface(c2, mk, MASK_X, MASK_Y);
        cairo_paint(c2);
        cairo_destroy(c2);
        set_colour(cr, colour);
        cairo_mask_surface(cr, cov, 0, 0);
        cairo_surface_destroy(mk);
        cairo_surface_destroy(cov);
    }
    cairo_destroy(cr);
}

/* What tests/amiga/OpenFontLibTest.c draws with OF_DrawGlyphsA (LAB_ there):
 * the pixels' FNV-1a hash, which the Amiga's must equal. */
#define LAB_W 160
#define LAB_H 40
static unsigned long lab_hash(OFGpu *g)
{
    static const char lab_text[] = "GFOpenFont through GFOpenGPU";
    static const int32_t lab_m[4] = { 65536, -13107, 0, 65536 };   /* a 0.2 shear */
    OFGpuTarget t;
    OFGpuRun r;
    unsigned long h = 2166136261UL;
    long x = 2 * 256 + 100;
    int i;
    memset(arena + TARGET, 0xFF, LAB_W * 4 * LAB_H);
    memset(&t, 0, sizeof t);
    t.address = TARGET; t.bpr = LAB_W * 4; t.width = LAB_W; t.height = LAB_H; t.format = OGPU_FMT_ARGB32;
    t.clip_w = LAB_W; t.clip_h = LAB_H;
    of_face_set_pixel_size(face, 16);
    for (count = 0; lab_text[count]; count++) {
        glyphs[count] = of_face_char_index(face, (unsigned char)lab_text[count]);
        pos[2 * count] = (int32_t)x;
        pos[2 * count + 1] = 16 * 256;
        x += of_face_glyph_advance_px(face, glyphs[count]) * 4;
    }
    memset(&r, 0, sizeof r);
    r.glyphs = glyphs; r.positions = pos; r.count = count; r.pixel_size = 16;
    r.color = 0xFF000000UL; r.op = OGPU_PD_OVER;
    of_gpu_draw(g, face, &t, &r);
    for (i = 0; i < (int)count; i++)
        pos[2 * i + 1] += 18 * 256;
    r.color = 0x80800000UL;
    r.matrix = lab_m;
    of_gpu_draw(g, face, &t, &r);
    for (i = 0; i < LAB_W * 4 * LAB_H; i++) {
        h ^= arena[TARGET + i];
        h = (h * 16777619UL) & 0xFFFFFFFFUL;
    }
    return h;
}

int main(int argc, char **argv)
{
    OFContext *ctx;
    OFGpu *g, *plain;
    OFGpuStats st;
    static unsigned char saved[W * H * 4];
    int x, y, n;
    static const int32_t rot[4] = { 56756, -32768, 32768, 56756 };    /* 30 degrees, 16.16 */
    static const double rotd[4] = { 0.866021729, -0.5, 0.5, 0.866021729 };
    const char *text = "Hamburgefonstiv AVAWAY 0123 {g}";
    if (argc < 2) {
        printf("usage: test_gpu FONT.ttf\n");
        return 2;
    }
    ctx = of_context_create();
    face = of_face_open(ctx, argv[1]);
    if (!face) {
        printf("can't open %s\n", argv[1]);
        return 2;
    }
    memset(&core, 0, sizeof core);
    ogpu_core_init(&core);
    core.map = map;
    for (y = 0; y < MASK_H; y++)            /* a mask: a ramp across, a hole in the middle */
        for (x = 0; x < MASK_W; x++) {
            int v = x * 255 / (MASK_W - 1);
            if (x > 60 && x < 90 && y > 10 && y < 30)
                v = 0;
            arena[MASK + y * MASK_W + x] = (unsigned char)v;
        }
    g = of_gpu_create(alloc, release, submit, 0, 1);
    check(g != 0, "of_gpu_create");
    if (!g)
        return 1;

    printf("against cairo filling the same outlines:\n");
    layout(text, 16, 3 * 256 + 77, 30 * 256 + 40);
    begin(0);
    n = draw(g, 16, 0xFF102040UL, 0, 0, 0, 0);
    check(n == (int)count, "a run: every glyph counted");
    cairo_run(16, 0xFF102040UL, 0, 0, 0, 1);
    compare("16 px, opaque", 0, 16);
    memcpy(saved, arena + TARGET, sizeof saved);

    of_gpu_stats(g, &st);
    printf("  cache after one run: %u filled, %u hits, %u resets\n", st.rasterised, st.hits, st.resets);
    check(st.rasterised > 10 && st.resets == 0, "the run's glyphs filled into the atlas");
    begin(0);
    draw(g, 16, 0xFF102040UL, 0, 0, 0, 0);
    {
        OFGpuStats s2;
        of_gpu_stats(g, &s2);
        check(s2.rasterised == st.rasterised, "drawn again: nothing filled");
        check(s2.hits - st.hits >= count - 4, "drawn again: hits");
    }
    same_as_saved("drawn again from the cache: the same", saved);

    /* the cache against each outline filled onto the target: positions on quarter pixels */
    layout("Hamburgefonstiv {g}", 16, 5 * 256 + 64, 31 * 256);
    for (n = 0; n < (int)count; n++)
        pos[2 * n] = (pos[2 * n] + 32) & ~63;
    begin(0);
    draw(g, 16, 0xC0603010UL, 0, 0, 0, 0);
    memcpy(saved, arena + TARGET, sizeof saved);
    begin(0);
    draw(g, 16, 0xC0603010UL, 0, OF_GPU_NOCACHE, 0, 0);
    same_as_saved("cache = each outline filled (quarter-pixel x)", saved);
    cairo_run(16, 0xC0603010UL, 0, 0, 0, 1);
    compare("16 px, translucent, NOCACHE", 0, 16);

    layout(text, 11, 2 * 256 + 200, 20 * 256 + 100);
    begin(0);
    draw(g, 11, 0xFF000000UL, 0, 0, 0, 0);
    cairo_run(11, 0xFF000000UL, 0, 0, 0, 1);
    compare("11 px, black", 0, 16);

    layout(text, 24, 256 * 1 + 13, 40 * 256 + 250);
    begin(1);
    draw(g, 24, 0xFF20A040UL, 0, 0, 1, 0);
    cairo_run(24, 0xFF20A040UL, 0, 0, 0, 1);
    compare("24 px, XRGB32 target", 1, 16);

    printf("clip mask:\n");
    layout(text, 18, 4 * 256 + 100, 36 * 256);
    begin(0);
    draw(g, 18, 0xFF800000UL, 0, 0, 0, 1);
    cairo_run(18, 0xFF800000UL, 0, 0, 1, 1);
    compare("18 px, under an A8 mask", 0, 16);
    begin(0);
    draw(g, 18, 0x80400000UL, 0, OF_GPU_NOCACHE, 0, 1);
    cairo_run(18, 0x80400000UL, 0, 0, 1, 1);
    compare("18 px, translucent, mask, NOCACHE", 0, 16);

    printf("anti-aliasing off:\n");
    layout(text, 16, 3 * 256 + 128, 30 * 256);
    begin(0);
    draw(g, 16, 0xFF000080UL, 0, OF_GPU_ALIASED, 0, 0);
    cairo_run(16, 0xFF000080UL, 0, 1, 0, 1);
    compare("16 px, aliased", 0, 255);
    for (y = n = 0; y < H; y++)             /* every pixel the colour or the background */
        for (x = 0; x < W; x++)
            if (ours(x, y) != 0xFF000080UL && ours(x, y) != background(x, y))
                n++;
    check(n == 0, "aliased: no pixel partly covered");

    printf("matrix (30 degrees):\n");
    layout("Rotated text", 16, 20 * 256 + 30, 50 * 256 + 17);
    begin(0);
    draw(g, 16, 0xFF004000UL, rot, 0, 0, 0);
    cairo_run(16, 0xFF004000UL, rotd, 0, 0, 0);
    compare("16 px, rotated", 0, 16);

    printf("refusals and counts:\n");
    plain = of_gpu_create(alloc, release, submit, 0, 0);
    layout(text, 16, 0, 30 * 256);
    check(draw(plain, 16, 0xFF000000UL, 0, OF_GPU_ALIASED, 0, 0) == OF_ERR_ARGUMENT,
        "aliased refused without OGPU_PATH_ALIASED");
    of_gpu_destroy(plain);
    {
        OFGpuTarget t = target_of(0, 0);
        OFGpuRun r;
        memset(&r, 0, sizeof r);
        r.glyphs = glyphs; r.positions = pos; r.count = count; r.pixel_size = 16; r.color = 0xFF000000UL;
        r.op = OGPU_PD_OVER;
        t.clip_x = W; t.clip_w = 10;
        submits = 0;
        check(of_gpu_draw(g, face, &t, &r) == (int)count && submits == 0, "outside the clip: counted, nothing sent");
        r.pixel_size = 0;
        check(of_gpu_draw(g, face, &t, &r) == OF_ERR_ARGUMENT, "pixel size 0 refused");
        t = target_of(0, 0);
        t.address = ARENA + 0x1000;             /* memory the back end can't reach */
        r.pixel_size = 16;
        check(of_gpu_draw(g, face, &t, &r) == OF_ERR_GPU && of_gpu_error(g) == OGPU_ERR_NOMAP,
            "a target the back end can't reach: OF_ERR_GPU, OGPU_ERR_NOMAP");
        layout(text, 16, 3 * 256 + 77, 30 * 256 + 40);
        begin(0);
        check(draw(g, 16, 0xFF102040UL, 0, 0, 0, 0) == (int)count && of_gpu_error(g) == 0, "and the next run draws");
        cairo_run(16, 0xFF102040UL, 0, 0, 0, 1);
        compare("16 px after a failed batch", 0, 16);
    }
    {   /* the face's own size put back */
        OFFaceInfo fi;
        of_face_set_pixel_size(face, 13);
        layout(text, 21, 0, 30 * 256);
        of_face_set_pixel_size(face, 13);
        begin(0);
        draw(g, 21, 0xFF000000UL, 0, 0, 0, 0);
        of_face_info(face, &fi);
        check(fi.pixel_size == 13, "the face's pixel size kept");
    }

    printf("the atlas emptied for room:\n");
    {
        uint32_t s;
        OFGpuStats a, b;
        of_gpu_stats(g, &a);
        for (s = 30; s <= 58; s += 2) {
            layout("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz", s, 0, 50 * 256);
            begin(0);
            draw(g, s, 0xFF000000UL, 0, 0, 0, 0);
        }
        of_gpu_stats(g, &b);
        printf("  %u filled, %u resets\n", b.rasterised - a.rasterised, b.resets - a.resets);
        check(b.resets > a.resets, "large sizes emptied the atlas");
        layout(text, 16, 3 * 256 + 77, 30 * 256 + 40);
        begin(0);
        draw(g, 16, 0xFF102040UL, 0, 0, 0, 0);
        cairo_run(16, 0xFF102040UL, 0, 0, 0, 1);
        compare("16 px after the atlas was emptied", 0, 16);
    }
    printf("LAB %08lx (tests/amiga/OpenFontLibTest.c's EXPECT_GPU_HASH)\n", lab_hash(g));
    of_gpu_destroy(g);
    check(live == 0, "every block released");
    of_face_close(face);
    of_context_destroy(ctx);
    cairo_surface_destroy(cs);
    printf("%d checks, %d failed\n", checks, failures);
    return failures != 0;
}
