/* compare_harfbuzz: GFOpenFont's own shaper against HarfBuzz, for the
 * reference gate of stage 2. Host only; it links a HarfBuzz build the
 * caller gives (OPENFONT_HARFBUZZ_REF).
 *
 *   compare_harfbuzz TEXTFILE FONT...
 *
 * Every line of TEXTFILE (UTF-8), and a few broken UTF-8 strings, shaped by
 * both at 12, 16 and 33 pixels. HarfBuzz runs with its OpenType functions
 * and GFOpenFont's glyph advances (as gfopenfont.library did before its own
 * shaper), script and direction guessed from the text. The glyphs, their
 * clusters, advances and offsets must be the same, and so must the
 * unsafe-to-break flags.
 *
 * Known differences, counted apart and not failures:
 *   - right-to-left runs (GFOpenFont shapes left to right only for now);
 *   - .notdef marks in a font with no GPOS: HarfBuzz places them by a rule
 *     of its own; GFOpenFont leaves them at the pen.
 *
 * Copyright (c) 2026 AmiCradle Team. MIT licence (see LICENSE). */
#include "gfopenfont/gfopenfont.h"
#include "of_font.h"
#include "of_layout.h"
#include "of_unicode.h"

#include <hb.h>
#include <hb-ot.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct Face {
    OFFont font;
    uint32_t ppem;
} Face;

static hb_position_t advance(hb_font_t *font, void *data, hb_codepoint_t glyph, void *user)
{
    Face *f = (Face *)data;
    (void)font;
    (void)user;
    return glyph < f->font.num_glyphs ? of_font_advance_px(&f->font, glyph, f->ppem) : 0;
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
    d = (unsigned char *)malloc(n + 1);
    if (d && fread(d, 1, n, f) != (size_t)n) {
        free(d);
        d = NULL;
    }
    fclose(f);
    if (d)
        d[n] = 0;
    *size = (size_t)n;
    return d;
}

static long runs, same, rtl, notdef_marks, fails;

static void compare(Face *face, hb_font_t *hb, const char *name, const char *text, size_t len, unsigned line)
{
    hb_buffer_t *buf = hb_buffer_create();
    hb_glyph_info_t *info;
    hb_glyph_position_t *pos;
    unsigned n = 0, i;
    OFShBuf b;
    OFRunProps props = { OF_SCRIPT_UNKNOWN, 0, 0, OF_DIR_AUTO, NULL, 0 };
    int differs = 0, only_notdef = 1;

    hb_buffer_add_utf8(buf, text, (int)len, 0, (int)len);
    hb_buffer_guess_segment_properties(buf);
    hb_shape(hb, buf, NULL, 0);
    info = hb_buffer_get_glyph_infos(buf, &n);
    pos = hb_buffer_get_glyph_positions(buf, &n);
    memset(&b, 0, sizeof b);
    of_shape_text(&face->font, face->ppem, text, len, 0, len, &props, &b);
    runs++;
    if (HB_DIRECTION_IS_BACKWARD(hb_buffer_get_direction(buf))) {
        rtl++;
        hb_buffer_destroy(buf);
        free(b.g);
        return;
    }
    if (n != b.n) {
        differs = 1;
        only_notdef = 0;
    }
    for (i = 0; !differs && i < n; ++i) {
        const OFShGlyph *g = &b.g[i];
        int unsafe = !!(hb_glyph_info_get_glyph_flags(&info[i]) & HB_GLYPH_FLAG_UNSAFE_TO_BREAK);
        if (info[i].codepoint != g->gid || info[i].cluster != g->cluster || pos[i].x_advance != g->xa ||
            pos[i].y_advance != g->ya || unsafe != !!(g->props & OF_SH_UNSAFE)) {
            differs = 1;
            only_notdef = 0;
        } else if (pos[i].x_offset != g->xo || pos[i].y_offset != g->yo) {
            differs = 1;
            if (g->gid != 0 || face->font.gpos.len)
                only_notdef = 0;
        }
    }
    if (!differs) {
        same++;
    } else if (only_notdef) {
        notdef_marks++;
    } else {
        fails++;
        if (fails <= 20) {
            printf("DIFF %s %upx line %u:\n  HarfBuzz:", name, face->ppem, line);
            for (i = 0; i < n; ++i)
                printf(" %u/%u/%d/%d/%d%s", info[i].codepoint, info[i].cluster, pos[i].x_advance, pos[i].x_offset,
                       pos[i].y_offset, (hb_glyph_info_get_glyph_flags(&info[i]) & HB_GLYPH_FLAG_UNSAFE_TO_BREAK) ? "u" : "");
            printf("\n  GFOpenFont:");
            for (i = 0; i < b.n; ++i)
                printf(" %u/%u/%d/%d/%d%s", b.g[i].gid, b.g[i].cluster, b.g[i].xa, b.g[i].xo, b.g[i].yo,
                       (b.g[i].props & OF_SH_UNSAFE) ? "u" : "");
            printf("\n");
        }
    }
    hb_buffer_destroy(buf);
    free(b.g);
}

int main(int argc, char **argv)
{
    static const unsigned sizes[] = { 12, 16, 33 };
    static const char *broken[] = { "ab\xFF" "cd\xC3", "x\xE2\x82y", "a\x80\x80" "b", "a\xC0\xAF" "b",
                                    "a\xED\xA0\x80" "b", "a\xF4\x90\x80\x80" "b", "end\xCC\x81" };
    size_t tsize;
    char *text;
    int a;
    if (argc < 3) {
        fprintf(stderr, "usage: compare_harfbuzz TEXTFILE FONT...\n");
        return 2;
    }
    text = (char *)read_file(argv[1], &tsize);
    if (!text)
        return 2;
    for (a = 2; a < argc; ++a) {
        size_t size;
        unsigned char *data = read_file(argv[a], &size);
        const char *name = strrchr(argv[a], '/') ? strrchr(argv[a], '/') + 1 : argv[a];
        Face face;
        hb_blob_t *blob;
        hb_face_t *hface;
        hb_font_t *parent, *hb;
        hb_font_funcs_t *funcs;
        unsigned s;
        if (!data || of_font_open(&face.font, data, size, 0) != OF_OK) {
            printf("SKIP %s\n", name);
            free(data);
            continue;
        }
        blob = hb_blob_create((const char *)data, (unsigned)size, HB_MEMORY_MODE_READONLY, NULL, NULL);
        hface = hb_face_create(blob, 0);
        parent = hb_font_create(hface);
        hb = hb_font_create_sub_font(parent);
        funcs = hb_font_funcs_create();
        hb_font_funcs_set_glyph_h_advance_func(funcs, advance, NULL, NULL);
        hb_font_set_funcs(hb, funcs, &face, NULL);
        for (s = 0; s < sizeof sizes / sizeof sizes[0]; ++s) {
            char *p = text;
            unsigned line = 1, k;
            face.ppem = sizes[s];
            hb_font_set_scale(hb, (int)sizes[s] * 64, (int)sizes[s] * 64);
            hb_font_set_ppem(hb, sizes[s], sizes[s]);
            while (*p) {
                char *e = strchr(p, '\n');
                size_t len = e ? (size_t)(e - p) : strlen(p);
                if (len)
                    compare(&face, hb, name, p, len, line);
                line++;
                p += len + (e ? 1 : 0);
            }
            for (k = 0; k < sizeof broken / sizeof broken[0]; ++k)
                compare(&face, hb, name, broken[k], strlen(broken[k]), 1000 + k);
        }
        hb_font_funcs_destroy(funcs);
        hb_font_destroy(hb);
        hb_font_destroy(parent);
        hb_face_destroy(hface);
        hb_blob_destroy(blob);
        of_font_close(&face.font);
        free(data);
    }
    printf("%ld runs: %ld the same, %ld right-to-left (not compared), %ld differ only in .notdef marks of a font "
           "without GPOS, %ld differ\n", runs, same, rtl, notdef_marks, fails);
    printf(fails ? "COMPARE FAIL\n" : "COMPARE PASS\n");
    free(text);
    return fails ? 1 : 0;
}
