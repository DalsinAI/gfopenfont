/* GFOpenDiskFont's core: diskfont.library's work in lean C++.
 * Copyright (c) 2026 AmigaChrome Team. MIT.
 *
 * Clean-room: written from the diskfont autodocs' documented behaviour and
 * the NDK 3.2 layouts, never from Kickstart, Hyperion or AROS code. Where
 * the autodocs leave a corner open the choice made here is marked
 * "ABI test": tests/DiskFontABITest measures OS 3.2's answer. The outline
 * side (the .otag, the engine calls, a size drawn glyph by glyph),
 * ObtainCharsetInfo, AFF_OTAG and AFF_TYPE, weighing through graphics and
 * the expunge guard are ported from the other team's rewrite (PR #74, plain
 * C, the same clean-room rules); README.md says where the two differed.
 *
 * Built with openamigagcc's lean C++ library profile:
 *   -std=gnu++23 -fno-exceptions -fno-rtti -fno-threadsafe-statics
 *   -fno-asynchronous-unwind-tables -fno-unwind-tables -nostdlib++
 * No static constructors, no function-local statics, no standard headers
 * that allocate, nothing from libstdc++: memory comes from the env, and
 * the only calls out are the env's and libgcc's. The entry points are
 * extern "C" (diskfont_core.h).
 *
 * The rules this file implements (README.md says the same in prose):
 *
 *  Weight. On the Amiga, graphics' WeighTAMatch (the env's weigh), asked
 *  only about fonts of the requested YSize. The core's own, the tests'
 *  model of it: a font weighs 0 for a request of another YSize. Otherwise it
 *  starts at MAXFONTMATCHWEIGHT (32767) and loses 2048 for each of bold,
 *  italic and extended that differs, 512 when underlined differs, 1024 for
 *  each of FPF_REVPATH, FPF_TALLDOT and FPF_WIDEDOT that differs, and 4096
 *  when both name a TA_DeviceDPI and the two aspects (x/y) differ.
 *
 *  OpenDiskFont. The candidates are the fonts in memory with the name (not
 *  FPF_REMOVED), in the system list's order, then the entries of the first
 *  .font file found (the name's own directory when it has a path, else
 *  each directory of FONTS: in turn), in file order. The heaviest at the
 *  requested size wins, the earliest on a tie. With none at that size, and
 *  unless the request has FPF_DESIGNED, the source for scaling is the one
 *  that weighs most ignoring size, then is nearest in size, then larger,
 *  then earliest; fonts in memory without FPF_DESIGNED or FPF_ROMFONT
 *  (scaled ones) are never sources. With FPF_DESIGNED, or when scaling
 *  fails, the same order picks the font to return unscaled.
 *
 *  NewScaledDiskFont. Y scale Y/y0; X scale the same, times the aspect
 *  ratio change when both fonts name a TA_DeviceDPI. A value v scales to
 *  (v * n + d / 2) / d (negative values by their size, signed again). A
 *  glyph of width w > 0 gets width max(1, scaled w); glyphs are packed in
 *  order, the modulo is the bits rounded up to words (at least 2 bytes).
 *  A destination pixel (x, y) is set when any source pixel in the box
 *  [floor(x * w / w'), ceil((x + 1) * w / w')) by [floor(y * y0 / Y),
 *  ceil((y + 1) * y0 / Y)) is set.
 */
#include "diskfont_core.h"
#include <cstddef>
#include <type_traits>

using u8 = odf_u8;
using u16 = odf_u16;
using u32 = odf_u32;
using s16 = odf_s16;
using s32 = odf_s32;
using TagItem = odf_TagItem;
using TextFont = odf_TextFont;
using DFH = odf_DiskFontHeader;

/* ---- the NDK 3.2 layouts, checked ------------------------------------------ */

#define LAYOUT(T, size) \
    static_assert(std::is_standard_layout_v<T>, #T " is standard layout"); \
    static_assert(sizeof(T) == (size), #T "'s size")
#define AT(T, f, off) static_assert(offsetof(T, f) == (off), #T "." #f)

static_assert(sizeof(void *) == 4, "the core runs where a pointer is 32 bits, as on the 68k");
LAYOUT(odf_TagItem, 8);
LAYOUT(odf_Node, 14);
LAYOUT(odf_List, 14);
AT(odf_Node, ln_Type, 8); AT(odf_Node, ln_Pri, 9); AT(odf_Node, ln_Name, 10);
LAYOUT(odf_Message, 20);
AT(odf_Message, mn_ReplyPort, 14); AT(odf_Message, mn_Length, 18);
LAYOUT(odf_TextAttr, 8);
AT(odf_TextAttr, ta_YSize, 4); AT(odf_TextAttr, ta_Style, 6); AT(odf_TextAttr, ta_Flags, 7);
LAYOUT(odf_TTextAttr, 12);
AT(odf_TTextAttr, tta_Tags, 8);
LAYOUT(odf_TextFont, 52);
AT(odf_TextFont, tf_YSize, 20); AT(odf_TextFont, tf_Style, 22); AT(odf_TextFont, tf_Flags, 23);
AT(odf_TextFont, tf_XSize, 24); AT(odf_TextFont, tf_Baseline, 26); AT(odf_TextFont, tf_BoldSmear, 28);
AT(odf_TextFont, tf_Accessors, 30); AT(odf_TextFont, tf_LoChar, 32); AT(odf_TextFont, tf_HiChar, 33);
AT(odf_TextFont, tf_CharData, 34); AT(odf_TextFont, tf_Modulo, 38); AT(odf_TextFont, tf_CharLoc, 40);
AT(odf_TextFont, tf_CharSpace, 44); AT(odf_TextFont, tf_CharKern, 48);
LAYOUT(odf_TextFontExtension, 24);
AT(odf_TextFontExtension, tfe_BackPtr, 4); AT(odf_TextFontExtension, tfe_OrigReplyPort, 8);
AT(odf_TextFontExtension, tfe_Tags, 12); AT(odf_TextFontExtension, tfe_OFontPatchK, 20);
LAYOUT(odf_ColorFontColors, 8);
AT(odf_ColorFontColors, cfc_Count, 2); AT(odf_ColorFontColors, cfc_ColorTable, 4);
LAYOUT(odf_ColorTextFont, 96);
AT(odf_ColorTextFont, ctf_Flags, 52); AT(odf_ColorTextFont, ctf_Depth, 54); AT(odf_ColorTextFont, ctf_FgColor, 55);
AT(odf_ColorTextFont, ctf_Low, 56); AT(odf_ColorTextFont, ctf_High, 57); AT(odf_ColorTextFont, ctf_PlanePick, 58);
AT(odf_ColorTextFont, ctf_PlaneOnOff, 59); AT(odf_ColorTextFont, ctf_ColorFontColors, 60);
AT(odf_ColorTextFont, ctf_CharData, 64);
LAYOUT(odf_FontContents, 260);
AT(odf_FontContents, fc_YSize, 256); AT(odf_FontContents, fc_Style, 258); AT(odf_FontContents, fc_Flags, 259);
LAYOUT(odf_TFontContents, 260);
AT(odf_TFontContents, tfc_TagCount, 254); AT(odf_TFontContents, tfc_YSize, 256);
AT(odf_TFontContents, tfc_Style, 258); AT(odf_TFontContents, tfc_Flags, 259);
LAYOUT(odf_FontContentsHeader, 4);
LAYOUT(odf_DiskFontHeader, 106);
AT(odf_DiskFontHeader, dfh_FileID, 14); AT(odf_DiskFontHeader, dfh_Revision, 16);
AT(odf_DiskFontHeader, dfh_Segment, 18); AT(odf_DiskFontHeader, dfh_Name, 22); AT(odf_DiskFontHeader, dfh_TF, 54);
LAYOUT(struct odf_AvailFonts, 10);
AT(struct odf_AvailFonts, af_Attr, 2);
LAYOUT(odf_TAvailFonts, 14);
AT(odf_TAvailFonts, taf_Attr, 2);
LAYOUT(odf_AvailFontsHeader, 2);
LAYOUT(odf_GlyphEngine, 8);
AT(odf_GlyphEngine, gle_Name, 4);
LAYOUT(odf_GlyphMap, 36);
AT(odf_GlyphMap, glm_BMRows, 2); AT(odf_GlyphMap, glm_BlackLeft, 4); AT(odf_GlyphMap, glm_BlackTop, 6);
AT(odf_GlyphMap, glm_BlackWidth, 8); AT(odf_GlyphMap, glm_BlackHeight, 10); AT(odf_GlyphMap, glm_XOrigin, 12);
AT(odf_GlyphMap, glm_YOrigin, 16); AT(odf_GlyphMap, glm_X0, 20); AT(odf_GlyphMap, glm_Y0, 22);
AT(odf_GlyphMap, glm_X1, 24); AT(odf_GlyphMap, glm_Y1, 26); AT(odf_GlyphMap, glm_Width, 28);
AT(odf_GlyphMap, glm_BitMap, 32);
LAYOUT(odf_EGlyphEngine, 12);
AT(odf_EGlyphEngine, ege_BulletBase, 4); AT(odf_EGlyphEngine, ege_GlyphEngine, 8);
LAYOUT(odf_OutlineFont, 36);
AT(odf_OutlineFont, olf_OTagList, 4); AT(odf_OutlineFont, olf_EngineName, 8);
AT(odf_OutlineFont, olf_LibraryName, 12); AT(odf_OutlineFont, olf_EEngine, 16);
AT(odf_OutlineFont, olf_Reserved, 28); AT(odf_OutlineFont, olf_UserData, 32);
static_assert(std::is_standard_layout_v<odf_ctx>);
static_assert(std::is_standard_layout_v<odf_env>);

namespace {

constexpr u32 FCH_ENTRY = 260;
constexpr unsigned MAXTAGS = 64;      /* a tag list copied from memory stops here */

/* ---- bytes and strings (no libc) ---------------------------------------------- */

void copy(void *d, const void *s, u32 n)
{
    u8 *dp = static_cast<u8 *>(d);
    const u8 *sp = static_cast<const u8 *>(s);
    while (n--) *dp++ = *sp++;
}

u32 slen(const char *s)
{
    u32 n = 0;
    while (s[n]) n++;
    return n;
}

constexpr u8 lower(u8 c) { return (c >= 'A' && c <= 'Z') ? u8(c + 32) : c; }

/* Names compare without case, as AmigaDOS names do. ABI test: whether
 * OS 3.2 compares memory fonts' names with case. */
int icmp(const char *a, const char *b)
{
    for (;; a++, b++) {
        u8 x = lower(u8(*a)), y = lower(u8(*b));
        if (x != y) return x < y ? -1 : 1;
        if (!x) return 0;
    }
}

bool ieq(const char *a, const char *b) { return icmp(a, b) == 0; }

bool seq(const char *a, const char *b)
{
    while (*a && *a == *b) a++, b++;
    return *a == *b;
}

/* the name after its path: "FONTS:topaz.font" -> "topaz.font" */
const char *basename(const char *n)
{
    const char *b = n;
    for (; *n; n++)
        if (*n == ':' || *n == '/') b = n + 1;
    return b;
}

bool has_font_suffix(const char *n)
{
    u32 l = slen(n);
    return l >= 5 && ieq(n + l - 5, ".font");
}

/* ---- memory through the env --------------------------------------------------- */

void *alloc(odf_ctx *c, u32 n) { return c->env->alloc(c, n ? n : 1); }
void release(odf_ctx *c, void *p) { if (p) c->env->free(c, p); }

/* A growing array on the env's memory; T is plain data. */
template <class T> class Vec {
public:
    explicit Vec(odf_ctx *c) : c_(c) {}
    ~Vec() { release(c_, p_); }
    Vec(const Vec &) = delete;
    Vec &operator=(const Vec &) = delete;

    bool push(const T &v)
    {
        if (n_ == cap_ && !reserve(cap_ ? cap_ * 2 : 16)) return false;
        copy(&p_[n_++], &v, sizeof(T));
        return true;
    }
    bool append(const T *v, u32 k)
    {
        if (n_ + k > cap_ && !reserve(n_ + k > cap_ * 2 ? n_ + k : cap_ * 2)) return false;
        copy(&p_[n_], v, k * sizeof(T));
        n_ += k;
        return true;
    }
    T &operator[](u32 i) { return p_[i]; }
    const T &operator[](u32 i) const { return p_[i]; }
    T *data() { return p_; }
    u32 size() const { return n_; }
    bool ok() const { return !bad_; }

private:
    bool reserve(u32 k)
    {
        T *q = static_cast<T *>(alloc(c_, k * sizeof(T)));
        if (!q) { bad_ = true; return false; }
        copy(q, p_, n_ * sizeof(T));
        release(c_, p_);
        p_ = q;
        cap_ = k;
        return true;
    }
    odf_ctx *c_;
    T *p_ = nullptr;
    u32 n_ = 0, cap_ = 0;
    bool bad_ = false;
};

/* The font list's lock, held for a scope. busy counts the holders: the
 * semaphore lets the same task in again, so an expunge that low memory
 * runs inside one of our calls finds busy set (odf_Flush). From #74. */
class Locked {
public:
    explicit Locked(odf_ctx *c) : c_(c) { c_->env->lock(c_); c_->busy++; }
    ~Locked() { c_->busy--; c_->env->unlock(c_); }
    Locked(const Locked &) = delete;
    Locked &operator=(const Locked &) = delete;
private:
    odf_ctx *c_;
};

/* A directory the core locked, unlocked at the end of its scope */
class Dir {
public:
    explicit Dir(odf_ctx *c) : c_(c) {}
    ~Dir() { if (d_) c_->env->dir_unlock(c_, d_); }
    Dir(const Dir &) = delete;
    Dir &operator=(const Dir &) = delete;
    void set(void *d) { if (d_) c_->env->dir_unlock(c_, d_); d_ = d; }
    void *get() const { return d_; }
private:
    odf_ctx *c_;
    void *d_ = nullptr;
};

/* ---- tags ------------------------------------------------------------------------- */

/* Walks a tag list; TAG_MORE is followed only for lists in memory (never
 * for a list read from a file). */
template <class F> void walk_tags(const TagItem *t, bool more, F each)
{
    unsigned guard = 0;
    while (t && guard++ < 4096) {
        switch (t->ti_Tag) {
        case ODF_TAG_DONE: return;
        case ODF_TAG_IGNORE: t++; break;
        case ODF_TAG_MORE:
            if (!more) return;
            t = reinterpret_cast<const TagItem *>(t->ti_Data);
            break;
        case ODF_TAG_SKIP: t += 1 + t->ti_Data; break;
        default:
            if (!each(*t)) return;
            t++;
        }
    }
}

bool find_tag(const TagItem *tags, bool more, u32 tag, u32 *data)
{
    bool found = false;
    walk_tags(tags, more, [&](const TagItem &t) {
        if (t.ti_Tag != tag) return true;
        *data = t.ti_Data;
        found = true;
        return false;
    });
    return found;
}

/* a DPI is usable when both halves are non-zero */
u32 dpi_of(const TagItem *tags, bool more)
{
    u32 d = 0;
    if (!find_tag(tags, more, ODF_TA_DeviceDPI, &d)) return 0;
    return (d >> 16) && (d & 0xffff) ? d : 0;
}

const odf_TextFontExtension *extension(const TextFont *tf)
{
    /* ABI test: graphics' tfe_MatchWord; the back pointer is checked */
    auto *e = static_cast<const odf_TextFontExtension *>(tf->tf_Message.mn_ReplyPort);
    return e && e->tfe_BackPtr == tf ? e : nullptr;
}

/* ---- .font contents ---------------------------------------------------------------- */

struct Entry {
    const char *file;
    u16 ysize;
    u8 style, flags;
    const TagItem *tags;           /* TAG_DONE ends them; NULL for none */
    u16 ntags;                     /* TAG_DONE included */
};

struct Contents {
    const u8 *buf = nullptr;
    u32 len = 0;
    u16 id = 0, n = 0;

    bool parse(const void *b, u32 l)
    {
        buf = static_cast<const u8 *>(b);
        len = l;
        if (!buf || len < 4) return false;
        auto *h = reinterpret_cast<const odf_FontContentsHeader *>(buf);
        id = h->fch_FileID;
        if (id != ODF_FCH_ID && id != ODF_TFCH_ID && id != ODF_OFCH_ID) return false;
        u32 fit = (len - 4) / FCH_ENTRY;
        n = h->fch_NumEntries < fit ? h->fch_NumEntries : u16(fit);   /* ABI test: a short file */
        return true;
    }
    /* entry i, or false when its name isn't ended inside its room */
    bool entry(u16 i, Entry &e) const
    {
        const u8 *p = buf + 4 + u32(i) * FCH_ENTRY;
        auto *fc = reinterpret_cast<const odf_TFontContents *>(p);
        u32 room = ODF_MAXFONTPATH;
        e.tags = nullptr;
        e.ntags = 0;
        if (id != ODF_FCH_ID && fc->tfc_TagCount) {
            u16 k = fc->tfc_TagCount;
            if (k > 31) return false;
            room = ODF_MAXFONTPATH - 8u * k;
            auto *t = reinterpret_cast<const TagItem *>(p + room);
            if (t[k - 1].ti_Tag != ODF_TAG_DONE) return false;
            e.tags = t;
            e.ntags = k;
        }
        u32 l = 0;
        while (l < room && p[l]) l++;
        if (l == room || l == 0) return false;
        e.file = reinterpret_cast<const char *>(p);
        e.ysize = fc->tfc_YSize;
        e.style = fc->tfc_Style;
        e.flags = fc->tfc_Flags;
        return true;
    }
};

/* A .font file, read and parsed, with the directory it is in held */
class FontFile {
public:
    explicit FontFile(odf_ctx *c) : c_(c), dir_(c) {}
    ~FontFile() { release(c_, mem_); }
    FontFile(const FontFile &) = delete;
    FontFile &operator=(const FontFile &) = delete;

    /* name with a path: there; else the first directory of FONTS: with it */
    bool find(const char *name)
    {
        const char *b = basename(name);
        if (b != name) {
            u32 dl = u32(b - name);
            if (name[dl - 1] == '/') dl--;            /* "a/b/x.font" -> "a/b"; "w:x.font" -> "w:" */
            char path[ODF_MAXFONTPATH];
            if (dl >= sizeof path) return false;
            copy(path, name, dl);
            path[dl] = 0;
            dir_.set(c_->env->dir_lock(c_, nullptr, path));
            return dir_.get() && load(b);
        }
        file_ = b;
        c_->env->fonts_scan(c_, [](void *d, void *dir) -> int {
            auto *self = static_cast<FontFile *>(d);
            if (!self->try_dir(dir)) return 0;
            self->dir_.set(self->c_->env->dir_lock(self->c_, dir, ""));
            return 1;
        }, this);
        return dir_.get() && mem_;
    }
    bool load_in(void *dir, const char *name)   /* a .font in a directory the caller holds */
    {
        return read(dir, name);
    }
    void *dir() const { return dir_.get(); }

    Contents cont;

private:
    bool try_dir(void *dir) { return read(dir, file_); }
    bool load(const char *b) { return read(dir_.get(), b); }
    bool read(void *dir, const char *name)
    {
        u32 l = 0;
        void *m = c_->env->file_load(c_, dir, name, &l);
        if (!m) return false;
        Contents k;
        if (!k.parse(m, l)) { release(c_, m); return false; }
        release(c_, mem_);
        mem_ = m;
        cont = k;
        return true;
    }
    odf_ctx *c_;
    Dir dir_;
    void *mem_ = nullptr;
    const char *file_ = nullptr;
};

/* ---- weighing ------------------------------------------------------------------------- */

u32 weigh(u16 ry, u8 rs, u8 rf, u32 rdpi, u16 fy, u8 fs, u8 ff, u32 fdpi)
{
    if (ry != fy) return 0;
    u32 w = ODF_MAXFONTMATCHWEIGHT;
    u8 ds = rs ^ fs, df = rf ^ ff;
    if (ds & ODF_FSF_BOLD) w -= 2048;
    if (ds & ODF_FSF_ITALIC) w -= 2048;
    if (ds & ODF_FSF_EXTENDED) w -= 2048;
    if (ds & ODF_FSF_UNDERLINED) w -= 512;
    if (df & ODF_FPF_REVPATH) w -= 1024;
    if (df & ODF_FPF_TALLDOT) w -= 1024;
    if (df & ODF_FPF_WIDEDOT) w -= 1024;
    if (rdpi && fdpi && (rdpi >> 16) * (fdpi & 0xffff) != (fdpi >> 16) * (rdpi & 0xffff)) w -= 4096;
    return w;
}

struct Request {
    const char *name;
    u16 y;
    u8 s, f;
    u32 dpi;
};

/* A font's weight for a request, through graphics' WeighTAMatch when the
 * env has it (from #74), else the core's own. Either way 0 for another
 * YSize. Graphics is shown the bits the core weighs: bold, italic,
 * extended and underlined; FPF_REVPATH, FPF_TALLDOT and FPF_WIDEDOT; and
 * the DPI as a TA_DeviceDPI tag (ABI test: whether WeighTAMatch weighs
 * more than these). */
u32 weigh_env(odf_ctx *c, const Request &r, u16 fy, u8 fs, u8 ff, u32 fdpi)
{
    if (r.y != fy) return 0;
    if (!c->env->weigh) return weigh(r.y, r.s, r.f, r.dpi, fy, fs, ff, fdpi);
    constexpr u8 STYLES = ODF_FSF_UNDERLINED | ODF_FSF_BOLD | ODF_FSF_ITALIC | ODF_FSF_EXTENDED;
    constexpr u8 FLAGS = ODF_FPF_REVPATH | ODF_FPF_TALLDOT | ODF_FPF_WIDEDOT;
    TagItem rt[2] = {{ODF_TA_DeviceDPI, r.dpi}, {ODF_TAG_DONE, 0}};
    TagItem ft[2] = {{ODF_TA_DeviceDPI, fdpi}, {ODF_TAG_DONE, 0}};
    odf_TTextAttr req{const_cast<char *>(r.name), r.y, u8((r.s & STYLES) | (r.dpi ? ODF_FSF_TAGGED : 0)),
                      u8(r.f & FLAGS), r.dpi ? rt : nullptr};
    odf_TextAttr font{const_cast<char *>(r.name), fy, u8(fs & STYLES), u8(ff & FLAGS)};
    return c->env->weigh(c, &req, &font, fdpi ? ft : nullptr);
}

/* ---- loading and installing fonts ----------------------------------------------------- */

void set_name(DFH *d, const char *name)
{
    const char *b = basename(name);
    u32 l = slen(b);
    if (l > ODF_MAXFONTNAME - 1) l = ODF_MAXFONTNAME - 1;
    copy(d->dfh_Name, b, l);
    for (u32 i = l; i < ODF_MAXFONTNAME; i++) d->dfh_Name[i] = 0;
    d->dfh_DF.ln_Name = d->dfh_Name;
    d->dfh_TF.tf_Message.mn_Node.ln_Name = d->dfh_Name;
}

/* A font this library made or loaded joins the system list and ours.
 * tags: the font's own (from its file, or NewScaledDiskFont's). */
void install(odf_ctx *c, DFH *d, s32 seg)
{
    TextFont *tf = &d->dfh_TF;
    const TagItem *tags = (tf->tf_Style & ODF_FSF_TAGGED) ? reinterpret_cast<const TagItem *>(d->dfh_Segment)
                                                          : nullptr;
    d->dfh_Segment = seg;
    d->dfh_DF.ln_Type = ODF_NT_FONT;
    tf->tf_Message.mn_Node.ln_Type = ODF_NT_FONT;
    tf->tf_Accessors = 0;
    if (tags) c->env->extend_font(c, tf, tags);
    c->env->add_font(c, tf);
    d->dfh_DF.ln_Succ = reinterpret_cast<odf_Node *>(c->loaded);
    c->loaded = d;
}

/* A font file from disk: its DiskFontHeader, named after the .font it came
 * through, with FPF_DISKFONT. ABI test: whether OS 3.2 keeps the file's
 * own dfh_Name. */
TextFont *load_entry(odf_ctx *c, void *dir, const Entry &e, const char *fontname)
{
    u8 *first = nullptr;
    void *seg = c->env->seg_load(c, dir, e.file, &first);
    if (!seg) return nullptr;
    DFH *d = odf_FontHeader(first);
    if (!d) { c->env->seg_unload(c, seg); return nullptr; }
    set_name(d, fontname);
    d->dfh_TF.tf_Flags |= ODF_FPF_DISKFONT;
    install(c, d, s32(reinterpret_cast<u32>(seg)));
    return c->env->open_font(c, &d->dfh_TF);
}

/* ---- scaling ----------------------------------------------------------------------------- */

using u64 = unsigned long long;

struct Ratio {
    u64 n, d;
    u32 scale(u32 v) const { return u32((v * n + d / 2) / d); }
    s16 sscale(s16 v) const { return v < 0 ? s16(-s32(scale(u32(-s32(v))))) : s16(scale(u32(v))); }
    u32 floor_(u32 v) const { return u32(v * n / d); }
};

u32 align2(u32 v) { return (v + 1) & ~1u; }

bool get_bit(const u8 *data, u32 modulo, u32 x, u32 y)
{
    return (data[y * modulo + (x >> 3)] >> (7 - (x & 7))) & 1;
}

void set_bit(u8 *data, u32 modulo, u32 x, u32 y)
{
    data[y * modulo + (x >> 3)] |= u8(0x80 >> (x & 7));
}

/* The distinct glyph planes of a font: tf_CharData and, for a colour
 * font, ctf_CharData[0 .. depth-1]. */
struct Planes {
    const void *src[9];
    unsigned n = 0;
    int tf = -1;                        /* which one is tf_CharData */
    int ctf[8];

    void gather(const TextFont *tf_)
    {
        for (int &k : ctf) k = -1;
        if (tf_->tf_Style & ODF_FSF_COLORFONT) {
            auto *ct = reinterpret_cast<const odf_ColorTextFont *>(tf_);
            unsigned depth = ct->ctf_Depth > 8 ? 8 : ct->ctf_Depth;
            for (unsigned p = 0; p < depth; p++)
                if (ct->ctf_CharData[p]) ctf[p] = add(ct->ctf_CharData[p]);
        }
        if (tf_->tf_CharData) tf = add(tf_->tf_CharData);
    }
    int add(const void *p)
    {
        for (unsigned i = 0; i < n; i++)
            if (src[i] == p) return int(i);
        src[n] = p;
        return int(n++);
    }
};

DFH *scale_font(odf_ctx *c, const TextFont *s, u16 ny, u32 rdpi)
{
    if (!s || !ny || !s->tf_YSize || s->tf_HiChar < s->tf_LoChar || !s->tf_CharLoc) return nullptr;
    const bool color = s->tf_Style & ODF_FSF_COLORFONT;
    const u32 nglyph = u32(s->tf_HiChar - s->tf_LoChar) + 2;
    const u32 sdpi = odf_FontDPI(s);
    const u32 ddpi = rdpi ? rdpi : sdpi;
    Ratio ry{ny, s->tf_YSize};
    Ratio rx = ry;
    if (sdpi && ddpi) {
        rx.n = u64(ny) * (ddpi >> 16) * (sdpi & 0xffff);
        rx.d = u64(s->tf_YSize) * (ddpi & 0xffff) * (sdpi >> 16);
    }
    auto *sloc = static_cast<const u32 *>(s->tf_CharLoc);
    auto *sspace = static_cast<const s16 *>(s->tf_CharSpace);
    auto *skern = static_cast<const s16 *>(s->tf_CharKern);
    Planes pl;
    pl.gather(s);

    u32 bits = 0;
    for (u32 i = 0; i < nglyph; i++) {
        u32 w = sloc[i] & 0xffff;
        if (w) { u32 nw = rx.scale(w); bits += nw ? nw : 1; }
    }
    if (bits > 0xffff) return nullptr;      /* a glyph's offset is 16 bits */
    u32 modulo = ((bits + 15) >> 4) << 1;
    if (modulo < 2) modulo = 2;

    const auto *scf = color ? reinterpret_cast<const odf_ColorTextFont *>(s)->ctf_ColorFontColors : nullptr;
    const u32 ncolors = scf && scf->cfc_ColorTable ? scf->cfc_Count : 0;
    u32 o = 54 + (color ? sizeof(odf_ColorTextFont) : sizeof(TextFont));
    const u32 o_tags = o;          if (ddpi) o += 2 * sizeof(TagItem);
    const u32 o_cfc = o;           if (scf) o += sizeof(odf_ColorFontColors) + align2(ncolors * 2);
    const u32 o_loc = o;           o += nglyph * 4;
    const u32 o_space = o;         if (sspace) o += nglyph * 2;
    const u32 o_kern = o;          if (skern) o += nglyph * 2;
    const u32 o_data = o;          o += pl.n * modulo * ny;

    u8 *m = static_cast<u8 *>(alloc(c, o));
    if (!m) return nullptr;
    DFH *d = reinterpret_cast<DFH *>(m);
    TextFont *t = &d->dfh_TF;
    d->dfh_FileID = ODF_DFH_ID;
    set_name(d, s->tf_Message.mn_Node.ln_Name ? s->tf_Message.mn_Node.ln_Name : "");
    t->tf_Message.mn_Length = s->tf_Message.mn_Length;    /* ABI test */
    t->tf_YSize = ny;
    t->tf_Style = u8(s->tf_Style & ~ODF_FSF_TAGGED);
    /* ABI test: the flags a scaled font gets */
    t->tf_Flags = u8((s->tf_Flags & ~(ODF_FPF_ROMFONT | ODF_FPF_DESIGNED | ODF_FPF_REMOVED)) | ODF_FPF_DISKFONT);
    u32 xs = rx.scale(s->tf_XSize);
    t->tf_XSize = u16(xs ? xs : 1);
    u32 bl = ry.scale(s->tf_Baseline);
    t->tf_Baseline = u16(bl < ny ? bl : ny - 1u);
    u32 bs = rx.scale(s->tf_BoldSmear);
    t->tf_BoldSmear = u16(s->tf_BoldSmear && !bs ? 1 : bs);
    t->tf_LoChar = s->tf_LoChar;
    t->tf_HiChar = s->tf_HiChar;
    t->tf_Modulo = u16(modulo);
    if (ddpi) {
        auto *tg = reinterpret_cast<TagItem *>(m + o_tags);
        tg[0].ti_Tag = ODF_TA_DeviceDPI;
        tg[0].ti_Data = ddpi;
        tg[1].ti_Tag = ODF_TAG_DONE;
        t->tf_Style |= ODF_FSF_TAGGED;
        d->dfh_Segment = s32(reinterpret_cast<u32>(tg));     /* dfh_TagList */
    }

    auto *loc = reinterpret_cast<u32 *>(m + o_loc);
    t->tf_CharLoc = loc;
    u32 at = 0;
    for (u32 i = 0; i < nglyph; i++) {
        u32 w = sloc[i] & 0xffff, nw = 0;
        if (w) { nw = rx.scale(w); if (!nw) nw = 1; }
        loc[i] = (at << 16) | nw;
        at += nw;
    }
    if (sspace) {
        auto *sp = reinterpret_cast<s16 *>(m + o_space);
        for (u32 i = 0; i < nglyph; i++) sp[i] = rx.sscale(sspace[i]);
        t->tf_CharSpace = sp;
    }
    if (skern) {
        auto *kn = reinterpret_cast<s16 *>(m + o_kern);
        for (u32 i = 0; i < nglyph; i++) kn[i] = rx.sscale(skern[i]);
        t->tf_CharKern = kn;
    }

    u8 *planes[9];
    const u32 smod = s->tf_Modulo, sy = s->tf_YSize;
    for (unsigned p = 0; p < pl.n; p++) {
        u8 *dp = m + o_data + p * modulo * ny;
        planes[p] = dp;
        const u8 *sp = static_cast<const u8 *>(pl.src[p]);
        for (u32 i = 0; i < nglyph; i++) {
            u32 soff = sloc[i] >> 16, w = sloc[i] & 0xffff;
            u32 doff = loc[i] >> 16, nw = loc[i] & 0xffff;
            Ratio gx{w, nw ? nw : 1};
            for (u32 y = 0; y < ny; y++) {
                u32 y0 = u32(u64(y) * sy / ny), y1 = u32((u64(y + 1) * sy + ny - 1) / ny);
                for (u32 x = 0; x < nw; x++) {
                    u32 x0 = gx.floor_(x), x1 = u32((u64(x + 1) * w + nw - 1) / nw);
                    bool on = false;
                    for (u32 yy = y0; yy < y1 && !on; yy++)
                        for (u32 xx = x0; xx < x1 && !on; xx++)
                            on = get_bit(sp, smod, soff + xx, yy);
                    if (on) set_bit(dp, modulo, doff + x, y);
                }
            }
        }
    }
    if (pl.tf >= 0) t->tf_CharData = planes[pl.tf];
    if (color) {
        auto *sct = reinterpret_cast<const odf_ColorTextFont *>(s);
        auto *dct = reinterpret_cast<odf_ColorTextFont *>(t);
        dct->ctf_Flags = sct->ctf_Flags;
        dct->ctf_Depth = sct->ctf_Depth;
        dct->ctf_FgColor = sct->ctf_FgColor;
        dct->ctf_Low = sct->ctf_Low;
        dct->ctf_High = sct->ctf_High;
        dct->ctf_PlanePick = sct->ctf_PlanePick;
        dct->ctf_PlaneOnOff = sct->ctf_PlaneOnOff;
        for (unsigned p = 0; p < 8; p++)
            if (pl.ctf[p] >= 0) dct->ctf_CharData[p] = planes[pl.ctf[p]];
        if (scf) {
            auto *cf = reinterpret_cast<odf_ColorFontColors *>(m + o_cfc);
            cf->cfc_Reserved = scf->cfc_Reserved;
            cf->cfc_Count = scf->cfc_Count;
            if (ncolors) {
                cf->cfc_ColorTable = reinterpret_cast<u16 *>(m + o_cfc + sizeof *cf);
                copy(cf->cfc_ColorTable, scf->cfc_ColorTable, ncolors * 2);
            }
            dct->ctf_ColorFontColors = cf;
        }
    }
    return d;
}

/* ---- outline fonts (from #74) ---------------------------------------------------------- */

/* The length of a name without its ".font" or ".otag" */
u32 stem_len(const char *n)
{
    u32 l = slen(n);
    if (l >= 5 && (ieq(n + l - 5, ".font") || ieq(n + l - 5, ODF_OTSUFFIX))) l -= 5;
    return l;
}

/* "x.font" (or "x.otag", or "x") -> "x.otag"; false when it doesn't fit */
bool otag_name(const char *name, char *out, u32 room)
{
    u32 l = stem_len(name);
    if (!l || l + 6 > room) return false;
    copy(out, name, l);
    copy(out + l, ODF_OTSUFFIX, 6);
    return true;
}

/* The path an engine is given (OT_OTagPath): the name's own directory and
 * its .otag, or "FONTS:" and its .otag when it has none (as #74). */
bool otag_path(const char *name, char *out, u32 room)
{
    const char *b = basename(name);
    u32 dl = u32(b - name);
    const char *pre = dl ? name : "FONTS:";
    u32 pl = dl ? dl : 6;
    if (pl >= room) return false;
    copy(out, pre, pl);
    return otag_name(b, out + pl, room - pl);
}

/* An .otag's length (its OT_FileIdent) */
u32 otag_len(const TagItem *ot) { return ot[0].ti_Data; }

/* An OT_Indirect string in a loaded .otag, ended inside it; NULL if none */
const char *otag_str(const TagItem *ot, u32 tag)
{
    u32 a = 0;
    if (!find_tag(ot, false, tag, &a) || !a) return nullptr;
    const u8 *base = reinterpret_cast<const u8 *>(ot), *end = base + otag_len(ot);
    const u8 *p = reinterpret_cast<const u8 *>(a);
    if (p < base || p >= end) return nullptr;
    for (const u8 *q = p; q < end; q++)
        if (!*q) return reinterpret_cast<const char *>(p);
    return nullptr;
}

/* OT_AvailSizes: a count, then the sizes; their number (at most
 * OT_MAXAVAILSIZES), *sizes at the first */
u32 otag_sizes(const TagItem *ot, const u16 **sizes)
{
    u32 a = 0;
    if (!find_tag(ot, false, ODF_OT_AvailSizes, &a) || !a || (a & 1)) return 0;
    const u8 *base = reinterpret_cast<const u8 *>(ot), *end = base + otag_len(ot);
    auto *p = reinterpret_cast<const u16 *>(a);
    if (reinterpret_cast<const u8 *>(p) < base || reinterpret_cast<const u8 *>(p + 1) > end) return 0;
    u32 n = p[0] < ODF_OT_MAXAVAILSIZES ? p[0] : ODF_OT_MAXAVAILSIZES;
    if (reinterpret_cast<const u8 *>(p + 1 + n) > end) return 0;
    *sizes = p + 1;
    return n;
}

/* The .otag named after name, in dir; NULL when there is none */
TagItem *otag_in(odf_ctx *c, void *dir, const char *name, char *full = nullptr, u32 room = 0)
{
    char on[ODF_MAXFONTPATH];
    if (!otag_name(basename(name), on, sizeof on)) return nullptr;
    u32 len = 0;
    void *m = c->env->file_load(c, dir, on, &len);
    TagItem *ot = m ? odf_OTagLoad(c, m, len) : nullptr;
    /* the .otag's full name, as OS 3.2's OpenOutlineFont gives it
     * ("System:FONTS/CGTimes.otag"): ABI test */
    if (ot && full && (!c->env->name_of || !c->env->name_of(c, dir, on, full, room))) full[0] = 0;
    return ot;
}

/* The .otag for a name: in the name's own directory, else the first of
 * FONTS:'s directories that has it */
TagItem *otag_find(odf_ctx *c, const char *name, char *full = nullptr, u32 room = 0)
{
    const char *b = basename(name);
    if (b != name) {
        u32 dl = u32(b - name);
        if (name[dl - 1] == '/') dl--;
        char path[ODF_MAXFONTPATH];
        if (dl >= sizeof path) return nullptr;
        copy(path, name, dl);
        path[dl] = 0;
        Dir d(c);
        d.set(c->env->dir_lock(c, nullptr, path));
        return d.get() ? otag_in(c, d.get(), b, full, room) : nullptr;
    }
    struct Find { odf_ctx *c; const char *b; TagItem *ot; char *full; u32 room; } f{c, b, nullptr, full, room};
    c->env->fonts_scan(c, [](void *d, void *dir) -> int {
        auto *k = static_cast<Find *>(d);
        k->ot = otag_in(k->c, dir, k->b, k->full, k->room);
        return k->ot ? 1 : 0;
    }, &f);
    return f.ot;
}

u32 ecall(odf_ctx *c, void *lib, s32 lvo, const void *a0, const void *a1)
{
    if (!lib || !c->env->lib_call) return u32(ODF_OTERR_Failure);
    return c->env->lib_call(c, lib, lvo, const_cast<void *>(a0), const_cast<void *>(a1));
}

/* The engine's library: OT_Engine's name (bullet when none) and
 * ".library"; false when it doesn't fit */
bool engine_lib(const TagItem *ot, char *out, u32 room)
{
    const char *e = otag_str(ot, ODF_OT_Engine);
    if (!e) e = ODF_OTE_Bullet;
    u32 l = slen(e);
    if (!l || l + 9 > room) return false;
    copy(out, e, l);
    copy(out + l, ".library", 9);
    return true;
}

/* One glyph as the engine drew it */
struct Glyph {
    const odf_GlyphMap *gm;        /* NULL: none */
    s32 adv, left;                 /* advance and black box's left, from the origin */
    u32 wid;                       /* black width */
};

/* An outline font at one size (from #74): the engine draws characters 32
 * to 255 at the request's TA_DeviceDPI, else DFCTRL_XDPI and _YDPI, the
 * point height being the YSize at that vertical DPI (72 points to the
 * inch), the dot size DFCTRL_XDOTP and _YDOTP. Each glyph's black box is
 * packed in order (its width the box's), tf_CharKern its left from the
 * origin, tf_CharSpace the rest of its advance (at least 0); one without
 * a map is 0 wide and advances 0. The baseline is under the tallest
 * ascent; the descent below it gets what room is left, the ascent the
 * rest when there is none. The last glyph (characters outside 32..255) is
 * a column one pixel wide, the ascent high, advancing half the widest
 * advance. ABI test: all of it, and that the font is plain (tf_Style 0,
 * FSF_TAGGED with the request's DPI), FPF_PROPORTIONAL | FPF_DISKFONT,
 * BoldSmear 1, XSize the widest advance. */
DFH *engine_open(odf_ctx *c, void *dir, const char *fontname, const odf_TTextAttr *ta)
{
    constexpr u32 LO = 32, HI = 255, N = HI - LO + 1;
    const u16 ysize = ta->tta_YSize;
    if (!ysize || !c->env->lib_open) return nullptr;
    TagItem *ot = otag_in(c, dir, fontname);
    if (!ot) return nullptr;
    char lname[48], path[ODF_MAXFONTPATH + 8];
    void *lib = nullptr;
    odf_GlyphEngine *ge = nullptr;
    Glyph *g = nullptr;
    DFH *d = nullptr;
    const u32 rdpi = (ta->tta_Style & ODF_FSF_TAGGED) ? dpi_of(ta->tta_Tags, true) : 0;
    const u32 dpi = rdpi ? rdpi : (u32(c->ctrl[0]) << 16) | (u32(c->ctrl[1]) & 0xffff);
    const u32 ydpi = (dpi & 0xffff) ? (dpi & 0xffff) : 72;
    TagItem t[4];

    if (!engine_lib(ot, lname, sizeof lname) || !otag_path(ta->tta_Name, path, sizeof path)) goto out;
    if (!(lib = c->env->lib_open(c, lname))) goto out;
    if (!(ge = reinterpret_cast<odf_GlyphEngine *>(ecall(c, lib, ODF_LVO_OpenEngine, nullptr, nullptr)))) goto out;
    t[0] = {ODF_OT_OTagPath, reinterpret_cast<u32>(path)};
    t[1] = {ODF_OT_OTagList, reinterpret_cast<u32>(ot)};
    t[2] = {ODF_TAG_DONE, 0};
    if (ecall(c, lib, ODF_LVO_SetInfoA, ge, t) != u32(ODF_OTERR_Success)) goto out;
    t[0] = {ODF_OT_DeviceDPI, dpi};
    t[1] = {ODF_OT_PointHeight, u32((u64(ysize) * 72 << 16) / ydpi)};
    t[2] = {ODF_OT_DotSize, (u32(c->ctrl[2]) << 16) | (u32(c->ctrl[3]) & 0xffff)};
    t[3] = {ODF_TAG_DONE, 0};
    if (ecall(c, lib, ODF_LVO_SetInfoA, ge, t) != u32(ODF_OTERR_Success)) goto out;
    if (!(g = static_cast<Glyph *>(alloc(c, N * sizeof(Glyph))))) goto out;

    {
        s32 top = 0, bottom = 0, maxadv = 0;
        u32 bits = 1;                                   /* the last glyph's column */
        for (u32 i = 0; i < N; i++) {
            odf_GlyphMap *gm = nullptr;
            t[0] = {ODF_OT_GlyphCode, LO + i};
            t[1] = {ODF_TAG_DONE, 0};
            if (ecall(c, lib, ODF_LVO_SetInfoA, ge, t) != u32(ODF_OTERR_Success)) continue;
            t[0] = {ODF_OT_GlyphMap, reinterpret_cast<u32>(&gm)};
            if (ecall(c, lib, ODF_LVO_ObtainInfoA, ge, t) != u32(ODF_OTERR_Success) || !gm) continue;
            g[i].gm = gm;
            /* a black box outside the map is drawn as nothing */
            if (u32(gm->glm_BlackLeft) + gm->glm_BlackWidth > u32(gm->glm_BMModulo) * 8 ||
                u32(gm->glm_BlackTop) + gm->glm_BlackHeight > gm->glm_BMRows)
                continue;
            g[i].adv = s32(gm->glm_X1) - gm->glm_X0;
            g[i].left = s32(gm->glm_BlackLeft) - gm->glm_X0;
            g[i].wid = gm->glm_BlackWidth;
            s32 up = s32(gm->glm_Y0) - gm->glm_BlackTop;
            s32 down = s32(gm->glm_BlackTop) + gm->glm_BlackHeight - gm->glm_Y0;
            if (up > top) top = up;
            if (down > bottom) bottom = down;
            if (g[i].adv > maxadv) maxadv = g[i].adv;
            bits += g[i].wid;
        }
        s32 ascent = top, descent = bottom;
        if (ascent + descent > s32(ysize)) {
            descent -= ascent + descent - s32(ysize);
            if (descent < 0) { ascent += descent; descent = 0; }
        }
        if (ascent < 1) ascent = ysize;
        if (bits > 0xffff) goto out;              /* a glyph's offset is 16 bits */

        const u32 nglyph = N + 1, modulo = ((bits + 15) >> 4) << 1;
        u32 o = sizeof(DFH);
        const u32 o_tags = o;   if (rdpi) o += 2 * sizeof(TagItem);
        const u32 o_loc = o;    o += nglyph * 4;
        const u32 o_space = o;  o += nglyph * 2;
        const u32 o_kern = o;   o += nglyph * 2;
        const u32 o_data = o;   o += modulo * ysize;
        u8 *m = static_cast<u8 *>(alloc(c, o));
        if (!m) goto out;
        d = reinterpret_cast<DFH *>(m);
        TextFont *tf = &d->dfh_TF;
        auto *loc = reinterpret_cast<u32 *>(m + o_loc);
        auto *sp = reinterpret_cast<s16 *>(m + o_space);
        auto *kn = reinterpret_cast<s16 *>(m + o_kern);
        u8 *data = m + o_data;
        u32 x = 0;
        for (u32 i = 0; i < N; i++) {
            const Glyph &k = g[i];
            loc[i] = (x << 16) | k.wid;
            sp[i] = s16(k.adv - k.left > 0 ? k.adv - k.left : 0);
            kn[i] = s16(k.left);
            const odf_GlyphMap *gm = k.gm;
            for (u32 y = 0; gm && k.wid && y < gm->glm_BlackHeight; y++) {
                s32 dy = ascent - (s32(gm->glm_Y0) - gm->glm_BlackTop) + s32(y);
                if (dy < 0 || dy >= s32(ysize)) continue;
                for (u32 sx = 0; sx < k.wid; sx++)
                    if (get_bit(gm->glm_BitMap, gm->glm_BMModulo, gm->glm_BlackLeft + sx, gm->glm_BlackTop + y))
                        set_bit(data, modulo, x + sx, u32(dy));
            }
            x += k.wid;
        }
        loc[N] = (x << 16) | 1;
        sp[N] = s16(maxadv > 1 ? maxadv >> 1 : 1);
        for (u32 y = 0; y < u32(ascent) && y < ysize; y++) set_bit(data, modulo, x, y);

        d->dfh_FileID = ODF_DFH_ID;
        set_name(d, ta->tta_Name);
        tf->tf_Message.mn_Length = sizeof(TextFont);
        tf->tf_YSize = ysize;
        tf->tf_Style = 0;
        tf->tf_Flags = ODF_FPF_PROPORTIONAL | ODF_FPF_DISKFONT;
        tf->tf_XSize = u16(maxadv > 0 ? maxadv : 1);
        tf->tf_Baseline = u16(ascent > 0 ? ascent - 1 : 0);
        tf->tf_BoldSmear = 1;
        tf->tf_LoChar = u8(LO);
        tf->tf_HiChar = u8(HI);
        tf->tf_Modulo = u16(modulo);
        tf->tf_CharData = data;
        tf->tf_CharLoc = loc;
        tf->tf_CharSpace = sp;
        tf->tf_CharKern = kn;
        if (rdpi) {
            auto *tg = reinterpret_cast<TagItem *>(m + o_tags);
            tg[0] = {ODF_TA_DeviceDPI, rdpi};
            tg[1] = {ODF_TAG_DONE, 0};
            tf->tf_Style |= ODF_FSF_TAGGED;
            d->dfh_Segment = s32(reinterpret_cast<u32>(tg));     /* dfh_TagList */
        }
    }

out:
    if (g) {
        for (u32 i = 0; i < N; i++)
            if (g[i].gm) {
                t[0] = {ODF_OT_GlyphMap, reinterpret_cast<u32>(g[i].gm)};
                t[1] = {ODF_TAG_DONE, 0};
                ecall(c, lib, ODF_LVO_ReleaseInfoA, ge, t);
            }
        release(c, g);
    }
    if (ge) ecall(c, lib, ODF_LVO_CloseEngine, ge, nullptr);
    if (lib) c->env->lib_close(c, lib);
    release(c, ot);
    return d;
}

/* ---- choosing ------------------------------------------------------------------------------ */

struct Cand {
    TextFont *mem;                 /* a font in memory, or NULL for contents entry idx */
    u16 idx;
    u16 ysize;
    u8 style, flags;
    u32 dpi;
};

/* The candidate the rules prefer for scaling from (or, unscaled, when no
 * size matches), or -1. ABI test: the order of preference. */
int pick_near(odf_ctx *c, const Vec<Cand> &cs, const Request &r, bool sources_only)
{
    int best = -1;
    u32 bw = 0, bd = 0;
    for (u32 i = 0; i < cs.size(); i++) {
        const Cand &k = cs[i];
        if (sources_only && k.mem && !(k.flags & (ODF_FPF_DESIGNED | ODF_FPF_ROMFONT))) continue;
        if (!k.ysize) continue;
        u32 w = weigh_env(c, r, r.y, k.style, k.flags, k.dpi);
        u32 dist = k.ysize > r.y ? k.ysize - r.y : r.y - k.ysize;
        bool better = best < 0 || w > bw || (w == bw && dist < bd) ||
                      (w == bw && dist == bd && k.ysize > cs[u32(best)].ysize);
        if (better) { best = int(i); bw = w; bd = dist; }
    }
    return best;
}

}  // namespace

/* ---- the calls ----------------------------------------------------------------------------- */

extern "C" {

const odf_outline odf_engine_outline = {engine_open};

void odf_Init(odf_ctx *c, const odf_env *env, void *user)
{
    c->env = env;
    c->user = user;
    c->outline = &odf_engine_outline;
    c->loaded = nullptr;
    c->busy = 0;
    /* ABI test: OS 3.2's defaults */
    c->ctrl[0] = 72;                       /* XDPI */
    c->ctrl[1] = 72;                       /* YDPI */
    c->ctrl[2] = 100;                      /* XDOTP */
    c->ctrl[3] = 100;                      /* YDOTP */
    c->ctrl[4] = 1;                        /* CACHE */
    c->ctrl[5] = ODF_DFCTRL_SORT_OFF;      /* SORTMODE */
    c->ctrl[6] = 0;                        /* CACHEFLUSH */
    c->ctrl[7] = 0;                        /* CHARSET */
}

DFH *odf_FontHeader(u8 *first)
{
    if (!first) return nullptr;
    /* ABI test: whether OS 3.2 checks the return code (MOVEQ #-1,D0; RTS) */
    DFH *d = reinterpret_cast<DFH *>(first + 4);
    if (d->dfh_FileID != ODF_DFH_ID) return nullptr;
    const TextFont *tf = &d->dfh_TF;
    if (!tf->tf_YSize || tf->tf_HiChar < tf->tf_LoChar || !tf->tf_CharLoc) return nullptr;
    return d;
}

u32 odf_Weigh(u16 ry, u8 rs, u8 rf, u32 rdpi, u16 fy, u8 fs, u8 ff, u32 fdpi)
{
    return weigh(ry, rs, rf, rdpi, fy, fs, ff, fdpi);
}

u32 odf_FontDPI(const TextFont *tf)
{
    const odf_TextFontExtension *e = extension(tf);
    return e ? dpi_of(e->tfe_Tags, true) : 0;
}

int odf_FindTag(const TagItem *tags, u32 tag, u32 *data)
{
    return find_tag(tags, true, tag, data);
}

int odf_Pixel(const TextFont *tf, unsigned plane, unsigned glyph, unsigned x, unsigned y)
{
    const void *data = tf->tf_CharData;
    if (tf->tf_Style & ODF_FSF_COLORFONT)
        data = plane < 8 ? reinterpret_cast<const odf_ColorTextFont *>(tf)->ctf_CharData[plane] : nullptr;
    else if (plane)
        data = nullptr;
    if (!data || glyph > u32(tf->tf_HiChar - tf->tf_LoChar) + 1 || y >= tf->tf_YSize) return 0;
    u32 loc = static_cast<const u32 *>(tf->tf_CharLoc)[glyph];
    if (x >= (loc & 0xffff)) return 0;
    return get_bit(static_cast<const u8 *>(data), tf->tf_Modulo, (loc >> 16) + x, y);
}

TextFont *odf_OpenDiskFont(odf_ctx *c, const odf_TextAttr *ta)
{
    if (!ta || !ta->ta_Name) return nullptr;
    Request r{ta->ta_Name, ta->ta_YSize, ta->ta_Style, ta->ta_Flags, 0};
    if (r.s & ODF_FSF_TAGGED)
        r.dpi = dpi_of(reinterpret_cast<const odf_TTextAttr *>(ta)->tta_Tags, true);
    const char *base = basename(ta->ta_Name);

    Locked held(c);
    Vec<Cand> cs(c);
    struct Walk { Vec<Cand> *cs; const char *base; } walk{&cs, base};
    c->env->mem_fonts(c, [](void *d, TextFont *tf) -> int {
        auto *w = static_cast<Walk *>(d);
        const char *n = tf->tf_Message.mn_Node.ln_Name;
        if (n && !(tf->tf_Flags & ODF_FPF_REMOVED) && ieq(n, w->base))
            w->cs->push(Cand{tf, 0, tf->tf_YSize, tf->tf_Style, tf->tf_Flags, odf_FontDPI(tf)});
        return 0;
    }, &walk);
    FontFile ff(c);
    if (ff.find(ta->ta_Name)) {
        for (u16 i = 0; i < ff.cont.n; i++) {
            Entry e;
            if (ff.cont.entry(i, e))
                cs.push(Cand{nullptr, i, e.ysize, e.style, e.flags, dpi_of(e.tags, false)});
        }
    }
    if (!cs.ok()) return nullptr;

    auto open = [&](const Cand &k) -> TextFont * {
        if (k.mem) return c->env->open_font(c, k.mem);
        Entry e;
        if (!ff.cont.entry(k.idx, e)) return nullptr;
        return load_entry(c, ff.dir(), e, ta->ta_Name);
    };

    /* the heaviest at the size; the earliest on a tie */
    int best = -1;
    u32 bw = 0;
    for (u32 i = 0; i < cs.size(); i++) {
        u32 w = weigh_env(c, r, cs[i].ysize, cs[i].style, cs[i].flags, cs[i].dpi);
        if (w > bw) { bw = w; best = int(i); }
    }
    if (best >= 0) return open(cs[u32(best)]);

    /* An outline font draws the size (not with FPF_DESIGNED: ABI test;
     * #74 drew it then too). ABI test: OS 3.2 also honours DFCTRL_CACHE. */
    if (r.y && !(r.f & ODF_FPF_DESIGNED)) {
        if (c->outline && c->outline->outline_open && ff.dir() && ff.cont.id == ODF_OFCH_ID) {
            odf_TTextAttr tta{ta->ta_Name, ta->ta_YSize, ta->ta_Style, ta->ta_Flags,
                              (r.s & ODF_FSF_TAGGED) ? reinterpret_cast<const odf_TTextAttr *>(ta)->tta_Tags
                                                     : nullptr};
            if (DFH *d = c->outline->outline_open(c, ff.dir(), base, &tta)) {
                set_name(d, ta->ta_Name);
                install(c, d, 0);
                return c->env->open_font(c, &d->dfh_TF);
            }
        }
        int src = pick_near(c, cs, r, true);
        if (src >= 0) {
            if (TextFont *s = open(cs[u32(src)])) {
                DFH *d = scale_font(c, s, r.y, r.dpi);
                c->env->close_font(c, s);
                if (d) {
                    install(c, d, 0);
                    return c->env->open_font(c, &d->dfh_TF);
                }
            }
        }
    }
    int near = pick_near(c, cs, r, false);
    return near >= 0 ? open(cs[u32(near)]) : nullptr;
}

DFH *odf_NewScaledDiskFont(odf_ctx *c, TextFont *src, const odf_TextAttr *dest)
{
    if (!src || !dest) return nullptr;
    u32 dpi = (dest->ta_Style & ODF_FSF_TAGGED)
                  ? dpi_of(reinterpret_cast<const odf_TTextAttr *>(dest)->tta_Tags, true) : 0;
    return scale_font(c, src, dest->ta_YSize, dpi);
}

int odf_Flush(odf_ctx *c)
{
    Locked held(c);
    if (c->busy > 1) return c->loaded == nullptr;   /* inside one of our calls: from #74 */
    DFH **link = &c->loaded;
    while (DFH *d = *link) {
        if (!c->env->rem_font(c, &d->dfh_TF)) {
            link = reinterpret_cast<DFH **>(&d->dfh_DF.ln_Succ);
            continue;
        }
        *link = reinterpret_cast<DFH *>(d->dfh_DF.ln_Succ);
        c->env->strip_font(c, &d->dfh_TF);
        if (d->dfh_Segment) c->env->seg_unload(c, reinterpret_cast<void *>(d->dfh_Segment));
        else release(c, d);
    }
    return c->loaded == nullptr;
}

/* ---- AvailFonts -------------------------------------------------------------------------- */

namespace {

struct Rec {
    u16 type, ysize;
    u8 style, flags;
    u32 name;                      /* offset in the names */
    u32 tag, ntags;                /* in the tags, TAG_DONE included; 0 for none */
};

struct Avail {
    odf_ctx *c;
    u32 flags;
    Vec<Rec> recs;
    Vec<char> names;
    Vec<TagItem> tags;
    Vec<u32> seen;                 /* the .font files already listed, by name offset */

    explicit Avail(odf_ctx *c_, u32 f) : c(c_), flags(f), recs(c_), names(c_), tags(c_), seen(c_) {}

    u32 add_name(const char *n)
    {
        u32 at = names.size();
        names.append(n, slen(n) + 1);
        return at;
    }
    void add(u16 type, u16 y, u8 s, u8 f, u32 name, const TagItem *t, bool more, u32 nt)
    {
        Rec r{type, y, s, f, name, 0, 0};
        if ((flags & ODF_AFF_TAGGED) && t) {
            r.tag = tags.size();
            u32 k = 0;
            walk_tags(t, more, [&](const TagItem &ti) {
                if (k + 1 >= nt) return false;
                tags.push(ti);
                k++;
                return true;
            });
            tags.push(TagItem{ODF_TAG_DONE, 0});   /* ABI test: TAG_DONE's ti_Data */
            r.ntags = k + 1;
        }
        recs.push(r);
    }
    void font_file(void *dir, const char *name)
    {
        for (u32 i = 0; i < seen.size(); i++)
            if (ieq(&names[seen[i]], name)) return;      /* ABI test: a name twice in FONTS: */
        FontFile ff(c);
        if (!ff.load_in(dir, name)) return;
        const bool outline = ff.cont.id == ODF_OFCH_ID;
        /* ABI test: AFF_BITMAP, AFF_OTAG (as #74: both given, no disk font) */
        if ((flags & ODF_AFF_BITMAP) && outline) return;
        if ((flags & ODF_AFF_OTAG) && !outline) return;
        const u16 type = u16(ODF_AFF_DISK | ((flags & ODF_AFF_TYPE) ? (outline ? ODF_AFF_OTAG : ODF_AFF_BITMAP) : 0));
        u32 n = add_name(name);
        seen.push(n);
        for (u16 i = 0; i < ff.cont.n; i++) {
            Entry e;
            if (ff.cont.entry(i, e)) add(type, e.ysize, e.style, e.flags, n, e.tags, false, e.ntags);
        }
        /* an outline font scales to any size: with AFF_SCALED, an entry for
         * each size its .otag's OT_AvailSizes names (from #74; ABI test) */
        if (outline && (flags & ODF_AFF_SCALED)) {
            if (TagItem *ot = otag_in(c, dir, name)) {
                const u16 *sz = nullptr;
                u32 k = otag_sizes(ot, &sz);
                for (u32 i = 0; i < k; i++)
                    add(u16(type | ODF_AFF_SCALED), sz[i], 0, ODF_FPF_DISKFONT, n, nullptr, false, 0);
                release(c, ot);
            }
        }
    }
};

int rec_cmp(const Avail &a, const Rec &x, const Rec &y)
{
    int k = icmp(&a.names[x.name], &a.names[y.name]);
    if (k) return k;
    if (x.ysize != y.ysize) return x.ysize < y.ysize ? -1 : 1;
    if (x.style != y.style) return x.style < y.style ? -1 : 1;
    if (x.flags != y.flags) return x.flags < y.flags ? -1 : 1;
    if (x.type != y.type) return x.type < y.type ? -1 : 1;
    return 0;
}

}  // namespace

/* The buffer: the AvailFontsHeader, the entries, (with AFF_TAGGED) the
 * tags, then the names. An entry whose name is the same as the entry's
 * before it shares that string. Too small: nothing is written and the
 * answer is the number of bytes more that are needed. ABI test: both. */
s32 odf_AvailFonts(odf_ctx *c, void *buf, s32 bytes, u32 flags)
{
    Avail a(c, flags);
    {
        Locked held(c);
        if (flags & ODF_AFF_MEMORY) {
            c->env->mem_fonts(c, [](void *d, TextFont *tf) -> int {
                auto *av = static_cast<Avail *>(d);
                const char *n = tf->tf_Message.mn_Node.ln_Name;
                if (!n || (tf->tf_Flags & ODF_FPF_REMOVED)) return 0;
                u16 type = ODF_AFF_MEMORY;
                if (!(tf->tf_Flags & (ODF_FPF_DESIGNED | ODF_FPF_ROMFONT))) {   /* scaled; ABI test */
                    if (!(av->flags & ODF_AFF_SCALED)) return 0;
                    type |= ODF_AFF_SCALED;
                }
                const odf_TextFontExtension *e = extension(tf);
                av->add(type, tf->tf_YSize, tf->tf_Style, tf->tf_Flags, av->add_name(n),
                        e ? e->tfe_Tags : nullptr, true, MAXTAGS);
                return 0;
            }, &a);
        }
    }
    if (flags & ODF_AFF_DISK) {
        c->env->fonts_scan(c, [](void *d, void *dir) -> int {
            auto *av = static_cast<Avail *>(d);
            struct Each { Avail *av; void *dir; } each{av, dir};
            av->c->env->dir_scan(av->c, dir, [](void *d2, const char *name, int is_dir) -> int {
                auto *e = static_cast<Each *>(d2);
                if (!is_dir && has_font_suffix(name)) e->av->font_file(e->dir, name);
                return 0;
            }, &each);
            return 0;
        }, &a);
    }
    if (!a.recs.ok() || !a.names.ok() || !a.tags.ok() || !a.seen.ok()) return bytes > 0 ? bytes : 1;

    /* DFCTRL_SORTMODE: a stable insertion sort */
    s32 sort = c->ctrl[5];
    if (sort) {
        for (u32 i = 1; i < a.recs.size(); i++) {
            Rec r = a.recs[i];
            u32 j = i;
            while (j > 0 && rec_cmp(a, a.recs[j - 1], r) * sort > 0) {
                a.recs[j] = a.recs[j - 1];
                j--;
            }
            a.recs[j] = r;
        }
    }

    const bool tagged = flags & ODF_AFF_TAGGED;
    const u32 n = a.recs.size();
    const u32 esz = tagged ? sizeof(odf_TAvailFonts) : sizeof(struct odf_AvailFonts);
    u32 need = 2 + n * esz;
    const u32 o_tags = need;
    if (tagged)
        for (u32 i = 0; i < n; i++) need += a.recs[i].ntags * sizeof(TagItem);
    const u32 o_names = need;
    for (u32 i = 0; i < n; i++)
        if (!i || !seq(&a.names[a.recs[i].name], &a.names[a.recs[i - 1].name]))
            need += slen(&a.names[a.recs[i].name]) + 1;
    if (bytes < 0 || u32(bytes) < need) return s32(need - (bytes > 0 ? u32(bytes) : 0));

    u8 *b = static_cast<u8 *>(buf);
    reinterpret_cast<odf_AvailFontsHeader *>(b)->afh_NumEntries = u16(n);
    u32 ot = o_tags, on = o_names, last = 0;
    for (u32 i = 0; i < n; i++) {
        const Rec &r = a.recs[i];
        const char *nm = &a.names[r.name];
        if (!i || !seq(nm, &a.names[a.recs[i - 1].name])) {
            u32 l = slen(nm) + 1;
            copy(b + on, nm, l);
            last = on;
            on += l;
        }
        odf_TTextAttr *at;
        if (tagged) {
            auto *e = reinterpret_cast<odf_TAvailFonts *>(b + 2 + i * esz);
            e->taf_Type = r.type;
            at = &e->taf_Attr;
            at->tta_Tags = nullptr;
            if (r.ntags) {
                copy(b + ot, &a.tags[r.tag], r.ntags * sizeof(TagItem));
                at->tta_Tags = reinterpret_cast<TagItem *>(b + ot);
                ot += r.ntags * sizeof(TagItem);
            }
        } else {
            auto *e = reinterpret_cast<struct odf_AvailFonts *>(b + 2 + i * esz);
            e->af_Type = r.type;
            at = reinterpret_cast<odf_TTextAttr *>(&e->af_Attr);
        }
        at->tta_Name = reinterpret_cast<char *>(b + last);
        at->tta_YSize = r.ysize;
        at->tta_Style = r.style;
        at->tta_Flags = r.flags;
    }
    return 0;
}


/* ---- NewFontContents, DisposeFontContents, WriteFontContents ------------------------------- */

namespace {

struct Found {
    u32 file;                      /* offset in the names: "drawer/file" */
    u16 ysize;
    u8 style, flags;
    u32 tag, ntags;                /* in the tags, TAG_DONE included; 0 for none */
};

struct Scan {
    odf_ctx *c;
    void *dir;
    const char *drawer;
    u32 dl;
    Vec<Found> found;
    Vec<char> names;
    Vec<TagItem> tags;
    explicit Scan(odf_ctx *c_) : c(c_), dir(nullptr), drawer(nullptr), dl(0), found(c_), names(c_), tags(c_) {}

    void file(const char *name)
    {
        u8 *first = nullptr;
        void *seg = c->env->seg_load(c, dir, name, &first);
        if (!seg) return;
        if (const DFH *d = odf_FontHeader(first)) {
            const TextFont *tf = &d->dfh_TF;
            Found f{names.size(), tf->tf_YSize, tf->tf_Style, tf->tf_Flags, 0, 0};
            names.append(drawer, dl);
            names.push('/');
            names.append(name, slen(name) + 1);
            if ((tf->tf_Style & ODF_FSF_TAGGED) && d->dfh_Segment) {
                f.tag = tags.size();
                walk_tags(reinterpret_cast<const TagItem *>(d->dfh_Segment), false, [&](const TagItem &t) {
                    if (f.ntags >= 30) return false;
                    tags.push(t);
                    f.ntags++;
                    return true;
                });
                tags.push(TagItem{ODF_TAG_DONE, 0});
                f.ntags++;
            }
            found.push(f);
        }
        c->env->seg_unload(c, seg);
    }
};

int bcmp_(const char *a, const char *b)
{
    for (; *a && *a == *b; a++, b++) {}
    return u8(*a) < u8(*b) ? -1 : u8(*a) > u8(*b) ? 1 : 0;
}

int found_cmp(const Scan &s, const Found &x, const Found &y)
{
    if (x.ysize != y.ysize) return x.ysize < y.ysize ? -1 : 1;
    if (x.style != y.style) return x.style < y.style ? -1 : 1;
    if (x.flags != y.flags) return x.flags < y.flags ? -1 : 1;
    return bcmp_(&s.names[x.file], &s.names[y.file]);
}

}  // namespace

/* Every font file in the drawer named after the font ("topaz.font" ->
 * "topaz"), loaded and read: one entry each, sorted by YSize, style,
 * flags and file name; TFCH_ID when any has tags, else FCH_ID, but OFCH_ID
 * when the .font there is an outline font's (from #74: it stays one). A
 * file that isn't a font, or whose name and tags don't fit an entry, is
 * left out. ABI test: the order, and whether OS 3.2 needs the ".font". */
odf_FontContentsHeader *odf_NewFontContents(odf_ctx *c, void *fontsLock, const char *fontName)
{
    if (!fontName) return nullptr;
    u32 l = slen(fontName);
    u32 dl = has_font_suffix(fontName) ? l - 5 : l;
    char drawer[ODF_MAXFONTPATH];
    if (!dl || dl + 2 >= sizeof drawer) return nullptr;
    copy(drawer, fontName, dl);
    drawer[dl] = 0;
    Dir d(c);
    d.set(c->env->dir_lock(c, fontsLock, drawer));
    if (!d.get()) return nullptr;
    Scan s(c);
    s.dir = d.get();
    s.drawer = drawer;
    s.dl = dl;
    c->env->dir_scan(c, d.get(), [](void *p, const char *name, int is_dir) -> int {
        if (!is_dir) static_cast<Scan *>(p)->file(name);
        return 0;
    }, &s);
    if (!s.found.ok() || !s.names.ok() || !s.tags.ok()) return nullptr;
    bool outline = false;
    {
        u32 fl = 0;
        void *fm = c->env->file_load(c, fontsLock, fontName, &fl);
        Contents k;
        outline = fm && k.parse(fm, fl) && k.id == ODF_OFCH_ID;
        release(c, fm);
    }

    for (u32 i = 1; i < s.found.size(); i++) {
        Found f = s.found[i];
        u32 j = i;
        while (j > 0 && found_cmp(s, s.found[j - 1], f) > 0) {
            s.found[j] = s.found[j - 1];
            j--;
        }
        s.found[j] = f;
    }
    u32 n = 0;
    bool tagged = false;
    for (u32 i = 0; i < s.found.size(); i++) {
        const Found &f = s.found[i];
        if (slen(&s.names[f.file]) + 1 > ODF_MAXFONTPATH - 8 * f.ntags) continue;
        n++;
        if (f.ntags) tagged = true;
    }
    auto *h = static_cast<odf_FontContentsHeader *>(alloc(c, 4 + n * FCH_ENTRY));
    if (!h) return nullptr;
    h->fch_FileID = outline ? ODF_OFCH_ID : tagged ? ODF_TFCH_ID : ODF_FCH_ID;
    h->fch_NumEntries = u16(n);
    u8 *p = reinterpret_cast<u8 *>(h) + 4;
    for (u32 i = 0; i < s.found.size(); i++) {
        const Found &f = s.found[i];
        const char *nm = &s.names[f.file];
        u32 nl = slen(nm) + 1;
        if (nl > ODF_MAXFONTPATH - 8 * f.ntags) continue;
        auto *e = reinterpret_cast<odf_TFontContents *>(p);
        copy(e->tfc_FileName, nm, nl);
        if (f.ntags) {
            copy(p + ODF_MAXFONTPATH - 8 * f.ntags, &s.tags[f.tag], 8 * f.ntags);
            e->tfc_TagCount = u16(f.ntags);       /* TAG_DONE's ti_Data's low word */
        }
        e->tfc_YSize = f.ysize;
        e->tfc_Style = f.style;
        e->tfc_Flags = f.flags;
        p += FCH_ENTRY;
    }
    return h;
}

void odf_DisposeFontContents(odf_ctx *c, odf_FontContentsHeader *fch)
{
    release(c, fch);
}

/* The header and its entries, as they are, to fontName in fontsLock.
 * ABI test: the result's values (TRUE here), and the name used as given. */
s32 odf_WriteFontContents(odf_ctx *c, void *fontsLock, const char *fontName, const odf_FontContentsHeader *fch)
{
    if (!fontName || !fch) return 0;
    if (fch->fch_FileID != ODF_FCH_ID && fch->fch_FileID != ODF_TFCH_ID && fch->fch_FileID != ODF_OFCH_ID) return 0;
    u32 len = 4 + u32(fch->fch_NumEntries) * FCH_ENTRY;
    return c->env->file_save(c, fontsLock, fontName, fch, len) ? 1 : 0;
}

/* ---- WriteDiskFontHeaderA: a TextFont as a font file ---------------------------------------- */

namespace {

void put16(u8 *p, u32 v) { p[0] = u8(v >> 8); p[1] = u8(v); }
void put32(u8 *p, u32 v) { p[0] = u8(v >> 24); p[1] = u8(v >> 16); p[2] = u8(v >> 8); p[3] = u8(v); }

}  // namespace

/* One code hunk: MOVEQ #-1,D0; RTS, the DiskFontHeader with its TextFont
 * (or ColorTextFont), then the tags, the colours, the glyph planes, the
 * locations, spacing and kerning, every pointer relocated (HUNK_RELOC32).
 * The tags are tags if given and not empty, else the font's own (its
 * extension's). ABI test: OS 3.2's order of the parts and its flags. */
u8 *odf_FontFile(odf_ctx *c, const TextFont *tf, const TagItem *tags, u32 *len)
{
    if (!tf || !tf->tf_YSize || tf->tf_HiChar < tf->tf_LoChar || !tf->tf_CharLoc) return nullptr;
    const bool color = tf->tf_Style & ODF_FSF_COLORFONT;
    const u32 nglyph = u32(tf->tf_HiChar - tf->tf_LoChar) + 2;
    const auto *ct = reinterpret_cast<const odf_ColorTextFont *>(tf);
    const odf_ColorFontColors *cfc = color ? ct->ctf_ColorFontColors : nullptr;
    const u32 ncolors = cfc && cfc->cfc_ColorTable ? cfc->cfc_Count : 0;

    bool own = true;
    if (tags) walk_tags(tags, true, [&](const TagItem &) { own = false; return false; });
    if (own) {
        const odf_TextFontExtension *e = extension(tf);
        tags = e ? e->tfe_Tags : nullptr;
    }
    u32 nt = 0;
    walk_tags(tags, true, [&](const TagItem &) { return ++nt < MAXTAGS; });

    Planes pl;
    pl.gather(tf);
    const u32 psize = align2(u32(tf->tf_Modulo) * tf->tf_YSize);
    const u32 base = 4;                               /* the DiskFontHeader, past the code */
    const u32 tfo = base + 54;
    u32 o = tfo + (color ? sizeof(odf_ColorTextFont) : sizeof(TextFont));
    const u32 o_tags = o;   if (nt) o += (nt + 1) * 8;
    const u32 o_cfc = o;    if (cfc) o += 8 + align2(ncolors * 2);
    const u32 o_data = o;   o += pl.n * psize;
    const u32 o_loc = o;    o += nglyph * 4;
    const u32 o_space = o;  if (tf->tf_CharSpace) o += nglyph * 2;
    const u32 o_kern = o;   if (tf->tf_CharKern) o += nglyph * 2;
    const u32 hsize = (o + 3) & ~3u;

    constexpr u32 MAXRELOC = 24;                      /* 7, a colour font's 2 and 8 planes at most */
    u32 relocs[MAXRELOC], nr = 0;
    const u32 head = 32;                              /* HUNK_HEADER's 6 longs, HUNK_CODE's 2 */
    const u32 total = head + hsize + 4 * (3 + MAXRELOC + 1) + 4;
    u8 *f = static_cast<u8 *>(alloc(c, total));
    if (!f) return nullptr;
    put32(f, ODF_HUNK_HEADER);
    put32(f + 8, 1);                                  /* one hunk, 0 to 0 */
    put32(f + 20, hsize / 4);
    put32(f + 24, ODF_HUNK_CODE);
    put32(f + 28, hsize / 4);
    u8 *h = f + head;
    auto ptr = [&](u32 at, u32 to) { put32(h + at, to); relocs[nr++] = at; };

    put32(h, 0x70FF4E75);                             /* MOVEQ #-1,D0; RTS */
    h[base + 8] = ODF_NT_FONT;
    ptr(base + 10, base + 22);                        /* dfh_DF.ln_Name */
    put16(h + base + 14, ODF_DFH_ID);
    if (nt) ptr(base + 18, o_tags);                   /* dfh_TagList */
    const char *nm = tf->tf_Message.mn_Node.ln_Name ? basename(tf->tf_Message.mn_Node.ln_Name) : "";
    u32 nl = slen(nm);
    copy(h + base + 22, nm, nl < ODF_MAXFONTNAME - 1 ? nl : ODF_MAXFONTNAME - 1);

    h[tfo + 8] = ODF_NT_FONT;
    ptr(tfo + 10, base + 22);
    put16(h + tfo + 18, tf->tf_Message.mn_Length);
    put16(h + tfo + 20, tf->tf_YSize);
    h[tfo + 22] = u8(nt ? tf->tf_Style | ODF_FSF_TAGGED : tf->tf_Style & ~ODF_FSF_TAGGED);
    h[tfo + 23] = u8(tf->tf_Flags & ~ODF_FPF_REMOVED);
    put16(h + tfo + 24, tf->tf_XSize);
    put16(h + tfo + 26, tf->tf_Baseline);
    put16(h + tfo + 28, tf->tf_BoldSmear);
    h[tfo + 32] = tf->tf_LoChar;
    h[tfo + 33] = tf->tf_HiChar;
    if (pl.tf >= 0) ptr(tfo + 34, o_data + u32(pl.tf) * psize);
    put16(h + tfo + 38, tf->tf_Modulo);
    ptr(tfo + 40, o_loc);
    if (tf->tf_CharSpace) ptr(tfo + 44, o_space);
    if (tf->tf_CharKern) ptr(tfo + 48, o_kern);
    if (color) {
        put16(h + tfo + 52, ct->ctf_Flags);
        h[tfo + 54] = ct->ctf_Depth;
        h[tfo + 55] = ct->ctf_FgColor;
        h[tfo + 56] = ct->ctf_Low;
        h[tfo + 57] = ct->ctf_High;
        h[tfo + 58] = ct->ctf_PlanePick;
        h[tfo + 59] = ct->ctf_PlaneOnOff;
        if (cfc) {
            ptr(tfo + 60, o_cfc);
            put16(h + o_cfc, cfc->cfc_Reserved);
            put16(h + o_cfc + 2, cfc->cfc_Count);
            if (ncolors) {
                ptr(o_cfc + 4, o_cfc + 8);
                for (u32 i = 0; i < ncolors; i++) put16(h + o_cfc + 8 + 2 * i, cfc->cfc_ColorTable[i]);
            }
        }
        for (unsigned p = 0; p < 8; p++)
            if (pl.ctf[p] >= 0) ptr(tfo + 64 + 4 * p, o_data + u32(pl.ctf[p]) * psize);
    }
    u32 k = 0;
    walk_tags(tags, true, [&](const TagItem &t) {
        put32(h + o_tags + 8 * k, t.ti_Tag);
        put32(h + o_tags + 8 * k + 4, t.ti_Data);
        return ++k < nt;
    });
    for (unsigned p = 0; p < pl.n; p++)
        copy(h + o_data + p * psize, pl.src[p], u32(tf->tf_Modulo) * tf->tf_YSize);
    auto *loc = static_cast<const u32 *>(tf->tf_CharLoc);
    auto *sp = static_cast<const u16 *>(tf->tf_CharSpace);
    auto *kn = static_cast<const u16 *>(tf->tf_CharKern);
    for (u32 i = 0; i < nglyph; i++) {
        put32(h + o_loc + 4 * i, loc[i]);
        if (sp) put16(h + o_space + 2 * i, sp[i]);
        if (kn) put16(h + o_kern + 2 * i, kn[i]);
    }

    for (u32 i = 1; i < nr; i++) {                    /* ascending offsets */
        u32 r = relocs[i], j = i;
        for (; j > 0 && relocs[j - 1] > r; j--) relocs[j] = relocs[j - 1];
        relocs[j] = r;
    }
    u8 *q = h + hsize;
    put32(q, ODF_HUNK_RELOC32);
    put32(q + 4, nr);
    put32(q + 8, 0);
    q += 12;
    for (u32 i = 0; i < nr; i++, q += 4) put32(q, relocs[i]);
    put32(q, 0);
    put32(q + 4, ODF_HUNK_END);
    *len = u32(q + 8 - f);
    return f;
}

/* ABI test: the result (TRUE when written here) and its tags' meaning */
s32 odf_WriteDiskFontHeaderA(odf_ctx *c, const TextFont *tf, const char *fileName, const TagItem *tags)
{
    if (!fileName) return 0;
    u32 len = 0;
    u8 *f = odf_FontFile(c, tf, tags, &len);
    if (!f) return 0;
    int ok = c->env->file_save(c, nullptr, fileName, f, len);
    release(c, f);
    return ok ? 1 : 0;
}

/* ---- GetDiskFontCtrl, SetDiskFontCtrlA ------------------------------------------------------- */

/* ABI test: an unknown tag's answer (0 here) */
s32 odf_GetDiskFontCtrl(odf_ctx *c, u32 tag)
{
    u32 i = tag - ODF_DFCTRL_BASE - 1;
    return i < ODF_NCTRL ? c->ctrl[i] : 0;
}

void odf_SetDiskFontCtrlA(odf_ctx *c, const TagItem *tags)
{
    walk_tags(tags, true, [&](const TagItem &t) {
        u32 i = t.ti_Tag - ODF_DFCTRL_BASE - 1;
        if (t.ti_Tag == ODF_DFCTRL_CACHEFLUSH) {
            if (t.ti_Data) odf_Flush(c);
        } else if (i < ODF_NCTRL) {
            c->ctrl[i] = s32(t.ti_Data);
        }
        return true;
    });
}

/* ---- the outline calls (from #74) ------------------------------------------------------------ */

TagItem *odf_OTagLoad(odf_ctx *c, void *mem, u32 len)
{
    auto *t = static_cast<TagItem *>(mem);
    if (!mem) return nullptr;
    /* the length is the file's, strings and all: odd is usual (OS 3.2's own
     * CGTimes.otag is 261 bytes), the tags being whole TagItems before it */
    if (len < 16 || t[0].ti_Tag != ODF_OT_FileIdent || t[0].ti_Data != len) {
        release(c, mem);
        return nullptr;
    }
    const u32 n = len / sizeof(TagItem);
    for (u32 i = 0; i < n; i++) {
        if (t[i].ti_Tag == ODF_TAG_DONE) return t;
        if ((t[i].ti_Tag & ODF_TAG_USER) && (t[i].ti_Tag & ODF_OT_Indirect) && t[i].ti_Data < len)
            t[i].ti_Data += reinterpret_cast<u32>(mem);
    }
    release(c, mem);                      /* no TAG_DONE inside it */
    return nullptr;
}

/* The E calls: the engine library's own, through the base and engine the
 * caller keeps in its EGlyphEngine */
odf_GlyphEngine *odf_EOpenEngine(odf_ctx *c, odf_EGlyphEngine *ege)
{
    if (!ege || !ege->ege_BulletBase) return nullptr;
    ege->ege_GlyphEngine =
        reinterpret_cast<odf_GlyphEngine *>(ecall(c, ege->ege_BulletBase, ODF_LVO_OpenEngine, nullptr, nullptr));
    return ege->ege_GlyphEngine;
}

void odf_ECloseEngine(odf_ctx *c, odf_EGlyphEngine *ege)
{
    if (!ege || !ege->ege_BulletBase || !ege->ege_GlyphEngine) return;
    ecall(c, ege->ege_BulletBase, ODF_LVO_CloseEngine, ege->ege_GlyphEngine, nullptr);
    ege->ege_GlyphEngine = nullptr;
}

namespace {
u32 e_info(odf_ctx *c, odf_EGlyphEngine *ege, s32 lvo, const TagItem *tags)
{
    if (!ege || !ege->ege_BulletBase || !ege->ege_GlyphEngine) return u32(ODF_OTERR_Failure);
    return ecall(c, ege->ege_BulletBase, lvo, ege->ege_GlyphEngine, tags);
}
}  // namespace

u32 odf_ESetInfoA(odf_ctx *c, odf_EGlyphEngine *ege, const TagItem *tags)
{
    return e_info(c, ege, ODF_LVO_SetInfoA, tags);
}

u32 odf_EObtainInfoA(odf_ctx *c, odf_EGlyphEngine *ege, const TagItem *tags)
{
    return e_info(c, ege, ODF_LVO_ObtainInfoA, tags);
}

u32 odf_EReleaseInfoA(odf_ctx *c, odf_EGlyphEngine *ege, const TagItem *tags)
{
    return e_info(c, ege, ODF_LVO_ReleaseInfoA, tags);
}

namespace {
/* OpenOutlineFont's list (the autodoc): the engine libraries opened for
 * the caller, one node each, shared by its later opens and closed with
 * the last font that uses it. ABI test: one node after an OFF_OPEN open,
 * none after its close. */
struct ENode { odf_Node n; void *base; u32 users; char name[48]; };

void *engine_get(odf_ctx *c, odf_List *l, const char *lname)
{
    if (!c->env->lib_open) return nullptr;
    if (!l) return c->env->lib_open(c, lname);
    for (odf_Node *n = l->lh_Head; n && n->ln_Succ; n = n->ln_Succ) {
        auto *e = reinterpret_cast<ENode *>(n);
        if (ieq(e->name, lname)) { e->users++; return e->base; }
    }
    void *b = c->env->lib_open(c, lname);
    if (!b) return nullptr;
    auto *e = static_cast<ENode *>(alloc(c, sizeof(ENode)));
    if (!e) { c->env->lib_close(c, b); return nullptr; }
    copy(e->name, lname, slen(lname) + 1);
    e->n.ln_Name = e->name;
    e->base = b;
    e->users = 1;
    e->n.ln_Succ = reinterpret_cast<odf_Node *>(&l->lh_Tail);     /* AddTail */
    e->n.ln_Pred = l->lh_TailPred;
    l->lh_TailPred->ln_Succ = &e->n;
    l->lh_TailPred = &e->n;
    return b;
}

void engine_put(odf_ctx *c, odf_List *l, void *base)
{
    if (!base) return;
    if (l)
        for (odf_Node *n = l->lh_Head; n && n->ln_Succ; n = n->ln_Succ) {
            auto *e = reinterpret_cast<ENode *>(n);
            if (e->base != base) continue;
            if (--e->users) return;
            e->n.ln_Pred->ln_Succ = e->n.ln_Succ;                     /* Remove */
            e->n.ln_Succ->ln_Pred = e->n.ln_Pred;
            release(c, e);
            break;
        }
    c->env->lib_close(c, base);
}
}  // namespace

/* name: the font's ("x.font"), its .otag ("x.otag") or "x", with a path
 * or in FONTS:. The .otag is loaded and its engine named; with OFF_OPEN
 * the engine is opened on it (OT_OTagPath and OT_OTagList). One allocation
 * holds the OutlineFont and its strings; olf_OTagList is the .otag's own.
 * The path is the .otag's full name where the env can give it (OS 3.2's
 * form), and the .otag must name its engine (the autodoc: OT_Engine is
 * validated). list: the engine cache above, with OFF_OPEN. */
odf_OutlineFont *odf_OpenOutlineFont(odf_ctx *c, const char *name, void *list, u32 flags)
{
    if (!name) return nullptr;
    char path[ODF_MAXFONTPATH + 8], lname[48];
    TagItem *ot = otag_find(c, name, path, sizeof path);
    if (!ot) return nullptr;
    if ((!path[0] && !otag_path(name, path, sizeof path)) || !otag_str(ot, ODF_OT_Engine) ||
        !engine_lib(ot, lname, sizeof lname)) {
        release(c, ot);
        return nullptr;
    }
    const u32 pl = slen(path) + 1, ll = slen(lname) + 1, el = ll - 8;
    auto *olf = static_cast<odf_OutlineFont *>(alloc(c, sizeof(odf_OutlineFont) + pl + el + ll));
    if (!olf) { release(c, ot); return nullptr; }
    char *s = reinterpret_cast<char *>(olf + 1);
    copy(s, path, pl);
    olf->olf_OTagPath = s;
    copy(s + pl, lname, el - 1);
    olf->olf_EngineName = s + pl;
    copy(s + pl + el, lname, ll);
    olf->olf_LibraryName = s + pl + el;
    olf->olf_OTagList = ot;
    if (flags & ODF_OFF_OPEN) {
        odf_EGlyphEngine *e = &olf->olf_EEngine;
        e->ege_BulletBase = engine_get(c, static_cast<odf_List *>(list), olf->olf_LibraryName);
        TagItem t[3] = {{ODF_OT_OTagPath, reinterpret_cast<u32>(olf->olf_OTagPath)},
                        {ODF_OT_OTagList, reinterpret_cast<u32>(ot)}, {ODF_TAG_DONE, 0}};
        if (!odf_EOpenEngine(c, e) || odf_ESetInfoA(c, e, t) != u32(ODF_OTERR_Success)) {
            odf_CloseOutlineFont(c, olf, list);
            return nullptr;
        }
    }
    return olf;
}

void odf_CloseOutlineFont(odf_ctx *c, odf_OutlineFont *olf, void *list)
{
    if (!olf) return;
    odf_ECloseEngine(c, &olf->olf_EEngine);
    engine_put(c, static_cast<odf_List *>(list), olf->olf_EEngine.ege_BulletBase);
    release(c, olf->olf_OTagList);
    release(c, olf);
}

/* ---- ObtainCharsetInfo (from #74) -------------------------------------------------------------- */

namespace {

/* ISO-8859-1's table: the Unicode code point of each of its 256 */
struct Map { u32 v[256]; };

constexpr Map latin1_map()
{
    Map m{};
    for (u32 i = 0; i < 256; i++) m.v[i] = i;
    return m;
}

constexpr Map latin1 = latin1_map();

}  // namespace

/* As OS 3.2 answers with no L:CharSets (measured in a lab of Instance-32):
 * it knows ISO-8859-1 (4) alone, by name or MIME name (without case),
 * "ISO-8859-1" being both; every number, known or not, answers ISO-8859-1's
 * name, MIME name and table, DFCS_NEXTNUMBER 0, and any other name 0.
 * Not yet: the sets L:CharSets/ adds (a table file per MIME name). */
u32 odf_ObtainCharsetInfo(odf_ctx *c, u32 known, u32 value, u32 wanted)
{
    static const char latin1_name[] = "ISO-8859-1";
    (void)c;
    if (known == ODF_DFCS_NAME || known == ODF_DFCS_MIMENAME) {
        const char *v = reinterpret_cast<const char *>(value);
        if (!v || !ieq(v, latin1_name)) return 0;
    } else if (known != ODF_DFCS_NUMBER) {
        return 0;
    }
    switch (wanted) {
    case ODF_DFCS_NUMBER: return 4;
    case ODF_DFCS_NEXTNUMBER: return 0;
    case ODF_DFCS_NAME:
    case ODF_DFCS_MIMENAME: return reinterpret_cast<u32>(latin1_name);
    case ODF_DFCS_MAPTABLE: return reinterpret_cast<u32>(latin1.v);
    }
    return 0;
}

} /* extern "C" */
