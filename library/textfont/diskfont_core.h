/* GFOpenDiskFont's core: diskfont.library's work, no Amiga headers.
 * Copyright (c) 2026 AmiCradle Team. MIT.
 *
 * Clean-room: written from the diskfont autodocs' documented behaviour and
 * the NDK 3.2 layouts (diskfont/diskfont.h, diskfont/diskfonttag.h,
 * graphics/text.h), never from Kickstart, Hyperion or AROS code. Where
 * the autodocs leave a corner open, diskfont_core.cpp says "ABI test":
 * tests/DiskFontABITest settles it against OS 3.2.
 *
 * The core is lean C++ (gfopengcc docs/lean-cxx.md, library profile);
 * this header is plain C so the C library shell (opendiskfont_lib.c) and
 * the C tests use it as is. Everything the core needs from exec, dos and
 * graphics comes in through struct odf_env, so tests/m68k drives it under
 * qemu-m68k with fonts in host files. Nothing per call is kept in globals:
 * a library's state is its struct odf_ctx, in the library base, and its
 * font list is locked through the env (a SignalSemaphore on the Amiga).
 *
 * Types follow the 68k ABI: 32-bit pointers, big-endian, 2-byte alignment
 * of words, longs and pointers; every structure below is laid out as the
 * NDK lays it out, and diskfont_core.cpp checks each offset and size.
 */
#ifndef DISKFONT_CORE_H
#define DISKFONT_CORE_H

#ifdef __cplusplus
extern "C" {
#endif

typedef unsigned char  odf_u8;
typedef signed char    odf_s8;
typedef unsigned short odf_u16;
typedef signed short   odf_s16;
typedef unsigned long  odf_u32;
typedef signed long    odf_s32;

/* ---- utility/tagitem.h ------------------------------------------------- */
struct odf_TagItem { odf_u32 ti_Tag, ti_Data; };
#define ODF_TAG_DONE   0UL
#define ODF_TAG_IGNORE 1UL
#define ODF_TAG_MORE   2UL
#define ODF_TAG_SKIP   3UL
#define ODF_TAG_USER   0x80000000UL

/* ---- exec/nodes.h, exec/ports.h ---------------------------------------- */
struct odf_Node {
    struct odf_Node *ln_Succ, *ln_Pred;
    odf_u8 ln_Type;
    odf_s8 ln_Pri;
    char *ln_Name;
};                                     /* 14 bytes */
#define ODF_NT_FONT 12

struct odf_List {                      /* exec/lists.h */
    struct odf_Node *lh_Head, *lh_Tail, *lh_TailPred;
    odf_u8 lh_Type, l_pad;
};                                     /* 14 bytes */

struct odf_Message {
    struct odf_Node mn_Node;
    void *mn_ReplyPort;                /* tf_Extension, for a TextFont */
    odf_u16 mn_Length;
};                                     /* 20 bytes */

/* ---- graphics/text.h --------------------------------------------------- */
struct odf_TextAttr {
    char *ta_Name;
    odf_u16 ta_YSize;
    odf_u8 ta_Style, ta_Flags;
};                                     /* 8 bytes */

struct odf_TTextAttr {
    char *tta_Name;
    odf_u16 tta_YSize;
    odf_u8 tta_Style, tta_Flags;
    struct odf_TagItem *tta_Tags;
};                                     /* 12 bytes */

#define ODF_FSF_UNDERLINED 0x01
#define ODF_FSF_BOLD       0x02
#define ODF_FSF_ITALIC     0x04
#define ODF_FSF_EXTENDED   0x08
#define ODF_FSF_COLORFONT  0x40
#define ODF_FSF_TAGGED     0x80

#define ODF_FPF_ROMFONT      0x01
#define ODF_FPF_DISKFONT     0x02
#define ODF_FPF_REVPATH      0x04
#define ODF_FPF_TALLDOT      0x08
#define ODF_FPF_WIDEDOT      0x10
#define ODF_FPF_PROPORTIONAL 0x20
#define ODF_FPF_DESIGNED     0x40
#define ODF_FPF_REMOVED      0x80

#define ODF_TA_DeviceDPI (ODF_TAG_USER | 1)   /* (x dpi << 16) | y dpi */
#define ODF_MAXFONTMATCHWEIGHT 32767

struct odf_TextFont {
    struct odf_Message tf_Message;     /* ln_Name is the font's name */
    odf_u16 tf_YSize;                  /* 20 */
    odf_u8 tf_Style, tf_Flags;         /* 22, 23 */
    odf_u16 tf_XSize, tf_Baseline;     /* 24, 26 */
    odf_u16 tf_BoldSmear, tf_Accessors;/* 28, 30 */
    odf_u8 tf_LoChar, tf_HiChar;       /* 32, 33 */
    void *tf_CharData;                 /* 34 */
    odf_u16 tf_Modulo;                 /* 38 */
    void *tf_CharLoc;                  /* 40: (bit offset << 16 | width) per glyph */
    void *tf_CharSpace;                /* 44: WORD per glyph, or NULL */
    void *tf_CharKern;                 /* 48: WORD per glyph, or NULL */
};                                     /* 52 bytes */

struct odf_TextFontExtension {
    odf_u16 tfe_MatchWord;
    odf_u8 tfe_Flags0, tfe_Flags1;
    struct odf_TextFont *tfe_BackPtr;
    void *tfe_OrigReplyPort;
    struct odf_TagItem *tfe_Tags;
    odf_u16 *tfe_OFontPatchS, *tfe_OFontPatchK;
};                                     /* 24 bytes */

struct odf_ColorFontColors {
    odf_u16 cfc_Reserved, cfc_Count;
    odf_u16 *cfc_ColorTable;
};                                     /* 8 bytes */

struct odf_ColorTextFont {
    struct odf_TextFont ctf_TF;
    odf_u16 ctf_Flags;                 /* 52 */
    odf_u8 ctf_Depth, ctf_FgColor, ctf_Low, ctf_High, ctf_PlanePick, ctf_PlaneOnOff;
    struct odf_ColorFontColors *ctf_ColorFontColors;   /* 60 */
    void *ctf_CharData[8];             /* 64 */
};                                     /* 96 bytes */

/* ---- diskfont/diskfont.h ----------------------------------------------- */
#define ODF_MAXFONTPATH 256
#define ODF_MAXFONTNAME 32

struct odf_FontContents {
    char fc_FileName[ODF_MAXFONTPATH];
    odf_u16 fc_YSize;
    odf_u8 fc_Style, fc_Flags;
};                                     /* 260 bytes */

/* With tfc_TagCount (TAG_DONE included) non-zero, the tags end where
 * fc_FileName would: they start at &tfc_FileName[MAXFONTPATH - 8 *
 * tfc_TagCount], so tfc_TagCount itself is the low word of TAG_DONE's
 * ti_Data. */
struct odf_TFontContents {
    char tfc_FileName[ODF_MAXFONTPATH - 2];
    odf_u16 tfc_TagCount;
    odf_u16 tfc_YSize;
    odf_u8 tfc_Style, tfc_Flags;
};                                     /* 260 bytes */

#define ODF_FCH_ID  0x0f00             /* FontContents follow */
#define ODF_TFCH_ID 0x0f02             /* TFontContents follow */
#define ODF_OFCH_ID 0x0f03             /* TFontContents, and an outline (.otag) */

struct odf_FontContentsHeader {
    odf_u16 fch_FileID, fch_NumEntries;
};                                     /* 4 bytes, then the entries */

#define ODF_DFH_ID 0x0f80

/* In a font file the DiskFontHeader is the first hunk's, after its
 * 4-byte return code (MOVEQ #-1,D0; RTS); once loaded it is 8 bytes past
 * the segment's start (the next segment's BPTR, then that code). */
struct odf_DiskFontHeader {
    struct odf_Node dfh_DF;            /* 0 */
    odf_u16 dfh_FileID;                /* 14: DFH_ID */
    odf_u16 dfh_Revision;              /* 16 */
    odf_s32 dfh_Segment;               /* 18: dfh_TagList in the file when tf_Style has FSF_TAGGED */
    char dfh_Name[ODF_MAXFONTNAME];    /* 22 */
    struct odf_TextFont dfh_TF;        /* 54 */
};                                     /* 106 bytes */

#define ODF_AFF_MEMORY 0x0001
#define ODF_AFF_DISK   0x0002
#define ODF_AFF_SCALED 0x0004
#define ODF_AFF_BITMAP 0x0008
#define ODF_AFF_OTAG   0x0010          /* outline fonts only (V46) */
#define ODF_AFF_TYPE   0x0040          /* af_Type says AFF_BITMAP or AFF_OTAG (V46) */
#define ODF_AFF_TAGGED 0x10000UL

struct odf_AvailFonts {
    odf_u16 af_Type;
    struct odf_TextAttr af_Attr;
};                                     /* 10 bytes */

struct odf_TAvailFonts {
    odf_u16 taf_Type;
    struct odf_TTextAttr taf_Attr;
};                                     /* 14 bytes */

struct odf_AvailFontsHeader { odf_u16 afh_NumEntries; };   /* then the entries */

/* GetDiskFontCtrl and SetDiskFontCtrlA (V45). ABI test: the values, as
 * the NDK names them, are checked by DiskFontABITest. */
#define ODF_DFCTRL_BASE       (ODF_TAG_USER + 0x0B000000UL)
#define ODF_DFCTRL_XDPI       (ODF_DFCTRL_BASE + 1)
#define ODF_DFCTRL_YDPI       (ODF_DFCTRL_BASE + 2)
#define ODF_DFCTRL_XDOTP      (ODF_DFCTRL_BASE + 3)
#define ODF_DFCTRL_YDOTP      (ODF_DFCTRL_BASE + 4)
#define ODF_DFCTRL_CACHE      (ODF_DFCTRL_BASE + 5)
#define ODF_DFCTRL_SORTMODE   (ODF_DFCTRL_BASE + 6)
#define ODF_DFCTRL_CACHEFLUSH (ODF_DFCTRL_BASE + 7)
#define ODF_DFCTRL_CHARSET    (ODF_DFCTRL_BASE + 8)
#define ODF_DFCTRL_SORT_OFF        0
#define ODF_DFCTRL_SORT_ASCENDING  1
#define ODF_DFCTRL_SORT_DESCENDING (-1)

/* ObtainCharsetInfo's "known" and "wanted" (V46), as NDK 3.2's
 * diskfont/diskfonttag.h numbers them: DFCTRL_BASE + 0 to 4 */
#define ODF_DFCS_NUMBER     (ODF_DFCTRL_BASE + 0)
#define ODF_DFCS_NEXTNUMBER (ODF_DFCTRL_BASE + 1)
#define ODF_DFCS_NAME       (ODF_DFCTRL_BASE + 2)
#define ODF_DFCS_MIMENAME   (ODF_DFCTRL_BASE + 3)
#define ODF_DFCS_MAPTABLE   (ODF_DFCTRL_BASE + 4)

/* ---- diskfont/diskfonttag.h, diskfont/glyph.h, diskfont/oterrors.h ------
 * The outline engines' tags (an .otag file is a tag list; OT_Indirect
 * tags' data are offsets from the file's start until it is loaded). */
#define ODF_OT_Level0      ODF_TAG_USER
#define ODF_OT_Level1      (ODF_TAG_USER | 0x1000)
#define ODF_OT_Indirect    0x8000
#define ODF_OT_DeviceDPI   (ODF_OT_Level0 | 0x01)
#define ODF_OT_DotSize     (ODF_OT_Level0 | 0x02)
#define ODF_OT_PointHeight (ODF_OT_Level0 | 0x08)
#define ODF_OT_GlyphCode   (ODF_OT_Level0 | 0x11)
#define ODF_OT_OTagPath    (ODF_OT_Level0 | ODF_OT_Indirect | 0x14)
#define ODF_OT_OTagList    (ODF_OT_Level0 | ODF_OT_Indirect | 0x15)
#define ODF_OT_GlyphMap    (ODF_OT_Level0 | ODF_OT_Indirect | 0x20)
#define ODF_OT_FileIdent   (ODF_OT_Level1 | 0x01)
#define ODF_OT_YSizeFactor (ODF_OT_Level1 | 0x11)   /* high word: point height term, low: black height term */
#define ODF_OT_Engine      (ODF_OT_Level1 | ODF_OT_Indirect | 0x02)
#define ODF_OT_AvailSizes  (ODF_OT_Level1 | ODF_OT_Indirect | 0x20)
#define ODF_OT_MAXAVAILSIZES 20
#define ODF_OTE_Bullet     "bullet"
#define ODF_OTSUFFIX       ".otag"
#define ODF_OTERR_Success  0L
#define ODF_OTERR_Failure  (-1L)
#define ODF_OFF_OPEN       0x00000001UL   /* OpenOutlineFont: open the engine too */

/* An engine library's own calls (bullet.library's), by LVO */
#define ODF_LVO_OpenEngine   (-30)
#define ODF_LVO_CloseEngine  (-36)
#define ODF_LVO_SetInfoA     (-42)
#define ODF_LVO_ObtainInfoA  (-48)
#define ODF_LVO_ReleaseInfoA (-54)

struct odf_GlyphEngine {
    void *gle_Library;
    char *gle_Name;
};                                     /* 8 bytes, then the engine's own */

struct odf_GlyphMap {
    odf_u16 glm_BMModulo, glm_BMRows;              /* 0, 2 */
    odf_u16 glm_BlackLeft, glm_BlackTop;           /* 4, 6 */
    odf_u16 glm_BlackWidth, glm_BlackHeight;       /* 8, 10 */
    odf_s32 glm_XOrigin, glm_YOrigin;              /* 12, 16: FIXED */
    odf_s16 glm_X0, glm_Y0, glm_X1, glm_Y1;        /* 20 .. 26 */
    odf_s32 glm_Width;                             /* 28: FIXED */
    odf_u8 *glm_BitMap;                            /* 32 */
};                                     /* 36 bytes */

struct odf_EGlyphEngine {              /* NDK 3.2 diskfont/diskfont.h */
    void *ege_Reserved;                /* 0: the system's */
    void *ege_BulletBase;              /* 4: the engine library */
    struct odf_GlyphEngine *ege_GlyphEngine;   /* 8 */
};                                     /* 12 bytes */

struct odf_OutlineFont {
    char *olf_OTagPath;                /* 0 */
    struct odf_TagItem *olf_OTagList;  /* 4: the .otag, loaded */
    char *olf_EngineName;              /* 8: "bullet" */
    char *olf_LibraryName;             /* 12: "bullet.library" */
    struct odf_EGlyphEngine olf_EEngine;   /* 16 */
    void *olf_Reserved;                /* 28 */
    void *olf_UserData;                /* 32: the caller's */
};                                     /* 36 bytes (the shell checks the NDK's) */

/* hunk file blocks (dos/doshunks.h) */
#define ODF_HUNK_CODE    0x3E9
#define ODF_HUNK_DATA    0x3EA
#define ODF_HUNK_BSS     0x3EB
#define ODF_HUNK_RELOC32 0x3EC
#define ODF_HUNK_SYMBOL  0x3F0
#define ODF_HUNK_DEBUG   0x3F1
#define ODF_HUNK_END     0x3F2
#define ODF_HUNK_HEADER  0x3F3

/* ---- the env: exec, dos and graphics ------------------------------------ */

struct odf_ctx;
/* A directory is the env's handle for one (a Lock on the Amiga); a
 * segment is LoadSeg's seglist. */
typedef int (*odf_dir_each)(void *data, const char *name, int is_dir);
typedef int (*odf_font_each)(void *data, struct odf_TextFont *tf);

struct odf_env {
    /* exec */
    void *(*alloc)(struct odf_ctx *c, odf_u32 size);      /* cleared, public; NULL when out */
    void (*free)(struct odf_ctx *c, void *mem);          /* NULL is allowed */
    void (*lock)(struct odf_ctx *c);                     /* the library's font list semaphore */
    void (*unlock)(struct odf_ctx *c);
    /* dos. dir_lock: Lock(name) inside dir (CurrentDir), or name as is
     * when dir is NULL; NULL when there is none. */
    void *(*dir_lock)(struct odf_ctx *c, void *dir, const char *name);
    void (*dir_unlock)(struct odf_ctx *c, void *dir);
    /* calls each(data, name, is_dir) for every entry of dir, stopping
     * when it returns non-zero; 0 when dir can't be read */
    int (*dir_scan)(struct odf_ctx *c, void *dir, odf_dir_each each, void *data);
    /* calls each(data, dir) for every directory of FONTS: in order */
    int (*fonts_scan)(struct odf_ctx *c, int (*each)(void *data, void *dir), void *data);
    /* a whole file, in memory from alloc (free it with free); NULL when none */
    void *(*file_load)(struct odf_ctx *c, void *dir, const char *name, odf_u32 *len);
    int (*file_save)(struct odf_ctx *c, void *dir, const char *name, const void *buf, odf_u32 len);
    /* LoadSeg: the seglist, and *first the first hunk's memory */
    void *(*seg_load)(struct odf_ctx *c, void *dir, const char *name, odf_u8 **first);
    void (*seg_unload)(struct odf_ctx *c, void *seg);
    /* graphics: walks GfxBase->TextFonts (each returns non-zero to stop) */
    void (*mem_fonts)(struct odf_ctx *c, odf_font_each each, void *data);
    struct odf_TextFont *(*open_font)(struct odf_ctx *c, struct odf_TextFont *tf); /* GFOpenFont giving tf */
    void (*close_font)(struct odf_ctx *c, struct odf_TextFont *tf);
    void (*add_font)(struct odf_ctx *c, struct odf_TextFont *tf);
    /* RemFont, only when nobody has tf open (tf_Accessors 0, checked with
     * the system's font list held); 0 when it is in use */
    int (*rem_font)(struct odf_ctx *c, struct odf_TextFont *tf);
    int (*extend_font)(struct odf_ctx *c, struct odf_TextFont *tf, const struct odf_TagItem *tags);
    void (*strip_font)(struct odf_ctx *c, struct odf_TextFont *tf);
    /* graphics' WeighTAMatch(req, font, fonttags): the core asks it only
     * for a font of the request's YSize. NULL: the core's own weights
     * (odf_Weigh), which are also the tests' model of graphics'. */
    odf_u32 (*weigh)(struct odf_ctx *c, const struct odf_TTextAttr *req, const struct odf_TextAttr *font,
                     const struct odf_TagItem *fonttags);
    /* exec: an engine library (OpenLibrary(name, 0)), and one of its calls
     * by LVO with A6 its base, A0 and A1 given, D0 the answer */
    void *(*lib_open)(struct odf_ctx *c, const char *name);
    void (*lib_close)(struct odf_ctx *c, void *lib);
    odf_u32 (*lib_call)(struct odf_ctx *c, void *lib, odf_s32 lvo, void *a0, void *a1);
    /* dos: the full name of name in dir (NameFromLock, AddPart) into out;
     * 0 when it doesn't fit. NULL: names stay as the caller gave them. */
    int (*name_of)(struct odf_ctx *c, void *dir, const char *name, char *out, odf_u32 room);
};

/* The seam for outline fonts. An outline .font (OFCH_ID) has an .otag
 * beside it naming its engine. OpenDiskFont asks the seam first for an
 * OFCH font at a size the .font doesn't list (and no FPF_DESIGNED); it
 * returns a DiskFontHeader made as NewScaledDiskFont's are (alloc'd,
 * dfh_Segment 0, or the DPI tags there with FSF_TAGGED), or NULL, and
 * then the bitmap sizes the .font lists are scaled like any other's.
 * odf_Init sets odf_engine_outline: the .otag's engine (bullet.library)
 * through the env draws every glyph. GFOpenFont (gfopenfont.library) is meant
 * to replace it. dir is the directory the .font was found in; ta's name
 * is the request's. */
struct odf_outline {
    struct odf_DiskFontHeader *(*outline_open)(struct odf_ctx *c, void *dir, const char *fontname,
                                               const struct odf_TTextAttr *ta);
};
extern const struct odf_outline odf_engine_outline;

#define ODF_NCTRL 8
struct odf_ctx {
    const struct odf_env *env;
    void *user;                            /* the env's own: the library base, or a test's state */
    const struct odf_outline *outline;     /* NULL: no outline fonts but their bitmap sizes */
    struct odf_DiskFontHeader *loaded;     /* fonts loaded or scaled here, through dfh_DF.ln_Succ */
    odf_u32 busy;                          /* calls inside the font list's lock: an expunge in
                                              one of them (the same task) flushes nothing */
    odf_s32 ctrl[ODF_NCTRL];               /* DFCTRL_XDPI ... DFCTRL_CHARSET */
};

/* ---- the calls ------------------------------------------------------------ */

void odf_Init(struct odf_ctx *c, const struct odf_env *env, void *user);
struct odf_TextFont *odf_OpenDiskFont(struct odf_ctx *c, const struct odf_TextAttr *ta);
odf_s32 odf_AvailFonts(struct odf_ctx *c, void *buf, odf_s32 bytes, odf_u32 flags);
struct odf_FontContentsHeader *odf_NewFontContents(struct odf_ctx *c, void *fontsLock, const char *fontName);
void odf_DisposeFontContents(struct odf_ctx *c, struct odf_FontContentsHeader *fch);
struct odf_DiskFontHeader *odf_NewScaledDiskFont(struct odf_ctx *c, struct odf_TextFont *src,
                                                 const struct odf_TextAttr *dest);
odf_s32 odf_GetDiskFontCtrl(struct odf_ctx *c, odf_u32 tag);
void odf_SetDiskFontCtrlA(struct odf_ctx *c, const struct odf_TagItem *tags);
odf_s32 odf_WriteFontContents(struct odf_ctx *c, void *fontsLock, const char *fontName,
                              const struct odf_FontContentsHeader *fch);
odf_s32 odf_WriteDiskFontHeaderA(struct odf_ctx *c, const struct odf_TextFont *tf, const char *fileName,
                                 const struct odf_TagItem *tags);
/* Expunge: removes and frees every font this library loaded or scaled that
 * nobody has open; 1 when none is left. Nothing, when called (from low
 * memory's expunge) inside one of this library's own calls. */
int odf_Flush(struct odf_ctx *c);

/* The outline calls (V47's E calls; OpenOutlineFont V46) */
struct odf_GlyphEngine *odf_EOpenEngine(struct odf_ctx *c, struct odf_EGlyphEngine *ege);
void odf_ECloseEngine(struct odf_ctx *c, struct odf_EGlyphEngine *ege);
odf_u32 odf_ESetInfoA(struct odf_ctx *c, struct odf_EGlyphEngine *ege, const struct odf_TagItem *tags);
odf_u32 odf_EObtainInfoA(struct odf_ctx *c, struct odf_EGlyphEngine *ege, const struct odf_TagItem *tags);
odf_u32 odf_EReleaseInfoA(struct odf_ctx *c, struct odf_EGlyphEngine *ege, const struct odf_TagItem *tags);
struct odf_OutlineFont *odf_OpenOutlineFont(struct odf_ctx *c, const char *name, void *list, odf_u32 flags);
void odf_CloseOutlineFont(struct odf_ctx *c, struct odf_OutlineFont *olf, void *list);
/* value is a number or a string (a pointer), as known says; the answer
 * likewise, as wanted says; 0 when unknown */
odf_u32 odf_ObtainCharsetInfo(struct odf_ctx *c, odf_u32 known, odf_u32 value, odf_u32 wanted);

/* ---- the parts, for the tests -------------------------------------------- */

/* The DiskFontHeader in a loaded font's first hunk, checked; NULL if it
 * isn't a font. */
struct odf_DiskFontHeader *odf_FontHeader(odf_u8 *first);
/* The weight of a font (ysize, style, flags, dpi; dpi 0 = none) for a
 * request: 0 when the sizes differ, MAXFONTMATCHWEIGHT when it is exact. */
odf_u32 odf_Weigh(odf_u16 ry, odf_u8 rs, odf_u8 rf, odf_u32 rdpi,
                  odf_u16 fy, odf_u8 fs, odf_u8 ff, odf_u32 fdpi);
/* A font's TA_DeviceDPI (its extension's tags), or 0 */
odf_u32 odf_FontDPI(const struct odf_TextFont *tf);
/* ti_Data of tag in a list (TAG_MORE, TAG_SKIP and TAG_IGNORE followed) */
int odf_FindTag(const struct odf_TagItem *tags, odf_u32 tag, odf_u32 *data);
/* One glyph's pixel at (x, y) in a plane (0 for a plain font), with the
 * glyph's index (char - lo; hi - lo + 1 is the default glyph) */
int odf_Pixel(const struct odf_TextFont *tf, unsigned plane, unsigned glyph, unsigned x, unsigned y);
/* An .otag file loaded (alloc'd, len bytes): checked (OT_FileIdent is its
 * length) and its OT_Indirect tags made into addresses; NULL if it isn't
 * one (then it is freed) */
struct odf_TagItem *odf_OTagLoad(struct odf_ctx *c, void *mem, odf_u32 len);
/* The font as a loadable hunk file (WriteDiskFontHeaderA's), alloc'd */
odf_u8 *odf_FontFile(struct odf_ctx *c, const struct odf_TextFont *tf, const struct odf_TagItem *tags,
                     odf_u32 *len);

#ifdef __cplusplus
}
#endif
#endif
