#include "gfopenfont/gfopenfont.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t hash_bytes(uint32_t h, const unsigned char *p, size_t n)
{
    while (n--) {
        h ^= *p++;
        h *= 16777619u;
    }
    return h;
}

/* a number's bytes low first, whatever this machine's byte order, so the
 * 68k lab test (tests/amiga/OpenFontLibTest.c) gets the same hash */
static uint32_t hash_u32(uint32_t h, uint32_t v)
{
    unsigned char b[4];
    b[0] = (unsigned char)v;
    b[1] = (unsigned char)(v >> 8);
    b[2] = (unsigned char)(v >> 16);
    b[3] = (unsigned char)(v >> 24);
    return hash_bytes(h, b, 4);
}

int main(int argc, char **argv)
{
    static const char text[] = "Office AVWa fi \xC3\xA9";
    OFContext *ctx;
    OFFace *face;
    OFGlyph glyphs[64];
    OFBitmap bitmap;
    size_t n, i;
    int64_t advance;
    uint32_t hash = 2166136261u;
    if (argc != 2) {
        fprintf(stderr, "usage: %s FONT\n", argv[0]);
        return 2;
    }
    ctx = of_context_create();
    face = ctx ? of_face_open(ctx, argv[1]) : NULL;
    if (!face) {
        fprintf(stderr, "open failed\n");
        of_context_destroy(ctx);
        return 1;
    }
    if (of_face_set_pixel_size(face, 16) != OF_OK)
        return 1;
    n = of_shape_utf8(face, text, strlen(text), glyphs, 64);
    if (!n || n > 64)
        return 1;
    advance = of_measure_utf8(face, text, strlen(text));
    for (i = 0; i < n; ++i) {
        hash = hash_u32(hash, glyphs[i].glyph_id);
        hash = hash_u32(hash, (uint32_t)glyphs[i].x_advance_26_6);
    }
    if (of_raster_glyph_a8(face, glyphs[0].glyph_id, &bitmap) != OF_OK)
        return 1;
    if (bitmap.pixels)
        hash = hash_bytes(hash, bitmap.pixels, (size_t)bitmap.width * bitmap.height);
    printf("OPENFONT glyphs=%lu advance=%lld first=%u bitmap=%ux%u hash=%08x\n",
           (unsigned long)n, (long long)advance, glyphs[0].glyph_id,
           bitmap.width, bitmap.height, hash);
    of_bitmap_release(&bitmap);

    /* revision 3's calls: UTF-16 right to left with Arabic joining and a
     * feature turned off, the metrics, an outline and an ink box, a table.
     * tests/amiga/OpenFontLibTest.c hashes the same. */
    {
        static const uint16_t rtl[] = { 0x0645, 0x0631, 0x062D, 0x0628, 0x0627, 0x0020, 0x05E9, 0x05DC, 0x05D5, 0x05DD };
        static const uint16_t latin[] = { 'o', 'f', 'f', 'i', 'c', 'e' };
        OFShapeOptions o;
        OFFeature noliga = { OF_MAKE_TAG('l', 'i', 'g', 'a'), 0, 0, OF_FEATURE_END };
        OFFaceMetrics m;
        OFGlyphExtents e;
        uint8_t verbs[256];
        int32_t pts[512];
        size_t nv = 0, np = 0, tlen = 0, k;
        uint32_t lh = 2166136261u;
        memset(&o, 0, sizeof o);
        o.direction = OF_DIR_RTL;
        n = of_shape_utf16(face, rtl, 10, 0, 10, &o, glyphs, 64);
        for (i = 0; i < n && i < 64; ++i) {
            lh = hash_u32(lh, glyphs[i].glyph_id);
            lh = hash_u32(lh, glyphs[i].cluster);
            lh = hash_u32(lh, (uint32_t)glyphs[i].x_advance_26_6);
        }
        memset(&o, 0, sizeof o);
        o.features = &noliga;
        o.num_features = 1;
        n = of_shape_utf16(face, latin, 6, 0, 6, &o, glyphs, 64);
        lh = hash_u32(lh, (uint32_t)n);
        of_face_metrics(face, &m);
        lh = hash_u32(lh, (uint32_t)m.ascender);
        lh = hash_u32(lh, (uint32_t)m.descender);
        lh = hash_u32(lh, (uint32_t)m.x_height);
        lh = hash_u32(lh, (uint32_t)m.cap_height);
        lh = hash_u32(lh, (uint32_t)m.underline_position);
        of_face_glyph_outline(face, of_face_char_index(face, 'O'), verbs, 256, pts, 256, &nv, &np);
        lh = hash_u32(lh, (uint32_t)nv);
        lh = hash_u32(lh, (uint32_t)np);
        for (k = 0; k < np && k < 256; ++k) {
            lh = hash_u32(lh, (uint32_t)pts[2 * k]);
            lh = hash_u32(lh, (uint32_t)pts[2 * k + 1]);
        }
        of_face_glyph_extents(face, of_face_char_index(face, 'O'), &e);
        lh = hash_u32(lh, (uint32_t)e.x_bearing);
        lh = hash_u32(lh, (uint32_t)e.y_bearing);
        lh = hash_u32(lh, (uint32_t)e.width);
        lh = hash_u32(lh, (uint32_t)e.height);
        of_face_table(face, OF_MAKE_TAG('h', 'e', 'a', 'd'), &tlen);
        lh = hash_u32(lh, (uint32_t)tlen);
        printf("OPENFONT LAYOUT rtl+liga=%lu O=%lu/%lu xheight=%d hash=%08x\n",
               (unsigned long)n, (unsigned long)nv, (unsigned long)np, m.x_height, lh);
    }
    of_face_close(face);
    of_context_destroy(ctx);
    return 0;
}
