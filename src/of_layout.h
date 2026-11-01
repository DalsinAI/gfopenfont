/* GFOpenFont's OpenType layout: GDEF, GSUB and GPOS, applied to a run of
 * glyphs, and the shaper around them (of_shape.c).
 *
 * Copyright (c) 2026 AmiCradle Team. MIT licence (see LICENSE).
 *
 * Written from the OpenType specification (GDEF, GSUB, GPOS, the common
 * table formats, and 'kern'). No HarfBuzz code. */
#ifndef OF_LAYOUT_H
#define OF_LAYOUT_H

#include <stddef.h>
#include <stdint.h>

#include "of_font.h"
#include "gfopenfont/gfopenfont.h"

/* glyph classes (GDEF) */
#define OF_CLASS_BASE      1
#define OF_CLASS_LIGATURE  2
#define OF_CLASS_MARK      3
#define OF_CLASS_COMPONENT 4

/* OFShGlyph.props */
#define OF_SH_IGNORABLE   0x0001  /* a default ignorable: hidden after shaping */
#define OF_SH_ZWJ         0x0002
#define OF_SH_ZWNJ        0x0004
#define OF_SH_UNSAFE      0x0008  /* breaking before its cluster changes the shaping */
#define OF_SH_LIGATED     0x0010
#define OF_SH_MULTIPLIED  0x0020
#define OF_SH_SPACE_FALLBACK 0x0040 /* a space the font lacks, drawn as its space */
#define OF_SH_CONTINUATION 0x0080 /* joins the cluster before it */

/* feature masks */
#define OF_MASK_GLOBAL 0x01
#define OF_MASK_FRAC   0x02
#define OF_MASK_NUMR   0x04
#define OF_MASK_DNOM   0x08
/* Arabic-style joining forms (ArabicShaping.txt), one per glyph */
#define OF_MASK_ISOL   0x10
#define OF_MASK_FINA   0x20
#define OF_MASK_MEDI   0x40
#define OF_MASK_INIT   0x80
/* the first mask of a feature over part of the text; the n-th such
 * feature has OF_MASK_RANGED << n */
#define OF_MASK_RANGED 0x100

/* attachment kinds */
#define OF_ATTACH_NONE   0
#define OF_ATTACH_MARK   1
#define OF_ATTACH_CURSIVE 2

typedef struct OFShGlyph {
    uint32_t cp;                /* the character it came from */
    uint32_t gid;
    uint32_t cluster;
    uint32_t mask;
    uint16_t props;
    uint8_t gclass;             /* OF_CLASS_* */
    uint8_t mark_class;         /* GDEF mark attachment class */
    uint8_t lig_id;             /* the ligature it is in or under, 0 none */
    uint8_t lig_comp;           /* its component of that ligature, from 1 */
    uint8_t lig_count;          /* a ligature: how many components */
    uint8_t attach_kind;
    int32_t attach_to;          /* the glyph it hangs from, by index */
    uint8_t space_kind;         /* for a space fallback: which space */
    int32_t xa, ya, xo, yo;     /* 26.6 */
} OFShGlyph;

typedef struct OFShBuf {
    OFShGlyph *g;
    uint32_t n, cap;
    int failed;
    uint8_t next_lig_id;
} OFShBuf;

/* grows the buffer for one more; 0 when out of memory (failed set) */
int of_shbuf_reserve(OFShBuf *b, uint32_t more);

/* breaking inside glyphs [from, to] (whole clusters) changes the shaping:
 * each glyph there whose cluster is not the first's gets OF_SH_UNSAFE */
void of_layout_unsafe(OFShBuf *b, long from, long to);

/* The layout tables' tags, as 32-bit numbers */
#define OF_TAG(a, b, c, d) (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | ((uint32_t)(c) << 8) | (uint32_t)(d))

/* What a run is shaped with beyond its font. */
typedef struct OFLayoutSpec {
    uint32_t script_tag;        /* OpenType script tag */
    uint32_t lang_tag;          /* OpenType language system tag, 0: the default */
    int rtl;                    /* right to left: the buffer is still in logical order */
    int joining;                /* the run has joining characters: isol, fina, medi, init */
    const OFFeature *features;  /* the program's, at most OF_MAX_FEATURES */
    uint32_t nfeatures;
    uint32_t ranged_mask[OF_MAX_FEATURES]; /* each feature's mask, 0 for one over the whole text */
} OFLayoutSpec;

/* whether tag is one of the features the shaper turns on by itself for
 * spec's direction and joining (before the program's) */
int of_layout_default_feature(const OFLayoutSpec *spec, uint32_t tag);

/* The glyph classes of b from GDEF, or from the characters when the font
 * has none (marks by their Unicode category). */
void of_layout_classes(const OFFont *f, OFShBuf *b);

/* GSUB's lookups for spec's script, language and features. OF_OK or
 * OF_ERR_MEMORY. */
int of_layout_substitute(const OFFont *f, OFShBuf *b, const OFLayoutSpec *spec);

/* whether GPOS or GSUB has the feature (one the shaper uses) for the script */
int of_layout_has_gpos_feature(const OFFont *f, const OFLayoutSpec *spec, uint32_t feature);
int of_layout_has_gsub_feature(const OFFont *f, const OFLayoutSpec *spec, uint32_t feature);

/* GPOS's lookups: values in 26.6 at ppem, added to the advances already in
 * b; then marks' advances zeroed and attachments resolved, for spec's
 * direction (b still in logical order). Uses 'kern' when GPOS has no
 * kerning and the program has not turned 'kern' off. */
int of_layout_position(const OFFont *f, OFShBuf *b, const OFLayoutSpec *spec, uint32_t ppem);

/* ---- the shaper (of_shape.c) ---- */

/* A run's script, language, direction and features as the shaper takes
 * them: script and direction OF_SCRIPT_UNKNOWN and OF_DIR_AUTO are worked
 * out from the text the first time and kept, for the pieces of the same
 * run a fallback face shapes. */
typedef struct OFRunProps {
    unsigned script;            /* OF_SCRIPT_* */
    uint32_t script_tag;        /* OpenType tag; 0: from script */
    uint32_t lang_tag;
    uint32_t direction;         /* OF_DIR_* */
    const OFFeature *features;
    uint32_t nfeatures;
} OFRunProps;

/* Shapes text[off, off + len) (UTF-8; the rest of text is context) with f
 * at ppem pixels, into out (emptied first): glyphs in visual order, left to
 * right, their clusters byte offsets into text. Feature ranges are byte
 * offsets into text. OF_OK or OF_ERR_MEMORY. */
int of_shape_text(const OFFont *f, uint32_t ppem, const char *text, size_t bytes,
                  size_t off, size_t len, OFRunProps *props, OFShBuf *out);

/* the OpenType script tag for a script (OF_SCRIPT_*), DFLT when none */
uint32_t of_script_tag(unsigned script);

/* the script (OF_SCRIPT_*) an ISO 15924 tag ('Arab') names, or
 * OF_SCRIPT_UNKNOWN */
unsigned of_script_from_iso(uint32_t iso);

/* whether a script is written right to left */
int of_script_rtl(unsigned script);

#endif
