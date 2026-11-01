/* gfopenfont.library's Amiga fonts: OF_OpenTextFont gives a program the
 * TextFont that diskfont.library's OpenDiskFont would, without
 * diskfont.library or bullet.library. A bitmap font is read from FONTS:,
 * matched and scaled by GFOpenDiskFont's core; an outline font (an .otag
 * beside its .font) is drawn by GFOpenFont, through GFOpenBullet's core: the
 * .otag's tags, the size, DPI, shear and emboldening, glyph maps from
 * GFOpenFont's coverage.
 *
 * The two cores are AmiCradle's (amigachrome-guest libraries/opendiskfont
 * and libraries/openbullet), copied into textfont/ unchanged: lean C++,
 * integer only, everything they need through their env. Here the env is
 * exec, dos and graphics for diskfont's, and GFOpenFont's own functions for
 * bullet's, called in this library, under its lock, on its stack. The
 * engine diskfont's core opens by name, "bullet.library", is that core:
 * nothing is opened. Any other engine an .otag names is opened as
 * diskfont.library would open it.
 *
 * Copyright (c) 2026 AmiCradle Team. OpenFont, MIT licence (LICENSE). */
#include <exec/types.h>
#include <exec/memory.h>
#include <exec/libraries.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <graphics/gfxbase.h>
#include <graphics/text.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/graphics.h>

#include <string.h>

#include "of_textfont.h"
#include "textfont/bullet_core.h"

_Static_assert(sizeof(struct TextFont) == sizeof(struct odf_TextFont), "TextFont");
_Static_assert(sizeof(struct TextAttr) == sizeof(struct odf_TextAttr), "TextAttr");
_Static_assert(sizeof(struct obl_a8) == sizeof(OFBitmap), "obl_a8 is OFBitmap");
_Static_assert(__builtin_offsetof(struct obl_a8, pixels) == __builtin_offsetof(OFBitmap, pixels), "obl_a8.pixels");

struct GfxBase *GfxBase;

static const char fonts_name[] = "FONTS:";
static const char bullet_name[] = "bullet.library";
static const char engine_name[] = "bullet";

#define T(c) ((struct OFTextFonts *)(c)->user)

/* ---- diskfont's env: exec ---------------------------------------------- */

static void *e_alloc(struct odf_ctx *c, odf_u32 size)
{
    (void)c;
    return AllocVec(size, MEMF_PUBLIC | MEMF_CLEAR);
}

static void e_free(struct odf_ctx *c, void *mem)
{
    (void)c;
    if (mem) FreeVec(mem);
}

static void e_lock(struct odf_ctx *c) { ObtainSemaphore(&T(c)->sem); }
static void e_unlock(struct odf_ctx *c) { ReleaseSemaphore(&T(c)->sem); }

/* ---- diskfont's env: dos ----------------------------------------------- */

static void *e_dir_lock(struct odf_ctx *c, void *dir, const char *name)
{
    BPTR l, old = 0;
    (void)c;
    if (dir) old = CurrentDir((BPTR)dir);
    l = Lock((STRPTR)name, SHARED_LOCK);
    if (dir) CurrentDir(old);
    return (void *)l;
}

static void e_dir_unlock(struct odf_ctx *c, void *dir)
{
    (void)c;
    UnLock((BPTR)dir);
}

static int e_dir_scan(struct odf_ctx *c, void *dir, odf_dir_each each, void *data)
{
    struct FileInfoBlock *fib = AllocDosObject(DOS_FIB, NULL);
    int ok = 0;
    (void)c;
    if (!fib) return 0;
    if (Examine((BPTR)dir, fib) && fib->fib_DirEntryType > 0) {
        ok = 1;
        while (ExNext((BPTR)dir, fib))
            if (each(data, (const char *)fib->fib_FileName, fib->fib_DirEntryType > 0)) break;
    }
    FreeDosObject(DOS_FIB, fib);
    return ok;
}

/* each directory of the FONTS: assign, in its order */
static int e_fonts_scan(struct odf_ctx *c, int (*each)(void *data, void *dir), void *data)
{
    struct DevProc *dp = NULL;
    (void)c;
    while ((dp = GetDeviceProc((STRPTR)fonts_name, dp)) != NULL) {
        if (dp->dvp_Lock && each(data, (void *)dp->dvp_Lock)) break;
        if (!(dp->dvp_Flags & DVPF_ASSIGN)) break;
    }
    if (dp) FreeDeviceProc(dp);
    return 1;
}

static void *e_file_load(struct odf_ctx *c, void *dir, const char *name, odf_u32 *len)
{
    BPTR fh, old = 0;
    LONG n;
    void *m = NULL;
    if (dir) old = CurrentDir((BPTR)dir);
    fh = Open((STRPTR)name, MODE_OLDFILE);
    if (dir) CurrentDir(old);
    if (!fh) return NULL;
    if (Seek(fh, 0, OFFSET_END) >= 0 && (n = Seek(fh, 0, OFFSET_BEGINNING)) >= 0 &&
        (m = e_alloc(c, (odf_u32)n + 1)) != NULL) {
        if (Read(fh, m, n) != n) { e_free(c, m); m = NULL; }
        else *len = (odf_u32)n;
    }
    Close(fh);
    return m;
}

static int e_name_of(struct odf_ctx *c, void *dir, const char *name, char *out, odf_u32 room)
{
    (void)c;
    if (!dir || room < 2) return 0;
    if (!NameFromLock((BPTR)dir, (STRPTR)out, (LONG)room)) return 0;
    return AddPart((STRPTR)out, (CONST_STRPTR)name, room) != 0;
}

static int e_file_save(struct odf_ctx *c, void *dir, const char *name, const void *buf, odf_u32 len)
{
    BPTR fh, old = 0;
    int ok;
    (void)c;
    if (dir) old = CurrentDir((BPTR)dir);
    fh = Open((STRPTR)name, MODE_NEWFILE);
    if (dir) CurrentDir(old);
    if (!fh) return 0;
    ok = Write(fh, (APTR)buf, (LONG)len) == (LONG)len;
    return Close(fh) && ok;
}

static void *e_seg_load(struct odf_ctx *c, void *dir, const char *name, odf_u8 **first)
{
    BPTR seg, old = 0;
    (void)c;
    if (dir) old = CurrentDir((BPTR)dir);
    seg = LoadSeg((STRPTR)name);
    if (dir) CurrentDir(old);
    if (seg) *first = (odf_u8 *)BADDR(seg) + 4;
    return (void *)seg;
}

static void e_seg_unload(struct odf_ctx *c, void *seg)
{
    (void)c;
    UnLoadSeg((BPTR)seg);
}

/* ---- diskfont's env: graphics ------------------------------------------ */

static void e_mem_fonts(struct odf_ctx *c, odf_font_each each, void *data)
{
    struct Node *n;
    (void)c;
    Forbid();
    for (n = GfxBase->TextFonts.lh_Head; n->ln_Succ; n = n->ln_Succ)
        if (each(data, (struct odf_TextFont *)n)) break;
    Permit();
}

static struct odf_TextFont *e_open_font(struct odf_ctx *c, struct odf_TextFont *tf)
{
    (void)c;
    Forbid();
    tf->tf_Accessors++;
    Permit();
    return tf;
}

static void e_close_font(struct odf_ctx *c, struct odf_TextFont *tf)
{
    (void)c;
    CloseFont((struct TextFont *)tf);
}

static void e_add_font(struct odf_ctx *c, struct odf_TextFont *tf)
{
    (void)c;
    AddFont((struct TextFont *)tf);
}

static int e_rem_font(struct odf_ctx *c, struct odf_TextFont *tf)
{
    int ok = 0;
    (void)c;
    Forbid();
    if (!tf->tf_Accessors) {
        if (!(tf->tf_Flags & FPF_REMOVED)) RemFont((struct TextFont *)tf);
        ok = 1;
    }
    Permit();
    return ok;
}

static int e_extend_font(struct odf_ctx *c, struct odf_TextFont *tf, const struct odf_TagItem *tags)
{
    (void)c;
    return ExtendFont((struct TextFont *)tf, (struct TagItem *)tags) != 0;
}

static void e_strip_font(struct odf_ctx *c, struct odf_TextFont *tf)
{
    (void)c;
    StripFont((struct TextFont *)tf);
}

static odf_u32 e_weigh(struct odf_ctx *c, const struct odf_TTextAttr *req, const struct odf_TextAttr *font,
                       const struct odf_TagItem *fonttags)
{
    WORD w;
    (void)c;
    w = WeighTAMatch((struct TextAttr *)req, (struct TextAttr *)font, (struct TagItem *)fonttags);
    return w > 0 ? (odf_u32)w : 0;
}

/* ---- bullet's env: GFOpenFont, in this library ----------------------------- */

static void *b_alloc(void *user, obl_u32 size)
{
    (void)user;
    return AllocVec(size, MEMF_PUBLIC | MEMF_CLEAR);
}

static void b_free(void *user, void *mem)
{
    (void)user;
    if (mem) FreeVec(mem);
}

/* the first face of the outline file, when it is scalable; no requester:
 * a volume that isn't there is a file that isn't there */
static void *b_face_open(void *user, const char *path, obl_u32 *upm)
{
    struct OFTextFonts *t = (struct OFTextFonts *)user;
    struct Process *me = (struct Process *)FindTask(NULL);
    OFFaceInfo info;
    OFFace *face;
    APTR win = me->pr_WindowPtr;
    int err;
    me->pr_WindowPtr = (APTR)-1;
    face = of_face_open_file(t->of, path, 0, &err);
    me->pr_WindowPtr = win;
    if (!face) return NULL;
    if (of_face_info(face, &info) != OF_OK || !(info.flags & OF_FACE_SCALABLE)) {
        of_face_close(face);
        return NULL;
    }
    *upm = info.units_per_em;
    return face;
}

static void b_face_close(void *user, void *face)
{
    (void)user;
    of_face_close((OFFace *)face);
}

static obl_u32 b_char_index(void *user, void *face, obl_u32 code)
{
    (void)user;
    return of_face_char_index((OFFace *)face, code);
}

static obl_s32 b_advance(void *user, void *face, obl_u32 glyph)
{
    (void)user;
    return of_face_glyph_advance((OFFace *)face, glyph);
}

static obl_s32 b_raster(void *user, void *face, obl_u32 glyph, const obl_s32 m[4], struct obl_a8 *out)
{
    (void)user;
    return of_raster_glyph_matrix_a8((OFFace *)face, glyph, (const int32_t *)m, (OFBitmap *)out);
}

static void b_raster_free(void *user, void *face, struct obl_a8 *a8)
{
    (void)user;
    (void)face;
    if (a8->pixels) of_bitmap_release((OFBitmap *)a8);
}

/* a code as UTF-8; its length */
static size_t utf8(char *o, obl_u32 c)
{
    if (c < 0x80) { o[0] = (char)c; return 1; }
    if (c < 0x800) { o[0] = (char)(0xC0 | (c >> 6)); o[1] = (char)(0x80 | (c & 0x3F)); return 2; }
    if (c < 0x10000) {
        o[0] = (char)(0xE0 | (c >> 12)); o[1] = (char)(0x80 | ((c >> 6) & 0x3F)); o[2] = (char)(0x80 | (c & 0x3F));
        return 3;
    }
    o[0] = (char)(0xF0 | ((c >> 18) & 7)); o[1] = (char)(0x80 | ((c >> 12) & 0x3F));
    o[2] = (char)(0x80 | ((c >> 6) & 0x3F)); o[3] = (char)(0x80 | (c & 0x3F));
    return 4;
}

/* The pair shaped at one pixel to the font unit: the first glyph's shaped
 * advance less its own is the kerning, in font units (as GFOpenBullet). */
static obl_s32 b_kern(void *user, void *face, obl_u32 left, obl_u32 right)
{
    OFFace *f = (OFFace *)face;
    OFFaceInfo info;
    OFGlyph g[4];
    char text[8];
    size_t n;
    int32_t shaped, own;
    (void)user;
    if (of_face_info(f, &info) != OF_OK || of_face_set_pixel_size(f, info.units_per_em) != OF_OK) return 0;
    n = utf8(text, left);
    n += utf8(text + n, right);
    if (of_shape_utf8(f, text, n, g, 4) != 2) return 0;
    own = of_face_glyph_advance(f, g[0].glyph_id);
    if (own < 0) return 0;
    shaped = g[0].x_advance_26_6 - own * 64;
    return shaped >= 0 ? (shaped + 32) >> 6 : -((-shaped + 32) >> 6);
}

static const struct obl_env bullet_env = {
    b_alloc, b_free, b_face_open, b_face_close, b_char_index, b_advance, b_raster, b_raster_free, b_kern
};

/* ---- diskfont's env: the outline engine ---------------------------------- */

/* "bullet.library" is GFOpenBullet's core, here: this token stands for it */
static const char bullet_token = 0;
#define IS_BULLET(lib) ((const void *)(lib) == (const void *)&bullet_token)

#define LVO_OpenEngine   (-30)
#define LVO_CloseEngine  (-36)
#define LVO_SetInfoA     (-42)
#define LVO_ObtainInfoA  (-48)
#define LVO_ReleaseInfoA (-54)

static void *e_lib_open(struct odf_ctx *c, const char *name)
{
    (void)c;
    if (!strcmp(name, bullet_name)) return (void *)&bullet_token;
    return OpenLibrary((CONST_STRPTR)name, 0);
}

static void e_lib_close(struct odf_ctx *c, void *lib)
{
    (void)c;
    if (!IS_BULLET(lib)) CloseLibrary((struct Library *)lib);
}

static odf_u32 e_lib_call(struct odf_ctx *c, void *lib, odf_s32 lvo, void *p0, void *p1)
{
    if (IS_BULLET(lib)) {
        struct obl_GlyphEngine *ge = (struct obl_GlyphEngine *)p0;
        const struct obl_TagItem *tags = (const struct obl_TagItem *)p1;
        switch (lvo) {
        case LVO_OpenEngine:   return (odf_u32)obl_OpenEngine(&bullet_env, T(c), (void *)&bullet_token, engine_name);
        case LVO_CloseEngine:  obl_CloseEngine(ge); return 0;
        case LVO_SetInfoA:     return obl_SetInfoA(ge, tags);
        case LVO_ObtainInfoA:  return obl_ObtainInfoA(ge, tags);
        case LVO_ReleaseInfoA: return obl_ReleaseInfoA(ge, tags);
        default:               return 0;
        }
    }
    {
        /* another engine's library: its call by LVO, A6 its base, loaded
         * around the call (A6 may be the frame pointer) */
        register ULONG d0 __asm("d0");
        register LONG off __asm("d1") = lvo;
        register APTR a0 __asm("a0") = p0;
        register APTR a1 __asm("a1") = p1;
        (void)c;
        __asm volatile("move.l %%a6,-(%%sp)\n\tmove.l %[lib],%%a6\n\tjsr (%%a6,%%d1.l)\n\tmove.l (%%sp)+,%%a6"
                       : "=r"(d0), "+r"(off), "+r"(a0), "+r"(a1) : [lib] "a"(lib) : "cc", "memory");
        return d0;
    }
}

static const struct odf_env diskfont_env = {
    e_alloc, e_free, e_lock, e_unlock,
    e_dir_lock, e_dir_unlock, e_dir_scan, e_fonts_scan, e_file_load, e_file_save,
    e_seg_load, e_seg_unload,
    e_mem_fonts, e_open_font, e_close_font, e_add_font, e_rem_font, e_extend_font, e_strip_font,
    e_weigh, e_lib_open, e_lib_close, e_lib_call, e_name_of,
};

/* ---- the calls ----------------------------------------------------------- */

int of_textfont_setup(struct OFTextFonts *t)
{
    GfxBase = (struct GfxBase *)OpenLibrary((CONST_STRPTR)"graphics.library", 39);
    if (!GfxBase) return 0;
    InitSemaphore(&t->sem);
    odf_Init(&t->ctx, &diskfont_env, t);
    t->of = NULL;
    return 1;
}

void of_textfont_cleanup(void)
{
    if (GfxBase) CloseLibrary((struct Library *)GfxBase);
    GfxBase = NULL;
}

struct TextFont *of_textfont_open(struct OFTextFonts *t, OFContext *of, const struct TextAttr *ta)
{
    t->of = of;
    return (struct TextFont *)odf_OpenDiskFont(&t->ctx, (const struct odf_TextAttr *)ta);
}

/* Expunge runs in Forbid: the list's semaphore is only attempted, and a
 * call already inside it (the same task) flushes nothing */
int of_textfont_flush(struct OFTextFonts *t)
{
    if (!AttemptSemaphore(&t->sem)) return 0;
    ReleaseSemaphore(&t->sem);
    return odf_Flush(&t->ctx);
}
