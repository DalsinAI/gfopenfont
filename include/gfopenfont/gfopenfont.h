#ifndef OPENFONT_OPENFONT_H
#define OPENFONT_OPENFONT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define OF_VERSION_MAJOR 0
#define OF_VERSION_MINOR 6
#define OF_VERSION_PATCH 0

/* Glyphs in one of_cache_fill_range call before the GFOpenMulticore path is worth it.
 * Below this, the same worker runs in a loop on this CPU. */
#define OF_RANGE_BREAK_EVEN 8
#define OF_RANGE_PIXELS 4096

typedef struct OFContext OFContext;
typedef struct OFFace OFFace;
typedef struct OFCache OFCache;

/* A face's fallbacks: up to this many, tried in order for text the face
 * itself has no glyphs for. */
#define OF_MAX_FALLBACKS 8

/* OFGlyph.flags */
#define OF_GLYPH_MISSING        0x0001  /* no face in the chain has it: glyph 0 of face 0 */
#define OF_GLYPH_UNSAFE_TO_BREAK 0x0002 /* breaking the run here changes the shaping */

typedef struct OFGlyph {
    uint32_t glyph_id;
    uint32_t cluster;               /* byte offset in the text of its cluster's first byte */
    int32_t x_advance_26_6;
    int32_t y_advance_26_6;
    int32_t x_offset_26_6;
    int32_t y_offset_26_6;
    uint16_t face;                  /* 0: the face shaped; n: its n-th fallback */
    uint16_t flags;
} OFGlyph;

/* OFFaceInfo.flags */
#define OF_FACE_ITALIC   0x0001
#define OF_FACE_FIXED    0x0002     /* every glyph the same width */
#define OF_FACE_SCALABLE 0x0004
#define OF_FACE_COLOR    0x0008

typedef struct OFFaceInfo {
    char family[64];
    char style[64];
    uint16_t weight;                /* 100 to 900, 400 regular, 700 bold */
    uint16_t width;                 /* 1 to 9, 5 normal (OS/2 usWidthClass) */
    uint16_t flags;
    uint16_t units_per_em;
    uint32_t pixel_size;
    uint32_t num_glyphs;
    uint32_t num_faces;             /* in its file (a collection holds several) */
    int32_t ascender_26_6;          /* at the current pixel size */
    int32_t descender_26_6;         /* below the baseline: negative */
    int32_t height_26_6;            /* baseline to baseline */
    uint32_t unicode_ranges[4];     /* OS/2 ulUnicodeRange1-4, 0 when absent */
} OFFaceInfo;

typedef struct OFBitmap {
    uint32_t width;
    uint32_t height;
    int32_t left;
    int32_t top;
    int32_t advance_x_26_6;
    uint32_t stride;
    uint8_t *pixels;
} OFBitmap;

/* One codepoint of a range. sizeof is a multiple of 16 so it can be an
 * GFOpenMulticore slice: index i writes only slots[i]. */
typedef struct OFRangeSlot {
    int32_t status;
    uint32_t glyph_id;
    int32_t left;
    int32_t top;
    int32_t advance_x_26_6;
    uint32_t width;
    uint32_t height;
    uint32_t npixels;
    uint8_t pixels[OF_RANGE_PIXELS];
} OFRangeSlot;

typedef int (*OFIndexFn)(uint32_t index, void *arg);

/* run_range may call fn for distinct indices at the same time. fn writes
 * only its own index. NULL run_range, or a count below break_even, stays
 * on this CPU. break_even 0 means OF_RANGE_BREAK_EVEN. */
typedef struct OFExec {
    int (*run_range)(OFIndexFn fn, uint32_t count, void *arg, void *user);
    void *user;
    uint32_t break_even;
} OFExec;

enum {
    OF_OK = 0,
    OF_ERR_ARGUMENT = -1,
    OF_ERR_MEMORY = -2,
    OF_ERR_IO = -3,
    OF_ERR_FONT = -4,
    OF_ERR_RANGE = -5,
    OF_ERR_BUFFER = -6,
    OF_ERR_CACHE = -7,
    OF_ERR_EXEC = -8
};

OFContext *of_context_create(void);
void of_context_destroy(OFContext *ctx);

OFFace *of_face_open(OFContext *ctx, const char *path);
/* The face_index-th face of a file (a .ttc or .otc holds several). */
OFFace *of_face_open_index(OFContext *ctx, const char *path, uint32_t face_index);
/* The same, with why it failed in *error when error is not NULL:
 * OF_ERR_IO (the file), OF_ERR_MEMORY, OF_ERR_FONT (not a font GFOpenFont
 * reads), OF_ERR_ARGUMENT. */
OFFace *of_face_open_file(OFContext *ctx, const char *path, uint32_t face_index, int *error);
/* A face from a font in memory. With take set, the face frees data with
 * free() when it is closed; otherwise data must outlive the face. */
OFFace *of_face_open_memory(OFContext *ctx, uint8_t *data, size_t size,
                            uint32_t face_index, int take);
/* Faces are counted: a face is freed when its last holder closes it. A
 * fallback holds the faces it falls back to. */
void of_face_close(OFFace *face);
int of_face_set_pixel_size(OFFace *face, uint32_t pixels);
int of_face_info(OFFace *face, OFFaceInfo *info);
/* The glyph a codepoint maps to in this face alone, 0 when it has none. */
uint32_t of_face_char_index(OFFace *face, uint32_t codepoint);
/* The characters the face's cmap maps, as [first, last] pairs (see
 * of_font_cmap_ranges): how many there are, which may be more than cap. */
size_t of_face_cmap_ranges(OFFace *face, uint32_t *ranges, size_t cap);

/* Adds fallback to the end of face's chain, holding it. OF_ERR_RANGE when
 * the chain is full, OF_ERR_ARGUMENT for a face falling back to itself or
 * to a face whose fallbacks lead back to it, however far. */
int of_face_add_fallback(OFFace *face, OFFace *fallback);
uint32_t of_face_fallback_count(const OFFace *face);
/* 0: face itself; 1..count: its fallbacks, set to face's pixel size (a
 * fallback may serve several faces), so its glyphs draw as they shaped.
 * NULL past the end. Not held. */
OFFace *of_face_fallback(OFFace *face, uint32_t n);

/* Shapes text (OpenType: ligatures, kerning, marks, the script's own
 * rules), falling back cluster by cluster to the face's fallbacks where it
 * has no glyph. Writes up to capacity glyphs and returns how many the run
 * has, so a call with out NULL measures what to allocate. 0 on failure. */
size_t of_shape_utf8(OFFace *face, const char *utf8, size_t bytes,
                     OFGlyph *out, size_t capacity);
int64_t of_measure_utf8(OFFace *face, const char *utf8, size_t bytes);

/* An OpenType feature a program turns on or off ('liga' 0, 'smcp' 1,
 * 'salt' 2 for the second alternate), over the text's [start, end): bytes
 * for UTF-8, 16-bit units for UTF-16, counted from the start of the whole
 * text. start 0 and end OF_FEATURE_END cover all of it. */
#define OF_FEATURE_END 0xFFFFFFFFu
typedef struct OFFeature {
    uint32_t tag;
    uint32_t value;
    uint32_t start;
    uint32_t end;
} OFFeature;

/* At most this many features in one call; more are ignored. A feature
 * over part of the text counts towards OF_MAX_RANGED_FEATURES too. */
#define OF_MAX_FEATURES 32
#define OF_MAX_RANGED_FEATURES 24

/* OFShapeOptions.direction */
#define OF_DIR_AUTO 0               /* from the run's script */
#define OF_DIR_LTR  1
#define OF_DIR_RTL  2

#define OF_MAKE_TAG(a, b, c, d) (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | ((uint32_t)(c) << 8) | (uint32_t)(d))

/* How a run is shaped beyond its face. Zeroed, it is what of_shape_utf8
 * does: the script and direction from the text, the script's default
 * language system, the default features. */
typedef struct OFShapeOptions {
    uint32_t script;                /* ISO 15924 tag ('Latn', 'Arab'), 0: from the text */
    uint32_t language;              /* OpenType language system tag ('TRK '), 0: the default */
    uint32_t direction;             /* OF_DIR_* */
    const OFFeature *features;
    uint32_t num_features;
} OFShapeOptions;

/* Shapes text[off, off + len), the whole text as its context, with
 * options (NULL: the defaults). Glyphs come in visual order, left to
 * right: a right-to-left run's last character first. Each glyph's cluster
 * is the offset of its cluster's first character in the whole text: bytes
 * for of_shape, 16-bit units for of_shape_utf16 (a pair of surrogates is
 * one character, a lone surrogate is U+FFFD). Writes up to capacity
 * glyphs and returns how many the run has; 0 on failure. */
size_t of_shape(OFFace *face, const char *utf8, size_t bytes, size_t off, size_t len,
                const OFShapeOptions *options, OFGlyph *out, size_t capacity);
size_t of_shape_utf16(OFFace *face, const uint16_t *text, size_t units, size_t off, size_t len,
                      const OFShapeOptions *options, OFGlyph *out, size_t capacity);

/* A face's measurements at its pixel size, 26.6 pixels, y up. */
typedef struct OFFaceMetrics {
    uint32_t units_per_em;
    uint32_t pixel_size;
    int32_t ascender;               /* the line's, as OFFaceInfo's */
    int32_t descender;              /* negative below the baseline */
    int32_t line_gap;
    int32_t x_height;               /* OS/2, else measured from 'x'; 0 when neither */
    int32_t cap_height;             /* OS/2, else measured from 'H'; 0 when neither */
    int32_t underline_position;     /* post: the top of the underline, negative below */
    int32_t underline_thickness;
    int32_t strikeout_position;     /* OS/2, else 0 */
    int32_t strikeout_thickness;
    int32_t max_advance;            /* hhea advanceWidthMax */
    int32_t space_advance;          /* U+0020's, 0 when it has none */
    uint32_t flags;                 /* OF_FACE_* */
} OFFaceMetrics;

int of_face_metrics(OFFace *face, OFFaceMetrics *m);

/* A glyph's horizontal advance at the face's pixel size, 26.6; -1 for a
 * glyph the face doesn't have. */
int32_t of_face_glyph_advance_px(OFFace *face, uint32_t glyph_id);

/* A glyph's ink box at the face's pixel size, 26.6, y up: the box of its
 * outline's points (of its bitmap for a face without outlines). */
typedef struct OFGlyphExtents {
    int32_t x_bearing;              /* from the pen to the box's left edge */
    int32_t y_bearing;              /* from the baseline up to its top */
    int32_t width;
    int32_t height;                 /* positive: the box goes down from y_bearing */
} OFGlyphExtents;

int of_face_glyph_extents(OFFace *face, uint32_t glyph_id, OFGlyphExtents *e);

/* A glyph's outline at the face's pixel size as verbs and points, 26.6,
 * y up: OF_PATH_MOVE and OF_PATH_LINE take one point, OF_PATH_QUAD two
 * (control, end), OF_PATH_CUBIC three, OF_PATH_CLOSE none. Each contour
 * starts with a move and ends with a close. Writes what fits (points as
 * x, y pairs: 2 int32_t each) and sets *nverbs and *npoints to the whole
 * outline's; OF_ERR_BUFFER when it did not fit, so a call with no room
 * measures. A space has no verbs. */
#define OF_PATH_MOVE  0
#define OF_PATH_LINE  1
#define OF_PATH_QUAD  2
#define OF_PATH_CUBIC 3
#define OF_PATH_CLOSE 4
int of_face_glyph_outline(OFFace *face, uint32_t glyph_id,
                          uint8_t *verbs, size_t verb_capacity,
                          int32_t *points, size_t point_capacity,
                          size_t *nverbs, size_t *npoints);

/* The face's table tag ('MATH', 'OS/2') as it is in the font, or NULL
 * when it has none. It lives as long as the face. */
const uint8_t *of_face_table(OFFace *face, uint32_t tag, size_t *length);

/* A glyph's colour bitmap as the font holds it: a PNG image from CBDT/CBLC
 * (Google's colour fonts, such as Noto Color Emoji) or sbix (Apple's), from
 * the strike nearest ppem: the smallest at least ppem, else the largest
 * (ppem 0: the largest). The image lives as long as the face; decoding it
 * is the caller's (gfopenfont.library's OF_GlyphColorBitmap does it through
 * the PNG datatype). OF_OK; OF_ERR_FONT when the glyph has no PNG colour
 * bitmap; OF_ERR_RANGE for a glyph the face doesn't have. */
typedef struct OFColorPng {
    const uint8_t *png;
    uint32_t length;
    uint32_t ppem;                  /* the strike's: scale by size / ppem */
    int32_t left;                   /* the image's left edge from the pen, strike pixels */
    int32_t top;                    /* its top edge above the baseline, strike pixels */
    uint32_t width, height;         /* the image's, from its header */
} OFColorPng;

int of_face_color_png(OFFace *face, uint32_t glyph_id, uint32_t ppem, OFColorPng *out);

int of_raster_glyph_a8(OFFace *face, uint32_t glyph_id, OFBitmap *out);
/* The same through a 2 x 2 matrix m of 16.16 pixels to the em, y up
 * (of_path_transform in src/of_font.h): {size, 0, 0, size} draws the glyph
 * as of_raster_glyph_a8 does at that size, the other entries stretch,
 * shear or rotate it. The face's own pixel size is not used or changed.
 * out's advance_x_26_6 is the advance's x through m. For engines with
 * their own transforms, such as bullet.library's. */
int of_raster_glyph_matrix_a8(OFFace *face, uint32_t glyph_id, const int32_t m[4], OFBitmap *out);
/* A glyph's advance in font units; -1 for a glyph the face doesn't have. */
int32_t of_face_glyph_advance(OFFace *face, uint32_t glyph_id);
void of_bitmap_release(OFBitmap *bitmap);

uint64_t of_face_source_fingerprint(const OFFace *face);

int of_compile_cache(OFFace *face, const char *path,
                     const uint16_t *sizes, size_t size_count,
                     uint32_t first_codepoint, uint32_t last_codepoint);

OFCache *of_cache_open(const char *path);
void of_cache_close(OFCache *cache);
uint64_t of_cache_source_fingerprint(const OFCache *cache);
int of_cache_matches_face(const OFCache *cache, const OFFace *face);

/* Returns a malloc-owned A8 bitmap compatible with of_bitmap_release().
 * glyph_id may be NULL. */
int of_cache_get_a8(OFCache *cache, uint16_t pixel_size, uint32_t codepoint,
                    uint32_t *glyph_id, OFBitmap *out);

/* Copy one cached glyph into slot. No allocation. A bitmap larger than
 * OF_RANGE_PIXELS sets OF_ERR_BUFFER and npixels to the size needed. */
int of_cache_fill_slot(const OFCache *cache, uint16_t pixel_size,
                       uint32_t codepoint, OFRangeSlot *slot);

/* Fill slots[0 .. count) for codepoints first .. first+count-1.
 * exec NULL is the 68k fallback. The same worker runs either way. */
int of_cache_fill_range(const OFCache *cache, uint16_t pixel_size,
                        uint32_t first_codepoint, uint32_t count,
                        OFRangeSlot *slots, const OFExec *exec);

const char *of_error_string(int code);

#ifdef __cplusplus
}
#endif

#endif
