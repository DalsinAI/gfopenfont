/* GFOpenBullet's core: bullet.library's glyph engine, no Amiga headers.
 * Copyright (c) 2026 AmiCradle Team. MIT.
 *
 * Clean-room: written from the NDK 3.2 headers' published layouts and
 * comments (diskfont/glyph.h, diskfont/diskfonttag.h, diskfont/oterrors.h)
 * and the bullet.library and diskfont.library autodocs; never from
 * Kickstart, Agfa, Hyperion or AROS code. Fonts are read and drawn by
 * GFOpenFont, through the env. Where those leave a corner open, bullet_core.cpp
 * says "ABI test" and tests/BulletABITest measures OS 3.2's answer.
 *
 * The core is lean C++ (gfopengcc docs/lean-cxx.md, library profile);
 * this header is plain C, so the C library shell (openbullet_lib.c) and
 * the C tests use it as is. Memory and the font come in through struct
 * obl_env, so tests/m68k drives the core under qemu-m68k over fonts in
 * host files. Nothing per call is kept in globals: an engine's state is
 * its own allocation, so any number of engines run at once, on any core.
 *
 * Types follow the 68k ABI: 32-bit pointers, big-endian, 2-byte alignment
 * of words, longs and pointers; bullet_core.cpp checks every offset and
 * size below against the NDK's.
 */
#ifndef BULLET_CORE_H
#define BULLET_CORE_H

#ifdef __cplusplus
extern "C" {
#endif

typedef unsigned char  obl_u8;
typedef signed char    obl_s8;
typedef unsigned short obl_u16;
typedef signed short   obl_s16;
typedef unsigned long  obl_u32;
typedef signed long    obl_s32;

/* ---- utility/tagitem.h, exec/nodes.h, exec/lists.h --------------------- */
struct obl_TagItem { obl_u32 ti_Tag, ti_Data; };
#define OBL_TAG_DONE   0UL
#define OBL_TAG_IGNORE 1UL
#define OBL_TAG_MORE   2UL
#define OBL_TAG_SKIP   3UL
#define OBL_TAG_USER   0x80000000UL

struct obl_MinNode { struct obl_MinNode *mln_Succ, *mln_Pred; };            /* 8 bytes */
struct obl_MinList { struct obl_MinNode *mlh_Head, *mlh_Tail, *mlh_TailPred; }; /* 12 bytes */

/* ---- diskfont/glyph.h --------------------------------------------------- */
struct obl_GlyphEngine {
    void *gle_Library;                 /* the engine's library base */
    char *gle_Name;                    /* "bullet" */
};                                     /* 8 bytes; the engine's own follows */

struct obl_GlyphMap {
    obl_u16 glm_BMModulo;              /* 0: bytes a row, a multiple of 4 */
    obl_u16 glm_BMRows;                /* 2 */
    obl_u16 glm_BlackLeft, glm_BlackTop;       /* 4, 6 */
    obl_u16 glm_BlackWidth, glm_BlackHeight;   /* 8, 10 */
    obl_s32 glm_XOrigin, glm_YOrigin;  /* 12, 16: FIXED, from the bitmap's top left */
    obl_s16 glm_X0, glm_Y0;            /* 20, 22: the origin in whole pixels */
    obl_s16 glm_X1, glm_Y1;            /* 24, 26: the origin plus the advance */
    obl_s32 glm_Width;                 /* 28: FIXED advance, a fraction of the em */
    obl_u8 *glm_BitMap;                /* 32 */
};                                     /* 36 bytes */

struct obl_GlyphWidthEntry {
    struct obl_MinNode gwe_Node;       /* 0 */
    obl_u16 gwe_Code;                  /* 8 */
    obl_s32 gwe_Width;                 /* 10: FIXED, a fraction of the em */
};                                     /* 14 bytes */

/* ---- diskfont/diskfonttag.h -------------------------------------------- */
#define OBL_OT_Level0   OBL_TAG_USER
#define OBL_OT_Level1   (OBL_TAG_USER | 0x1000)
#define OBL_OT_Level2   (OBL_TAG_USER | 0x2000)
#define OBL_OT_Level3   (OBL_TAG_USER | 0x3000)
#define OBL_OT_Indirect 0x8000

#define OBL_OT_DeviceDPI      (OBL_OT_Level0 | 0x01)   /* x dpi << 16 | y dpi */
#define OBL_OT_DotSize        (OBL_OT_Level0 | 0x02)   /* x % << 16 | y % */
#define OBL_OT_PointHeight    (OBL_OT_Level0 | 0x08)   /* FIXED points */
#define OBL_OT_SetFactor      (OBL_OT_Level0 | 0x09)   /* FIXED */
#define OBL_OT_ShearSin       (OBL_OT_Level0 | 0x0a)
#define OBL_OT_ShearCos       (OBL_OT_Level0 | 0x0b)
#define OBL_OT_RotateSin      (OBL_OT_Level0 | 0x0c)
#define OBL_OT_RotateCos      (OBL_OT_Level0 | 0x0d)
#define OBL_OT_EmboldenX      (OBL_OT_Level0 | 0x0e)
#define OBL_OT_EmboldenY      (OBL_OT_Level0 | 0x0f)
#define OBL_OT_PointSize      (OBL_OT_Level0 | 0x10)   /* obsolete */
#define OBL_OT_GlyphCode      (OBL_OT_Level0 | 0x11)
#define OBL_OT_GlyphCode2     (OBL_OT_Level0 | 0x12)
#define OBL_OT_GlyphWidth     (OBL_OT_Level0 | 0x13)
#define OBL_OT_OTagPath       (OBL_OT_Level0 | OBL_OT_Indirect | 0x14)
#define OBL_OT_OTagList       (OBL_OT_Level0 | OBL_OT_Indirect | 0x15)
#define OBL_OT_GlyphMap       (OBL_OT_Level0 | OBL_OT_Indirect | 0x20)
#define OBL_OT_WidthList      (OBL_OT_Level0 | OBL_OT_Indirect | 0x21)
#define OBL_OT_TextKernPair   (OBL_OT_Level0 | OBL_OT_Indirect | 0x22)
#define OBL_OT_DesignKernPair (OBL_OT_Level0 | OBL_OT_Indirect | 0x23)
#define OBL_OT_UnderLined     (OBL_OT_Level0 | 0x24)
#define OBL_OTUL_None         0
#define OBL_OTUL_Solid        1
#define OBL_OTUL_Broken       2
#define OBL_OTUL_DoubleSolid  3
#define OBL_OTUL_DoubleBroken 4
#define OBL_OT_StrikeThrough  (OBL_OT_Level0 | 0x25)

/* the .otag file's own (Level 1 and up); the core reads only these */
#define OBL_OT_FileIdent      (OBL_OT_Level1 | 0x01)
#define OBL_OT_Engine         (OBL_OT_Level1 | OBL_OT_Indirect | 0x02)
#define OBL_OT_SpecCount      (OBL_OT_Level1 | 0x100)
#define OBL_OT_Spec1          (OBL_OT_Level1 | 0x101)   /* | OT_Indirect: the outline file */

/* ---- diskfont/oterrors.h ----------------------------------------------- */
#define OBL_OTERR_Failure      (-1L)
#define OBL_OTERR_Success      0L
#define OBL_OTERR_BadTag       1L
#define OBL_OTERR_UnknownTag   2L
#define OBL_OTERR_BadData      3L
#define OBL_OTERR_NoMemory     4L
#define OBL_OTERR_NoFace       5L
#define OBL_OTERR_BadFace      6L
#define OBL_OTERR_NoGlyph      7L
#define OBL_OTERR_BadGlyph     8L
#define OBL_OTERR_NoShear      9L
#define OBL_OTERR_NoRotate     10L
#define OBL_OTERR_TooSmall     11L
#define OBL_OTERR_UnknownGlyph 12L

/* bullet.library's calls, by LVO */
#define OBL_LVO_OpenEngine   (-30)
#define OBL_LVO_CloseEngine  (-36)
#define OBL_LVO_SetInfoA     (-42)
#define OBL_LVO_ObtainInfoA  (-48)
#define OBL_LVO_ReleaseInfoA (-54)

/* The core's limits (README.md, "Edge cases") */
#define OBL_MAXPPEM    1024            /* pixels to the em, either way */
#define OBL_MAXPATH    256             /* OT_OTagPath, with its NUL */

/* ---- the env ------------------------------------------------------------ */

/* One glyph's 8-bit coverage, as GFOpenFont draws it: 0 clear, 255 solid,
 * rows top down. The same layout as gfopenfont.library's struct OFBitmap. */
struct obl_a8 {
    obl_u32 width, height;
    obl_s32 left;                      /* from the origin to the left column */
    obl_s32 top;                       /* from the baseline up to the top row */
    obl_s32 xadvance;                  /* 26.6; not used by the core */
    obl_u32 stride;
    obl_u8 *pixels;                    /* NULL for an empty glyph */
};                                     /* 28 bytes */

struct obl_env {
    /* exec: cleared memory (AllocVec MEMF_CLEAR on the Amiga); NULL when out */
    void *(*alloc)(void *user, obl_u32 size);
    void (*free)(void *user, void *mem);          /* NULL is allowed */
    /* The font, through GFOpenFont (gfopenfont.library on the Amiga). The core
     * reads no font file and draws no outline itself.
     * face_open: the first face of the outline file at path, or NULL when
     * there is none GFOpenFont reads (or no GFOpenFont); *upm its units per em. */
    void *(*face_open)(void *user, const char *path, obl_u32 *upm);
    void (*face_close)(void *user, void *face);
    /* the glyph a code maps to, 0 when none */
    obl_u32 (*char_index)(void *user, void *face, obl_u32 code);
    /* a glyph's advance in font units, < 0 when it has none */
    obl_s32 (*advance)(void *user, void *face, obl_u32 glyph);
    /* draws a glyph through m (16.16 pixels to the em, y up: X = (x m[0] +
     * y m[1]) / upm, Y = (x m[2] + y m[3]) / upm); 0, or OBL_OF_ an error */
    obl_s32 (*raster)(void *user, void *face, obl_u32 glyph, const obl_s32 m[4], struct obl_a8 *out);
    void (*raster_free)(void *user, void *face, struct obl_a8 *a8);   /* frees its pixels */
    /* the kerning between two codes in font units, 0 when none */
    obl_s32 (*kern)(void *user, void *face, obl_u32 left, obl_u32 right);
};

/* raster's errors: gfopenfont.library's OFERR_ values */
#define OBL_OF_ARGUMENT (-1L)
#define OBL_OF_MEMORY   (-2L)
#define OBL_OF_FONT     (-4L)
#define OBL_OF_RANGE    (-5L)

/* ---- the calls ------------------------------------------------------------
 * OpenEngine gives an engine with gle_Library = library and gle_Name =
 * name (the shell's own constant string), or NULL when out of memory. The
 * other calls answer an OTERR_ code as a ULONG. */
struct obl_GlyphEngine *obl_OpenEngine(const struct obl_env *env, void *user, void *library, const char *name);
void obl_CloseEngine(struct obl_GlyphEngine *ge);
obl_u32 obl_SetInfoA(struct obl_GlyphEngine *ge, const struct obl_TagItem *tags);
obl_u32 obl_ObtainInfoA(struct obl_GlyphEngine *ge, const struct obl_TagItem *tags);
obl_u32 obl_ReleaseInfoA(struct obl_GlyphEngine *ge, const struct obl_TagItem *tags);

#ifdef __cplusplus
}
#endif
#endif
