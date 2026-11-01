/* GFOpenFont's shaper: UTF-8 text to positioned glyphs, for left-to-right
 * scripts written letter by letter (Latin, Greek, Cyrillic and the like).
 *
 * Copyright (c) 2026 AmigaChrome Team. MIT licence (see LICENSE).
 *
 * Written from the OpenType specification (its default features and
 * layout processing) and the Unicode Standard (canonical equivalence, the
 * default ignorables, grapheme clusters). No HarfBuzz code.
 *
 * The steps:
 *   1. UTF-8 to characters, each with the byte offset it starts at (its
 *      cluster); a byte that starts no valid sequence is one U+FFFD;
 *   2. marks and joiners join the cluster of the character before them;
 *   3. characters the font lacks are decomposed into ones it has, marks put
 *      in canonical order, and base and mark composed again where the font
 *      has the composed character;
 *   4. characters to glyphs (cmap); a space the font lacks is drawn as its
 *      space, as wide as that space should be;
 *   5. GSUB with the default features (fractions only around U+2044);
 *   6. advances, then GPOS (or 'kern'), marks' advances zeroed and marks
 *      placed on their bases;
 *   7. the default ignorables hidden: the space glyph, no advance. */
#include "gfopenfont/gfopenfont.h"
#include "of_layout.h"
#include "of_unicode.h"

#include <stdlib.h>
#include <string.h>

uint32_t of_script_tag(unsigned script)
{
    /* the scripts whose OpenType tag is not their ISO 15924 code in lower
     * case; Devanagari and Bengali take the newer shaping's tags */
    static const struct {
        uint16_t script;
        uint32_t tag;
    } tags[] = {
        { OF_SCRIPT_HIRAGANA, OF_TAG('k', 'a', 'n', 'a') },
        { OF_SCRIPT_KATAKANA, OF_TAG('k', 'a', 'n', 'a') },
        { OF_SCRIPT_LAO, OF_TAG('l', 'a', 'o', ' ') },
        { OF_SCRIPT_YI, OF_TAG('y', 'i', ' ', ' ') },
        { OF_SCRIPT_NKO, OF_TAG('n', 'k', 'o', ' ') },
        { OF_SCRIPT_VAI, OF_TAG('v', 'a', 'i', ' ') },
        { OF_SCRIPT_DEVANAGARI, OF_TAG('d', 'e', 'v', '2') },
        { OF_SCRIPT_BENGALI, OF_TAG('b', 'n', 'g', '2') },
    };
    size_t i;
    uint32_t iso;
    if (script == OF_SCRIPT_UNKNOWN || script == OF_SCRIPT_COMMON || script == OF_SCRIPT_INHERITED ||
        script >= OF_SCRIPT_COUNT)
        return OF_TAG('D', 'F', 'L', 'T');
    for (i = 0; i < sizeof tags / sizeof tags[0]; ++i)
        if (tags[i].script == script)
            return tags[i].tag;
    iso = of_uni_script_iso(script);
    return iso | 0x20000000u;   /* the first letter in lower case */
}

unsigned of_script_from_iso(uint32_t iso)
{
    return of_uni_script_from_iso(iso);
}

int of_script_rtl(unsigned script)
{
    switch (script) {
    case OF_SCRIPT_ARABIC: case OF_SCRIPT_HEBREW: case OF_SCRIPT_SYRIAC: case OF_SCRIPT_THAANA:
    case OF_SCRIPT_NKO: case OF_SCRIPT_SAMARITAN: case OF_SCRIPT_MANDAIC: case OF_SCRIPT_ADLAM:
    case OF_SCRIPT_HANIFI_ROHINGYA: case OF_SCRIPT_YEZIDI: case OF_SCRIPT_MENDE_KIKAKUI:
    case OF_SCRIPT_IMPERIAL_ARAMAIC: case OF_SCRIPT_PHOENICIAN: case OF_SCRIPT_AVESTAN:
    case OF_SCRIPT_KHAROSHTHI: case OF_SCRIPT_LYDIAN: case OF_SCRIPT_MANICHAEAN:
    case OF_SCRIPT_OLD_NORTH_ARABIAN: case OF_SCRIPT_NABATAEAN: case OF_SCRIPT_PALMYRENE:
    case OF_SCRIPT_INSCRIPTIONAL_PAHLAVI: case OF_SCRIPT_PSALTER_PAHLAVI:
    case OF_SCRIPT_INSCRIPTIONAL_PARTHIAN: case OF_SCRIPT_OLD_SOUTH_ARABIAN:
    case OF_SCRIPT_SOGDIAN: case OF_SCRIPT_OLD_SOGDIAN: case OF_SCRIPT_ELYMAIC:
    case OF_SCRIPT_CHORASMIAN: case OF_SCRIPT_OLD_UYGHUR: case OF_SCRIPT_CYPRIOT:
    case OF_SCRIPT_OLD_TURKIC: case OF_SCRIPT_HATRAN: case OF_SCRIPT_OLD_HUNGARIAN:
    case OF_SCRIPT_MEROITIC_CURSIVE: case OF_SCRIPT_MEROITIC_HIEROGLYPHS:
    case OF_SCRIPT_GARAY:
        return 1;
    default:
        return 0;
    }
}

/* ---- 1. UTF-8 ---- */

/* the character at p (n bytes left) and its length; a byte that starts no
 * valid sequence (short, overlong, a surrogate, past U+10FFFF) is U+FFFD,
 * one byte long */
static uint32_t utf8_next(const uint8_t *p, size_t n, size_t *used)
{
    uint32_t c = p[0], min, k, len;
    *used = 1;
    if (c < 0x80)
        return c;
    if (c >= 0xC2 && c <= 0xDF) {
        len = 2;
        c &= 0x1F;
        min = 0x80;
    } else if (c >= 0xE0 && c <= 0xEF) {
        len = 3;
        c &= 0x0F;
        min = 0x800;
    } else if (c >= 0xF0 && c <= 0xF4) {
        len = 4;
        c &= 0x07;
        min = 0x10000;
    } else {
        return 0xFFFD;
    }
    if (n < len)
        return 0xFFFD;
    for (k = 1; k < len; ++k) {
        if ((p[k] & 0xC0) != 0x80)
            return 0xFFFD;
        c = (c << 6) | (p[k] & 0x3F);
    }
    if (c < min || c > 0x10FFFF || (c >= 0xD800 && c <= 0xDFFF))
        return 0xFFFD;
    *used = len;
    return c;
}

/* ---- 2. clusters ---- */

/* a character that belongs with the one before it: a mark, a joiner, an
 * emoji modifier, a variation selector or a tag */
static int continues(uint32_t cp)
{
    if (of_uni_is_mark(cp))
        return 1;
    return cp == 0x200D || (cp >= 0x1F3FB && cp <= 0x1F3FF) ||
           (cp >= 0xFE00 && cp <= 0xFE0F) || (cp >= 0xE0020 && cp <= 0xE007F) ||
           (cp >= 0xE0100 && cp <= 0xE01EF);
}

/* ---- 3. composed and decomposed characters ---- */

static int has(const OFFont *f, uint32_t cp)
{
    return of_font_glyph(f, cp) != 0;
}

/* cp's canonical decomposition into characters the font has, appended to
 * out (room for 32); how many, or 0 when there is none */
static int decompose(const OFFont *f, uint32_t cp, uint32_t *out, int depth)
{
    uint32_t a, b;
    int n, k = of_uni_decompose(cp, &a, &b);
    if (!k || depth > 8)
        return 0;
    if (k == 2 && !has(f, b)) {
        /* b may itself decompose (rarely): then not this way */
        return 0;
    }
    if (has(f, a)) {
        out[0] = a;
        n = 1;
    } else if ((n = decompose(f, a, out, depth + 1)) == 0) {
        return 0;
    }
    if (k == 2)
        out[n++] = b;
    return n;
}

static int insert_at(OFShBuf *b, uint32_t at, uint32_t count)
{
    if (!of_shbuf_reserve(b, count))
        return 0;
    memmove(&b->g[at + count], &b->g[at], (b->n - at) * sizeof(OFShGlyph));
    b->n += count;
    return 1;
}

/* The combining class marks are put in order by: the Unicode one, but for
 * the Hebrew points and the Arabic harakat, which other shapers (and fonts
 * made for them) order differently from canonical order: in Hebrew the
 * shin and sin dots, then dagesh and rafe, then the vowels; in Arabic
 * shadda before the other harakat. */
static unsigned mark_class(uint32_t cp)
{
    static const uint8_t hebrew[27] = {           /* classes 10 to 26 */
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        22, 15, 16, 17, 23, 18, 19, 20, 21, 14, 24, 12, 25, 13, 10, 11, 26
    };
    static const uint8_t arabic[9] = { 28, 29, 30, 31, 32, 33, 27, 34, 35 };   /* classes 27 to 35 */
    unsigned cc = of_uni_combining_class(cp);
    if (cc >= 10 && cc <= 26)
        return hebrew[cc];
    if (cc >= 27 && cc <= 35)
        return arabic[cc - 27];
    return cc;
}

static void normalise(const OFFont *f, OFShBuf *b)
{
    uint32_t i, j;
    /* decompose what the font lacks */
    for (i = 0; i < b->n; ++i) {
        uint32_t parts[32];
        int n;
        if (has(f, b->g[i].cp) || (n = decompose(f, b->g[i].cp, parts, 0)) == 0)
            continue;
        if (n > 1 && !insert_at(b, i + 1, (uint32_t)n - 1))
            return;
        for (j = 0; j < (uint32_t)n; ++j) {
            b->g[i + j] = b->g[i];
            b->g[i + j].cp = parts[j];
            b->g[i + j].props &= (uint16_t)~OF_SH_CONTINUATION;
        }
        i += (uint32_t)n - 1;
    }
    /* marks in order: each run of non-starters sorted by class
     * (mark_class), keeping the order of equal ones */
    for (i = 0; i < b->n; ++i) {
        uint32_t end, k;
        if (!of_uni_combining_class(b->g[i].cp))
            continue;
        for (end = i; end < b->n && of_uni_combining_class(b->g[end].cp); ++end)
            ;
        for (k = i + 1; k < end; ++k) {
            OFShGlyph x = b->g[k];
            unsigned cc = mark_class(x.cp);
            uint32_t m = k;
            while (m > i && mark_class(b->g[m - 1].cp) > cc) {
                b->g[m] = b->g[m - 1];
                m--;
            }
            b->g[m] = x;
        }
        i = end - 1;
    }
    /* compose a base with the marks after it, where the font has the
     * composed character and nothing between blocks it */
    for (i = 0; i < b->n; ++i) {
        unsigned last_cc = 0;
        if (of_uni_combining_class(b->g[i].cp))
            continue;
        for (j = i + 1; j < b->n;) {
            unsigned cc = of_uni_combining_class(b->g[j].cp);
            uint32_t c;
            if (!of_uni_is_mark(b->g[j].cp))
                break;
            if ((last_cc < cc || (last_cc == 0 && j == i + 1)) &&
                (c = of_uni_compose(b->g[i].cp, b->g[j].cp)) != 0 && has(f, c)) {
                b->g[i].cp = c;
                memmove(&b->g[j], &b->g[j + 1], (b->n - j - 1) * sizeof(OFShGlyph));
                b->n--;
                continue;
            }
            if (!cc)
                break;
            last_cc = cc;
            j++;
        }
    }
}

/* ---- 4. spaces the font lacks ---- */

enum { SP_NONE, SP_SPACE, SP_EM, SP_EM_2, SP_EM_3, SP_EM_4, SP_EM_5, SP_EM_6, SP_EM_16, SP_4_EM_18, SP_FIGURE, SP_PUNCTUATION, SP_NARROW };

static int space_kind(uint32_t cp)
{
    switch (cp) {
    case 0x0020: case 0x00A0: return SP_SPACE;
    case 0x2000: case 0x2002: return SP_EM_2;
    case 0x2001: case 0x2003: case 0x3000: return SP_EM;
    case 0x2004: return SP_EM_3;
    case 0x2005: return SP_EM_4;
    case 0x2006: return SP_EM_6;
    case 0x2007: return SP_FIGURE;
    case 0x2008: return SP_PUNCTUATION;
    case 0x2009: return SP_EM_5;
    case 0x200A: return SP_EM_16;
    case 0x202F: return SP_NARROW;
    case 0x205F: return SP_4_EM_18;
    default: return of_uni_category(cp) == OF_GC_ZS ? SP_SPACE : SP_NONE;
    }
}

static int32_t space_width(const OFFont *f, uint32_t ppem, int kind, uint32_t space_gid)
{
    int32_t em = (int32_t)ppem * 64;
    uint32_t g;
    switch (kind) {
    case SP_EM: return em;
    /* the fractions of an em to the nearest 64th, but the medium
     * mathematical space's 4/18 and the narrow space's half rounded down,
     * as other shapers draw them */
    case SP_EM_2: return (em + 1) / 2;
    case SP_EM_3: return (em + 1) / 3;
    case SP_EM_4: return (em + 2) / 4;
    case SP_EM_5: return (em + 2) / 5;
    case SP_EM_6: return (em + 3) / 6;
    case SP_EM_16: return (em + 8) / 16;
    case SP_4_EM_18: return em * 4 / 18;
    case SP_FIGURE:
        g = of_font_glyph(f, '0');
        return g ? of_font_advance_px(f, g, ppem) : em / 2;
    case SP_PUNCTUATION:
        g = of_font_glyph(f, '.');
        return g ? of_font_advance_px(f, g, ppem) : em / 5;
    case SP_NARROW:
        return of_font_advance_px(f, space_gid, ppem) / 2;
    default:
        return of_font_advance_px(f, space_gid, ppem);
    }
}

/* ---- 5. fractions ---- */

static void fractions(OFShBuf *b)
{
    uint32_t i;
    for (i = 0; i < b->n; ++i) {
        uint32_t start, end, k;
        if (b->g[i].cp != 0x2044)
            continue;
        for (start = i; start > 0 && of_uni_category(b->g[start - 1].cp) == OF_GC_ND; --start)
            ;
        for (end = i + 1; end < b->n && of_uni_category(b->g[end].cp) == OF_GC_ND; ++end)
            ;
        if (start == i || end == i + 1)
            continue;
        for (k = start; k < end; ++k)
            b->g[k].mask |= OF_MASK_FRAC | (k < i ? OF_MASK_NUMR : (k > i ? OF_MASK_DNOM : 0));
        of_layout_unsafe(b, (long)start, (long)end - 1);   /* one fraction */
        i = end - 1;
    }
}

/* ---- joining (Arabic, Syriac, N'Ko, Mongolian and the like) ---- */

static int joins_next(unsigned t)
{
    return t == OF_JT_L || t == OF_JT_D || t == OF_JT_C;
}

static int joins_prev(unsigned t)
{
    return t == OF_JT_R || t == OF_JT_D || t == OF_JT_C;
}

/* Each joining character's form, as a mask for isol, fina, medi or init:
 * it joins the character before it (logical order, transparent ones
 * skipped) when that one joins forward and it joins back. 1 when any
 * character of the run joins at all. */
static int joining_forms(OFShBuf *b)
{
    uint32_t i;
    long prev = -1;
    int any = 0;
    unsigned char *link = NULL;
    for (i = 0; i < b->n; ++i) {
        unsigned t = of_uni_joining_type(b->g[i].cp);
        if (t == OF_JT_R || t == OF_JT_L || t == OF_JT_D)
            any = 1;
    }
    if (!any)
        return 0;
    link = (unsigned char *)calloc(b->n ? b->n : 1, 1);  /* bit 1: to the one before; bit 2: to the one after */
    if (!link) {
        b->failed = 1;
        return 1;
    }
    for (i = 0; i < b->n; ++i) {
        unsigned t = of_uni_joining_type(b->g[i].cp);
        if (t == OF_JT_T)
            continue;
        if (prev >= 0 && joins_next(of_uni_joining_type(b->g[prev].cp)) && joins_prev(t)) {
            link[prev] |= 2;
            link[i] |= 1;
            of_layout_unsafe(b, prev, (long)i);
        }
        prev = (long)i;
    }
    for (i = 0; i < b->n; ++i) {
        unsigned t = of_uni_joining_type(b->g[i].cp);
        uint32_t form = 0;
        if (t == OF_JT_D)
            form = link[i] == 3 ? OF_MASK_MEDI : link[i] == 1 ? OF_MASK_FINA : link[i] == 2 ? OF_MASK_INIT : OF_MASK_ISOL;
        else if (t == OF_JT_R)
            form = (link[i] & 1) ? OF_MASK_FINA : OF_MASK_ISOL;
        else if (t == OF_JT_L)
            form = (link[i] & 2) ? OF_MASK_INIT : OF_MASK_ISOL;
        b->g[i].mask |= form;
    }
    free(link);
    return 1;
}

/* ---- the program's features ---- */

/* Gives each feature over part of the text its mask, and sets it on the
 * glyphs where the feature is on: inside its range by its value, outside
 * it as the shaper would have it by itself. */
static void feature_masks(OFShBuf *b, OFLayoutSpec *spec)
{
    uint32_t k, i, next = 0;
    for (k = 0; k < spec->nfeatures; ++k) {
        const OFFeature *ft = &spec->features[k];
        uint32_t m;
        int dflt;
        spec->ranged_mask[k] = 0;
        if (ft->start == 0 && ft->end == OF_FEATURE_END)
            continue;
        if (next >= OF_MAX_RANGED_FEATURES)
            continue;
        m = OF_MASK_RANGED << next++;
        spec->ranged_mask[k] = m;
        dflt = of_layout_default_feature(spec, ft->tag);
        for (i = 0; i < b->n; ++i) {
            uint32_t c = b->g[i].cluster;
            int on = (c >= ft->start && c < ft->end) ? ft->value != 0 : dflt;
            if (on)
                b->g[i].mask |= m;
        }
    }
}

/* ---- the whole ---- */

static int first_strong_script(const OFShBuf *b)
{
    uint32_t i;
    for (i = 0; i < b->n; ++i) {
        unsigned s = of_uni_script(b->g[i].cp);
        if (s != OF_SCRIPT_COMMON && s != OF_SCRIPT_INHERITED && s != OF_SCRIPT_UNKNOWN)
            return (int)s;
    }
    return OF_SCRIPT_COMMON;
}

static void reverse(OFShBuf *b)
{
    uint32_t i, j;
    if (!b->n)
        return;
    for (i = 0, j = b->n - 1; i < j; ++i, --j) {
        OFShGlyph x = b->g[i];
        b->g[i] = b->g[j];
        b->g[j] = x;
    }
}

int of_shape_text(const OFFont *f, uint32_t ppem, const char *text, size_t bytes,
                  size_t off, size_t len, OFRunProps *props, OFShBuf *b)
{
    const uint8_t *p = (const uint8_t *)text;
    size_t at = off, end = off + len;
    uint32_t i, space;
    OFLayoutSpec spec;
    int rc;

    b->n = 0;
    b->failed = 0;
    if (end > bytes)
        end = bytes;
    /* 1, 2: characters and clusters */
    while (at < end) {
        size_t used;
        uint32_t cp = utf8_next(p + at, end - at, &used);
        OFShGlyph *x;
        if (!of_shbuf_reserve(b, 1))
            return OF_ERR_MEMORY;
        x = &b->g[b->n];
        memset(x, 0, sizeof(*x));
        x->cp = cp;
        x->cluster = (uint32_t)at;
        x->mask = OF_MASK_GLOBAL;
        x->attach_to = -1;
        if (b->n && continues(cp)) {
            x->cluster = b->g[b->n - 1].cluster;
            x->props |= OF_SH_CONTINUATION;
        }
        if (of_uni_default_ignorable(cp))
            x->props |= OF_SH_IGNORABLE;
        if (cp == 0x200D)
            x->props |= OF_SH_ZWJ;
        if (cp == 0x200C)
            x->props |= OF_SH_ZWNJ;
        b->n++;
        at += used;
    }
    /* the run's script: its first letter's that belongs to one */
    if (props->script == OF_SCRIPT_UNKNOWN)
        props->script = (unsigned)first_strong_script(b);
    if (props->direction == OF_DIR_AUTO)
        props->direction = of_script_rtl(props->script) ? OF_DIR_RTL : OF_DIR_LTR;
    memset(&spec, 0, sizeof spec);
    spec.script_tag = props->script_tag ? props->script_tag : of_script_tag(props->script);
    spec.lang_tag = props->lang_tag;
    spec.rtl = props->direction == OF_DIR_RTL;
    spec.features = props->features;
    spec.nfeatures = props->nfeatures < OF_MAX_FEATURES ? props->nfeatures : OF_MAX_FEATURES;

    /* 3 */
    normalise(f, b);
    if (b->failed)
        return OF_ERR_MEMORY;

    /* 4: glyphs; right to left, a mirrored character's mirror when the
     * font has it ('rtlm' may still change the rest) */
    space = of_font_glyph(f, ' ');
    for (i = 0; i < b->n; ++i) {
        OFShGlyph *x = &b->g[i];
        uint32_t m;
        x->gid = 0;
        if (spec.rtl && (m = of_uni_mirror(x->cp)) != 0)
            x->gid = of_font_glyph(f, m);
        if (!x->gid)
            x->gid = of_font_glyph(f, x->cp);
        if (!x->gid && space) {
            int kind = space_kind(x->cp);
            if (kind != SP_NONE) {
                x->gid = space;
                x->props |= OF_SH_SPACE_FALLBACK;
                x->space_kind = (uint8_t)kind;
            }
        }
        if (!x->gid && x->cp == 0x2011)
            x->gid = of_font_glyph(f, 0x2010);  /* a non-breaking hyphen looks like a hyphen */
    }
    /* joining forms, fractions when the font has them, the program's
     * features over parts of the text */
    spec.joining = joining_forms(b);
    if (b->failed)
        return OF_ERR_MEMORY;
    if (of_layout_has_gsub_feature(f, &spec, OF_TAG('f', 'r', 'a', 'c')) ||
        of_layout_has_gsub_feature(f, &spec, OF_TAG('n', 'u', 'm', 'r')) ||
        of_layout_has_gsub_feature(f, &spec, OF_TAG('d', 'n', 'o', 'm')))
        fractions(b);
    feature_masks(b, &spec);

    /* 5 */
    of_layout_classes(f, b);
    if ((rc = of_layout_substitute(f, b, &spec)) != OF_OK)
        return rc;

    /* 6 */
    for (i = 0; i < b->n; ++i) {
        OFShGlyph *x = &b->g[i];
        x->xa = (x->props & OF_SH_SPACE_FALLBACK) ? space_width(f, ppem, x->space_kind, space)
                                                  : of_font_advance_px(f, x->gid, ppem);
        x->ya = x->xo = x->yo = 0;
    }
    if ((rc = of_layout_position(f, b, &spec, ppem)) != OF_OK)
        return rc;

    /* 7 */
    for (i = 0; i < b->n; ++i) {
        OFShGlyph *x = &b->g[i];
        if (!(x->props & OF_SH_IGNORABLE))
            continue;
        /* with no GPOS, a hidden character of its own is marked unsafe to
         * break before, as other shapers mark it */
        if (!f->gpos.len && i > 0)
            of_layout_unsafe(b, (long)i - 1, (long)i);
        if (space) {
            x->gid = space;
            x->xa = x->ya = 0;
        } else {
            memmove(x, x + 1, (b->n - i - 1) * sizeof(OFShGlyph));
            b->n--;
            i--;
        }
    }
    /* 8: right to left, into visual order */
    if (spec.rtl)
        reverse(b);
    return OF_OK;
}
