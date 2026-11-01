#ifndef LIBRARIES_OPENFONT_H
#define LIBRARIES_OPENFONT_H
/* openfont.library: fonts, shaping, glyphs and the font index for AmigaOS
 * 3.2 on the 68040, with its own font reader and rasteriser. Programs call
 * it in place of FreeType, HarfBuzz and fontconfig; see
 * Autodocs/openfont.doc.
 *
 * Version 1 is a preview: its calls and structures may still change. They
 * are frozen from version 2, once OpenFont's design tests pass.
 *
 * Copyright (c) 2026 AmigaChrome Team. OpenFont, MIT licence (LICENSE). */

#ifndef EXEC_TYPES_H
#include <exec/types.h>
#endif
#ifndef UTILITY_TAGITEM_H
#include <utility/tagitem.h>
#endif

#define OPENFONTNAME "openfont.library"
#define OPENFONT_VERSION 1
#define OPENFONT_REVISION_TEXTFONT 2   /* lib_Revision with OF_OpenTextFont */
#define OPENFONT_REVISION_LAYOUT 3     /* lib_Revision with OF_ShapeA and the calls after it */
#define OPENFONT_REVISION_COLOR 4      /* lib_Revision with OF_GlyphColorBitmap */
#define OPENFONT_REVISION_GPU 5        /* lib_Revision with OF_DrawGlyphsA */

/* A face (one font of a file) and a compiled glyph cache: opaque. */
struct OFFace;
struct OFCache;

/* A face's fallbacks: up to this many, tried in order for text the face
 * itself has no glyphs for. */
#define OF_MAX_FALLBACKS 8

/* One shaped glyph. Positions and advances are 26.6 fixed point pixels. */
struct OFGlyph {
    ULONG gl_Glyph;         /* in the face gl_Face names */
    ULONG gl_Cluster;       /* byte offset in the text of its cluster's first byte */
    LONG gl_XAdvance;
    LONG gl_YAdvance;
    LONG gl_XOffset;
    LONG gl_YOffset;
    UWORD gl_Face;          /* 0: the face shaped; n: its n-th fallback (OF_GetFallback) */
    UWORD gl_Flags;         /* OFGF_ below */
};                          /* 28 bytes */

#define OFGF_MISSING        0x0001  /* no face in the chain has it: glyph 0 of face 0 */
#define OFGF_UNSAFE_TO_BREAK 0x0002 /* breaking the run here changes the shaping */

/* One glyph's 8-bit coverage: 0 clear, 255 solid, one byte a pixel. */
struct OFBitmap {
    ULONG bm_Width;
    ULONG bm_Height;
    LONG bm_Left;           /* from the pen to the bitmap's left edge */
    LONG bm_Top;            /* from the baseline up to its top row */
    LONG bm_XAdvance;       /* 26.6 */
    ULONG bm_Stride;        /* bytes from one row to the next */
    UBYTE *bm_Pixels;       /* NULL for an empty glyph; free with OF_FreeBitmap */
};                          /* 28 bytes */

#define OFFF_ITALIC   0x0001
#define OFFF_FIXED    0x0002  /* every glyph the same width */
#define OFFF_SCALABLE 0x0004
#define OFFF_COLOR    0x0008

struct OFFaceInfo {
    char fi_Family[64];
    char fi_Style[64];
    UWORD fi_Weight;        /* 100 to 900: 400 regular, 700 bold */
    UWORD fi_Width;         /* 1 to 9: 5 normal */
    UWORD fi_Flags;         /* OFFF_ above */
    UWORD fi_UnitsPerEm;
    ULONG fi_PixelSize;
    ULONG fi_NumGlyphs;
    ULONG fi_NumFaces;      /* in its file: a collection holds several */
    LONG fi_Ascender;       /* 26.6, at the current size */
    LONG fi_Descender;      /* below the baseline: negative */
    LONG fi_Height;         /* baseline to baseline */
    ULONG fi_UnicodeRanges[4]; /* OS/2 ulUnicodeRange1-4, 0 when absent */
};                          /* 176 bytes */

/* Tags for OF_OpenFaceA and OF_FindFaceA */
#define OFA_Dummy     (TAG_USER + 0x4F460000)   /* 'OF' */
#define OFA_Index     (OFA_Dummy + 1)   /* ULONG: the face in a collection (0) */
#define OFA_Size      (OFA_Dummy + 2)   /* ULONG: pixel size to start at (16) */
#define OFA_Weight    (OFA_Dummy + 3)   /* ULONG: 100 to 900 (400); OF_FindFaceA */
#define OFA_Italic    (OFA_Dummy + 4)   /* BOOL (FALSE); OF_FindFaceA */
#define OFA_Fallbacks (OFA_Dummy + 5)   /* BOOL: add the default fallback chain (TRUE); OF_FindFaceA */
#define OFA_Error     (OFA_Dummy + 6)   /* LONG *: why it failed, an OFERR_ code */
/* Tags for OF_ShapeA (revision 3) */
#define OFA_Script    (OFA_Dummy + 7)   /* ULONG: ISO 15924 code as a tag ('Arab'); 0 from the text */
#define OFA_Language  (OFA_Dummy + 8)   /* ULONG: OpenType language system tag ('TRK '); 0 the default */
#define OFA_Direction (OFA_Dummy + 9)   /* ULONG: OFDIR_ below (OFDIR_AUTO) */
#define OFA_Features  (OFA_Dummy + 10)  /* struct OFFeature *: features to turn on or off */
#define OFA_NumFeatures (OFA_Dummy + 11) /* ULONG: how many (up to OF_MAX_FEATURES) */
#define OFA_Offset    (OFA_Dummy + 12)  /* ULONG: the run's start in the text, in its units (0) */
#define OFA_Length    (OFA_Dummy + 13)  /* ULONG: the run's length (to the end of the text) */
#define OFA_UTF16     (OFA_Dummy + 14)  /* BOOL: the text is UTF-16 (UWORDs), lengths and clusters in units (FALSE) */
/* Tag for OF_FindFaceA (revision 3) */
#define OFA_Exact     (OFA_Dummy + 15)  /* BOOL: only the family or its aliases, else NULL with OFERR_FONT (FALSE) */
/* revision 5, OF_DrawGlyphsA */
#define OFA_GPUError  (OFA_Dummy + 16)  /* LONG *: OpenGPU's error (OGPU_ERR_) for the last batch, 0 when it drew */
#define OFA_GPUStats  (OFA_Dummy + 17)  /* struct OFGPUStats *: the glyph cache's counts after the call */

/* OFA_Direction */
#define OFDIR_AUTO 0    /* from the run's script */
#define OFDIR_LTR  1
#define OFDIR_RTL  2    /* glyphs still come left to right, the run's last character first */

/* An OpenType feature over [of_Start, of_End) of the text, in its units
 * from the start of the whole text; 0 and OF_FEATURE_END cover all of it.
 * of_Value 0 turns it off, 1 on, n picks the n-th alternate. */
#define OF_FEATURE_END 0xFFFFFFFFUL
#define OF_MAKETAG(a, b, c, d) (((ULONG)(a) << 24) | ((ULONG)(b) << 16) | ((ULONG)(c) << 8) | (ULONG)(d))
#define OF_MAX_FEATURES 32
struct OFFeature {
    ULONG of_Tag;           /* 'liga', 'smcp' */
    ULONG of_Value;
    ULONG of_Start;
    ULONG of_End;
};                          /* 16 bytes */

/* A face's measurements at its pixel size: 26.6 pixels, y up */
struct OFFaceMetrics {
    ULONG fm_UnitsPerEm;
    ULONG fm_PixelSize;
    LONG fm_Ascender;
    LONG fm_Descender;          /* negative below the baseline */
    LONG fm_LineGap;
    LONG fm_XHeight;            /* OS/2, else measured from 'x' */
    LONG fm_CapHeight;          /* OS/2, else measured from 'H' */
    LONG fm_UnderlinePosition;  /* the top of the underline, negative below */
    LONG fm_UnderlineThickness;
    LONG fm_StrikeoutPosition;
    LONG fm_StrikeoutThickness;
    LONG fm_MaxAdvance;
    LONG fm_SpaceAdvance;       /* U+0020's, 0 when it has none */
    ULONG fm_Flags;             /* OFFF_ above */
};                              /* 56 bytes */

/* A glyph's ink box at the face's size: 26.6 pixels, y up */
struct OFGlyphExtents {
    LONG ge_XBearing;           /* from the pen to the box's left edge */
    LONG ge_YBearing;           /* from the baseline up to its top */
    LONG ge_Width;
    LONG ge_Height;
};                              /* 16 bytes */

/* A glyph's outline, for OF_GlyphOutline: verbs (OFPV_ below) and points
 * (x, y pairs of LONGs, 26.6 pixels at the face's size, y up). The
 * library fills what fits and always sets ol_NumVerbs and ol_NumPoints to
 * the whole outline's, returning OFERR_BUFFER when it did not fit. */
struct OFOutline {
    UBYTE *ol_Verbs;
    LONG *ol_Points;
    ULONG ol_MaxVerbs;
    ULONG ol_MaxPoints;         /* points: pairs, so ol_Points holds twice as many LONGs */
    ULONG ol_NumVerbs;
    ULONG ol_NumPoints;
};                              /* 24 bytes */

/* A colour glyph (an emoji), for OF_GlyphColorBitmap: the font's PNG
 * image for it, from the strike nearest the size asked, decoded through
 * the PNG datatype. Draw it scaled by your size / cb_PPEM. */
struct OFColorBitmap {
    ULONG *cb_Pixels;           /* ARGB (0xAARRGGBB, not premultiplied), cb_Width per row;
                                   free with OF_FreeColorBitmap */
    ULONG cb_Width;
    ULONG cb_Height;
    LONG cb_Left;               /* the image's left edge from the pen, in the strike's pixels */
    LONG cb_Top;                /* its top edge above the baseline */
    ULONG cb_PPEM;              /* the strike's pixels per em */
};                              /* 24 bytes */

/* A run of glyphs drawn through OpenGPU, for OF_DrawGlyphsA (revision 5).
 * Where: an OpenGPU surface (gt_Format: OGPU_FMT_ARGB32, XRGB32, RGB565 and
 * the other colour formats of <opengpu/stream.h>) at gt_Address, in memory
 * the back end reaches (the Amiga's own). Drawing stays inside the clip
 * rectangle and the surface; with gt_MaskAddress, also inside the A8 mask
 * covering gt_MaskX, gt_MaskY, gt_MaskW x gt_MaskH of the surface, whose
 * coverage multiplies what is drawn (a clip path drawn into an A8 surface:
 * outside it nothing is drawn). */
struct OFGPUTarget {
    APTR gt_Address;
    LONG gt_BytesPerRow;
    UWORD gt_Width;
    UWORD gt_Height;
    ULONG gt_Format;            /* OGPU_FMT_ */
    WORD gt_ClipX;
    WORD gt_ClipY;
    WORD gt_ClipW;
    WORD gt_ClipH;
    APTR gt_MaskAddress;        /* NULL: no mask */
    LONG gt_MaskBytesPerRow;
    WORD gt_MaskX;              /* where the mask's top-left pixel is on the surface */
    WORD gt_MaskY;
    WORD gt_MaskW;
    WORD gt_MaskH;
};                              /* 40 bytes */

/* The glyphs (of the face, as OF_ShapeA gives them) at gr_PixelSize, each
 * at its pen position (x, y pairs of LONGs: 24.8 surface pixels, the
 * baseline's origin, y down), in gr_Color (premultiplied 0xAARRGGBB) by
 * gr_Operator (an OGPU_PD_ operator; text uses OGPU_PD_OVER). With
 * gr_Matrix (four LONGs, 16.16: x' = m0 x + m1 y, y' = m2 x + m3 y, about
 * each pen position, y down) each glyph's outline is filled under it
 * (skewed, rotated or stretched text); without, glyphs come from the
 * glyph cache. */
struct OFGlyphRun {
    ULONG *gr_Glyphs;
    LONG *gr_Positions;
    ULONG gr_Count;
    ULONG gr_PixelSize;
    ULONG gr_Color;
    ULONG gr_Operator;
    LONG *gr_Matrix;            /* NULL: none */
    ULONG gr_Flags;             /* OFGR_ below */
};                              /* 32 bytes */

#define OFGR_ALIASED  0x0001    /* no anti-aliasing: each pixel in or out by its centre;
                                   only where OpenGPU says it takes OGPU_PATH_ALIASED
                                   (opengpu.library 0.24's OGPU_QUERY_FLAG), else OFERR_ARGUMENT */
#define OFGR_NOCACHE  0x0002    /* fill each glyph's outline onto the surface, past the cache */

/* The glyph cache's counts (OFA_GPUStats), since the library opened. */
struct OFGPUStats {
    ULONG gs_Filled;            /* glyphs filled into the cache */
    ULONG gs_Hits;              /* glyphs drawn from it */
    ULONG gs_Direct;            /* glyphs filled straight onto a surface */
    ULONG gs_Emptied;           /* times the cache was emptied for room */
};                              /* 16 bytes */

#define OFPV_MOVE  0    /* one point */
#define OFPV_LINE  1    /* one point */
#define OFPV_QUAD  2    /* two: control, end */
#define OFPV_CUBIC 3    /* three: control, control, end */
#define OFPV_CLOSE 4    /* none */

/* Errors */
#define OFERR_OK        0
#define OFERR_ARGUMENT  -1
#define OFERR_MEMORY    -2
#define OFERR_IO        -3
#define OFERR_FONT      -4
#define OFERR_RANGE     -5
#define OFERR_BUFFER    -6
#define OFERR_CACHE     -7
#define OFERR_EXEC      -8
#define OFERR_NOTPROCESS -9   /* the caller is a task, and the call reads files */
#define OFERR_NOGPU     -10   /* no opengpu.library 0.18 or later, or it doesn't fill paths for that format */
#define OFERR_GPU       -11   /* OpenGPU failed to draw (OFA_GPUError says how) */

#endif /* LIBRARIES_OPENFONT_H */
