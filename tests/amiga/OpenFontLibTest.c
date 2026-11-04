/* OpenFontLibTest: gfopenfont.library's calls on AmigaOS 3.2, for a lab.
 *
 *   OpenFontLibTest FONT/A,FONTDIR/A,CACHE,OUTLINE/K,COLOR/K,INTELLIFONT/K
 *
 * FONT is DejaVuSans.ttf: the shaping and raster hash, and the layout
 * hash of revision 3's calls, must be the ones the host's
 * tests/test_openfont.c prints for it at 16 pixels (EXPECT below), since
 * both run GFOpenFont's own reader, shaper and rasteriser (integers only).
 * FONTDIR holds DejaVu
 * and Liberation, for the index, the aliases and fallback. CACHE is an
 * .ofc file openfontcompile made on the host from FONT (sizes 9 12 16 24).
 * INTELLIFONT (1.6) is an outline .font whose outline GFOpenFont can't read,
 * such as OS 3.2's CGTriumvirate.font (an Intellifont .type): with the
 * system's bullet.library in LIBS: it must come back at 83 and then at 72
 * pixels exactly, as diskfont.library's OpenDiskFont gives it (1.5 gave the
 * nearest size already in memory, or none). No bullet.library: not run.
 * OUTLINE is an outline .font in FONTS: whose .otag names a TrueType or
 * OpenType file (OT_Spec1), for OF_OpenTextFont at a size it has no bitmap
 * for; without it only bitmap fonts are opened (topaz.font).
 * COLOR is a colour font with PNG emoji (Noto Color Emoji, CBDT, or an
 * sbix font): revision 4's OF_GlyphColorBitmap decodes U+1F600 through the
 * PNG datatype and prints its pixels' hash (ARGB bytes, FNV-1a), which
 * tests/ref/color_png_hash.py gives on the host for the same font.
 *
 * Revision 5's OF_DrawGlyphsA draws two runs through gfopengpu.library (which
 * must be in LIBS:, 0.18 or later) as tests/test_gpu.c's lab_hash does on
 * the host, and the pixels' hash must be the one it prints (LAB).
 *
 * Start it with a 4 KB stack: the library works on its own.
 *
 * Prints one line per check and "OPENFONTLIB PASS" or "OPENFONTLIB FAIL n".
 *
 * Copyright (c) 2026 AmiCradle Team. OpenFont, MIT licence (LICENSE). */
#include <exec/types.h>
#include <exec/tasks.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/graphics.h>
#include <graphics/text.h>
#include <libraries/gfopenfont.h>
#include <proto/gfopenfont.h>
#include <gfopengpu/stream.h>

#include <stdio.h>
#include <string.h>

/* tests/test_openfont.c on the host, DejaVuSans.ttf at 16 pixels */
#define EXPECT_GLYPHS 13
#define EXPECT_ADVANCE 8155
#define EXPECT_HASH 0x3a37fb49UL
/* and its LAYOUT line: revision 3's calls */
#define EXPECT_LAYOUT_HASH 0xc78fcff5UL
/* tests/test_gpu.c's LAB line: revision 5's OF_DrawGlyphsA on GFOpenGPU */
#define EXPECT_GPU_HASH 0x2734c1bcUL

struct Library *OpenFontBase;
struct GfxBase *GfxBase;
static int fails;

static void check(int ok, const char *what)
{
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok)
        fails++;
}

static ULONG hash_bytes(ULONG h, const UBYTE *p, ULONG n)
{
    while (n--) {
        h ^= *p++;
        h *= 16777619UL;
    }
    return h;
}

/* a number's bytes low first, as the host's test hashes them, so the two
 * hashes agree across byte orders */
static ULONG hash_u32(ULONG h, ULONG v)
{
    UBYTE b[4];
    b[0] = (UBYTE)v;
    b[1] = (UBYTE)(v >> 8);
    b[2] = (UBYTE)(v >> 16);
    b[3] = (UBYTE)(v >> 24);
    return hash_bytes(h, b, 4);
}

/* a plain task, not a process: the library must refuse to read files */
static volatile LONG task_result = 1, task_error = 0;
static struct Task *parent;
static void plain_task(void)
{
    LONG err = 0;
    struct OFFace *f = OF_FindFace((CONST_STRPTR)"DejaVu Sans", OFA_Error, (ULONG)&err, TAG_END);
    task_result = f != NULL;
    task_error = err;
    if (f)
        OF_CloseFace(f);
    Signal(parent, SIGBREAKF_CTRL_F);
}

int main(void)
{
    static const char text[] = "Office AVWa fi \xC3\xA9";
    static const char snow[] = "snow \xE2\x98\x83!";
    LONG args[6] = { 0, 0, 0, 0, 0, 0 };
    struct RDArgs *rda;
    struct OFFace *face, *found;
    struct OFFaceInfo info;
    struct OFGlyph glyphs[64];
    struct OFBitmap bm;
    ULONG n, i, hash = 2166136261UL;
    LONG advance, err;

    rda = ReadArgs((CONST_STRPTR)"FONT/A,FONTDIR/A,CACHE,OUTLINE/K,COLOR/K,INTELLIFONT/K", args, NULL);
    if (!rda) {
        printf("usage: OpenFontLibTest FONT/A,FONTDIR/A,CACHE,OUTLINE/K,COLOR/K,INTELLIFONT/K\n");
        return 20;
    }
    OpenFontBase = OpenLibrary((CONST_STRPTR)"gfopenfont.library", 1);
    check(OpenFontBase != NULL, "gfopenfont.library 1 opens");
    if (!OpenFontBase) {
        FreeArgs(rda);
        printf("OPENFONTLIB FAIL 1\n");
        return 20;
    }
    printf("     gfopenfont.library %u.%u, GFOpenFont %lu.%lu\n", OpenFontBase->lib_Version,
           OpenFontBase->lib_Revision, (unsigned long)(OF_Version() >> 16), (unsigned long)(OF_Version() & 0xFFFF));
    check(sizeof(struct OFGlyph) == 28 && sizeof(struct OFBitmap) == 28 && sizeof(struct OFFaceInfo) == 176,
          "structure sizes match the library's");

    err = 99;
    face = OF_OpenFace((CONST_STRPTR)args[0], OFA_Size, 16, OFA_Error, (ULONG)&err, TAG_END);
    check(face != NULL && err == OFERR_OK, "OF_OpenFace on the font");
    if (face) {
        check(OF_GetFaceInfo(face, &info) == OFERR_OK, "OF_GetFaceInfo");
        printf("     %s %s, weight %u, %lu glyphs, ascender %ld\n", info.fi_Family, info.fi_Style,
               info.fi_Weight, (unsigned long)info.fi_NumGlyphs, (long)info.fi_Ascender);
        check(!strcmp(info.fi_Family, "DejaVu Sans") && info.fi_Weight == 400 &&
              (info.fi_Flags & OFFF_SCALABLE) && info.fi_PixelSize == 16, "the face's information");

        n = OF_ShapeText(face, (CONST_STRPTR)text, strlen(text), NULL, 0);
        check(n == EXPECT_GLYPHS, "OF_ShapeText counts the glyphs");
        n = OF_ShapeText(face, (CONST_STRPTR)text, strlen(text), glyphs, 64);
        advance = OF_MeasureText(face, (CONST_STRPTR)text, strlen(text));
        for (i = 0; i < n && i < 64; ++i) {
            hash = hash_u32(hash, glyphs[i].gl_Glyph);
            hash = hash_u32(hash, (ULONG)glyphs[i].gl_XAdvance);
        }
        check(advance == EXPECT_ADVANCE, "OF_MeasureText gives the host's advance");
        memset(&bm, 0, sizeof bm);
        check(OF_RasterGlyph(face, glyphs[0].gl_Glyph, &bm) == OFERR_OK && bm.bm_Pixels, "OF_RasterGlyph");
        if (bm.bm_Pixels)
            hash = hash_bytes(hash, bm.bm_Pixels, bm.bm_Width * bm.bm_Height);
        printf("     glyphs=%lu advance=%ld first=%lu bitmap=%lux%lu hash=%08lx\n", (unsigned long)n, (long)advance,
               (unsigned long)glyphs[0].gl_Glyph, (unsigned long)bm.bm_Width,
               (unsigned long)bm.bm_Height, (unsigned long)hash);
        check(hash == EXPECT_HASH, "shaping and pixels match the host byte for byte");
        OF_FreeBitmap(&bm);
        check(OF_CharIndex(face, 'A') != 0 && OF_CharIndex(face, 0x10FFFD) == 0, "OF_CharIndex");

        /* revision 3: what a layout engine asks */
        check(OpenFontBase->lib_Revision >= OPENFONT_REVISION_LAYOUT, "gfopenfont.library 1.3: OF_ShapeA");
        check(sizeof(struct OFFeature) == 16 && sizeof(struct OFFaceMetrics) == 56 &&
              sizeof(struct OFGlyphExtents) == 16 && sizeof(struct OFOutline) == 24,
              "revision 3's structure sizes");
        if (OpenFontBase->lib_Revision >= OPENFONT_REVISION_LAYOUT) {
            static const UWORD rtl[] = { 0x0645, 0x0631, 0x062D, 0x0628, 0x0627, 0x0020, 0x05E9, 0x05DC, 0x05D5, 0x05DD };
            static const UWORD latin[] = { 'o', 'f', 'f', 'i', 'c', 'e' };
            struct OFFeature noliga = { OF_MAKETAG('l', 'i', 'g', 'a'), 0, 0, OF_FEATURE_END };
            struct OFFaceMetrics m;
            struct OFGlyphExtents e;
            struct OFOutline ol;
            static UBYTE verbs[256];
            static LONG pts[512];
            ULONG lh = 2166136261UL, tlen = 0, k, no = OF_CharIndex(face, 'O');
            LONG rc;
            n = OF_Shape(face, rtl, 10, glyphs, 64, OFA_UTF16, TRUE, OFA_Direction, OFDIR_RTL, TAG_END);
            check(n == 10 && glyphs[0].gl_Cluster == 9 && glyphs[n - 1].gl_Cluster == 0,
                  "OF_Shape: UTF-16, right to left, in visual order");
            for (i = 0; i < n && i < 64; ++i) {
                lh = hash_u32(lh, glyphs[i].gl_Glyph);
                lh = hash_u32(lh, glyphs[i].gl_Cluster);
                lh = hash_u32(lh, (ULONG)glyphs[i].gl_XAdvance);
            }
            n = OF_Shape(face, latin, 6, glyphs, 64, OFA_UTF16, TRUE, OFA_Features, (ULONG)&noliga,
                         OFA_NumFeatures, 1, TAG_END);
            check(n == 6, "OF_Shape: 'liga' off keeps every letter");
            lh = hash_u32(lh, n);
            check(OF_GetFaceMetrics(face, &m) == OFERR_OK && m.fm_XHeight > 0 && m.fm_CapHeight > m.fm_XHeight,
                  "OF_GetFaceMetrics");
            lh = hash_u32(lh, (ULONG)m.fm_Ascender);
            lh = hash_u32(lh, (ULONG)m.fm_Descender);
            lh = hash_u32(lh, (ULONG)m.fm_XHeight);
            lh = hash_u32(lh, (ULONG)m.fm_CapHeight);
            lh = hash_u32(lh, (ULONG)m.fm_UnderlinePosition);
            memset(&ol, 0, sizeof ol);
            rc = OF_GlyphOutline(face, no, &ol);
            check(rc == OFERR_BUFFER && ol.ol_NumVerbs > 0, "OF_GlyphOutline measures");
            ol.ol_Verbs = verbs;
            ol.ol_Points = pts;
            ol.ol_MaxVerbs = 256;
            ol.ol_MaxPoints = 256;
            rc = OF_GlyphOutline(face, no, &ol);
            check(rc == OFERR_OK && verbs[0] == OFPV_MOVE && verbs[ol.ol_NumVerbs - 1] == OFPV_CLOSE,
                  "OF_GlyphOutline: 'O'");
            lh = hash_u32(lh, ol.ol_NumVerbs);
            lh = hash_u32(lh, ol.ol_NumPoints);
            for (k = 0; k < ol.ol_NumPoints && k < 256; ++k) {
                lh = hash_u32(lh, (ULONG)pts[2 * k]);
                lh = hash_u32(lh, (ULONG)pts[2 * k + 1]);
            }
            check(OF_GlyphExtents(face, no, &e) == OFERR_OK && e.ge_Width > 0 && e.ge_Height > 0,
                  "OF_GlyphExtents");
            lh = hash_u32(lh, (ULONG)e.ge_XBearing);
            lh = hash_u32(lh, (ULONG)e.ge_YBearing);
            lh = hash_u32(lh, (ULONG)e.ge_Width);
            lh = hash_u32(lh, (ULONG)e.ge_Height);
            check(OF_GetFontTable(face, OF_MAKETAG('h', 'e', 'a', 'd'), &tlen) != NULL && tlen == 54,
                  "OF_GetFontTable('head')");
            lh = hash_u32(lh, tlen);
            check(OF_GlyphAdvancePx(face, no) == glyphs[0].gl_XAdvance || OF_GlyphAdvancePx(face, no) > 0,
                  "OF_GlyphAdvancePx");
            printf("     layout hash=%08lx\n", (unsigned long)lh);
            check(lh == EXPECT_LAYOUT_HASH, "revision 3's results match the host byte for byte");

            /* a face from memory, as a web font comes */
            {
                BPTR fh = Open((CONST_STRPTR)args[0], MODE_OLDFILE);
                LONG size = 0;
                UBYTE *mem = NULL;
                if (fh) {
                    Seek(fh, 0, OFFSET_END);
                    size = Seek(fh, 0, OFFSET_BEGINNING);
                    mem = size > 0 ? (UBYTE *)AllocVec((ULONG)size, MEMF_ANY) : NULL;
                    if (mem && Read(fh, mem, size) != size) {
                        FreeVec(mem);
                        mem = NULL;
                    }
                    Close(fh);
                }
                if (mem) {
                    struct OFFace *mf;
                    err = 99;
                    mf = OF_OpenFaceMemory(mem, (ULONG)size, OFA_Size, 16, OFA_Error, (ULONG)&err, TAG_END);
                    FreeVec(mem);   /* the face keeps its own copy */
                    check(mf != NULL && err == OFERR_OK, "OF_OpenFaceMemory");
                    if (mf) {
                        check(OF_MeasureText(mf, (CONST_STRPTR)text, strlen(text)) == EXPECT_ADVANCE,
                              "the face from memory measures as the file's");
                        OF_CloseFace(mf);
                    }
                    err = 0;
                    check(OF_OpenFaceMemory("not a font", 10, OFA_Error, (ULONG)&err, TAG_END) == NULL &&
                          err == OFERR_FONT, "OF_OpenFaceMemory: not a font");
                } else {
                    check(0, "reading the font into memory");
                }
            }
        }

        if (args[2]) {
            struct OFCache *c = OF_OpenCache((CONST_STRPTR)args[2]);
            ULONG gid = 0;
            check(c != NULL, "OF_OpenCache on the host's .ofc");
            if (c) {
                check(OF_CacheMatchesFace(c, face), "the cache was compiled from this face");
                memset(&bm, 0, sizeof bm);
                check(OF_CacheGetGlyph(c, 16, 'O', &bm, &gid) == OFERR_OK && gid == glyphs[0].gl_Glyph &&
                      bm.bm_Pixels, "OF_CacheGetGlyph: the glyph, with no rasterising");
                OF_FreeBitmap(&bm);
                OF_CloseCache(c);
            }
        }

        /* revision 5: glyph runs through GFOpenGPU, as tests/test_gpu.c's lab_hash */
        check(OpenFontBase->lib_Revision >= OPENFONT_REVISION_GPU, "gfopenfont.library 1.5: OF_DrawGlyphsA");
        check(sizeof(struct OFGPUTarget) == 40 && sizeof(struct OFGlyphRun) == 32 && sizeof(struct OFGPUStats) == 16,
              "revision 5's structures have the library's sizes");
        if (OpenFontBase->lib_Revision >= OPENFONT_REVISION_GPU) {
            static const char gtext[] = "GFOpenFont through GFOpenGPU";
            static LONG shear[4] = { 65536, -13107, 0, 65536 };
            static ULONG gl[32];
            static LONG gp[64];
            ULONG *pix = (ULONG *)AllocVec(160 * 4 * 40, MEMF_ANY);
            struct OFGPUTarget gt;
            struct OFGlyphRun gr;
            struct OFGPUStats st, st2;
            LONG x = 2 * 256 + 100, gerr = 0, rc, rc2;
            ULONG i, gn = 0, gh = 2166136261UL;
            if (pix) {
                memset(pix, 0xFF, 160 * 4 * 40);
                memset(&gt, 0, sizeof gt);
                gt.gt_Address = pix; gt.gt_BytesPerRow = 160 * 4; gt.gt_Width = 160; gt.gt_Height = 40;
                gt.gt_Format = OGPU_FMT_ARGB32; gt.gt_ClipW = 160; gt.gt_ClipH = 40;
                OF_SetSize(face, 16);
                for (gn = 0; gtext[gn]; gn++) {
                    gl[gn] = OF_CharIndex(face, (UBYTE)gtext[gn]);
                    gp[2 * gn] = x;
                    gp[2 * gn + 1] = 16 * 256;
                    x += OF_GlyphAdvancePx(face, gl[gn]) * 4;
                }
                memset(&gr, 0, sizeof gr);
                gr.gr_Glyphs = gl; gr.gr_Positions = gp; gr.gr_Count = gn; gr.gr_PixelSize = 16;
                gr.gr_Color = 0xFF000000UL; gr.gr_Operator = OGPU_PD_OVER;
                rc = OF_DrawGlyphs(face, &gt, &gr, OFA_GPUError, (ULONG)&gerr, OFA_GPUStats, (ULONG)&st, TAG_END);
                if (rc == OFERR_NOGPU) {
                    printf("     OF_DrawGlyphsA: no gfopengpu.library 0.18 or later in LIBS:, not run\n");
                } else {
                    for (i = 0; i < gn; i++)
                        gp[2 * i + 1] += 18 * 256;
                    gr.gr_Color = 0x80800000UL;
                    gr.gr_Matrix = shear;
                    rc2 = OF_DrawGlyphs(face, &gt, &gr, OFA_GPUError, (ULONG)&gerr, OFA_GPUStats, (ULONG)&st2, TAG_END);
                    check(rc == (LONG)gn && rc2 == (LONG)gn && gerr == 0, "OF_DrawGlyphs: two runs drawn");
                    gh = hash_bytes(gh, (const UBYTE *)pix, 160 * 4 * 40);
                    printf("     gpu hash=%08lx, %lu filled, %lu hits, %lu filled onto the surface\n", (unsigned long)gh,
                           (unsigned long)st2.gs_Filled, (unsigned long)st2.gs_Hits, (unsigned long)st2.gs_Direct);
                    check(gh == EXPECT_GPU_HASH, "OF_DrawGlyphs: the pixels match the host byte for byte");
                    /* drawn again: from the cache */
                    gr.gr_Matrix = NULL;
                    OF_DrawGlyphs(face, &gt, &gr, OFA_GPUStats, (ULONG)&st, TAG_END);
                    check(st.gs_Filled == st2.gs_Filled && st.gs_Hits >= st2.gs_Hits + gn - 4,
                          "OF_DrawGlyphs: a run drawn again comes from the cache");
                    gr.gr_PixelSize = 0;
                    check(OF_DrawGlyphs(face, &gt, &gr, TAG_END) == OFERR_ARGUMENT, "OF_DrawGlyphs: size 0 refused");
                }
                FreeVec(pix);
            }
        }
        OF_CloseFace(face);
    }

    /* the index over FONTDIR, and fallback */
    check(OF_AddFontDir((CONST_STRPTR)args[1]) == OFERR_OK, "OF_AddFontDir");
    check(OF_RescanFonts() == OFERR_OK, "OF_RescanFonts");
    found = OF_FindFace((CONST_STRPTR)"Arial", OFA_Size, 16, TAG_END);
    check(found != NULL, "OF_FindFace(\"Arial\")");
    if (found) {
        OF_GetFaceInfo(found, &info);
        printf("     Arial -> %s %s\n", info.fi_Family, info.fi_Style);
        check(!strcmp(info.fi_Family, "Liberation Sans"), "Arial is Liberation Sans");
        check(OF_GetFallback(found, 1) != NULL, "a default fallback chain");
        n = OF_ShapeText(found, (CONST_STRPTR)snow, strlen(snow), glyphs, 64);
        for (i = 0; i < n && i < 64; ++i)
            if (glyphs[i].gl_Cluster == 5)
                break;
        check(n == 7 && i < n && glyphs[i].gl_Face != 0 && glyphs[i].gl_Glyph != 0 &&
              !(glyphs[i].gl_Flags & OFGF_MISSING), "the snowman comes from a fallback");
        if (i < n) {
            struct OFFace *fb = OF_GetFallback(found, glyphs[i].gl_Face);
            memset(&bm, 0, sizeof bm);
            check(fb && OF_RasterGlyph(fb, glyphs[i].gl_Glyph, &bm) == OFERR_OK && bm.bm_Width,
                  "the fallback draws it");
            OF_FreeBitmap(&bm);
        }
        OF_CloseFace(found);
    }
    if (OpenFontBase->lib_Revision >= OPENFONT_REVISION_LAYOUT) {
        found = OF_FindFaceForChar(0x2603, OFA_Size, 16, TAG_END);
        check(found != NULL && OF_CharIndex(found, 0x2603) != 0, "OF_FindFaceForChar(U+2603)");
        if (found)
            OF_CloseFace(found);
        check(OF_FindFaceForChar(0x10FFFD, TAG_END) == NULL, "OF_FindFaceForChar: none has it");
    }
    found = OF_FindFace((CONST_STRPTR)"monospace", OFA_Weight, 700, TAG_END);
    if (found) {
        OF_GetFaceInfo(found, &info);
        printf("     monospace 700 -> %s %s\n", info.fi_Family, info.fi_Style);
        /* the first of the monospace alias's families FONTDIR has: DejaVu
         * Sans Mono on the host, Liberation Mono where DejaVu has no Mono */
        check((!strcmp(info.fi_Family, "DejaVu Sans Mono") || !strcmp(info.fi_Family, "Liberation Mono") ||
               !strcmp(info.fi_Family, "Noto Sans Mono")) && info.fi_Weight == 700, "monospace bold");
        OF_CloseFace(found);
    } else {
        check(0, "OF_FindFace(\"monospace\")");
    }

    /* OFA_Exact: a family that isn't installed is none, an alias still counts */
    if (OpenFontBase->lib_Revision >= 3) {
        found = OF_FindFace((CONST_STRPTR)"No Such Family", OFA_Exact, TRUE, OFA_Fallbacks, FALSE, TAG_END);
        check(found == NULL, "OF_FindFace(OFA_Exact) of a family not installed");
        if (found)
            OF_CloseFace(found);
        found = OF_FindFace((CONST_STRPTR)"Arial", OFA_Exact, TRUE, OFA_Fallbacks, FALSE, TAG_END);
        check(found != NULL, "OF_FindFace(OFA_Exact) of an alias");
        if (found)
            OF_CloseFace(found);
    }

    /* colour glyphs: an emoji's PNG through the PNG datatype */
    if (OpenFontBase->lib_Revision >= OPENFONT_REVISION_COLOR) {
        struct OFColorBitmap cb;
        face = OF_OpenFace((CONST_STRPTR)args[0], TAG_END);
        if (face) {
            check(OF_GlyphColorBitmap(face, OF_CharIndex(face, 'A'), 16, &cb) == OFERR_FONT && !cb.cb_Pixels,
                  "OF_GlyphColorBitmap: a plain font's A has none");
            OF_CloseFace(face);
        }
        if (args[4]) {
            face = OF_OpenFace((CONST_STRPTR)args[4], TAG_END);
            check(face != NULL, "the colour font opens");
            if (face) {
                ULONG ch = 2166136261UL, p, alpha = 0;
                LONG rc = OF_GlyphColorBitmap(face, OF_CharIndex(face, 0x1F600), 20, &cb);
                printf("     U+1F600: rc %ld, %lux%lu, left %ld top %ld, strike %lu\n", (long)rc,
                       (unsigned long)cb.cb_Width, (unsigned long)cb.cb_Height, (long)cb.cb_Left,
                       (long)cb.cb_Top, (unsigned long)cb.cb_PPEM);
                if (rc == OFERR_EXEC)
                    printf("     the datatype path stopped at step %ld (1 datatypes.library, 2 the object, 4 the read), IoErr %ld\n",
                           (long)cb.cb_Left, (long)cb.cb_Top);
                check(rc == OFERR_OK && cb.cb_Pixels && cb.cb_Width && cb.cb_Height && cb.cb_PPEM,
                      "OF_GlyphColorBitmap(U+1F600) through the PNG datatype");
                for (p = 0; rc == OFERR_OK && p < cb.cb_Width * cb.cb_Height; ++p) {
                    UBYTE px[4];
                    px[0] = cb.cb_Pixels[p] >> 24;
                    px[1] = cb.cb_Pixels[p] >> 16;
                    px[2] = cb.cb_Pixels[p] >> 8;
                    px[3] = cb.cb_Pixels[p];
                    ch = hash_bytes(ch, px, 4);
                    alpha += px[0] != 0;
                }
                printf("     pixels hash=%08lx, %lu not transparent\n", (unsigned long)ch, (unsigned long)alpha);
                check(rc != OFERR_OK || alpha > cb.cb_Width * cb.cb_Height / 4, "the emoji has ink");
                OF_FreeColorBitmap(&cb);
                check(!cb.cb_Pixels, "OF_FreeColorBitmap");
                OF_CloseFace(face);
            }
        }
    }

    /* a plain task gets OFERR_NOTPROCESS, and the library stays usable */
    {
        struct Task *t;
        APTR stack = AllocMem(4096, MEMF_CLEAR);
        parent = FindTask(NULL);
        SetSignal(0, SIGBREAKF_CTRL_F);
        t = stack ? (struct Task *)AllocMem(sizeof(struct Task), MEMF_PUBLIC | MEMF_CLEAR) : NULL;
        if (t) {
            t->tc_Node.ln_Type = NT_TASK;
            t->tc_Node.ln_Name = (char *)"OpenFontLibTest task";
            t->tc_SPLower = stack;
            t->tc_SPUpper = (UBYTE *)stack + 4096;
            t->tc_SPReg = t->tc_SPUpper;
            AddTask(t, (APTR)plain_task, NULL);
            Wait(SIGBREAKF_CTRL_F);
            Delay(5);           /* let it return before its memory goes */
            check(task_result == 0 && task_error == -9, "a plain task gets OFERR_NOTPROCESS");
            FreeMem(t, sizeof(struct Task));
        } else {
            check(0, "a plain task");
        }
        if (stack)
            FreeMem(stack, 4096);
    }
    check(!strcmp((const char *)OF_ErrorString(OFERR_FONT), "font error"), "OF_ErrorString");

    /* Amiga fonts as TextFonts, without diskfont.library */
    check(OpenFontBase->lib_Revision >= OPENFONT_REVISION_TEXTFONT, "gfopenfont.library 1.2: OF_OpenTextFont");
    GfxBase = (struct GfxBase *)OpenLibrary((CONST_STRPTR)"graphics.library", 39);
    if (GfxBase && OpenFontBase->lib_Revision >= OPENFONT_REVISION_TEXTFONT) {
        struct TextAttr ta = { (STRPTR)"topaz.font", 8, 0, 0 };
        struct TextFont *tf = OF_OpenTextFont(&ta);
        check(tf != NULL && tf->tf_YSize == 8, "OF_OpenTextFont(topaz 8)");
        if (tf) CloseFont(tf);
        ta.ta_YSize = 11;
        tf = OF_OpenTextFont(&ta);
        check(tf != NULL && tf->tf_YSize == 11, "OF_OpenTextFont(topaz 11), from FONTS:");
        if (tf) CloseFont(tf);
        ta.ta_YSize = 23;
        tf = OF_OpenTextFont(&ta);
        check(tf != NULL && tf->tf_YSize == 23, "OF_OpenTextFont(topaz 23), scaled");
        if (tf) CloseFont(tf);
        ta.ta_Name = (STRPTR)"no such.font";
        ta.ta_YSize = 8;
        check(OF_OpenTextFont(&ta) == NULL, "OF_OpenTextFont: no such font");
        if (args[3]) {
            ta.ta_Name = (STRPTR)args[3];
            ta.ta_YSize = 29;
            tf = OF_OpenTextFont(&ta);
            check(tf != NULL && tf->tf_YSize == 29 && tf->tf_CharLoc &&
                  ((ULONG *)tf->tf_CharLoc)['A' - tf->tf_LoChar] & 0xFFFF,
                  "OF_OpenTextFont: an outline font drawn at 29");
            if (tf) {
                printf("     %s 29: baseline %u, XSize %u\n", (char *)args[3], tf->tf_Baseline, tf->tf_XSize);
                CloseFont(tf);
            }
        }
        if (args[5]) {
            struct Library *bl = OpenLibrary((CONST_STRPTR)"bullet.library", 0);
            if (!bl) printf("     OF_OpenTextFont: no bullet.library in LIBS:, the Intellifont check not run\n");
            else {
                static const UWORD sizes[2] = { 83, 72 };
                int i;
                CloseLibrary(bl);
                for (i = 0; i < 2; i++) {
                    char what[80];
                    ta.ta_Name = (STRPTR)args[5];
                    ta.ta_YSize = sizes[i];
                    tf = OF_OpenTextFont(&ta);
                    snprintf(what, sizeof what, "OF_OpenTextFont: an Intellifont outline at %u", sizes[i]);
                    check(tf != NULL && tf->tf_YSize == sizes[i], what);
                    if (tf) { printf("     %s %u: got %u\n", (char *)args[5], sizes[i], tf->tf_YSize); CloseFont(tf); }
                }
            }
        }
    }
    if (GfxBase) CloseLibrary((struct Library *)GfxBase);

    CloseLibrary(OpenFontBase);
    FreeArgs(rda);
    if (fails) {
        printf("OPENFONTLIB FAIL %d\n", fails);
        return 10;
    }
    printf("OPENFONTLIB PASS\n");
    return 0;
}
