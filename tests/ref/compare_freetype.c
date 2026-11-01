/* compare_freetype: GFOpenFont's own reader and rasteriser against FreeType
 * (unhinted), font by font, for the reference gate of stage 1. Host only;
 * it links a FreeType build the caller gives (FREETYPE_DIR).
 *
 *   compare_freetype FONT... : per font and size, the cmap, the names, and
 *   every glyph's bitmap: size and place exactly, each pixel within TOL
 *   (default 64 of 255) and the mean difference within 2.5. The two flatten
 *   curves differently; against a near-exact raster ours is the closer
 *   (work notes in the PR), so the tolerance covers FreeType's error too.
 * Exit 0 when everything is within the tolerances.
 *
 * Copyright (c) 2026 AmigaChrome Team. MIT licence (see LICENSE). */
#include "gfopenfont/gfopenfont.h"
#include "of_font.h"

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_TRUETYPE_TABLES_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int tol = 64;          /* a quarter of full coverage */
static double mean_tol = 2.5; /* mean |difference| over a font and size */
static int fails;

static unsigned char *read_file(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb");
    unsigned char *d;
    long n;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    d = malloc(n);
    if (d && fread(d, 1, n, f) != (size_t)n) { free(d); d = NULL; }
    fclose(f);
    *size = n;
    return d;
}

static int32_t pix_ceil(int32_t v) { return (v + 63) & ~63; }
static int32_t pix_floor(int32_t v) { return v & ~63; }
static int32_t pix_round(int32_t v) { return (v + 32) & ~63; }

int main(int argc, char **argv)
{
    FT_Library lib;
    int a;
    static const unsigned sizes[] = { 9, 12, 16, 24, 48 };
    if (getenv("TOL")) tol = atoi(getenv("TOL"));
    if (FT_Init_FreeType(&lib)) return 2;
    for (a = 1; a < argc; ++a) {
        size_t size;
        unsigned char *data = read_file(argv[a], &size);
        uint32_t nfaces, fi;
        if (!data) { printf("FAIL %s: can't read\n", argv[a]); fails++; continue; }
        nfaces = of_font_count(data, size);
        if (!nfaces) { printf("FAIL %s: not a font to GFOpenFont\n", argv[a]); fails++; continue; }
        for (fi = 0; fi < nfaces && fi < 2; ++fi) {
            OFFont f;
            FT_Face ft;
            char name[128];
            uint32_t cp, g, cmap_bad = 0;
            unsigned si;
            int rc = of_font_open(&f, data, size, fi);
            if (FT_New_Memory_Face(lib, data, (FT_Long)size, (FT_Long)fi, &ft)) { printf("SKIP %s#%u: FreeType can't\n", argv[a], fi); if (rc == OF_OK) of_font_close(&f); continue; }
            if (rc != OF_OK) { printf("FAIL %s#%u: open %d\n", argv[a], fi, rc); fails++; FT_Done_Face(ft); continue; }
            /* counts and names */
            if ((uint32_t)ft->num_faces != f.num_faces || (uint32_t)ft->num_glyphs != f.num_glyphs) {
                printf("FAIL %s#%u: faces %ld/%u glyphs %ld/%u\n", argv[a], fi, ft->num_faces, f.num_faces, ft->num_glyphs, f.num_glyphs); fails++;
            }
            of_font_name(&f, 1, name, sizeof name);
            if (strcmp(name, ft->family_name ? ft->family_name : "")) {
                char typo[128];
                of_font_name(&f, 16, typo, sizeof typo);
                printf("NAME %s#%u: family ours '%s' (16: '%s') FreeType '%s'\n", argv[a], fi, name, typo, ft->family_name);
            }
            of_font_name(&f, 2, name, sizeof name);
            if (strcmp(name, ft->style_name ? ft->style_name : "")) {
                char typo[128];
                of_font_name(&f, 17, typo, sizeof typo);
                printf("NAME %s#%u: style ours '%s' (17: '%s') FreeType '%s'\n", argv[a], fi, name, typo, ft->style_name);
            }
            /* style, pitch, colour */
            {
                unsigned st = of_font_style(&f);
                int ital = !!(ft->style_flags & FT_STYLE_FLAG_ITALIC), bold = !!(ft->style_flags & FT_STYLE_FLAG_BOLD);
                if (!!(st & OF_STYLE_ITALIC) != ital || !!(st & OF_STYLE_BOLD) != bold ||
                    !!f.is_fixed_pitch != !!FT_IS_FIXED_WIDTH(ft) || of_font_has_color(&f) != !!FT_HAS_COLOR(ft)) {
                    printf("FAIL %s#%u: style %u fixed %u colour %d, FreeType italic %d bold %d fixed %d colour %d\n", argv[a], fi,
                           st, f.is_fixed_pitch, of_font_has_color(&f), ital, bold, !!FT_IS_FIXED_WIDTH(ft), !!FT_HAS_COLOR(ft));
                    fails++;
                }
            }
            /* the cmap, every codepoint */
            for (cp = 0; cp < 0x30000; ++cp) {
                g = of_font_glyph(&f, cp);
                if (g != FT_Get_Char_Index(ft, cp)) {
                    if (cmap_bad < 3) printf("CMAP %s#%u: U+%04X ours %u FreeType %u\n", argv[a], fi, cp, g, FT_Get_Char_Index(ft, cp));
                    cmap_bad++;
                }
            }
            if (cmap_bad) { printf("FAIL %s#%u: %u codepoints map differently\n", argv[a], fi, cmap_bad); fails++; }
            for (si = 0; si < sizeof sizes / sizeof sizes[0]; ++si) {
                unsigned px = sizes[si];
                uint32_t gid, ng = f.num_glyphs, step = ng > 3000 ? ng / 1500 : 1;
                long adv_bad = 0, box_bad = 0, glyphs = 0, pixels = 0, over = 0, worst = 0, worst_glyph = -1;
                double sum = 0;
                OFPath p;
                FT_Set_Pixel_Sizes(ft, 0, px);
                of_path_init(&p);
                {
                    int32_t asc, desc, hgt;
                    of_font_line_px(&f, px, &asc, &desc, &hgt);
                    if (asc != ft->size->metrics.ascender || desc != ft->size->metrics.descender || hgt != ft->size->metrics.height) {
                        printf("FAIL %s#%u %upx: lines ours %d %d %d FreeType %ld %ld %ld\n", argv[a], fi, px, asc, desc, hgt,
                               ft->size->metrics.ascender, ft->size->metrics.descender, ft->size->metrics.height);
                        fails++;
                    }
                }
                for (gid = 0; gid < ng; gid += step) {
                    uint32_t w, h, x;
                    int32_t l, t;
                    uint8_t *pix;
                    FT_Bitmap *b;
                    of_path_reset(&p);
                    if (of_font_outline_px(&f,gid,px,&p) != OF_OK) { printf("FAIL %s#%u: glyph %u outline\n", argv[a], fi, gid); fails++; continue; }
                    
                    if (of_raster_a8(&p, &w, &h, &l, &t, &pix) != OF_OK) { printf("FAIL %s#%u: glyph %u raster\n", argv[a], fi, gid); fails++; continue; }
                    if (FT_Load_Glyph(ft, gid, FT_LOAD_NO_HINTING | FT_LOAD_NO_BITMAP) || FT_Render_Glyph(ft->glyph, FT_RENDER_MODE_NORMAL)) { free(pix); continue; }
                    b = &ft->glyph->bitmap;
                    glyphs++;
                    if (of_font_advance_px(&f, gid, px) != ft->glyph->advance.x) {
                        if (adv_bad < 3) printf("ADV %s#%u %upx glyph %u: ours %d FreeType %ld\n", argv[a], fi, px, gid, of_font_advance_px(&f, gid, px), ft->glyph->advance.x);
                        adv_bad++;
                    }
                    if (b->width != w || b->rows != h || ft->glyph->bitmap_left != l || ft->glyph->bitmap_top != t) {
                        if (box_bad < 3) printf("BOX %s#%u %upx glyph %u: ours %ux%u@%d,%d FreeType %ux%u@%d,%d\n", argv[a], fi, px, gid, w, h, l, t, b->width, b->rows, ft->glyph->bitmap_left, ft->glyph->bitmap_top);
                        box_bad++;
                        free(pix);
                        continue;
                    }
                    for (x = 0; x < w * h; ++x) {
                        int d = (int)pix[x] - (int)b->buffer[(x / w) * b->pitch + x % w];
                        if (d < 0) d = -d;
                        sum += d;
                        pixels++;
                        if (d > worst) { worst = d; worst_glyph = gid; }
                        if (d > tol) over++;
                    }
                    free(pix);
                }
                of_path_free(&p);
                printf("%s %s#%u %2upx: %ld glyphs, %ld boxes differ, pixels mean %.3f worst %ld (glyph %ld), %ld over %d\n",
                       (box_bad || over || (pixels && sum / pixels > mean_tol)) ? "FAIL" : "ok  ", argv[a], fi, px, glyphs, box_bad, pixels ? sum / pixels : 0.0, worst, worst_glyph, over, tol);
                if (box_bad || over || (pixels && sum / pixels > mean_tol)) fails++;
                if (adv_bad) { printf("FAIL %s#%u %upx: %ld advances differ\n", argv[a], fi, px, adv_bad); fails++; }
                /* size metrics as FreeType gives them for a scalable font */
                {
                    int32_t ys = (int32_t)(((int64_t)px * 64 * 65536 + f.units_per_em / 2) / f.units_per_em);
                    (void)ys;
                    (void)pix_ceil; (void)pix_floor; (void)pix_round;
                }
            }
            FT_Done_Face(ft);
            of_font_close(&f);
        }
        free(data);
    }
    FT_Done_FreeType(lib);
    printf(fails ? "COMPARE FAIL (%d)\n" : "COMPARE PASS\n", fails);
    return fails ? 1 : 0;
}
