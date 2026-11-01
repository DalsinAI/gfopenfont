/* gfopenfont.library: GFOpenFont's core as an AmigaOS 3.2 library for the
 * 68040. The calls are in sdk/SFD/gfopenfont_lib.sfd, the structures in
 * sdk/Include_H/libraries/gfopenfont.h, and their meaning in
 * sdk/Autodocs/gfopenfont.doc.
 *
 * How it holds together:
 * - one lock: every call into the core (the reader, the shaper, the index,
 *   the memory pool) holds the library's SignalSemaphore, so the core runs
 *   as if single-threaded, which is how it is written;
 * - one stack: under that lock the work runs on the library's own 64 KB
 *   stack (of_stack.c), whatever the caller's;
 * - one pool: every byte the engines allocate comes from it (of_runtime.c),
 *   and expunging the library deletes it;
 * - files only for processes: a call that reads a font, a cache or the
 *   index answers OFERR_NOTPROCESS from a plain task, as dos.library would
 *   fail it;
 * - it patches nothing.
 *
 * Copyright (c) 2026 AmiCradle Team. OpenFont, MIT licence (LICENSE). */
/* The first code in the file, ahead of the OS headers: built with
 * -fno-toplevel-reorder, every function is emitted in the order it is
 * defined, and GFOpenNDK's inline headers define out-of-line varargs helpers
 * (AllocDosObjectTags() and the like). */
int start(void);
int start(void)
{
    return -1;                      /* run as a program, it does nothing */
}

#include <exec/types.h>
#include <exec/resident.h>
#include <exec/libraries.h>
#include <exec/execbase.h>
#include <exec/memory.h>
#include <exec/semaphores.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <utility/tagitem.h>
#include <utility/utility.h>
#include <utility/hooks.h>
#include <intuition/classes.h>
#include <datatypes/datatypes.h>
#include <datatypes/datatypesclass.h>
#include <datatypes/pictureclass.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/utility.h>
#include <proto/datatypes.h>

#include <stdlib.h>
#include <string.h>

#include "gfopenfont/gfopenfont.h"
#include "openfont_index.h"
#include "openfont_io.h"
#include "of_runtime.h"
#include "of_textfont.h"
#include "of_gpu.h"

#include <opengpu/opengpu.h>
#include <proto/opengpu.h>

#define LIB_VERSION 1
#define LIB_REVISION 5
#define STACK_SIZE (64 * 1024)

/* The same values as the SDK header's (sdk/Include_H/libraries/gfopenfont.h) */
#define OFA_Dummy     (TAG_USER + 0x4F460000)
#define OFA_Index     (OFA_Dummy + 1)
#define OFA_Size      (OFA_Dummy + 2)
#define OFA_Weight    (OFA_Dummy + 3)
#define OFA_Italic    (OFA_Dummy + 4)
#define OFA_Fallbacks (OFA_Dummy + 5)
#define OFA_Error     (OFA_Dummy + 6)
#define OFA_Script    (OFA_Dummy + 7)
#define OFA_Language  (OFA_Dummy + 8)
#define OFA_Direction (OFA_Dummy + 9)
#define OFA_Features  (OFA_Dummy + 10)
#define OFA_NumFeatures (OFA_Dummy + 11)
#define OFA_Offset    (OFA_Dummy + 12)
#define OFA_Length    (OFA_Dummy + 13)
#define OFA_UTF16     (OFA_Dummy + 14)
#define OFA_Exact     (OFA_Dummy + 15)
#define OFA_GPUError  (OFA_Dummy + 16)
#define OFA_GPUStats  (OFA_Dummy + 17)
#define OFERR_NOTPROCESS -9
#define OFERR_NOGPU   -10
#define OFERR_GPU     -11
typedef char check_gpu_err[OFERR_GPU == OF_ERR_GPU ? 1 : -1];

/* gfopengpu.library 0.24's form of OGPU_Query (stream.h), for SDK headers
 * from before it: the same values */
#ifndef OGPU_QUERY_FLAG
#define OGPU_QUERY_FLAG(op, bit) (0x01000000UL | ((unsigned long)((bit) & 31) << 16) | ((unsigned long)(op) & 0xFFFFUL))
#endif
#ifndef OGPU_PATH_ALIASED_BIT
#define OGPU_PATH_ALIASED_BIT 14
#endif
#define OPENGPU_PATHS_REVISION 18       /* gfopengpu.library 0.18: PATH_FILL (stream v1.6) */

/* OF_GlyphColorBitmap's result, as the SDK header's struct OFColorBitmap */
struct OFColorBitmapArg {
    ULONG *cb_Pixels;
    ULONG cb_Width;
    ULONG cb_Height;
    LONG cb_Left;
    LONG cb_Top;
    ULONG cb_PPEM;
};

/* OF_GlyphOutline's buffers, as the SDK header's struct OFOutline */
struct OFOutlineArg {
    UBYTE *ol_Verbs;
    LONG *ol_Points;
    ULONG ol_MaxVerbs;
    ULONG ol_MaxPoints;
    ULONG ol_NumVerbs;
    ULONG ol_NumPoints;
};

/* The SDK's structures are the core's, field for field: checked here, on the
 * 68k, where both are compiled with the same alignment. */
typedef char check_glyph[sizeof(OFGlyph) == 28 ? 1 : -1];
typedef char check_bitmap[sizeof(OFBitmap) == 28 ? 1 : -1];
typedef char check_info[sizeof(OFFaceInfo) == 176 ? 1 : -1];
typedef char check_feature[sizeof(OFFeature) == 16 ? 1 : -1];
typedef char check_metrics[sizeof(OFFaceMetrics) == 56 ? 1 : -1];
typedef char check_extents[sizeof(OFGlyphExtents) == 16 ? 1 : -1];
typedef char check_outline[sizeof(struct OFOutlineArg) == 24 ? 1 : -1];
typedef char check_color[sizeof(struct OFColorBitmapArg) == 24 ? 1 : -1];
/* OF_DrawGlyphsA's: the SDK's struct OFGPUTarget and struct OFGlyphRun */
typedef char check_gpu_target[sizeof(OFGpuTarget) == 40 ? 1 : -1];
typedef char check_glyph_run[sizeof(OFGpuRun) == 32 ? 1 : -1];
typedef char check_gpu_stats[sizeof(OFGpuStats) == 16 ? 1 : -1];

#define INDEX_FILE "ENVARC:GFOpenFont/FontIndex"
#define INDEX_DIR "ENVARC:GFOpenFont"
#define DIRS_FILE "ENVARC:GFOpenFont/FontDirs"
#define ALIASES_FILE "ENVARC:GFOpenFont/Aliases"
#define FALLBACKS_FILE "ENVARC:GFOpenFont/Fallbacks"

struct OpenFontBase {
    struct Library lib;
    UWORD pad;
    BPTR seglist;
    struct SignalSemaphore lock;
    struct Task *owner;             /* the task inside a call, for nesting */
    ULONG depth;
    APTR stack;
    struct StackSwapStruct sss;
    OFContext *ctx;                 /* the core's context, made on first use */
    OFIndex index;
    int index_ready;                /* folders and settings read, index loaded */
    int checked;                    /* the folders were checked at check_time */
    ULONG check_time;               /* in ticks */
    struct OFTextFonts fonts;       /* the Amiga fonts OF_OpenTextFont loaded */
    int datatypes_tried;            /* OF_GlyphColorBitmap opened datatypes.library, or failed to */
    int gpu_tried;                  /* OF_DrawGlyphsA opened gfopengpu.library, or failed to */
    OFGpu *gpu;                     /* its drawer and glyph cache */
};

struct ExecBase *SysBase;
struct DosLibrary *DOSBase;
struct Library *UtilityBase;       /* as proto/utility.h declares it */
struct Library *DataTypesBase;     /* opened by the first OF_GlyphColorBitmap */
struct Library *OpenGPUBase;       /* opened by the first OF_DrawGlyphsA */

static const char lib_name[] = "gfopenfont.library";
static const char lib_id[] = "gfopenfont.library 1.5 (1.11.2026) GFOpenFont 0.6, AmiCradle Team\r\n";
static const char ver_tag[] __attribute__((used)) = "$VER: gfopenfont.library 1.5 (1.11.2026) GFOpenFont 0.6, AmiCradle Team";

#define REG(r, decl) register decl __asm(#r)
#define BASE REG(a6, struct OpenFontBase *b)

/* ---- running a call: the lock, then the library's stack ---- */

typedef long (*WorkFn)(void *base, void *arg);

static long run(struct OpenFontBase *b, WorkFn fn, void *arg)
{
    long r;
    ObtainSemaphore(&b->lock);
    if (b->depth++ == 0) {
        b->owner = FindTask(NULL);
        b->sss.stk_Lower = b->stack;
        b->sss.stk_Upper = (ULONG)b->stack + STACK_SIZE;
        b->sss.stk_Pointer = (APTR)b->sss.stk_Upper;
        r = of_call_on_stack(&b->sss, (OFStackFn)fn, b, arg);
        b->owner = NULL;
    } else {
        r = fn(b, arg);             /* nested in the same task: already on our stack */
    }
    b->depth--;
    ReleaseSemaphore(&b->lock);
    return r;
}

static int is_process(void)
{
    struct Task *t = FindTask(NULL);
    return t->tc_Node.ln_Type == NT_PROCESS;
}

static OFContext *context(struct OpenFontBase *b)
{
    if (!b->ctx)
        b->ctx = of_context_create();
    return b->ctx;
}

/* ---- the index: folders, settings, the file ---- */

/* a small settings file, read whole and NUL-terminated; NULL when absent */
static char *read_text(const char *path)
{
    uint8_t *data = NULL;
    size_t size = 0;
    char *s;
    if (of_io_read_all(path, &data, &size) != OF_OK)
        return NULL;
    s = (char *)realloc(data, size + 1);
    if (!s) {
        free(data);
        return NULL;
    }
    s[size] = 0;
    return s;
}

static void trim_line(char *s)
{
    size_t n = strlen(s);
    while (n && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' '))
        s[--n] = 0;
}

static void ensure_index_dir(void)
{
    BPTR l = Lock((CONST_STRPTR)INDEX_DIR, SHARED_LOCK);
    if (l)
        UnLock(l);
    else if ((l = CreateDir((CONST_STRPTR)INDEX_DIR)) != 0)
        UnLock(l);
}

/* extra: a folder of the first OF_AddFontDir, on the list before the
 * index file is read, so the file is read once */
static int setup_index(struct OpenFontBase *b, const char *extra)
{
    char *dirs, *aliases, *fallbacks;
    OFIndex *ix = &b->index;
    if (b->index_ready)
        return OF_OK;
    of_index_free(ix);
    of_index_init(ix);
    of_index_add_dir(ix, "FONTS:");
    of_index_add_dir(ix, "LIBS:WebBrowser/Fonts");
    if (extra)
        of_index_add_dir(ix, extra);
    if ((dirs = read_text(DIRS_FILE)) != NULL) {
        char *p = dirs;
        while (*p) {
            char *eol = p;
            while (*eol && *eol != '\n')
                ++eol;
            if (*eol)
                *eol++ = 0;
            trim_line(p);
            if (*p && *p != '#' && *p != ';')
                of_index_add_dir(ix, p);
            p = eol;
        }
        free(dirs);
    }
    aliases = read_text(ALIASES_FILE);
    fallbacks = read_text(FALLBACKS_FILE);
    if (fallbacks)
        trim_line(fallbacks);
    of_index_configure(ix, aliases, fallbacks);
    free(aliases);
    free(fallbacks);
    if (of_index_load(ix, INDEX_FILE) != OF_OK)
        ix->count = 0;
    b->index_ready = 1;
    b->checked = 0;
    return OF_OK;
}

static ULONG ticks_now(void)
{
    struct DateStamp ds;
    DateStamp(&ds);
    return ((ULONG)ds.ds_Days * 1440 + (ULONG)ds.ds_Minute) * TICKS_PER_SECOND * 60 + (ULONG)ds.ds_Tick;
}

static int refresh_index(struct OpenFontBase *b, int force)
{
    OFContext *ctx = context(b);
    int rc;
    if (!ctx)
        return OF_ERR_MEMORY;
    setup_index(b, NULL);
    /* the folders' dates once every two seconds, not at every lookup: a
     * page asks for a face many times in a moment */
    if (!force) {
        ULONG now = ticks_now();
        if (b->checked && now - b->check_time < 2 * TICKS_PER_SECOND)
            return OF_OK;
        b->checked = 1;
        b->check_time = now;
        if (!of_index_stale(&b->index))
            return OF_OK;
    }
    rc = of_index_scan(&b->index, ctx);
    if (rc == OF_OK) {
        ensure_index_dir();
        of_index_save(&b->index, INDEX_FILE);   /* a read-only ENVARC: only costs a rescan */
    }
    return rc;
}

/* ---- the calls' work, on the library's stack under its lock ---- */

struct OpenArgs {
    const char *path;               /* or the family, for find */
    const struct TagItem *tags;
    void *result;
};

static void set_error(const struct TagItem *tags, LONG code)
{
    LONG *err = (LONG *)GetTagData(OFA_Error, 0, (struct TagItem *)tags);
    if (err)
        *err = code;
}

static long w_open_face(void *base, void *arg)
{
    struct OpenFontBase *b = (struct OpenFontBase *)base;
    struct OpenArgs *a = (struct OpenArgs *)arg;
    OFContext *ctx = context(b);
    ULONG index = GetTagData(OFA_Index, 0, (struct TagItem *)a->tags);
    ULONG size = GetTagData(OFA_Size, 16, (struct TagItem *)a->tags);
    OFFace *face;
    int err;
    if (!ctx) {
        set_error(a->tags, OF_ERR_MEMORY);
        return 0;
    }
    face = of_face_open_file(ctx, a->path, index, &err);
    if (!face) {
        set_error(a->tags, err);     /* the file, memory, or not a font */
        return 0;
    }
    if (size && of_face_set_pixel_size(face, size) != OF_OK) {
        of_face_close(face);
        set_error(a->tags, OF_ERR_FONT);
        return 0;
    }
    set_error(a->tags, OF_OK);
    return (long)face;
}

static long w_find_face(void *base, void *arg)
{
    struct OpenFontBase *b = (struct OpenFontBase *)base;
    struct OpenArgs *a = (struct OpenArgs *)arg;
    ULONG weight = GetTagData(OFA_Weight, 400, (struct TagItem *)a->tags);
    ULONG italic = GetTagData(OFA_Italic, FALSE, (struct TagItem *)a->tags);
    ULONG fallbacks = GetTagData(OFA_Fallbacks, TRUE, (struct TagItem *)a->tags);
    ULONG size = GetTagData(OFA_Size, 16, (struct TagItem *)a->tags);
    ULONG exact = GetTagData(OFA_Exact, FALSE, (struct TagItem *)a->tags);
    OFFace *face;
    int rc = refresh_index(b, 0);
    if (rc != OF_OK) {
        set_error(a->tags, rc);
        return 0;
    }
    face = of_index_open_family(&b->index, b->ctx, a->path, (uint16_t)weight, italic != 0,
                                fallbacks != 0, exact != 0, &rc);
    if (!face) {
        set_error(a->tags, rc);      /* no fonts (OF_ERR_FONT), or what opening gave */
        return 0;
    }
    if (size && of_face_set_pixel_size(face, size) != OF_OK) {
        of_face_close(face);
        set_error(a->tags, OF_ERR_FONT);
        return 0;
    }
    set_error(a->tags, OF_OK);
    return (long)face;
}

struct FaceArgs {
    OFFace *face;
    OFFace *other;
    const char *utf8;
    ULONG bytes;
    ULONG n;
    void *out;
    ULONG capacity;
};

static long w_close_face(void *base, void *arg)
{
    (void)base;
    of_face_close(((struct FaceArgs *)arg)->face);
    return 0;
}

static long w_set_size(void *base, void *arg)
{
    struct FaceArgs *a = (struct FaceArgs *)arg;
    (void)base;
    return of_face_set_pixel_size(a->face, a->n);
}

static long w_face_info(void *base, void *arg)
{
    struct FaceArgs *a = (struct FaceArgs *)arg;
    (void)base;
    return of_face_info(a->face, (OFFaceInfo *)a->out);
}

static long w_shape(void *base, void *arg)
{
    struct FaceArgs *a = (struct FaceArgs *)arg;
    (void)base;
    return (long)of_shape_utf8(a->face, a->utf8, a->bytes, (OFGlyph *)a->out, a->capacity);
}

static long w_measure(void *base, void *arg)
{
    struct FaceArgs *a = (struct FaceArgs *)arg;
    (void)base;
    return (long)of_measure_utf8(a->face, a->utf8, a->bytes);
}

static long w_raster(void *base, void *arg)
{
    struct FaceArgs *a = (struct FaceArgs *)arg;
    (void)base;
    return of_raster_glyph_a8(a->face, a->n, (OFBitmap *)a->out);
}

static long w_raster_matrix(void *base, void *arg)
{
    struct FaceArgs *a = (struct FaceArgs *)arg;
    (void)base;
    return of_raster_glyph_matrix_a8(a->face, a->n, (const int32_t *)a->utf8, (OFBitmap *)a->out);
}

static long w_glyph_advance(void *base, void *arg)
{
    struct FaceArgs *a = (struct FaceArgs *)arg;
    (void)base;
    return of_face_glyph_advance(a->face, a->n);
}

static long w_free_bitmap(void *base, void *arg)
{
    (void)base;
    of_bitmap_release((OFBitmap *)((struct FaceArgs *)arg)->out);
    return 0;
}

static long w_add_fallback(void *base, void *arg)
{
    struct FaceArgs *a = (struct FaceArgs *)arg;
    (void)base;
    return of_face_add_fallback(a->face, a->other);
}

static long w_get_fallback(void *base, void *arg)
{
    struct FaceArgs *a = (struct FaceArgs *)arg;
    (void)base;
    return (long)of_face_fallback(a->face, a->n);
}

static long w_char_index(void *base, void *arg)
{
    struct FaceArgs *a = (struct FaceArgs *)arg;
    (void)base;
    return (long)of_face_char_index(a->face, a->n);
}

static long w_add_dir(void *base, void *arg)
{
    struct OpenFontBase *b = (struct OpenFontBase *)base;
    const char *path = ((struct OpenArgs *)arg)->path;
    size_t before;
    long rc;
    if (!b->index_ready)
        return setup_index(b, path);
    before = b->index.ndirs;
    rc = of_index_add_dir(&b->index, path);
    /* The index file was read before this folder was on the list, so its
     * fonts were left out: read it again with the folder, or every new
     * start of the library would read all the fonts again. Without a file
     * the folder is stale and the next lookup reads it. */
    if (rc == OF_OK && b->index.ndirs > before) {
        of_index_load(&b->index, INDEX_FILE);
        b->checked = 0;
    }
    return rc;
}

static long w_rescan(void *base, void *arg)
{
    (void)arg;
    return refresh_index((struct OpenFontBase *)base, 1);
}

struct CacheArgs {
    OFCache *cache;
    OFFace *face;
    const char *path;
    ULONG pixels, codepoint;
    OFBitmap *bitmap;
    ULONG *glyph;
};

static long w_open_cache(void *base, void *arg)
{
    (void)base;
    return (long)of_cache_open(((struct CacheArgs *)arg)->path);
}

static long w_close_cache(void *base, void *arg)
{
    (void)base;
    of_cache_close(((struct CacheArgs *)arg)->cache);
    return 0;
}

static long w_cache_get(void *base, void *arg)
{
    struct CacheArgs *a = (struct CacheArgs *)arg;
    uint32_t gid = 0;
    int rc;
    (void)base;
    if (a->pixels > 0xFFFF)
        return OF_ERR_ARGUMENT;
    rc = of_cache_get_a8(a->cache, (uint16_t)a->pixels, a->codepoint, &gid, a->bitmap);
    if (a->glyph)
        *a->glyph = gid;
    return rc;
}

static long w_cache_matches(void *base, void *arg)
{
    struct CacheArgs *a = (struct CacheArgs *)arg;
    (void)base;
    return of_cache_matches_face(a->cache, a->face) ? 1 : 0;
}

static long w_open_textfont(void *base, void *arg)
{
    struct OpenFontBase *b = (struct OpenFontBase *)base;
    OFContext *ctx = context(b);
    if (!ctx)
        return 0;
    return (long)of_textfont_open(&b->fonts, ctx, (const struct TextAttr *)arg);
}

/* ---- revision 3: what a layout engine (WebCore) asks ---- */

struct ShapeArgs {
    OFFace *face;
    const void *text;
    ULONG length;
    OFGlyph *out;
    ULONG capacity;
    const struct TagItem *tags;
};

static long w_shape_tags(void *base, void *arg)
{
    struct ShapeArgs *a = (struct ShapeArgs *)arg;
    struct TagItem *tags = (struct TagItem *)a->tags;
    OFShapeOptions o;
    ULONG off = GetTagData(OFA_Offset, 0, tags);
    ULONG len = GetTagData(OFA_Length, a->length, tags);
    (void)base;
    memset(&o, 0, sizeof o);
    o.script = GetTagData(OFA_Script, 0, tags);
    o.language = GetTagData(OFA_Language, 0, tags);
    o.direction = GetTagData(OFA_Direction, OF_DIR_AUTO, tags);
    o.features = (const OFFeature *)GetTagData(OFA_Features, 0, tags);
    o.num_features = o.features ? GetTagData(OFA_NumFeatures, 0, tags) : 0;
    if (GetTagData(OFA_UTF16, FALSE, tags))
        return (long)of_shape_utf16(a->face, (const uint16_t *)a->text, a->length, off, len, &o, a->out, a->capacity);
    return (long)of_shape(a->face, (const char *)a->text, a->length, off, len, &o, a->out, a->capacity);
}

static long w_metrics(void *base, void *arg)
{
    struct FaceArgs *a = (struct FaceArgs *)arg;
    (void)base;
    return of_face_metrics(a->face, (OFFaceMetrics *)a->out);
}

static long w_extents(void *base, void *arg)
{
    struct FaceArgs *a = (struct FaceArgs *)arg;
    (void)base;
    return of_face_glyph_extents(a->face, a->n, (OFGlyphExtents *)a->out);
}

static long w_outline(void *base, void *arg)
{
    struct FaceArgs *a = (struct FaceArgs *)arg;
    struct OFOutlineArg *o = (struct OFOutlineArg *)a->out;
    size_t nv = 0, np = 0;
    int rc;
    (void)base;
    rc = of_face_glyph_outline(a->face, a->n, o->ol_Verbs, o->ol_Verbs ? o->ol_MaxVerbs : 0,
                               (int32_t *)o->ol_Points, o->ol_Points ? o->ol_MaxPoints : 0, &nv, &np);
    o->ol_NumVerbs = (ULONG)nv;
    o->ol_NumPoints = (ULONG)np;
    return rc;
}

static long w_table(void *base, void *arg)
{
    struct FaceArgs *a = (struct FaceArgs *)arg;
    size_t len = 0;
    const uint8_t *t;
    (void)base;
    t = of_face_table(a->face, a->n, &len);
    if (a->out)
        *(ULONG *)a->out = (ULONG)len;
    return (long)t;
}

static long w_color_png(void *base, void *arg)
{
    struct FaceArgs *a = (struct FaceArgs *)arg;
    (void)base;
    return of_face_color_png(a->face, a->n, a->capacity, (OFColorPng *)a->out);
}

static long w_glyph_advance_px(void *base, void *arg)
{
    struct FaceArgs *a = (struct FaceArgs *)arg;
    (void)base;
    return of_face_glyph_advance_px(a->face, a->n);
}

struct MemArgs {
    const UBYTE *data;
    ULONG size;
    const struct TagItem *tags;
};

static long w_open_memory(void *base, void *arg)
{
    struct OpenFontBase *b = (struct OpenFontBase *)base;
    struct MemArgs *a = (struct MemArgs *)arg;
    OFContext *ctx = context(b);
    ULONG index = GetTagData(OFA_Index, 0, (struct TagItem *)a->tags);
    ULONG size = GetTagData(OFA_Size, 16, (struct TagItem *)a->tags);
    uint8_t *copy;
    OFFace *face;
    if (!ctx) {
        set_error(a->tags, OF_ERR_MEMORY);
        return 0;
    }
    /* the face keeps its own copy, in the library's pool, so the caller's
     * memory may go at once */
    copy = (uint8_t *)malloc(a->size);
    if (!copy) {
        set_error(a->tags, OF_ERR_MEMORY);
        return 0;
    }
    memcpy(copy, a->data, a->size);
    face = of_face_open_memory(ctx, copy, a->size, index, 1);
    if (!face) {
        set_error(a->tags, OF_ERR_FONT);    /* the face freed the copy, or never took it */
        return 0;
    }
    if (size && of_face_set_pixel_size(face, size) != OF_OK) {
        of_face_close(face);
        set_error(a->tags, OF_ERR_FONT);
        return 0;
    }
    set_error(a->tags, OF_OK);
    return (long)face;
}

struct CharArgs {
    ULONG codepoint;
    const struct TagItem *tags;
};

static long w_face_for_char(void *base, void *arg)
{
    struct OpenFontBase *b = (struct OpenFontBase *)base;
    struct CharArgs *a = (struct CharArgs *)arg;
    ULONG weight = GetTagData(OFA_Weight, 400, (struct TagItem *)a->tags);
    ULONG italic = GetTagData(OFA_Italic, FALSE, (struct TagItem *)a->tags);
    ULONG size = GetTagData(OFA_Size, 16, (struct TagItem *)a->tags);
    OFFace *face;
    int rc = refresh_index(b, 0);
    if (rc != OF_OK) {
        set_error(a->tags, rc);
        return 0;
    }
    face = of_index_open_char(&b->index, b->ctx, a->codepoint, (uint16_t)weight, italic != 0, &rc);
    if (!face) {
        set_error(a->tags, rc);
        return 0;
    }
    if (size && of_face_set_pixel_size(face, size) != OF_OK) {
        of_face_close(face);
        set_error(a->tags, OF_ERR_FONT);
        return 0;
    }
    set_error(a->tags, OF_OK);
    return (long)face;
}

/* ---- the library's vectors ---- */

static ULONG L_Version(BASE)
{
    (void)b;
    return ((ULONG)OF_VERSION_MAJOR << 16) | OF_VERSION_MINOR;
}

static OFFace *L_OpenFaceA(REG(a0, CONST_STRPTR path), REG(a1, const struct TagItem *tags), BASE)
{
    struct OpenArgs a;
    if (!path) {
        set_error(tags, OF_ERR_ARGUMENT);
        return NULL;
    }
    if (!is_process()) {
        set_error(tags, OFERR_NOTPROCESS);
        return NULL;
    }
    a.path = (const char *)path;
    a.tags = tags;
    return (OFFace *)run(b, w_open_face, &a);
}

static VOID L_CloseFace(REG(a0, OFFace *face), BASE)
{
    struct FaceArgs a;
    if (!face)
        return;
    a.face = face;
    run(b, w_close_face, &a);
}

static LONG L_SetSize(REG(a0, OFFace *face), REG(d0, ULONG pixels), BASE)
{
    struct FaceArgs a;
    if (!face || !pixels)
        return OF_ERR_ARGUMENT;
    a.face = face;
    a.n = pixels;
    return run(b, w_set_size, &a);
}

static LONG L_GetFaceInfo(REG(a0, OFFace *face), REG(a1, OFFaceInfo *info), BASE)
{
    struct FaceArgs a;
    if (!face || !info)
        return OF_ERR_ARGUMENT;
    a.face = face;
    a.out = info;
    return run(b, w_face_info, &a);
}

static ULONG L_ShapeText(REG(a0, OFFace *face), REG(a1, CONST_STRPTR utf8), REG(d0, ULONG bytes),
                         REG(a2, OFGlyph *glyphs), REG(d1, ULONG capacity), BASE)
{
    struct FaceArgs a;
    if (!face || !utf8 || !bytes)
        return 0;
    a.face = face;
    a.utf8 = (const char *)utf8;
    a.bytes = bytes;
    a.out = glyphs;
    a.capacity = glyphs ? capacity : 0;
    return (ULONG)run(b, w_shape, &a);
}

static LONG L_MeasureText(REG(a0, OFFace *face), REG(a1, CONST_STRPTR utf8), REG(d0, ULONG bytes), BASE)
{
    struct FaceArgs a;
    if (!face || !utf8 || !bytes)
        return 0;
    a.face = face;
    a.utf8 = (const char *)utf8;
    a.bytes = bytes;
    return run(b, w_measure, &a);
}

static LONG L_RasterGlyph(REG(a0, OFFace *face), REG(d0, ULONG glyph), REG(a1, OFBitmap *bitmap), BASE)
{
    struct FaceArgs a;
    if (!face || !bitmap)
        return OF_ERR_ARGUMENT;
    a.face = face;
    a.n = glyph;
    a.out = bitmap;
    return run(b, w_raster, &a);
}

static LONG L_RasterGlyphMatrix(REG(a0, OFFace *face), REG(d0, ULONG glyph), REG(a1, const LONG *matrix),
                                REG(a2, OFBitmap *bitmap), BASE)
{
    struct FaceArgs a;
    if (!face || !matrix || !bitmap)
        return OF_ERR_ARGUMENT;
    a.face = face;
    a.n = glyph;
    a.utf8 = (const char *)matrix;
    a.out = bitmap;
    return run(b, w_raster_matrix, &a);
}

static LONG L_GlyphAdvance(REG(a0, OFFace *face), REG(d0, ULONG glyph), BASE)
{
    struct FaceArgs a;
    if (!face)
        return -1;
    a.face = face;
    a.n = glyph;
    return run(b, w_glyph_advance, &a);
}

static VOID L_FreeBitmap(REG(a0, OFBitmap *bitmap), BASE)
{
    struct FaceArgs a;
    if (!bitmap)
        return;
    if (!bitmap->pixels) {
        /* an empty glyph: nothing to free, but clear it all the same */
        memset(bitmap, 0, sizeof(*bitmap));
        return;
    }
    a.out = bitmap;
    run(b, w_free_bitmap, &a);
}

static OFFace *L_FindFaceA(REG(a0, CONST_STRPTR family), REG(a1, const struct TagItem *tags), BASE)
{
    struct OpenArgs a;
    if (!is_process()) {
        set_error(tags, OFERR_NOTPROCESS);
        return NULL;
    }
    a.path = (const char *)family;
    a.tags = tags;
    return (OFFace *)run(b, w_find_face, &a);
}

static LONG L_AddFontDir(REG(a0, CONST_STRPTR dir), BASE)
{
    struct OpenArgs a;
    if (!dir)
        return OF_ERR_ARGUMENT;
    /* the first call to need the index reads its settings from ENVARC: */
    if (!is_process())
        return OFERR_NOTPROCESS;
    a.path = (const char *)dir;
    return run(b, w_add_dir, &a);
}

static LONG L_AddFallback(REG(a0, OFFace *face), REG(a1, OFFace *fallback), BASE)
{
    struct FaceArgs a;
    if (!face || !fallback)
        return OF_ERR_ARGUMENT;
    a.face = face;
    a.other = fallback;
    return run(b, w_add_fallback, &a);
}

static OFFace *L_GetFallback(REG(a0, OFFace *face), REG(d0, ULONG n), BASE)
{
    struct FaceArgs a;
    if (!face)
        return NULL;
    a.face = face;
    a.n = n;
    return (OFFace *)run(b, w_get_fallback, &a);
}

static ULONG L_CharIndex(REG(a0, OFFace *face), REG(d0, ULONG codepoint), BASE)
{
    struct FaceArgs a;
    if (!face)
        return 0;
    a.face = face;
    a.n = codepoint;
    return (ULONG)run(b, w_char_index, &a);
}

static OFCache *L_OpenCache(REG(a0, CONST_STRPTR path), BASE)
{
    struct CacheArgs a;
    if (!path || !is_process())
        return NULL;
    a.path = (const char *)path;
    return (OFCache *)run(b, w_open_cache, &a);
}

static VOID L_CloseCache(REG(a0, OFCache *cache), BASE)
{
    struct CacheArgs a;
    if (!cache)
        return;
    a.cache = cache;
    run(b, w_close_cache, &a);
}

static LONG L_CacheGetGlyph(REG(a0, OFCache *cache), REG(d0, ULONG pixels), REG(d1, ULONG codepoint),
                            REG(a1, OFBitmap *bitmap), REG(a2, ULONG *glyph), BASE)
{
    struct CacheArgs a;
    if (!cache || !bitmap)
        return OF_ERR_ARGUMENT;
    a.cache = cache;
    a.pixels = pixels;
    a.codepoint = codepoint;
    a.bitmap = bitmap;
    a.glyph = glyph;
    return run(b, w_cache_get, &a);
}

static LONG L_CacheMatchesFace(REG(a0, OFCache *cache), REG(a1, OFFace *face), BASE)
{
    struct CacheArgs a;
    if (!cache || !face)
        return 0;
    a.cache = cache;
    a.face = face;
    return run(b, w_cache_matches, &a);
}

static CONST_STRPTR L_ErrorString(REG(d0, LONG code), BASE)
{
    (void)b;
    if (code == OFERR_NOTPROCESS)
        return (CONST_STRPTR)"the caller is not a process";
    if (code == OFERR_NOGPU)
        return (CONST_STRPTR)"no gfopengpu.library that fills paths";
    if (code == OFERR_GPU)
        return (CONST_STRPTR)"GFOpenGPU failed to draw";
    return (CONST_STRPTR)of_error_string((int)code);
}

static LONG L_RescanFonts(BASE)
{
    if (!is_process())
        return OFERR_NOTPROCESS;
    return run(b, w_rescan, NULL);
}

static struct TextFont *L_OpenTextFont(REG(a0, struct TextAttr *ta), BASE)
{
    if (!ta || !ta->ta_Name || !is_process())
        return NULL;
    return (struct TextFont *)run(b, w_open_textfont, ta);
}

static ULONG L_ShapeA(REG(a0, OFFace *face), REG(a1, APTR text), REG(d0, ULONG length),
                      REG(a2, OFGlyph *glyphs), REG(d1, ULONG capacity), REG(a3, const struct TagItem *tags), BASE)
{
    struct ShapeArgs a;
    if (!face || !text || !length)
        return 0;
    a.face = face;
    a.text = text;
    a.length = length;
    a.out = glyphs;
    a.capacity = glyphs ? capacity : 0;
    a.tags = tags;
    return (ULONG)run(b, w_shape_tags, &a);
}

static LONG L_GetFaceMetrics(REG(a0, OFFace *face), REG(a1, OFFaceMetrics *metrics), BASE)
{
    struct FaceArgs a;
    if (!face || !metrics)
        return OF_ERR_ARGUMENT;
    a.face = face;
    a.out = metrics;
    return run(b, w_metrics, &a);
}

static LONG L_GlyphOutline(REG(a0, OFFace *face), REG(d0, ULONG glyph), REG(a1, struct OFOutlineArg *outline), BASE)
{
    struct FaceArgs a;
    if (!face || !outline)
        return OF_ERR_ARGUMENT;
    a.face = face;
    a.n = glyph;
    a.out = outline;
    return run(b, w_outline, &a);
}

static LONG L_GlyphExtents(REG(a0, OFFace *face), REG(d0, ULONG glyph), REG(a1, OFGlyphExtents *extents), BASE)
{
    struct FaceArgs a;
    if (!face || !extents)
        return OF_ERR_ARGUMENT;
    a.face = face;
    a.n = glyph;
    a.out = extents;
    return run(b, w_extents, &a);
}

static APTR L_GetFontTable(REG(a0, OFFace *face), REG(d0, ULONG tag), REG(a1, ULONG *length), BASE)
{
    struct FaceArgs a;
    if (length)
        *length = 0;
    if (!face)
        return NULL;
    a.face = face;
    a.n = tag;
    a.out = length;
    return (APTR)run(b, w_table, &a);
}

static OFFace *L_OpenFaceMemoryA(REG(a0, CONST_APTR data), REG(d0, ULONG size), REG(a1, const struct TagItem *tags), BASE)
{
    struct MemArgs a;
    if (!data || !size) {
        set_error(tags, OF_ERR_ARGUMENT);
        return NULL;
    }
    a.data = (const UBYTE *)data;
    a.size = size;
    a.tags = tags;
    return (OFFace *)run(b, w_open_memory, &a);
}

static OFFace *L_FindFaceForCharA(REG(d0, ULONG codepoint), REG(a0, const struct TagItem *tags), BASE)
{
    struct CharArgs a;
    if (!is_process()) {
        set_error(tags, OFERR_NOTPROCESS);
        return NULL;
    }
    a.codepoint = codepoint;
    a.tags = tags;
    return (OFFace *)run(b, w_face_for_char, &a);
}

static LONG L_GlyphAdvancePx(REG(a0, OFFace *face), REG(d0, ULONG glyph), BASE)
{
    struct FaceArgs a;
    if (!face)
        return -1;
    a.face = face;
    a.n = glyph;
    return run(b, w_glyph_advance_px, &a);
}

/* A PNG in memory through the PNG datatype (picture.datatype's V43
 * interface), as ARGB, outside the library's lock and stack: the datatype
 * reads with the caller's stack, as any program's would. */
static ULONG do_method(Object *o, Msg msg)
{
    return CallHookPkt((struct Hook *)OCLASS(o), o, msg);
}

/* The steps OF_GlyphColorBitmap reports in cb_Left when the datatype path
 * fails, with IoErr() in cb_Top. */
enum { COLOR_STEP_DATATYPES = 1, COLOR_STEP_OBJECT, COLOR_STEP_HEADER, COLOR_STEP_READ };

/* The PNG from memory (DTST_MEMORY), or, for a png.datatype that reads only
 * files, from a file in T: that goes when the object does. */
static Object *png_object(const UBYTE *png, ULONG length, char *path, ULONG pathSize)
{
    Object *o;
    BPTR fh;
    LONG written;
    path[0] = 0;
    o = NewDTObject(NULL,
                    DTA_SourceType, DTST_MEMORY,
                    DTA_SourceAddress, (ULONG)png,
                    DTA_SourceSize, length,
                    DTA_GroupID, GID_PICTURE,
                    PDTA_DestMode, PMODE_V43,
                    PDTA_Remap, FALSE,
                    TAG_DONE);
    if (o)
        return o;
    if (pathSize < 32)
        return NULL;
    {
        static const char hex[] = "0123456789abcdef";
        ULONG task = (ULONG)FindTask(NULL);
        int i;
        strcpy(path, "T:openfont-00000000.png");
        for (i = 0; i < 8; ++i)
            path[11 + i] = hex[(task >> (28 - 4 * i)) & 15];
    }
    if (!(fh = Open((CONST_STRPTR)path, MODE_NEWFILE)))
        return NULL;
    written = Write(fh, (APTR)png, length);
    Close(fh);
    if (written == (LONG)length)
        o = NewDTObject((APTR)path,
                        DTA_SourceType, DTST_FILE,
                        DTA_GroupID, GID_PICTURE,
                        PDTA_DestMode, PMODE_V43,
                        PDTA_Remap, FALSE,
                        TAG_DONE);
    if (!o) {
        LONG error = IoErr();
        DeleteFile((CONST_STRPTR)path);
        path[0] = 0;
        SetIoErr(error);
    }
    return o;
}

static VOID png_dispose(Object *o, const char *path)
{
    DisposeDTObject(o);
    if (path[0])
        DeleteFile((CONST_STRPTR)path);
}

static LONG decode_png(const UBYTE *png, ULONG length, ULONG width, ULONG height, ULONG **pixels, LONG *step)
{
    struct pdtBlitPixelArray read;
    struct BitMapHeader *bmh = NULL;
    char path[40];
    Object *o;
    ULONG *buf;
    ULONG done, p;
    *pixels = NULL;
    *step = COLOR_STEP_OBJECT;
    if (!(o = png_object(png, length, path, sizeof path)))
        return OF_ERR_EXEC;
    *step = COLOR_STEP_HEADER;
    if (GetDTAttrs(o, PDTA_BitMapHeader, (ULONG)&bmh, TAG_DONE) != 1 || !bmh
        || bmh->bmh_Width != width || bmh->bmh_Height != height) {
        png_dispose(o, path);
        return OF_ERR_FONT;
    }
    buf = (ULONG *)AllocVec(width * height * 4, MEMF_ANY | MEMF_CLEAR);
    if (!buf) {
        png_dispose(o, path);
        return OF_ERR_MEMORY;
    }
    *step = COLOR_STEP_READ;
    read.MethodID = PDTM_READPIXELARRAY;
    read.pbpa_PixelData = buf;
    read.pbpa_PixelFormat = PBPAFMT_ARGB;
    read.pbpa_PixelArrayMod = width * 4;
    read.pbpa_Left = 0;
    read.pbpa_Top = 0;
    read.pbpa_Width = width;
    read.pbpa_Height = height;
    done = do_method(o, (Msg)&read);
    /* Some picture.datatype versions return 0 from a read that worked, so
     * pixels in the buffer count as success too. */
    for (p = 0; !done && p < width * height; ++p)
        done = buf[p] != 0;
    png_dispose(o, path);
    if (!done) {
        FreeVec(buf);
        return OF_ERR_EXEC;
    }
    *pixels = buf;
    return OF_OK;
}

static LONG L_GlyphColorBitmap(REG(a0, OFFace *face), REG(d0, ULONG glyph), REG(d1, ULONG ppem),
                               REG(a1, struct OFColorBitmapArg *bitmap), BASE)
{
    struct FaceArgs a;
    OFColorPng png;
    LONG rc, step;
    if (!bitmap)
        return OF_ERR_ARGUMENT;
    memset(bitmap, 0, sizeof *bitmap);
    if (!face)
        return OF_ERR_ARGUMENT;
    if (!is_process())
        return OFERR_NOTPROCESS;
    memset(&a, 0, sizeof a);
    a.face = face;
    a.n = glyph;
    a.capacity = ppem;
    a.out = &png;
    if ((rc = run(b, w_color_png, &a)) != OF_OK)
        return rc;
    ObtainSemaphore(&b->lock);
    if (!b->datatypes_tried) {
        b->datatypes_tried = 1;
        DataTypesBase = OpenLibrary((CONST_STRPTR)"datatypes.library", 44);
    }
    ReleaseSemaphore(&b->lock);
    if (!DataTypesBase) {
        bitmap->cb_Left = COLOR_STEP_DATATYPES;
        return OF_ERR_EXEC;
    }
    if ((rc = decode_png(png.png, png.length, png.width, png.height, &bitmap->cb_Pixels, &step)) != OF_OK) {
        bitmap->cb_Left = step;
        bitmap->cb_Top = IoErr();
        return rc;
    }
    bitmap->cb_Width = png.width;
    bitmap->cb_Height = png.height;
    bitmap->cb_Left = png.left;
    bitmap->cb_Top = png.top;
    bitmap->cb_PPEM = png.ppem;
    return OF_OK;
}

static VOID L_FreeColorBitmap(REG(a0, struct OFColorBitmapArg *bitmap), BASE)
{
    (void)b;
    if (!bitmap)
        return;
    if (bitmap->cb_Pixels)
        FreeVec(bitmap->cb_Pixels);
    memset(bitmap, 0, sizeof *bitmap);
}

/* ---- glyph runs through GFOpenGPU (revision 5) ---- */

/* The drawer's memory: the back ends read the Amiga's own memory, at its
 * own addresses (the CPU back end, and ACRTG.gpu through the Cradle). */
static void *gpu_alloc(void *user, size_t bytes, uint32_t *address)
{
    void *p = AllocVec((ULONG)bytes, MEMF_ANY);
    (void)user;
    if (p)
        *address = (uint32_t)p;
    return p;
}

static void gpu_free(void *user, void *block)
{
    (void)user;
    FreeVec(block);
}

static long gpu_submit(void *user, const uint8_t *stream, long words)
{
    ULONG fence = 0;
    LONG r, w;
    (void)user;
    r = OGPU_Submit((APTR)stream, (ULONG)words, &fence);
    w = fence ? OGPU_Wait(fence) : OGPU_OK;
    return r != OGPU_OK ? r : w;
}

struct GpuArgs {
    OFFace *face;
    const OFGpuTarget *target;
    const OFGpuRun *run;
    LONG gpu_error;
    OFGpuStats stats;
};

static long w_draw_glyphs(void *base, void *arg)
{
    struct OpenFontBase *b = (struct OpenFontBase *)base;
    struct GpuArgs *a = (struct GpuArgs *)arg;
    long r;
    if (!b->gpu_tried) {
        b->gpu_tried = 1;
        OpenGPUBase = OpenLibrary((CONST_STRPTR)OPENGPU_NAME, OPENGPU_VERSION);
        if (OpenGPUBase && OpenGPUBase->lib_Version == 0 && OpenGPUBase->lib_Revision < OPENGPU_PATHS_REVISION) {
            CloseLibrary(OpenGPUBase);
            OpenGPUBase = NULL;
        }
        if (OpenGPUBase) {
            /* OGPU_PATH_ALIASED only where the back end says it takes it:
             * before gfopengpu.library 0.24 the answer is OGPU_NONE */
            int aliased = OGPU_ANSWER(OGPU_Query(OGPU_QUERY_FLAG(OGPU_OP_PATH_FILL, OGPU_PATH_ALIASED_BIT),
                                                 OGPU_FMT_A8)) != OGPU_NONE;
            b->gpu = of_gpu_create(gpu_alloc, gpu_free, gpu_submit, NULL, aliased);
        }
    }
    if (!OpenGPUBase)
        return OFERR_NOGPU;
    if (!b->gpu)
        return OF_ERR_MEMORY;
    if (OGPU_ANSWER(OGPU_Query(OGPU_OP_PATH_FILL, OGPU_FMT_A8)) == OGPU_NONE
        || OGPU_ANSWER(OGPU_Query(OGPU_OP_COMPOSITE_PD, a->target->format)) == OGPU_NONE)
        return OFERR_NOGPU;
    r = of_gpu_draw(b->gpu, a->face, a->target, a->run);
    a->gpu_error = of_gpu_error(b->gpu);
    of_gpu_stats(b->gpu, &a->stats);
    return r;
}

static LONG L_DrawGlyphsA(REG(a0, OFFace *face), REG(a1, const OFGpuTarget *target), REG(a2, const OFGpuRun *glyphs),
                          REG(a3, const struct TagItem *tags), BASE)
{
    struct GpuArgs a;
    LONG rc, *perr;
    OFGpuStats *pstats;
    if (!face || !target || !glyphs)
        rc = OF_ERR_ARGUMENT;
    else {
        memset(&a, 0, sizeof a);
        a.face = face;
        a.target = target;
        a.run = glyphs;
        rc = run(b, w_draw_glyphs, &a);
        if ((perr = (LONG *)GetTagData(OFA_GPUError, 0, tags)) != NULL)
            *perr = a.gpu_error;
        if ((pstats = (OFGpuStats *)GetTagData(OFA_GPUStats, 0, tags)) != NULL)
            *pstats = a.stats;
    }
    if (rc < 0)
        set_error(tags, rc);
    return rc;
}

/* ---- the library itself ---- */

static struct Library *lib_init(REG(d0, struct OpenFontBase *b), REG(a0, BPTR seglist), REG(a6, struct ExecBase *sys));
static struct Library *lib_open(BASE);
static BPTR lib_close(BASE);
static BPTR lib_expunge(BASE);
static ULONG lib_null(void);

static const APTR lib_vectors[] = {
    (APTR)lib_open, (APTR)lib_close, (APTR)lib_expunge, (APTR)lib_null,
    (APTR)L_Version,            /* -30 */
    (APTR)L_OpenFaceA,          /* -36 */
    (APTR)L_CloseFace,          /* -42 */
    (APTR)L_SetSize,            /* -48 */
    (APTR)L_GetFaceInfo,        /* -54 */
    (APTR)L_ShapeText,          /* -60 */
    (APTR)L_MeasureText,        /* -66 */
    (APTR)L_RasterGlyph,        /* -72 */
    (APTR)L_FreeBitmap,         /* -78 */
    (APTR)L_FindFaceA,          /* -84 */
    (APTR)L_AddFontDir,         /* -90 */
    (APTR)L_AddFallback,        /* -96 */
    (APTR)L_GetFallback,        /* -102 */
    (APTR)L_CharIndex,          /* -108 */
    (APTR)L_OpenCache,          /* -114 */
    (APTR)L_CloseCache,         /* -120 */
    (APTR)L_CacheGetGlyph,      /* -126 */
    (APTR)L_CacheMatchesFace,   /* -132 */
    (APTR)L_ErrorString,        /* -138 */
    (APTR)L_RescanFonts,        /* -144 */
    (APTR)L_RasterGlyphMatrix,  /* -150, revision 1 */
    (APTR)L_GlyphAdvance,       /* -156, revision 1 */
    (APTR)L_OpenTextFont,       /* -162, revision 2 */
    (APTR)L_ShapeA,             /* -168, revision 3 */
    (APTR)L_GetFaceMetrics,     /* -174, revision 3 */
    (APTR)L_GlyphOutline,       /* -180, revision 3 */
    (APTR)L_GlyphExtents,       /* -186, revision 3 */
    (APTR)L_GetFontTable,       /* -192, revision 3 */
    (APTR)L_OpenFaceMemoryA,    /* -198, revision 3 */
    (APTR)L_FindFaceForCharA,   /* -204, revision 3 */
    (APTR)L_GlyphAdvancePx,     /* -210, revision 3 */
    (APTR)L_GlyphColorBitmap,   /* -216, revision 4 */
    (APTR)L_FreeColorBitmap,    /* -222, revision 4 */
    (APTR)L_DrawGlyphsA,        /* -228, revision 5 */
    (APTR)-1
};

static const struct {
    ULONG size;
    const APTR *vectors;
    APTR data;
    APTR init;
} lib_inittable = {
    sizeof(struct OpenFontBase), lib_vectors, NULL, (APTR)lib_init
};

const struct Resident lib_romtag __attribute__((used)) = {
    RTC_MATCHWORD, (struct Resident *)&lib_romtag, (APTR)(&lib_romtag + 1),
    RTF_AUTOINIT, LIB_VERSION, NT_LIBRARY, 0,
    (char *)lib_name, (char *)lib_id, (APTR)&lib_inittable
};

/* Expunge runs under Forbid: nothing here may wait. Every byte the reader,
 * the shaper and the index hold is in the pool, so deleting it is enough,
 * and none of their code runs here. */
static void lib_free(struct OpenFontBase *b)
{
    /* the glyph cache first: its pages are AllocVec'd, its own blocks in the pool */
    of_gpu_destroy(b->gpu);
    b->gpu = NULL;
    if (OpenGPUBase)
        CloseLibrary(OpenGPUBase);
    OpenGPUBase = NULL;
    b->gpu_tried = 0;
    b->ctx = NULL;
    memset(&b->index, 0, sizeof b->index);
    of_runtime_exit();
    if (b->stack)
        FreeMem(b->stack, STACK_SIZE);
    b->stack = NULL;
    of_textfont_cleanup();
    if (DataTypesBase)
        CloseLibrary(DataTypesBase);
    DataTypesBase = NULL;
    b->datatypes_tried = 0;
    if (UtilityBase)
        CloseLibrary(UtilityBase);
    UtilityBase = NULL;
    if (DOSBase)
        CloseLibrary((struct Library *)DOSBase);
    DOSBase = NULL;
}

static struct Library *lib_init(REG(d0, struct OpenFontBase *b), REG(a0, BPTR seglist), REG(a6, struct ExecBase *sys))
{
    SysBase = sys;
    b->seglist = seglist;
    /* MakeLibrary fills only the sizes (there is no InitStruct table): the
     * node and the version are ours to set, before exec adds the library */
    b->lib.lib_Node.ln_Type = NT_LIBRARY;
    b->lib.lib_Node.ln_Name = (char *)lib_name;
    b->lib.lib_Flags = LIBF_SUMUSED | LIBF_CHANGED;
    b->lib.lib_Version = LIB_VERSION;
    b->lib.lib_Revision = LIB_REVISION;
    b->lib.lib_IdString = (APTR)lib_id;
    InitSemaphore(&b->lock);
    /* built for the 68040 only, with its FPU (-m68881): exec reports a 68040
     * and its working FPU (a 68060 with 68060.library reports both too). An
     * LC040 has no FPU, and the library's floating point would trap there. */
    if ((sys->AttnFlags & (AFF_68040 | AFF_FPU40)) != (AFF_68040 | AFF_FPU40))
        goto fail;
    DOSBase = (struct DosLibrary *)OpenLibrary((CONST_STRPTR)"dos.library", 39);
    UtilityBase = OpenLibrary((CONST_STRPTR)"utility.library", 39);
    b->stack = AllocMem(STACK_SIZE, MEMF_ANY);
    if (!DOSBase || !UtilityBase || !b->stack || !of_runtime_init() || !of_textfont_setup(&b->fonts))
        goto fail;
    of_index_init(&b->index);
    return &b->lib;
fail:
    lib_free(b);
    FreeMem((UBYTE *)b - b->lib.lib_NegSize, b->lib.lib_NegSize + b->lib.lib_PosSize);
    return NULL;
}

static struct Library *lib_open(BASE)
{
    b->lib.lib_OpenCnt++;
    b->lib.lib_Flags &= ~LIBF_DELEXP;
    return &b->lib;
}

static BPTR lib_close(BASE)
{
    if (--b->lib.lib_OpenCnt == 0 && (b->lib.lib_Flags & LIBF_DELEXP))
        return lib_expunge(b);
    return 0;
}

static BPTR lib_expunge(BASE)
{
    BPTR seglist;
    /* the fonts OF_OpenTextFont loaded go first; while one is still in use
     * the library stays */
    if (b->lib.lib_OpenCnt || !of_textfont_flush(&b->fonts)) {
        b->lib.lib_Flags |= LIBF_DELEXP;
        return 0;
    }
    seglist = b->seglist;
    Remove(&b->lib.lib_Node);
    lib_free(b);
    FreeMem((UBYTE *)b - b->lib.lib_NegSize, b->lib.lib_NegSize + b->lib.lib_PosSize);
    return seglist;
}

static ULONG lib_null(void)
{
    return 0;
}
