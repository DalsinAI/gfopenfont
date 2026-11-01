/* test_reader: GFOpenFont's own reader and rasteriser.
 *
 *   test_reader            the rasteriser on outlines made here
 *   test_reader FONT...    and each font: its cmap, names, outlines and
 *                          bitmaps, then the same font cut short and with
 *                          bytes changed, which must fail cleanly
 *
 * Per font it prints a fingerprint of every glyph's bitmap, place and
 * advance at 12 and 16 px. All of it is integer arithmetic, so the host and
 * the 68040 must print the same.
 *
 * Copyright (c) 2026 AmiCradle Team. MIT licence (see LICENSE). */
#include "gfopenfont/gfopenfont.h"
#include "of_font.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

#define CHECK(cond, ...) do { if (!(cond)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); failures++; } } while (0)

/* a rectangle in 26.6, clockwise (y up) or not */
static void rect(OFPath *p, int32_t x0, int32_t y0, int32_t x1, int32_t y1, int clockwise)
{
    if (clockwise) {
        of_path_point(p, x0, y0, OF_PT_ON);
        of_path_point(p, x0, y1, OF_PT_ON);
        of_path_point(p, x1, y1, OF_PT_ON);
        of_path_point(p, x1, y0, OF_PT_ON);
    } else {
        of_path_point(p, x0, y0, OF_PT_ON);
        of_path_point(p, x1, y0, OF_PT_ON);
        of_path_point(p, x1, y1, OF_PT_ON);
        of_path_point(p, x0, y1, OF_PT_ON);
    }
    of_path_close(p);
}

static long sum(const uint8_t *px, uint32_t n)
{
    long s = 0;
    uint32_t i;
    for (i = 0; i < n; ++i)
        s += px[i];
    return s;
}

static void raster_tests(void)
{
    OFPath p;
    uint32_t w, h, i;
    int32_t l, t;
    uint8_t *px;

    of_path_init(&p);

    /* a whole-pixel square: every pixel full, both directions */
    rect(&p, 64, 64, 192, 192, 1);
    CHECK(of_raster_a8(&p, &w, &h, &l, &t, &px) == OF_OK, "square");
    CHECK(w == 2 && h == 2 && l == 1 && t == 3, "square box %ux%u@%d,%d", w, h, l, t);
    for (i = 0; i < w * h; ++i)
        CHECK(px[i] == 255, "square pixel %u = %u", i, px[i]);
    free(px);
    of_path_reset(&p);
    rect(&p, 64, 64, 192, 192, 0);
    of_raster_a8(&p, &w, &h, &l, &t, &px);
    CHECK(w == 2 && h == 2 && sum(px, 4) == 4 * 255, "anticlockwise square");
    free(px);

    /* half a pixel off: the edges half covered, the corners a quarter */
    of_path_reset(&p);
    rect(&p, 32, 32, 160, 160, 1);
    of_raster_a8(&p, &w, &h, &l, &t, &px);
    CHECK(w == 3 && h == 3 && l == 0 && t == 3, "offset box %ux%u@%d,%d", w, h, l, t);
    if (w == 3 && h == 3) {
        CHECK(px[4] == 255, "centre %u", px[4]);
        CHECK(px[1] == 128 && px[3] == 128 && px[5] == 128 && px[7] == 128, "edges %u %u %u %u", px[1], px[3], px[5], px[7]);
        CHECK(px[0] == 64 && px[2] == 64 && px[6] == 64 && px[8] == 64, "corners %u %u %u %u", px[0], px[2], px[6], px[8]);
    }
    free(px);

    /* non-zero winding: two squares the same way over each other stay full;
     * one inside the other the other way is a hole */
    of_path_reset(&p);
    rect(&p, 0, 0, 256, 256, 1);
    rect(&p, 64, 64, 192, 192, 1);
    of_raster_a8(&p, &w, &h, &l, &t, &px);
    CHECK(w == 4 && h == 4 && sum(px, 16) == 16 * 255, "overlap %ld", sum(px, 16));
    free(px);
    of_path_reset(&p);
    rect(&p, 0, 0, 256, 256, 1);
    rect(&p, 64, 64, 192, 192, 0);
    of_raster_a8(&p, &w, &h, &l, &t, &px);
    CHECK(w == 4 && h == 4 && sum(px, 16) == 12 * 255 && px[5] == 0 && px[10] == 0, "hole %ld", sum(px, 16));
    free(px);

    /* a triangle: half of its box */
    of_path_reset(&p);
    of_path_point(&p, 0, 0, OF_PT_ON);
    of_path_point(&p, 0, 640, OF_PT_ON);
    of_path_point(&p, 640, 0, OF_PT_ON);
    of_path_close(&p);
    of_raster_a8(&p, &w, &h, &l, &t, &px);
    CHECK(w == 10 && h == 10, "triangle box %ux%u", w, h);
    CHECK(labs(sum(px, 100) - 50 * 255) <= 10, "triangle area %ld (want %d)", sum(px, 100), 50 * 255);
    free(px);

    /* a circle of radius 8 px from four quadratics, and from four cubics:
     * its area near pi r^2 (the quadratic one a little more; the lines a
     * curve is cut into lie inside it, up to 1/16 px, so a little less) */
    {
        static const int32_t r = 8 * 64, k = 8 * 64 * 5523 / 10000; /* cubic handle 0.5523 r */
        long area, want = 201;  /* pi * 64 px */
        of_path_reset(&p);
        of_path_point(&p, r, 0, OF_PT_ON);
        of_path_point(&p, r, r, OF_PT_QUAD);
        of_path_point(&p, 0, r, OF_PT_ON);
        of_path_point(&p, -r, r, OF_PT_QUAD);
        of_path_point(&p, -r, 0, OF_PT_ON);
        of_path_point(&p, -r, -r, OF_PT_QUAD);
        of_path_point(&p, 0, -r, OF_PT_ON);
        of_path_point(&p, r, -r, OF_PT_QUAD);
        of_path_close(&p);
        of_raster_a8(&p, &w, &h, &l, &t, &px);
        area = (sum(px, w * h) + 127) / 255;
        CHECK(w == 16 && h == 16 && area >= want && area <= 214, "quadratic circle %ux%u area %ld", w, h, area);
        free(px);
        of_path_reset(&p);
        of_path_point(&p, r, 0, OF_PT_ON);
        of_path_point(&p, r, k, OF_PT_CUBIC);
        of_path_point(&p, k, r, OF_PT_CUBIC);
        of_path_point(&p, 0, r, OF_PT_ON);
        of_path_point(&p, -k, r, OF_PT_CUBIC);
        of_path_point(&p, -r, k, OF_PT_CUBIC);
        of_path_point(&p, -r, 0, OF_PT_ON);
        of_path_point(&p, -r, -k, OF_PT_CUBIC);
        of_path_point(&p, -k, -r, OF_PT_CUBIC);
        of_path_point(&p, 0, -r, OF_PT_ON);
        of_path_point(&p, k, -r, OF_PT_CUBIC);
        of_path_point(&p, r, -k, OF_PT_CUBIC);
        of_path_close(&p);
        of_raster_a8(&p, &w, &h, &l, &t, &px);
        area = (sum(px, w * h) + 127) / 255;
        CHECK(w == 16 && h == 16 && labs(area - want) <= 3, "cubic circle %ux%u area %ld", w, h, area);
        free(px);
    }

    /* nothing: no bitmap */
    of_path_reset(&p);
    CHECK(of_raster_a8(&p, &w, &h, &l, &t, &px) == OF_OK && !px && !w && !h, "empty");
    of_path_free(&p);
}

static unsigned char *read_file(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb");
    unsigned char *d;
    long n;
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    d = (unsigned char *)malloc(n ? n : 1);
    if (d && fread(d, 1, n, f) != (size_t)n) {
        free(d);
        d = NULL;
    }
    fclose(f);
    *size = (size_t)n;
    return d;
}

/* opens face 0 of data and draws some glyphs: what a damaged font must
 * survive (any result but a crash) */
static void exercise(const uint8_t *data, size_t size, uint32_t step)
{
    OFFont f;
    OFPath p;
    uint32_t g;
    if (of_font_open(&f, data, size, 0) != OF_OK) {
        of_font_close(&f);
        return;
    }
    of_path_init(&p);
    of_font_glyph(&f, 'A');
    of_font_glyph(&f, 0x4E00);
    for (g = 0; g < f.num_glyphs; g += step) {
        uint32_t w, h;
        int32_t l, t;
        uint8_t *px;
        of_path_reset(&p);
        if (of_font_outline_px(&f, g, 12, &p) == OF_OK && of_raster_a8(&p, &w, &h, &l, &t, &px) == OF_OK)
            free(px);
        of_font_advance_px(&f, g, 12);
    }
    of_path_free(&p);
    of_font_close(&f);
}

static void font_tests(const char *path)
{
    size_t size;
    unsigned char *data = read_file(path, &size), *bad;
    uint32_t faces, fi, i;
    unsigned seed = 1;
    if (!data) {
        printf("SKIP %s: can't read\n", path);
        return;
    }
    faces = of_font_count(data, size);
    CHECK(faces >= 1, "%s: not a font", path);
    for (fi = 0; fi < faces && fi < 2; ++fi) {
        OFFont f;
        OFPath p;
        char name[128];
        uint32_t gA, w, h;
        int32_t l, t, asc, desc, hgt;
        uint8_t *px;
        if (of_font_open(&f, data, size, fi) != OF_OK) {
            CHECK(0, "%s#%u: open", path, fi);
            continue;
        }
        CHECK(f.num_faces == faces, "%s: faces", path);
        CHECK(of_font_name(&f, 1, name, sizeof name) && name[0], "%s#%u: no family", path, fi);
        gA = of_font_glyph(&f, 'A');
        CHECK(gA != 0, "%s#%u: no A", path, fi);
        CHECK(of_font_glyph(&f, 0x10FFFF) == 0, "%s#%u: U+10FFFF mapped", path, fi);
        CHECK(of_font_advance_px(&f, gA, 16) > 0, "%s#%u: A's advance", path, fi);
        of_font_line_px(&f, 16, &asc, &desc, &hgt);
        CHECK(asc > 0 && desc <= 0 && hgt > 0 && !(asc & 63) && !(desc & 63) && !(hgt & 63),
              "%s#%u: lines %d %d %d", path, fi, asc, desc, hgt);
        of_path_init(&p);
        CHECK(of_font_outline_px(&f, gA, 16, &p) == OF_OK && p.npoints > 2 && p.ncontours >= 1, "%s#%u: A's outline", path, fi);
        CHECK(of_raster_a8(&p, &w, &h, &l, &t, &px) == OF_OK && w >= 5 && w <= 16 && h >= 8 && h <= 16 && sum(px, w * h) > 255 * 10,
              "%s#%u: A at 16 px %ux%u", path, fi, w, h);
        free(px);
        of_path_reset(&p);
        CHECK(of_font_outline_px(&f, of_font_glyph(&f, ' '), 16, &p) == OF_OK && p.npoints == 0, "%s#%u: space has points", path, fi);
        CHECK(of_font_outline(&f, f.num_glyphs, &p) == OF_ERR_FONT, "%s#%u: glyph past the end", path, fi);
        of_path_free(&p);
        of_font_close(&f);
    }

    /* the fingerprint: every glyph (every 64th of a large font) */
    {
        OFFont f;
        OFPath p;
        uint32_t hash = 2166136261u, g, step, n = 0;
        static const uint32_t sizes[] = { 12, 16 };
        unsigned si;
        if (of_font_open(&f, data, size, 0) == OF_OK) {
            step = f.num_glyphs > 4000 ? 64 : 1;
            of_path_init(&p);
#define MIX(v) do { uint32_t v_ = (uint32_t)(v); int b_; for (b_ = 0; b_ < 4; ++b_) { hash ^= (v_ >> (8 * b_)) & 0xFF; hash *= 16777619u; } } while (0)
            for (si = 0; si < 2; ++si)
                for (g = 0; g < f.num_glyphs; g += step) {
                    uint32_t w = 0, h = 0, k;
                    int32_t l = 0, t = 0;
                    uint8_t *px = NULL;
                    of_path_reset(&p);
                    if (of_font_outline_px(&f, g, sizes[si], &p) == OF_OK)
                        of_raster_a8(&p, &w, &h, &l, &t, &px);
                    MIX(w); MIX(h); MIX(l); MIX(t);
                    MIX(of_font_advance_px(&f, g, sizes[si]));
                    for (k = 0; k < w * h; ++k) {
                        hash ^= px[k];
                        hash *= 16777619u;
                    }
                    free(px);
                    n++;
                }
#undef MIX
            of_path_free(&p);
            printf("%s: %u bitmaps, fingerprint %08lx\n", path, n, (unsigned long)hash);
        }
        of_font_close(&f);
    }

    /* damaged copies: cut short, and bytes changed in the tables */
    bad = (unsigned char *)malloc(size);
    if (bad) {
        size_t cut;
        uint32_t step = size > (1u << 20) ? 4000 : 40;
        for (cut = 0; cut < size; cut += size / 23 + 1)
            exercise(data, cut, step);
        for (i = 0; i < 40; ++i) {
            uint32_t k;
            memcpy(bad, data, size);
            for (k = 0; k < 64; ++k) {
                seed = seed * 1103515245u + 12345u;
                bad[(seed >> 4) % size] = (unsigned char)(seed >> 20);
            }
            exercise(bad, size, step);
        }
        free(bad);
    }
    free(data);
    printf("%s: %u face(s) checked\n", path, faces < 2 ? faces : 2);
}

int main(int argc, char **argv)
{
    int a;
    raster_tests();
    for (a = 1; a < argc; ++a)
        font_tests(argv[a]);
    printf(failures ? "test_reader: %d failures\n" : "test_reader: ok\n", failures);
    return failures ? 1 : 0;
}
