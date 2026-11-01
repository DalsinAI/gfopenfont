/* of_face_color_png: a colour font's emoji come back as PNG images from the
 * strike nearest the size asked (the smallest at least that size, else the
 * largest; 0 for the largest), with the image's size from its header; a
 * glyph or font without colour bitmaps is OF_ERR_FONT.
 *
 * usage: test_color COLOUR_FONT [PLAIN_FONT]
 *
 * Copyright (c) 2026 AmiCradle Team. MIT licence (see LICENSE). */
#include "gfopenfont/gfopenfont.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static const uint8_t png_sig[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };

int main(int argc, char **argv)
{
    OFContext *ctx;
    OFFace *face;
    OFColorPng p, largest;
    uint32_t g, a;
    int err = 0;
    if (argc < 2) {
        fprintf(stderr, "usage: test_color COLOUR_FONT [PLAIN_FONT]\n");
        return 2;
    }
    ctx = of_context_create();
    face = of_face_open_file(ctx, argv[1], 0, &err);
    CHECK(face, "open %s: %d", argv[1], err);
    if (!face)
        return 1;

    g = of_face_char_index(face, 0x1F600);      /* grinning face */
    CHECK(g, "no glyph for U+1F600");
    CHECK(of_face_color_png(face, g, 0, &largest) == OF_OK, "no colour bitmap for U+1F600");
    CHECK(largest.png && largest.length > 8 && !memcmp(largest.png, png_sig, 8), "not a PNG");
    CHECK(largest.ppem && largest.width && largest.height, "strike %u, %ux%u", largest.ppem, largest.width, largest.height);
    CHECK(largest.top > 0 && largest.top <= (int32_t)largest.height * 2, "top %d", largest.top);

    /* every size gives a strike at least as large when there is one, else the largest */
    for (a = 1; a <= 300; a += 7) {
        CHECK(of_face_color_png(face, g, a, &p) == OF_OK, "size %u", a);
        CHECK(p.ppem >= a || p.ppem == largest.ppem, "size %u: strike %u", a, p.ppem);
        CHECK(p.ppem <= largest.ppem, "size %u: strike %u above the largest %u", a, p.ppem, largest.ppem);
    }

    /* the same image for every character's glyph that has one; none for .notdef's ASCII */
    CHECK(of_face_color_png(face, of_face_char_index(face, 0x2764), 20, &p) == OF_OK, "no bitmap for U+2764");
    CHECK(of_face_color_png(face, 0xFFFFFF, 20, &p) == OF_ERR_RANGE, "a glyph past the end");
    CHECK(of_face_color_png(face, g, 20, NULL) == OF_ERR_ARGUMENT, "no out");
    of_face_close(face);

    if (argc > 2) {
        face = of_face_open_file(ctx, argv[2], 0, &err);
        CHECK(face, "open %s: %d", argv[2], err);
        if (face) {
            CHECK(of_face_color_png(face, of_face_char_index(face, 'A'), 20, &p) == OF_ERR_FONT, "a plain font's A");
            CHECK(!p.png && !p.length, "left set");
            of_face_close(face);
        }
    }
    of_context_destroy(ctx);
    printf(fails ? "test_color: %d failures\n" : "test_color: ok\n", fails);
    return fails ? 1 : 0;
}
