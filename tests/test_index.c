/* Host test: the font index (finding, aliases, the file, staleness) and
 * fallback by cluster. Copyright (c) 2026 AmiCradle Team. MIT. */
#include "gfopenfont/gfopenfont.h"
#include "openfont_index.h"

#include <stdio.h>
#include <string.h>

static int fails;

static void expect(int ok, const char *what)
{
    if (!ok) {
        printf("FAIL %s\n", what);
        fails++;
    }
}

static const char *family_of(const OFIndexEntry *e)
{
    return e ? e->family : "(none)";
}

int main(int argc, char **argv)
{
    OFContext *ctx = of_context_create();
    OFIndex ix, ix2;
    const OFIndexEntry *e;
    OFFace *face;
    OFGlyph glyphs[64];
    size_t n, i;
    int found_fallback = 0;
    /* "snow" and a snowman: Liberation Sans has no U+2603, DejaVu Sans has */
    static const char text[] = "snow \xE2\x98\x83!";

    if (argc < 3) {
        printf("usage: %s INDEXFILE DIR...\n", argv[0]);
        return 2;
    }
    of_index_init(&ix);
    for (i = 2; i < (size_t)argc; ++i)
        expect(of_index_add_dir(&ix, argv[i]) == OF_OK, "add dir");
    expect(of_index_stale(&ix), "a new index is stale");
    expect(of_index_scan(&ix, ctx) == OF_OK, "scan");
    expect(ix.count > 4, "fonts found");
    expect(!of_index_stale(&ix), "fresh after a scan");

    e = of_index_find(&ix, "Arial", 400, 0);
    expect(e && !strcmp(e->family, "Liberation Sans"), "Arial is Liberation Sans");
    printf("Arial -> %s %s\n", family_of(e), e ? e->style : "");
    e = of_index_find(&ix, "monospace", 700, 0);
    expect(e && !strcmp(e->family, "DejaVu Sans Mono") && e->weight == 700, "monospace bold");
    printf("monospace 700 -> %s %s\n", family_of(e), e ? e->style : "");
    e = of_index_find(&ix, "DejaVu Serif", 400, 1);
    expect(e && (e->flags & OF_FACE_ITALIC), "serif italic");
    e = of_index_find(&ix, "No Such Family", 400, 0);
    expect(e && !strcmp(e->family, "DejaVu Sans"), "unknown goes to sans-serif");
    expect(!of_index_find_exact(&ix, "No Such Family", 400, 0), "exact: unknown is none");
    e = of_index_find_exact(&ix, "Arial", 700, 0);
    expect(e && !strcmp(e->family, "Liberation Sans") && e->weight == 700, "exact: Arial's alias");
    e = of_index_find_exact(&ix, "serif", 400, 0);
    expect(e && !strcmp(e->family, "DejaVu Serif"), "exact: a generic family");
    of_index_configure(&ix, "Comic=Liberation Mono\n", NULL);
    e = of_index_find(&ix, "Comic", 400, 0);
    expect(e && !strcmp(e->family, "Liberation Mono"), "the user's alias");

    expect(of_index_save(&ix, argv[1]) == OF_OK, "save");
    of_index_init(&ix2);
    for (i = 2; i < (size_t)argc; ++i)
        of_index_add_dir(&ix2, argv[i]);
    expect(of_index_load(&ix2, argv[1]) == OF_OK, "load");
    expect(ix2.count == ix.count, "same entries after load");
    expect(!of_index_stale(&ix2), "loaded index is fresh");
    for (i = 0; i < ix.count && i < ix2.count; ++i)
        if (memcmp(&ix.entries[i], &ix2.entries[i], sizeof ix.entries[i]))
            break;
    expect(i == ix.count, "entries round-trip");

    face = of_index_open(&ix, ctx, "Liberation Sans", 400, 0, 1, NULL);
    expect(face != NULL, "open with fallbacks");
    expect(of_face_fallback_count(face) >= 1, "a fallback chain");
    of_face_set_pixel_size(face, 16);
    n = of_shape_utf8(face, text, strlen(text), glyphs, 64);
    expect(n == 7, "seven glyphs");
    for (i = 0; i < n && i < 64; ++i) {
        printf("  g=%u cluster=%u face=%u flags=%u adv=%d\n", glyphs[i].glyph_id,
               glyphs[i].cluster, glyphs[i].face, glyphs[i].flags, glyphs[i].x_advance_26_6);
        if (glyphs[i].cluster == 5) {
            found_fallback = glyphs[i].face != 0 && glyphs[i].glyph_id != 0;
        } else {
            expect(glyphs[i].face == 0, "the rest from the face itself");
        }
    }
    expect(found_fallback, "the snowman from a fallback");
    {
        OFBitmap b;
        OFFace *fb = of_face_fallback(face, glyphs[5].face);
        expect(fb && of_raster_glyph_a8(fb, glyphs[5].glyph_id, &b) == OF_OK && b.width > 0,
               "the fallback face draws it");
        of_bitmap_release(&b);
    }
    /* a codepoint no face of the chain has: the first face's own glyph 0,
     * with its own advance, not the last fallback's */
    {
        static const char none[] = "\xF4\x8F\xBF\xBD";     /* U+10FFFD */
        OFFace *alone = of_index_open(&ix, ctx, "Liberation Sans", 400, 0, 0, NULL);
        OFGlyph a[4], c[4];
        expect(alone && of_face_fallback_count(alone) == 0, "a face with no fallbacks");
        if (alone) {
            of_face_set_pixel_size(alone, 16);
            expect(of_shape_utf8(alone, none, 4, a, 4) == 1 && of_shape_utf8(face, none, 4, c, 4) == 1,
                   "one glyph for an unknown codepoint");
            expect(c[0].glyph_id == 0 && c[0].face == 0 && (c[0].flags & OF_GLYPH_MISSING), "marked missing");
            expect(c[0].x_advance_26_6 == a[0].x_advance_26_6, "the first face's own advance for it");
            printf("  missing: adv=%d (the face alone %d)\n", c[0].x_advance_26_6, a[0].x_advance_26_6);
            of_face_close(alone);
        }
    }
    of_face_close(face);

    /* a face for a character: the same answer before and after the faces'
     * coverage is known, and none for a character no face has */
    {
        static const uint32_t cps[] = { 0x2603, 'A', 0x10FFFD, 0x2603, 0x10FFFD, 0x0416, 'A' };
        size_t k, known = 0;
        for (k = 0; k < sizeof cps / sizeof cps[0]; ++k) {
            int rc = 0;
            OFFace *f = of_index_open_char(&ix, ctx, cps[k], 400, 0, &rc);
            if (cps[k] == 0x10FFFD)
                expect(!f && rc == OF_ERR_FONT, "no face has U+10FFFD");
            else
                expect(f && of_face_char_index(f, cps[k]) != 0, "a face has the character");
            of_face_close(f);
        }
        for (k = 0; k < ix.coverage_count; ++k)
            known += ix.coverage[k].known;
        expect(known > 0, "coverage learnt");
        printf("  coverage known for %lu of %lu faces\n", (unsigned long)known, (unsigned long)ix.count);
        {
            uint32_t r[4096];
            OFFace *f = of_index_open(&ix, ctx, "DejaVu Sans", 400, 0, 0, NULL);
            size_t pairs = of_face_cmap_ranges(f, r, 2048), m, all = 1;
            expect(f && pairs > 0 && pairs <= 2048, "DejaVu Sans's ranges");
            /* every character with a glyph is in a range */
            for (m = 0x20; f && m < 0x3000; ++m) {
                size_t q;
                int in = 0;
                if (!of_face_char_index(f, (uint32_t)m))
                    continue;
                for (q = 0; q < pairs && !in; ++q)
                    in = m >= r[2 * q] && m <= r[2 * q + 1];
                all &= in;
            }
            expect(all, "the ranges hold every mapped character");
            of_face_close(f);
        }
    }

    /* no loops in the fallback graph, however long */
    {
        OFFace *fa = of_index_open(&ix, ctx, "DejaVu Sans", 400, 0, 0, NULL);
        OFFace *fb = of_index_open(&ix, ctx, "DejaVu Serif", 400, 0, 0, NULL);
        OFFace *fc = of_index_open(&ix, ctx, "Liberation Mono", 400, 0, 0, NULL);
        expect(fa && fb && fc, "three faces");
        if (fa && fb && fc) {
            expect(of_face_add_fallback(fa, fa) == OF_ERR_ARGUMENT, "not to itself");
            expect(of_face_add_fallback(fa, fb) == OF_OK, "A to B");
            expect(of_face_add_fallback(fb, fa) == OF_ERR_ARGUMENT, "not B back to A");
            expect(of_face_add_fallback(fb, fc) == OF_OK, "B to C");
            expect(of_face_add_fallback(fc, fa) == OF_ERR_ARGUMENT, "not C back to A");
            expect(of_face_add_fallback(fa, fc) == OF_OK, "A to C as well");
        }
        of_face_close(fa);
        of_face_close(fb);
        of_face_close(fc);
    }

    /* a folder added after the file was read: read again with it, the
     * folder's fonts come back and nothing is stale (the library's
     * OF_AddFontDir after its first lookup) */
    if (argc > 3) {
        OFIndex ix5;
        of_index_init(&ix5);
        for (i = 2; i < (size_t)argc - 1; ++i)
            of_index_add_dir(&ix5, argv[i]);
        expect(of_index_load(&ix5, argv[1]) == OF_OK && ix5.count < ix.count, "fewer fonts without the last folder");
        of_index_add_dir(&ix5, argv[argc - 1]);
        expect(of_index_stale(&ix5), "the added folder is stale");
        expect(of_index_load(&ix5, argv[1]) == OF_OK && ix5.count == ix.count, "all of them when read again");
        expect(!of_index_stale(&ix5), "and nothing to read");
        of_index_free(&ix5);
    }

    /* a folder that is there but can't be listed stays stale */
    {
        OFIndex ix3;
        char file[1024];
        snprintf(file, sizeof file, "%s/DejaVuSans.ttf", argv[2]);
        of_index_init(&ix3);
        of_index_add_dir(&ix3, argv[2]);
        of_index_add_dir(&ix3, file);
        expect(of_index_scan(&ix3, ctx) == OF_OK && ix3.count > 0, "scan with a folder that isn't one");
        expect(of_index_stale(&ix3), "it is read again next time");
        of_index_free(&ix3);
    }

    /* an index file cut short is refused */
    {
        OFIndex ix4;
        FILE *in = fopen(argv[1], "rb"), *out;
        char cut[1100], buf[1 << 16];
        size_t got = in ? fread(buf, 1, sizeof buf, in) : 0;
        if (in)
            fclose(in);
        snprintf(cut, sizeof cut, "%s.cut", argv[1]);
        out = fopen(cut, "wb");
        expect(got > 100 && out != NULL, "the index file to cut");
        if (out) {
            fwrite(buf, 1, got / 2, out);
            fclose(out);
        }
        of_index_init(&ix4);
        for (i = 2; i < (size_t)argc; ++i)
            of_index_add_dir(&ix4, argv[i]);
        expect(of_index_load(&ix4, cut) == OF_ERR_CACHE && ix4.count == 0, "a cut index is refused");
        expect(of_index_stale(&ix4), "and the folders are read again");
        of_index_free(&ix4);
        remove(cut);
    }

    /* one fallback for two faces at two sizes: each gets it at its own size */
    {
        OFFace *a16 = of_index_open(&ix, ctx, "Liberation Sans", 400, 0, 0, NULL);
        OFFace *b32 = of_index_open(&ix, ctx, "Liberation Serif", 400, 0, 0, NULL);
        OFFace *shared = of_index_open(&ix, ctx, "DejaVu Sans", 400, 0, 0, NULL);
        OFFaceInfo fi;
        expect(a16 && b32 && shared, "three faces to share a fallback");
        if (a16 && b32 && shared) {
            of_face_set_pixel_size(a16, 16);
            of_face_set_pixel_size(b32, 32);
            of_face_add_fallback(a16, shared);
            of_face_add_fallback(b32, shared);
            of_shape_utf8(a16, text, strlen(text), glyphs, 64);
            of_shape_utf8(b32, text, strlen(text), glyphs, 64);
            of_face_info(of_face_fallback(a16, 1), &fi);
            expect(fi.pixel_size == 16, "the shared fallback at the first face's size");
            of_face_info(of_face_fallback(b32, 1), &fi);
            expect(fi.pixel_size == 32, "and at the second's");
        }
        of_face_close(a16);
        of_face_close(b32);
        of_face_close(shared);
    }

    /* two faces of one file share it: the second still draws once the
     * first is closed */
    {
        char file[1024];
        OFFace *a, *b;
        OFBitmap bm;
        snprintf(file, sizeof file, "%s/DejaVuSans.ttf", argv[2]);
        a = of_face_open_file(ctx, file, 0, NULL);
        b = of_face_open_file(ctx, file, 0, NULL);
        expect(a && b && a != b, "two faces of one file");
        of_face_close(a);
        expect(b && of_raster_glyph_a8(b, of_face_char_index(b, 'g'), &bm) == OF_OK && bm.width > 0,
               "the second draws after the first is closed");
        of_bitmap_release(&bm);
        of_face_close(b);
        a = of_face_open_file(ctx, file, 0, NULL);
        expect(a != NULL, "and the file opens again after both");
        of_face_close(a);
    }

    /* why a face did not open */
    {
        int err = 0;
        expect(!of_face_open_file(ctx, "/no/such/font.ttf", 0, &err) && err == OF_ERR_IO, "no file: I/O");
        expect(!of_face_open_file(ctx, argv[1], 0, &err) && err == OF_ERR_FONT, "not a font: font");
    }

    of_index_free(&ix);
    of_index_free(&ix2);
    of_context_destroy(ctx);
    if (fails)
        return 1;
    printf("index: all passed\n");
    return 0;
}
