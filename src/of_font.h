/* GFOpenFont's own font reader and outline rasteriser.
 *
 * Copyright (c) 2026 AmigaChrome Team. MIT licence (see LICENSE).
 *
 * Written from the OpenType specification (the sfnt container, cmap, head,
 * hhea, hmtx, maxp, name, OS/2, post, loca and glyf tables, and the CFF and
 * Type 2 charstring specifications Adobe publishes). No FreeType code.
 *
 * Integer arithmetic only, so the 68040 needs no floating point here, and the
 * rasteriser (of_raster.c) is a pure function of an outline: it can move to
 * GFOpenGPU later as it is. */
#ifndef OF_FONT_H
#define OF_FONT_H

#include <stddef.h>
#include <stdint.h>

/* ---- an outline ---- */

/* point tags */
#define OF_PT_ON     1      /* on the curve */
#define OF_PT_QUAD   0      /* a quadratic control point (TrueType) */
#define OF_PT_CUBIC  2      /* a cubic control point (CFF); they come in pairs */

/* Points in 16.16 font units (OFPath from a font) or 26.6 pixels (after
 * of_path_scale). Contours are closed. */
typedef struct OFPath {
    int32_t *x, *y;
    uint8_t *tag;
    uint32_t *ends;         /* index of each contour's last point */
    uint32_t npoints, ncontours;
    uint32_t cap_points, cap_contours;
    int failed;             /* out of memory while building */
} OFPath;

void of_path_init(OFPath *p);
void of_path_free(OFPath *p);
void of_path_reset(OFPath *p);
void of_path_point(OFPath *p, int32_t x, int32_t y, uint8_t tag);
void of_path_close(OFPath *p);   /* ends the contour begun since the last close */

/* x, y in 16.16 font units to 26.6 pixels at ppem pixels per em. */
void of_path_scale(OFPath *p, uint32_t units_per_em, uint32_t ppem);
/* ppem * 64 / units_per_em, 16.16, rounded: font units to 26.6 */
int32_t of_scale_16_16(uint32_t units_per_em, uint32_t ppem);

/* x, y in 16.16 font units to 26.6 pixels through a 2 x 2 matrix m of
 * 16.16 pixels to the em, y up: X = (x m[0] + y m[1]) / units_per_em,
 * Y = (x m[2] + y m[3]) / units_per_em, each rounded half up. m = {ppem,
 * 0, 0, ppem} (as 16.16) is of_path_scale's size; the other entries shear,
 * rotate or stretch it. OF_ERR_ARGUMENT when an entry is beyond 1024
 * pixels to the em either way, or units_per_em is 0. */
int of_path_transform(OFPath *p, uint32_t units_per_em, const int32_t m[4]);

/* ---- the rasteriser ---- */

/* An A8 bitmap of a 26.6 outline, with non-zero winding: the outline's
 * control box rounded out to whole pixels gives its size and place. left and
 * top are pixels from the origin (top above the baseline). The pixels are
 * malloc'd (NULL for an empty outline). OF_OK or OF_ERR_MEMORY. */
int of_raster_a8(const OFPath *path, uint32_t *width, uint32_t *height,
                 int32_t *left, int32_t *top, uint8_t **pixels);

/* ---- a font file ---- */

typedef struct OFCff OFCff;

typedef struct OFTable {
    uint32_t off, len;
} OFTable;

typedef struct OFFont {
    const uint8_t *data;
    size_t size;
    uint32_t num_faces;             /* in the file: a collection holds several */
    uint32_t dir;                   /* this face's table directory */
    OFTable head, hhea, hmtx, maxp, cmap, loca, glyf, cff, cff2, name, os2, post,
            kern, gsub, gpos, gdef, colr, cbdt, cblc, sbix, svg;
    uint16_t units_per_em;
    uint16_t mac_style;
    int16_t index_to_loc;           /* 0 short, 1 long */
    uint16_t num_glyphs;
    uint16_t num_hmetrics;
    int16_t ascender, descender, line_gap;   /* hhea */
    int16_t x_min, y_min, x_max, y_max;      /* head */
    /* OS/2 (os2_version 0xFFFF: none) */
    uint16_t os2_version, weight, width, fs_selection;
    int16_t typo_ascender, typo_descender, typo_line_gap;
    uint16_t win_ascent, win_descent;
    uint32_t unicode_ranges[4];
    uint32_t is_fixed_pitch;        /* post */
    uint32_t cmap_sub;              /* the subtable used (offset in the file) */
    uint16_t cmap_format;
    OFCff *cff_font;                /* the CFF outlines, or NULL */
    int scalable;                   /* it has outlines (glyf, CFF or CFF2) */
} OFFont;

/* The faces in data: 1 for a plain font, the count of a collection, 0 when
 * data is no font. */
uint32_t of_font_count(const uint8_t *data, size_t size);

/* OF_OK, OF_ERR_FONT (not a font this reads, or face_index past the end),
 * OF_ERR_MEMORY. data must outlive the font. A font without outlines this
 * reads (CFF2, bitmaps only) opens; its outlines are OF_ERR_FONT. */
int of_font_open(OFFont *f, const uint8_t *data, size_t size, uint32_t face_index);

/* Only what describes the face (head, hhea, maxp, name, OS/2, post and the
 * table directory): for probing, where data may hold just those tables. Its
 * glyphs, metrics and outlines are not for use. */
int of_font_open_info(OFFont *f, const uint8_t *data, size_t size, uint32_t face_index);
void of_font_close(OFFont *f);

/* The glyph a codepoint maps to, 0 when none. */
uint32_t of_font_glyph(const OFFont *f, uint32_t codepoint);

/* The characters the font's cmap maps, as [first, last] pairs in r (at most
 * cap of them), ascending. Returns how many pairs there are, which may be
 * more than cap. A range may hold characters without a glyph (format 4's
 * segments count whole); a Symbol font's U+F0xx stand-ins are not mapped
 * back to U+00xx. */
size_t of_font_cmap_ranges(const OFFont *f, uint32_t *r, size_t cap);

/* A glyph's advance and left side bearing in font units. */
void of_font_hmetrics(const OFFont *f, uint32_t glyph, uint32_t *advance, int32_t *lsb);

/* A name (nameID: 1 family, 2 style, 16 typographic family, 17 typographic
 * style...) in UTF-8, from the Windows Unicode (3,1 or 3,10) English record
 * first, then any Unicode or Macintosh Roman one. 0 when the font has none. */
int of_font_name(const OFFont *f, uint16_t name_id, char *out, size_t size);

/* A glyph's outline in 16.16 font units, appended to path. OF_OK, OF_ERR_FONT
 * for a glyph the font can't give, OF_ERR_MEMORY. An empty glyph (a space)
 * appends nothing and is OF_OK. */
int of_font_outline(const OFFont *f, uint32_t glyph, OFPath *path);

/* The same in 26.6 pixels at ppem pixels per em. A TrueType composite is
 * scaled as it is put together: each component's points, then its offset on
 * its own, its transform applied to the scaled points. */
int of_font_outline_px(const OFFont *f, uint32_t glyph, uint32_t ppem, OFPath *path);

/* A glyph's advance in font units, as hmtx gives it (a TrueType composite
 * with USE_MY_METRICS: its component's). */
uint32_t of_font_advance_units(const OFFont *f, uint32_t glyph);

/* A glyph's advance in 26.6 pixels at ppem, not rounded to whole pixels. */
int32_t of_font_advance_px(const OFFont *f, uint32_t glyph, uint32_t ppem);

/* The face's line metrics in font units: OS/2's typographic ones when the
 * font asks for them (USE_TYPO_METRICS), else hhea's, or the typographic
 * ones when hhea has none, or the Windows ones when neither has. */
void of_font_line_units(const OFFont *f, int32_t *ascender, int32_t *descender, int32_t *height);

/* The same at ppem in 26.6: the ascender rounded up to a whole pixel, the
 * descender down, the height to the nearest. */
void of_font_line_px(const OFFont *f, uint32_t ppem, int32_t *ascender, int32_t *descender, int32_t *height);

/* style bits: OS/2 fsSelection when there is an OS/2 table, else head's
 * macStyle */
#define OF_STYLE_BOLD   1
#define OF_STYLE_ITALIC 2
unsigned of_font_style(const OFFont *f);

/* whether it has colour glyphs (COLR, CBDT, sbix or SVG) */
int of_font_has_color(const OFFont *f);

/* A glyph's PNG colour bitmap (CBDT/CBLC or sbix) from the strike nearest
 * ppem (gfopenfont.h, of_face_color_png). OF_OK, or OF_ERR_FONT when the
 * glyph has none. */
struct OFColorPng;
int of_font_color_png(const OFFont *f, uint32_t glyph, uint32_t ppem, struct OFColorPng *out);

/* ---- internal: CFF (of_cff.c) ---- */

int of_cff_open(OFCff **out, const uint8_t *data, uint32_t size, uint32_t units_per_em);
void of_cff_close(OFCff *cff);
int of_cff_outline(const OFCff *cff, uint32_t glyph, OFPath *path);

/* big-endian reads, bounds checked by the callers */
static inline uint16_t of_u16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static inline int16_t of_s16(const uint8_t *p) { return (int16_t)of_u16(p); }
static inline uint32_t of_u32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

#endif
