/* GFOpenBullet's core: bullet.library's glyph engine in lean C++.
 * Copyright (c) 2026 AmigaChrome Team. MIT.
 *
 * Clean-room: written from the NDK 3.2 headers' layouts and comments
 * (diskfont/glyph.h, diskfont/diskfonttag.h, diskfont/oterrors.h) and the
 * autodocs; never from Kickstart, Agfa, Hyperion or AROS code. Where those
 * leave a corner open the choice made here says "ABI test":
 * tests/BulletABITest prints OS 3.2's answer next to ours.
 *
 * OS 3.2's bullet.library draws Agfa Intellifont outlines, whose file
 * format has no public specification. This engine is bullet.library's API
 * over GFOpenFont (gfopenfont.library): GFOpenFont reads the outline file
 * (TrueType and OpenType, CFF too) and draws each glyph's coverage through
 * the matrix the tags make; the core keeps the tags, the errors, the
 * emboldening and the GlyphMap. An outline file GFOpenFont can't read, or no
 * GFOpenFont, is OTERR_BadFace, so diskfont.library falls back to the font's
 * bitmap sizes (README.md, "Font formats").
 *
 * Built with openamigagcc's lean C++ library profile:
 *   -std=gnu++23 -fno-exceptions -fno-rtti -fno-threadsafe-statics
 *   -fno-asynchronous-unwind-tables -fno-unwind-tables -nostdlib++
 * No static constructors, no function-local statics, no standard header
 * that allocates, nothing from libstdc++: memory and the font come from
 * the env, the only calls out are the env's and libgcc's arithmetic. The
 * entry points are extern "C" (bullet_core.h). Integer arithmetic only.
 *
 * The rules (README.md says the same in prose):
 *
 *  Size. ppem_y = PointHeight * ydpi / 72 and ppem_x = PointHeight *
 *  SetFactor * xdpi / 72, as 16.16, each division rounded half up. Below
 *  one pixel: OTERR_TooSmall; above OBL_MAXPPEM: OTERR_BadData.
 *
 *  The matrix. Font units (y up) to 26.6 device pixels: scale by ppem_x
 *  across and ppem_y up, then shear X += Y * ShearSin / ShearCos, then
 *  rotate X' = X cos - Y sin, Y' = X sin + Y cos. As one 2 x 2 matrix of
 *  16.16 pixels to the em, each entry rounded half up, handed to GFOpenFont.
 *  No hinting.
 *
 *  Pixels. GFOpenFont's coverage (exact area, 0 to 255); a pixel is set at
 *  128 and over.
 *
 *  Emboldening. EmboldenX and Y are fractions of the em; in pixels ex =
 *  rnd(EmboldenX * ppem_x / 2^32), ey likewise. ex > 0 sets the ex pixels
 *  right of each set pixel, ex < 0 keeps a pixel only when the |ex| right
 *  of it are set too; ey the same upwards. The advance grows by ex.
 *
 *  Advance. a = rnd(adv * ppem_x / (upm * 1024)) (adv the font's advance,
 *  or OT_GlyphWidth's a = rnd(GlyphWidth * ppem_x / 2^26) when it isn't
 *  0), rotated as a point (a, 0), plus ex * 64 on x; X1 and Y1 are it in
 *  whole pixels, rnd(v / 64). glm_Width is GlyphWidth, or rnd(adv * 65536
 *  / upm).
 *
 *  GlyphMap. The bitmap spans the black box, the origin and the advance's
 *  end; glm_BMModulo is its width rounded up to 32 bits, in bytes. Its
 *  rows run top down; glm_Y0 is the row under the baseline, glm_X0 the
 *  origin's column; glm_XOrigin and _YOrigin are those as FIXED. A glyph
 *  with no pixels has a black box of 0 at 0, 0.
 */
#include "bullet_core.h"
#include <cstddef>
#include <type_traits>

using u8 = obl_u8;
using s8 = obl_s8;
using u16 = obl_u16;
using s16 = obl_s16;
using u32 = obl_u32;
using s32 = obl_s32;
using s64 = long long;
using TagItem = obl_TagItem;
using GlyphMap = obl_GlyphMap;

/* ---- the NDK's layouts (checked where pointers are 32 bits: the 68k) ---- */
#define LAYOUT(T, n) static_assert(sizeof(T) == (n), #T); static_assert(std::is_standard_layout_v<T>, #T)
#define AT(T, f, o) static_assert(offsetof(T, f) == (o), #T "." #f)
#if __SIZEOF_POINTER__ == 4
LAYOUT(obl_TagItem, 8);
LAYOUT(obl_MinNode, 8);
LAYOUT(obl_MinList, 12);
AT(obl_MinList, mlh_Tail, 4); AT(obl_MinList, mlh_TailPred, 8);
LAYOUT(obl_GlyphEngine, 8);
AT(obl_GlyphEngine, gle_Name, 4);
LAYOUT(obl_GlyphMap, 36);
AT(obl_GlyphMap, glm_BMRows, 2); AT(obl_GlyphMap, glm_BlackLeft, 4); AT(obl_GlyphMap, glm_BlackTop, 6);
AT(obl_GlyphMap, glm_BlackWidth, 8); AT(obl_GlyphMap, glm_BlackHeight, 10); AT(obl_GlyphMap, glm_XOrigin, 12);
AT(obl_GlyphMap, glm_YOrigin, 16); AT(obl_GlyphMap, glm_X0, 20); AT(obl_GlyphMap, glm_Y0, 22);
AT(obl_GlyphMap, glm_X1, 24); AT(obl_GlyphMap, glm_Y1, 26); AT(obl_GlyphMap, glm_Width, 28);
AT(obl_GlyphMap, glm_BitMap, 32);
LAYOUT(obl_GlyphWidthEntry, 14);
AT(obl_GlyphWidthEntry, gwe_Code, 8); AT(obl_GlyphWidthEntry, gwe_Width, 10);
#endif

namespace {

constexpr u32 ERR(long e) { return u32(e); }

/* ---- arithmetic ----------------------------------------------------------- */

/* floor(a / b), any signs */
s64 fdiv(s64 a, s64 b)
{
    s64 q = a / b;
    if ((a % b) != 0 && ((a < 0) != (b < 0))) q--;
    return q;
}
/* floor(a / b + 1/2) */
s64 rnd(s64 a, s64 b) { return fdiv(2 * a + b, 2 * b); }

/* ---- the engine ------------------------------------------------------------ */

struct Engine {
    obl_GlyphEngine ge;            /* first: the caller sees this */
    const obl_env *env;
    void *user;
    void *face;                    /* GFOpenFont's face; NULL: no face */
    u32 upm;
    bool shear_part, rot_part;     /* the sine given, the cosine not yet */
    bool have_code, have_code2;
    char path[OBL_MAXPATH];
    u32 dpi, dotsize, point_height, set_factor, glyph_width;
    s32 shear_sin, shear_cos, rot_sin, rot_cos, embolden_x, embolden_y;
    u32 code, code2;
    u32 underlined, strike;
};

void *alloc(Engine *e, u32 n) { return e->env->alloc(e->user, n); }
void release(Engine *e, void *m) { if (m) e->env->free(e->user, m); }

/* utility's NextTagItem: TAG_DONE ends, TAG_IGNORE, TAG_MORE and TAG_SKIP
 * are followed; any other tag is handed out */
const TagItem *next_tag(const TagItem **tp)
{
    for (;;) {
        const TagItem *t = *tp;
        if (!t) return nullptr;
        switch (t->ti_Tag) {
        case OBL_TAG_DONE: return nullptr;
        case OBL_TAG_IGNORE: *tp = t + 1; break;
        case OBL_TAG_MORE: *tp = reinterpret_cast<const TagItem *>(t->ti_Data); break;
        case OBL_TAG_SKIP: *tp = t + 1 + t->ti_Data; break;
        default: *tp = t + 1; return t;
        }
    }
}

/* ---- the font ---------------------------------------------------------- */

/* The glyph a code maps to (GFOpenFont finds a Microsoft Symbol font's
 * codes at 0xF000 up itself) */
u32 glyph_of(Engine *e, u32 code) { return e->env->char_index(e->user, e->face, code); }

/* a glyph's advance in font units (0 when the font gives none) */
u32 advance_of(Engine *e, u32 g)
{
    s32 a = e->env->advance(e->user, e->face, g);
    return a > 0 ? u32(a) : 0;
}

/* ---- OT_GlyphMap ------------------------------------------------------------ */

bool get_px(const u8 *grid, s32 w, s32 c, s32 r) { return grid[r * w + c] != 0; }

u32 glyph_map(Engine *e, GlyphMap **out)
{
    if (!e->face) return ERR(OBL_OTERR_NoFace);
    if (e->shear_part) return ERR(OBL_OTERR_NoShear);
    if (e->rot_part) return ERR(OBL_OTERR_NoRotate);
    if (!e->have_code) return ERR(OBL_OTERR_NoGlyph);
    const s64 xdpi = e->dpi >> 16, ydpi = e->dpi & 0xffff;
    const s64 ppy = rnd(s64(e->point_height) * ydpi, 72);
    const s64 ppx = rnd(rnd(s64(e->point_height) * e->set_factor, 65536) * xdpi, 72);
    if (ppx < 65536 || ppy < 65536) return ERR(OBL_OTERR_TooSmall);
    if (ppx > s64(OBL_MAXPPEM) * 65536 || ppy > s64(OBL_MAXPPEM) * 65536) return ERR(OBL_OTERR_BadData);
    const u32 g = glyph_of(e, e->code);
    if (!g) return ERR(OBL_OTERR_UnknownGlyph);

    /* the matrix: scale, shear, rotate (16.16 pixels to the em) */
    const s64 rc = e->rot_cos, rs = e->rot_sin;
    const s64 sh = rnd(ppy * e->shear_sin, e->shear_cos);        /* x per y, after scaling */
    const s64 lim = s64(OBL_MAXPPEM) << 16;
    const s64 m64[4] = {rnd(rc * ppx, 65536), rnd(rc * sh - rs * ppy, 65536),
                        rnd(rs * ppx, 65536), rnd(rs * sh + rc * ppy, 65536)};
    s32 m[4];
    for (int i = 0; i < 4; i++) {
        if (m64[i] > lim || m64[i] < -lim) return ERR(OBL_OTERR_BadGlyph);   /* sheared past what a font draws */
        m[i] = s32(m64[i]);
    }
    obl_a8 a8{};
    const s32 rr = e->env->raster(e->user, e->face, g, m, &a8);
    if (rr == OBL_OF_MEMORY) return ERR(OBL_OTERR_NoMemory);
    if (rr) return ERR(OBL_OTERR_BadGlyph);

    /* emboldening, in pixels */
    const s32 ex = s32(rnd(s64(e->embolden_x) * ppx, s64(1) << 32));
    const s32 ey = s32(rnd(s64(e->embolden_y) * ppy, s64(1) << 32));

    /* the pixels: a byte each over the coverage's box, rows y up, room for
     * emboldening */
    const s32 clo = a8.pixels ? a8.left : 0, rlo = a8.pixels ? a8.top - s32(a8.height) : 0;
    const s32 cw = a8.pixels ? s32(a8.width) : 0, ch = a8.pixels ? s32(a8.height) : 0;
    const s32 gw = cw + (ex > 0 ? ex : 0), gh = ch + (ey > 0 ? ey : 0);
    u8 *grid = nullptr;
    if (cw > 0 && ch > 0) {
        if (s64(gw) * gh > 4L * 1024 * 1024) {
            e->env->raster_free(e->user, e->face, &a8);
            return ERR(OBL_OTERR_NoMemory);
        }
        grid = static_cast<u8 *>(alloc(e, u32(gw * gh)));
        if (!grid) {
            e->env->raster_free(e->user, e->face, &a8);
            return ERR(OBL_OTERR_NoMemory);
        }
        for (s32 r = 0; r < ch; r++) {
            const u8 *src = a8.pixels + u32(ch - 1 - r) * a8.stride;
            u8 *row = grid + r * gw;
            for (s32 c = 0; c < cw; c++) row[c] = src[c] >= 128;
        }
        /* emboldening: x, then y */
        for (s32 r = 0; ex && r < gh; r++) {
            u8 *row = grid + r * gw;
            if (ex > 0) {
                for (s32 c = gw - 1; c >= 0; c--)
                    for (s32 k = 1; k <= ex && !row[c] && c - k >= 0; k++)
                        if (row[c - k]) row[c] = 2;            /* 2: set here, not read as set yet */
                for (s32 c = 0; c < gw; c++) row[c] = row[c] != 0;
            } else {
                for (s32 c = 0; c < gw; c++) {
                    bool all = row[c] != 0;
                    for (s32 k = 1; all && k <= -ex; k++) all = c + k < gw && row[c + k];
                    row[c] = all;
                }
            }
        }
        if (ey) {
            for (s32 c = 0; c < gw; c++) {
                if (ey > 0) {
                    for (s32 r = gh - 1; r >= 0; r--)
                        for (s32 k = 1; k <= ey && !grid[r * gw + c] && r - k >= 0; k++)
                            if (grid[(r - k) * gw + c] == 1) grid[r * gw + c] = 2;
                    for (s32 r = 0; r < gh; r++) grid[r * gw + c] = grid[r * gw + c] != 0;
                } else {
                    for (s32 r = 0; r < gh; r++) {
                        bool all = grid[r * gw + c] != 0;
                        for (s32 k = 1; all && k <= -ey; k++) all = r + k < gh && grid[(r + k) * gw + c];
                        grid[r * gw + c] = all;
                    }
                }
            }
        }
    }
    e->env->raster_free(e->user, e->face, &a8);
    /* the black box, in pixels around the origin */
    s32 bl = 0, br = -1, bb = 0, bt = -1;
    bool any = false;
    for (s32 r = 0; r < gh; r++)
        for (s32 c = 0; c < gw; c++)
            if (get_px(grid, gw, c, r)) {
                s32 pc = c + clo, pr = r + rlo;
                if (!any) { bl = br = pc; bb = bt = pr; any = true; }
                if (pc < bl) bl = pc;
                if (pc > br) br = pc;
                if (pr < bb) bb = pr;
                if (pr > bt) bt = pr;
            }

    /* the advance */
    const u32 adv = advance_of(e, g);
    s64 ax = e->glyph_width ? rnd(s64(e->glyph_width) * ppx, s64(1) << 26) : rnd(s64(adv) * ppx, s64(e->upm) * 1024);
    s64 rx = rnd(ax * rc, 65536) + s64(ex) * 64, ry = rnd(ax * rs, 65536);
    const s32 x1 = s32(rnd(rx, 64)), y1 = s32(rnd(ry, 64));

    /* the bitmap: the black box, the origin and the advance's end */
    s32 L = 0, R = 0, B = 0, T = 0;                   /* columns [L, R), rows [B, T), y up */
    if (any) { L = bl; R = br + 1; B = bb; T = bt + 1; }
    if (L > 0) L = 0;
    if (R < 0) R = 0;
    if (B > 0) B = 0;
    if (T < 0) T = 0;
    if (x1 < L) L = x1;
    if (x1 > R) R = x1;
    if (y1 < B) B = y1;
    if (y1 > T) T = y1;
    const s32 W = R - L, H = T - B;
    if (W > 0x7fff || H > 0x7fff) {
        release(e, grid);
        return ERR(OBL_OTERR_BadData);
    }
    const u32 mod = ((u32(W) + 31) >> 5) * 4;
    GlyphMap *gm = static_cast<GlyphMap *>(alloc(e, sizeof(GlyphMap) + mod * u32(H)));
    if (!gm) {
        release(e, grid);
        return ERR(OBL_OTERR_NoMemory);
    }
    u8 *bm = reinterpret_cast<u8 *>(gm + 1);
    gm->glm_BMModulo = u16(mod);
    gm->glm_BMRows = u16(H);
    if (any) {
        gm->glm_BlackLeft = u16(bl - L);
        gm->glm_BlackTop = u16(T - 1 - bt);
        gm->glm_BlackWidth = u16(br - bl + 1);
        gm->glm_BlackHeight = u16(bt - bb + 1);
        for (s32 r = bb; r <= bt; r++)
            for (s32 c = bl; c <= br; c++)
                if (get_px(grid, gw, c - clo, r - rlo)) {
                    u32 x = u32(c - L), y = u32(T - 1 - r);
                    bm[y * mod + (x >> 3)] |= u8(0x80 >> (x & 7));
                }
    }
    gm->glm_X0 = s16(-L);
    gm->glm_Y0 = s16(T);
    gm->glm_X1 = s16(x1 - L);
    gm->glm_Y1 = s16(T - y1);
    gm->glm_XOrigin = s32(u32(-L) << 16);
    gm->glm_YOrigin = s32(u32(T) << 16);
    gm->glm_Width = e->glyph_width ? s32(e->glyph_width) : s32(rnd(s64(adv) * 65536, e->upm));
    gm->glm_BitMap = bm;
    release(e, grid);
    *out = gm;
    return ERR(OBL_OTERR_Success);
}

/* ---- OT_WidthList and the kern pairs -------------------------------------- */

u32 width_list(Engine *e, obl_MinList **out)
{
    if (!e->face) return ERR(OBL_OTERR_NoFace);
    if (!e->have_code || !e->have_code2) return ERR(OBL_OTERR_NoGlyph);
    if (e->code2 < e->code || e->code2 > 0xffff) return ERR(OBL_OTERR_BadGlyph);   /* ABI test */
    u32 n = 0;
    for (u32 c = e->code; c <= e->code2; c++)
        if (glyph_of(e, c)) n++;
    auto *m = static_cast<u8 *>(alloc(e, sizeof(obl_MinList) + n * sizeof(obl_GlyphWidthEntry)));
    if (!m) return ERR(OBL_OTERR_NoMemory);
    auto *l = reinterpret_cast<obl_MinList *>(m);
    auto *w = reinterpret_cast<obl_GlyphWidthEntry *>(m + sizeof(obl_MinList));
    l->mlh_Head = reinterpret_cast<obl_MinNode *>(&l->mlh_Tail);
    l->mlh_Tail = nullptr;
    l->mlh_TailPred = reinterpret_cast<obl_MinNode *>(&l->mlh_Head);
    for (u32 c = e->code; c <= e->code2; c++) {
        u32 g = glyph_of(e, c);
        if (!g) continue;
        w->gwe_Code = u16(c);
        w->gwe_Width = s32(rnd(s64(advance_of(e, g)) * 65536, e->upm));
        obl_MinNode *nd = &w->gwe_Node, *tail = reinterpret_cast<obl_MinNode *>(&l->mlh_Tail);
        nd->mln_Succ = tail;
        nd->mln_Pred = l->mlh_TailPred;
        l->mlh_TailPred->mln_Succ = nd;
        l->mlh_TailPred = nd;
        w++;
    }
    *out = l;
    return ERR(OBL_OTERR_Success);
}

/* The kerning GFOpenFont shapes between the two codes (its kern and GPOS
 * tables); text and design pairs are the same (ABI test). */
u32 kern_pair(Engine *e, s32 *out)
{
    if (!e->face) return ERR(OBL_OTERR_NoFace);
    if (!e->have_code || !e->have_code2) return ERR(OBL_OTERR_NoGlyph);
    if (!glyph_of(e, e->code) || !glyph_of(e, e->code2)) return ERR(OBL_OTERR_UnknownGlyph);
    const s32 v = e->env->kern(e->user, e->face, e->code, e->code2);
    *out = s32(rnd(s64(v) * 65536, e->upm));
    return ERR(OBL_OTERR_Success);
}

/* ---- the face ---------------------------------------------------------- */

void drop_face(Engine *e)
{
    if (e->face) e->env->face_close(e->user, e->face);
    e->face = nullptr;
}

u32 slen(const char *s) { u32 n = 0; while (s[n]) n++; return n; }

bool lower_eq(const char *a, const char *b)
{
    for (; *a && *b; a++, b++) {
        char x = *a >= 'A' && *a <= 'Z' ? char(*a + 32) : *a;
        if (x != *b) return false;
    }
    return !*a && !*b;
}

/* OT_OTagList: the outline file is the .otag's OT_Spec1 (an OT_Indirect
 * string; ABI test: which Spec OS 3.2's .otag files name their file
 * with), a name without a device taken in the .otag's own directory; with
 * none, the .otag's path with ".ttf" for ".otag". */
u32 set_face(Engine *e, const TagItem *ot)
{
    drop_face(e);
    if (!ot) return ERR(OBL_OTERR_BadData);
    const char *spec = nullptr;
    for (const TagItem *tp = ot, *t; (t = next_tag(&tp)) != nullptr;)
        if (t->ti_Tag == (OBL_OT_Spec1 | OBL_OT_Indirect) && t->ti_Data) {
            spec = reinterpret_cast<const char *>(t->ti_Data);
            break;
        }
    /* the list is the .otag as loaded, OT_FileIdent its length: an
     * OT_Spec1 diskfont left unrelocated (an offset past the file), or one
     * with no NUL before the file's end, names no file */
    if (spec && ot->ti_Tag == OBL_OT_FileIdent && ot->ti_Data) {
        const u8 *base = reinterpret_cast<const u8 *>(ot), *end = base + ot->ti_Data;
        const u8 *s = reinterpret_cast<const u8 *>(spec);
        if (s < base || s >= end) return ERR(OBL_OTERR_BadFace);
        while (s < end && *s) s++;
        if (s >= end) return ERR(OBL_OTERR_BadFace);
    }
    char file[2 * OBL_MAXPATH];
    u32 n = 0;
    if (spec) {
        bool dev = false;
        for (const char *s = spec; *s; s++) dev |= *s == ':';
        if (!dev) {
            u32 dl = 0;
            for (u32 i = 0; e->path[i]; i++)
                if (e->path[i] == '/' || e->path[i] == ':') dl = i + 1;
            for (u32 i = 0; i < dl; i++) file[n++] = e->path[i];
        }
        u32 sl = slen(spec);
        if (sl >= OBL_MAXPATH) return ERR(OBL_OTERR_BadFace);
        for (u32 i = 0; i <= sl; i++) file[n++] = spec[i];
    } else {
        u32 pl = slen(e->path);
        if (!pl) return ERR(OBL_OTERR_BadFace);
        for (u32 i = 0; i < pl; i++) file[n++] = e->path[i];
        if (pl > 5 && lower_eq(e->path + pl - 5, ".otag")) n -= 5;
        const char ext[] = ".ttf";
        for (u32 i = 0; i < sizeof ext; i++) file[n++] = ext[i];
    }
    u32 upm = 0;
    void *face = e->env->face_open(e->user, file, &upm);
    if (!face) return ERR(OBL_OTERR_BadFace);
    if (!upm) {
        e->env->face_close(e->user, face);
        return ERR(OBL_OTERR_BadFace);
    }
    e->face = face;
    e->upm = upm;
    return ERR(OBL_OTERR_Success);
}

/* ---- the tags ---------------------------------------------------------------- */

enum Kind { SPEC, INQUIRY, BOTH, UNKNOWN };

Kind kind(u32 tag)
{
    switch (tag) {
    case OBL_OT_DeviceDPI: case OBL_OT_DotSize: case OBL_OT_PointHeight: case OBL_OT_SetFactor:
    case OBL_OT_ShearSin: case OBL_OT_ShearCos: case OBL_OT_RotateSin: case OBL_OT_RotateCos:
    case OBL_OT_EmboldenX: case OBL_OT_EmboldenY: case OBL_OT_GlyphCode: case OBL_OT_GlyphCode2:
    case OBL_OT_GlyphWidth: case OBL_OT_OTagPath: case OBL_OT_OTagList:
        return SPEC;
    case OBL_OT_GlyphMap: case OBL_OT_WidthList: case OBL_OT_TextKernPair: case OBL_OT_DesignKernPair:
        return INQUIRY;
    case OBL_OT_UnderLined: case OBL_OT_StrikeThrough:
        return BOTH;
    }
    return UNKNOWN;                /* OT_PointSize ("no longer supported") too: ABI test */
}

u32 set_one(Engine *e, u32 tag, u32 data)
{
    const s32 sd = s32(data);
    switch (tag) {
    case OBL_OT_DeviceDPI:
        if (!(data >> 16) || !(data & 0xffff)) return ERR(OBL_OTERR_BadData);
        e->dpi = data;
        break;
    case OBL_OT_DotSize: e->dotsize = data; break;      /* kept; changes nothing drawn (ABI test) */
    case OBL_OT_PointHeight:
        if (sd <= 0) return ERR(OBL_OTERR_BadData);
        e->point_height = data;
        break;
    case OBL_OT_SetFactor:
        if (sd <= 0) return ERR(OBL_OTERR_BadData);
        e->set_factor = data;
        break;
    case OBL_OT_ShearSin:
    case OBL_OT_RotateSin:
        if (sd > 0x10000 || sd < -0x10000) return ERR(OBL_OTERR_BadData);     /* a sine */
        if (tag == OBL_OT_ShearSin) { e->shear_sin = sd; e->shear_part = true; }
        else { e->rot_sin = sd; e->rot_part = true; }
        break;
    case OBL_OT_ShearCos:
        if (sd <= 0 || sd > 0x10000) return ERR(OBL_OTERR_BadData); /* the stroke within 90 degrees of upright */
        e->shear_cos = sd;
        e->shear_part = false;
        break;
    case OBL_OT_RotateCos:
        if (sd > 0x10000 || sd < -0x10000 || (!sd && !e->rot_sin)) return ERR(OBL_OTERR_BadData);
        e->rot_cos = sd;
        e->rot_part = false;
        break;
    case OBL_OT_EmboldenX:
    case OBL_OT_EmboldenY:
        if (sd > 0x10000 || sd < -0x10000) return ERR(OBL_OTERR_BadData);   /* within an em */
        (tag == OBL_OT_EmboldenX ? e->embolden_x : e->embolden_y) = sd;
        break;
    case OBL_OT_GlyphCode: e->code = data; e->have_code = true; break;
    case OBL_OT_GlyphCode2: e->code2 = data; e->have_code2 = true; break;
    case OBL_OT_GlyphWidth:
        if (sd < 0) return ERR(OBL_OTERR_BadData);
        e->glyph_width = data;
        break;
    case OBL_OT_OTagPath: {
        const char *p = reinterpret_cast<const char *>(data);
        drop_face(e);
        e->path[0] = 0;
        if (!p) return ERR(OBL_OTERR_BadData);
        u32 l = slen(p);
        if (l >= OBL_MAXPATH) return ERR(OBL_OTERR_BadData);
        for (u32 i = 0; i <= l; i++) e->path[i] = p[i];
        break;
    }
    case OBL_OT_OTagList: return set_face(e, reinterpret_cast<const TagItem *>(data));
    case OBL_OT_UnderLined:
        if (data > OBL_OTUL_DoubleBroken) return ERR(OBL_OTERR_BadData);
        e->underlined = data;                           /* kept, not drawn (ABI test) */
        break;
    case OBL_OT_StrikeThrough: e->strike = data != 0; break;
    default:
        return ERR(kind(tag) == INQUIRY ? OBL_OTERR_BadTag : OBL_OTERR_UnknownTag);
    }
    return ERR(OBL_OTERR_Success);
}

}  // namespace

/* ---- the calls ------------------------------------------------------------------ */

extern "C" obl_GlyphEngine *obl_OpenEngine(const obl_env *env, void *user, void *library, const char *name)
{
    auto *e = static_cast<Engine *>(env->alloc(user, sizeof(Engine)));
    if (!e) return nullptr;
    e->env = env;
    e->user = user;
    e->ge.gle_Library = library;
    e->ge.gle_Name = const_cast<char *>(name);
    /* the defaults (ABI test): 72 dpi each way, dots 100%, no size yet,
     * set 1.0, upright, unrotated, not emboldened */
    e->dpi = (72UL << 16) | 72;
    e->dotsize = (100UL << 16) | 100;
    e->set_factor = 0x10000;
    e->shear_cos = 0x10000;
    e->rot_cos = 0x10000;
    return &e->ge;
}

extern "C" void obl_CloseEngine(obl_GlyphEngine *ge)
{
    if (!ge) return;
    auto *e = reinterpret_cast<Engine *>(ge);
    const obl_env *env = e->env;
    void *user = e->user;
    drop_face(e);
    env->free(user, e);
}

/* Each tag in order; the first that fails stops the rest, those before it
 * stay done (ABI test). */
extern "C" obl_u32 obl_SetInfoA(obl_GlyphEngine *ge, const obl_TagItem *tags)
{
    if (!ge) return ERR(OBL_OTERR_Failure);
    auto *e = reinterpret_cast<Engine *>(ge);
    for (const TagItem *tp = tags, *t; (t = next_tag(&tp)) != nullptr;) {
        u32 r = set_one(e, t->ti_Tag, t->ti_Data);
        if (r) return r;
    }
    return ERR(OBL_OTERR_Success);
}

/* Each tag's ti_Data is where its answer goes. A specification-only tag
 * is OTERR_BadTag here (ABI test). */
extern "C" obl_u32 obl_ObtainInfoA(obl_GlyphEngine *ge, const obl_TagItem *tags)
{
    if (!ge) return ERR(OBL_OTERR_Failure);
    auto *e = reinterpret_cast<Engine *>(ge);
    for (const TagItem *tp = tags, *t; (t = next_tag(&tp)) != nullptr;) {
        void *where = reinterpret_cast<void *>(t->ti_Data);
        u32 r = ERR(OBL_OTERR_Success);
        const Kind k = kind(t->ti_Tag);
        if (k == UNKNOWN) r = ERR(OBL_OTERR_UnknownTag);
        else if (k == SPEC) r = ERR(OBL_OTERR_BadTag);
        else if (!where) r = ERR(OBL_OTERR_BadData);
        else switch (t->ti_Tag) {
            case OBL_OT_GlyphMap: {
                GlyphMap *gm = nullptr;
                r = glyph_map(e, &gm);
                if (!r) *static_cast<GlyphMap **>(where) = gm;
                break;
            }
            case OBL_OT_WidthList: {
                obl_MinList *l = nullptr;
                r = width_list(e, &l);
                if (!r) *static_cast<obl_MinList **>(where) = l;
                break;
            }
            case OBL_OT_TextKernPair:
            case OBL_OT_DesignKernPair: {
                s32 v = 0;
                r = kern_pair(e, &v);
                if (!r) *static_cast<s32 *>(where) = v;
                break;
            }
            case OBL_OT_UnderLined: *static_cast<u32 *>(where) = e->underlined; break;
            case OBL_OT_StrikeThrough: *static_cast<u32 *>(where) = e->strike; break;
            }
        if (r) return r;
    }
    return ERR(OBL_OTERR_Success);
}

/* A GlyphMap's or a width list's ti_Data is what ObtainInfoA gave (NULL
 * is allowed); the other inquiries have nothing to give back. */
extern "C" obl_u32 obl_ReleaseInfoA(obl_GlyphEngine *ge, const obl_TagItem *tags)
{
    if (!ge) return ERR(OBL_OTERR_Failure);
    auto *e = reinterpret_cast<Engine *>(ge);
    for (const TagItem *tp = tags, *t; (t = next_tag(&tp)) != nullptr;) {
        const Kind k = kind(t->ti_Tag);
        if (k == UNKNOWN) return ERR(OBL_OTERR_UnknownTag);
        if (k == SPEC) return ERR(OBL_OTERR_BadTag);
        if (t->ti_Tag == OBL_OT_GlyphMap || t->ti_Tag == OBL_OT_WidthList)
            release(e, reinterpret_cast<void *>(t->ti_Data));
    }
    return ERR(OBL_OTERR_Success);
}
