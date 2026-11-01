/* of_raster_glyph_matrix_a8 and of_face_glyph_advance: the size matrix
 * draws what of_raster_glyph_a8 draws at that size, a stretch, a shear and
 * a quarter turn move the bitmap as they should, and bad matrices are
 * refused.
 *
 * Copyright (c) 2026 AmiCradle Team. MIT licence (see LICENSE). */
#include "gfopenfont/gfopenfont.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static long ink(const OFBitmap *b)
{
    long s = 0;
    uint32_t i;
    for (i = 0; b->pixels && i < b->width * b->height; ++i)
        s += b->pixels[i];
    return s;
}

int main(int argc, char **argv)
{
    static const uint32_t codes[] = {'A', 'g', 'O', 'W', '1', 0xE9};
    OFContext *ctx;
    OFFace *face;
    OFFaceInfo info;
    uint32_t c, ppem;
    if (argc != 2) {
        fprintf(stderr, "usage: %s FONT\n", argv[0]);
        return 2;
    }
    ctx = of_context_create();
    face = ctx ? of_face_open(ctx, argv[1]) : NULL;
    if (!face || of_face_info(face, &info) != OF_OK) {
        fprintf(stderr, "open failed\n");
        return 1;
    }
    for (ppem = 9; ppem <= 48; ppem += 13)
        for (c = 0; c < sizeof codes / sizeof codes[0]; ++c) {
            const uint32_t g = of_face_char_index(face, codes[c]);
            const int32_t s = (int32_t)(ppem << 16);
            const int32_t size[4] = {s, 0, 0, s}, wide[4] = {2 * s, 0, 0, s},
                          shear[4] = {s, s / 4, 0, s}, turn[4] = {0, -s, s, 0};
            OFBitmap a, m, w, sh, t;
            long ia, im;
            CHECK(g != 0, "no glyph for U+%04X", (unsigned)codes[c]);
            CHECK(of_face_set_pixel_size(face, ppem) == OF_OK, "size %u", (unsigned)ppem);
            CHECK(of_raster_glyph_a8(face, g, &a) == OF_OK, "raster");
            CHECK(of_raster_glyph_matrix_a8(face, g, size, &m) == OF_OK, "matrix");
            /* the same size: the same box within a pixel, the same ink within 3% */
            CHECK(abs((int)a.width - (int)m.width) <= 1 && abs((int)a.height - (int)m.height) <= 1 &&
                  abs(a.left - m.left) <= 1 && abs(a.top - m.top) <= 1,
                  "U+%04X %u px: box %ux%u@%d,%d vs %ux%u@%d,%d", (unsigned)codes[c], (unsigned)ppem,
                  (unsigned)a.width, (unsigned)a.height, (int)a.left, (int)a.top,
                  (unsigned)m.width, (unsigned)m.height, (int)m.left, (int)m.top);
            ia = ink(&a);
            im = ink(&m);
            CHECK(labs(ia - im) * 100 <= ia * 3 + 255, "U+%04X %u px: ink %ld vs %ld", (unsigned)codes[c],
                  (unsigned)ppem, ia, im);
            CHECK(labs(a.advance_x_26_6 - m.advance_x_26_6) <= 64, "advance %d vs %d",
                  (int)a.advance_x_26_6, (int)m.advance_x_26_6);
            /* twice as wide: twice the ink, the same height */
            CHECK(of_raster_glyph_matrix_a8(face, g, wide, &w) == OF_OK, "wide");
            CHECK(abs((int)w.width - 2 * (int)m.width) <= 2 && w.height == m.height, "wide box");
            CHECK(labs(ink(&w) - 2 * im) * 100 <= 2 * im * 5 + 255, "wide ink %ld vs %ld", ink(&w), 2 * im);
            /* sheared right by a quarter: as tall, wider, the same ink */
            CHECK(of_raster_glyph_matrix_a8(face, g, shear, &sh) == OF_OK, "shear");
            CHECK(abs((int)sh.height - (int)m.height) <= 1 && sh.width + 1 >= m.width, "shear box %ux%u vs %ux%u", (unsigned)sh.width, (unsigned)sh.height, (unsigned)m.width, (unsigned)m.height);
            CHECK(labs(ink(&sh) - im) * 100 <= im * 3 + 255, "shear ink");
            /* a quarter turn anticlockwise: width and height change places */
            CHECK(of_raster_glyph_matrix_a8(face, g, turn, &t) == OF_OK, "turn");
            CHECK(abs((int)t.width - (int)m.height) <= 1 && abs((int)t.height - (int)m.width) <= 1, "turn box %ux%u vs %ux%u",
                  (unsigned)t.width, (unsigned)t.height, (unsigned)m.width, (unsigned)m.height);
            CHECK(labs(ink(&t) - im) * 100 <= im * 3 + 255, "turn ink");
            of_bitmap_release(&a);
            of_bitmap_release(&m);
            of_bitmap_release(&w);
            of_bitmap_release(&sh);
            of_bitmap_release(&t);
        }
    {
        const uint32_t g = of_face_char_index(face, 'A');
        const int32_t big[4] = {(1024 << 16) + 1, 0, 0, 1 << 16};
        OFBitmap b;
        CHECK(of_raster_glyph_matrix_a8(face, g, big, &b) == OF_ERR_ARGUMENT, "past 1024 px");
        CHECK(of_raster_glyph_matrix_a8(face, info.num_glyphs, big, &b) != OF_OK, "no such glyph");
        CHECK(of_face_glyph_advance(face, info.num_glyphs) == -1, "advance past the end");
        CHECK(of_face_glyph_advance(face, g) > 0 && of_face_glyph_advance(face, g) < 4 * info.units_per_em,
              "advance of A %d", (int)of_face_glyph_advance(face, g));
    }
    of_face_close(face);
    of_context_destroy(ctx);
    if (!fails)
        printf("test_matrix: ok\n");
    return fails != 0;
}
