/* shapedump: every line of a text file shaped by GFOpenFont, glyph by glyph,
 * at a few sizes, for comparing two builds of GFOpenFont (one shaping with
 * HarfBuzz, one with GFOpenFont's own shaper).
 *
 *   shapedump TEXTFILE FONT...
 *
 * One line per run: font, size, line number, then glyph/cluster/advance/
 * x offset/y offset/flags for each glyph.
 *
 * Copyright (c) 2026 AmigaChrome Team. MIT licence (see LICENSE). */
#include "gfopenfont/gfopenfont.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *extra[] = { "ab\xFF" "cd\xC3", "x\xE2\x82y", "\xF0\x9F\x98\x80 ok", "end\xCC\x81" };

static void run(OFFace *face, const char *font, unsigned px, unsigned no, const char *text, size_t len)
{
    OFGlyph *g;
    size_t n, i;
    if (!len)
        return;
    n = of_shape_utf8(face, text, len, NULL, 0);
    g = (OFGlyph *)malloc((n ? n : 1) * sizeof(*g));
    n = of_shape_utf8(face, text, len, g, n);
    printf("%s %u %u:", font, px, no);
    for (i = 0; i < n; ++i)
        printf(" %u/%u/%d/%d/%d/%u", g[i].glyph_id, g[i].cluster, g[i].x_advance_26_6,
               g[i].x_offset_26_6, g[i].y_offset_26_6, g[i].flags);
    printf("\n");
    free(g);
}

int main(int argc, char **argv)
{
    static const unsigned sizes[] = { 12, 16, 33 };
    OFContext *ctx = of_context_create();
    FILE *f;
    char *text;
    long size;
    int a;
    if (argc < 3 || !ctx)
        return 2;
    f = fopen(argv[1], "rb");
    if (!f)
        return 2;
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    text = (char *)malloc(size + 1);
    if (fread(text, 1, size, f) != (size_t)size)
        return 2;
    text[size] = 0;
    fclose(f);
    for (a = 2; a < argc; ++a) {
        OFFace *face = of_face_open(ctx, argv[a]);
        const char *name = strrchr(argv[a], '/') ? strrchr(argv[a], '/') + 1 : argv[a];
        unsigned s;
        if (!face) {
            printf("%s: can't open\n", name);
            continue;
        }
        for (s = 0; s < sizeof sizes / sizeof sizes[0]; ++s) {
            char *p = text;
            unsigned no = 1, k;
            of_face_set_pixel_size(face, sizes[s]);
            while (*p) {
                char *e = strchr(p, '\n');
                size_t len = e ? (size_t)(e - p) : strlen(p);
                run(face, name, sizes[s], no++, p, len);
                p += len + (e ? 1 : 0);
            }
            for (k = 0; k < sizeof extra / sizeof extra[0]; ++k)
                run(face, name, sizes[s], 1000 + k, extra[k], strlen(extra[k]));
        }
        of_face_close(face);
    }
    of_context_destroy(ctx);
    return 0;
}
