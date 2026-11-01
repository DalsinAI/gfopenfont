#include "gfopenfont/gfopenfont.h"
#include "openfont_internal.h"
#include "openfont_io.h"

#include <stdlib.h>
#include <string.h>

#include "of_font.h"
#include "of_layout.h"
#include "of_unicode.h"

/* The fonts are read, shaped and drawn by GFOpenFont's own code: the reader
 * and rasteriser (of_font.c, of_cff.c, of_raster.c) and the shaper
 * (of_shape.c, of_layout.c). */

/* A font file read into memory, shared by every face of the context
 * opened from it while one is open: a face found for a character is most
 * often of a file already open (the page's own font), and reading a whole
 * file again from a slow disk is most of what finding it costs. */
typedef struct OFFile {
    struct OFFile *next;
    uint8_t *data;
    size_t size;
    uint32_t users;
    char path[1];
} OFFile;

struct OFContext {
    OFFile *files;
};

struct OFFace {
    OFContext *ctx;
    OFFont font;
    OFPath path;                    /* reused by every raster of this face */
    uint32_t pixel_size;
    uint32_t refs;
    uint8_t *data;                  /* the font file, owned when take was set */
    size_t data_size;
    uint32_t face_index;
    int owns_data;
    OFFile *file;                   /* the shared file its data is, or NULL */
    int have_fingerprint;           /* worked out the first time a cache asks */
    uint64_t source_fingerprint;
    uint32_t nfallbacks;
    OFFace *fallbacks[OF_MAX_FALLBACKS];
    uint32_t walk;                  /* the last walk of the fallback graph to visit it */
};

/* FNV-1a is used only as a stale-cache fingerprint, not as a security hash. */
static uint64_t fingerprint_bytes(const uint8_t *p, size_t n)
{
    uint64_t h = UINT64_C(14695981039346656037);
    size_t i;
    for (i = 0; i < n; ++i) {
        h ^= p[i];
        h *= UINT64_C(1099511628211);
    }
    return h;
}

OFContext *of_context_create(void)
{
    OFContext *ctx = (OFContext *)calloc(1, sizeof(*ctx));
    if (!ctx)
        return NULL;
    return ctx;
}

void of_context_destroy(OFContext *ctx)
{
    if (!ctx)
        return;
    while (ctx->files) {
        OFFile *f = ctx->files;
        ctx->files = f->next;
        free(f->data);
        free(f);
    }
    free(ctx);
}

static void release_file(OFContext *ctx, OFFile *f)
{
    OFFile **p;
    if (--f->users)
        return;
    for (p = &ctx->files; *p; p = &(*p)->next)
        if (*p == f) {
            *p = f->next;
            break;
        }
    free(f->data);
    free(f);
}

/* The file at path, read once while any face of it is open. */
static OFFile *use_file(OFContext *ctx, const char *path, int *error)
{
    OFFile *f;
    size_t n = strlen(path);
    for (f = ctx->files; f; f = f->next)
        if (!strcmp(f->path, path)) {
            f->users++;
            return f;
        }
    f = (OFFile *)malloc(sizeof(*f) + n);
    if (!f) {
        *error = OF_ERR_MEMORY;
        return NULL;
    }
    if ((*error = of_io_read_all(path, &f->data, &f->size)) != OF_OK) {
        free(f);
        return NULL;
    }
    memcpy(f->path, path, n + 1);
    f->users = 1;
    f->next = ctx->files;
    ctx->files = f;
    return f;
}

static OFFace *open_memory(OFContext *ctx, uint8_t *data, size_t size,
                           uint32_t face_index, int take, int *error)
{
    OFFace *face;
    int rc;
    if (!ctx || !data || !size) {
        if (take)
            free(data);
        *error = OF_ERR_ARGUMENT;
        return NULL;
    }
    face = (OFFace *)calloc(1, sizeof(*face));
    if (!face) {
        if (take)
            free(data);
        *error = OF_ERR_MEMORY;
        return NULL;
    }
    face->ctx = ctx;
    face->refs = 1;
    face->data = data;
    face->data_size = size;
    face->face_index = face_index;
    face->owns_data = take;
    of_path_init(&face->path);
    rc = size > 0xFFFFFFFFu ? OF_ERR_FONT : of_font_open(&face->font, data, size, face_index);
    if (rc != OF_OK) {
        of_font_close(&face->font);
        if (take)
            free(data);
        free(face);
        *error = rc;
        return NULL;
    }
    if ((*error = of_face_set_pixel_size(face, 16)) != OF_OK) {
        of_face_close(face);
        return NULL;
    }
    return face;
}

OFFace *of_face_open_memory(OFContext *ctx, uint8_t *data, size_t size,
                            uint32_t face_index, int take)
{
    int error;
    return open_memory(ctx, data, size, face_index, take, &error);
}

OFFace *of_face_open_file(OFContext *ctx, const char *path, uint32_t face_index, int *error)
{
    OFFile *f = NULL;
    int e;
    OFFace *face = NULL;
    if (!ctx || !path)
        e = OF_ERR_ARGUMENT;
    else if ((f = use_file(ctx, path, &e)) != NULL) {
        face = open_memory(ctx, f->data, f->size, face_index, 0, &e);
        if (face)
            face->file = f;
        else
            release_file(ctx, f);
    }
    if (error)
        *error = e;
    return face;
}

OFFace *of_face_open_index(OFContext *ctx, const char *path, uint32_t face_index)
{
    return of_face_open_file(ctx, path, face_index, NULL);
}

OFFace *of_face_open(OFContext *ctx, const char *path)
{
    return of_face_open_index(ctx, path, 0);
}

void of_face_close(OFFace *face)
{
    uint32_t i;
    if (!face || !face->refs)
        return;
    if (--face->refs)
        return;
    for (i = 0; i < face->nfallbacks; ++i)
        of_face_close(face->fallbacks[i]);
    of_font_close(&face->font);
    of_path_free(&face->path);
    if (face->owns_data)
        free(face->data);
    if (face->file)
        release_file(face->ctx, face->file);
    free(face);
}

int of_face_set_pixel_size(OFFace *face, uint32_t pixels)
{
    if (!face || !pixels)
        return OF_ERR_ARGUMENT;
    if (face->pixel_size == pixels)
        return OF_OK;
    if (pixels > 16384)
        return OF_ERR_FONT;
    face->pixel_size = pixels;   /* positions in 26.6, unhinted, as the bitmaps */
    return OF_OK;
}

/* the face as OFFaceInfo tells it, from a font opened in full or for its
 * description only; pixel_size 0 leaves the line metrics 0 */
static void font_info(const OFFont *f, uint32_t pixel_size, OFFaceInfo *info)
{
    unsigned style = of_font_style(f);
    memset(info, 0, sizeof(*info));
    /* the family and style a program shows: the font's own (name IDs 1 and
     * 2), or its typographic ones when it has only those */
    if (!of_font_name(f, 1, info->family, sizeof info->family))
        of_font_name(f, 16, info->family, sizeof info->family);
    if (!of_font_name(f, 2, info->style, sizeof info->style))
        of_font_name(f, 17, info->style, sizeof info->style);
    if (f->os2_version != 0xFFFF) {
        info->weight = f->weight;
        info->width = f->width;
        memcpy(info->unicode_ranges, f->unicode_ranges, sizeof info->unicode_ranges);
    }
    if (!info->weight)
        info->weight = (style & OF_STYLE_BOLD) ? 700 : 400;
    if (!info->width)
        info->width = 5;
    if (style & OF_STYLE_ITALIC)
        info->flags |= OF_FACE_ITALIC;
    if (f->is_fixed_pitch)
        info->flags |= OF_FACE_FIXED;
    if (f->scalable)
        info->flags |= OF_FACE_SCALABLE;
    if (of_font_has_color(f))
        info->flags |= OF_FACE_COLOR;
    info->units_per_em = f->units_per_em;
    info->pixel_size = pixel_size;
    info->num_glyphs = f->num_glyphs;
    info->num_faces = f->num_faces;
    if (pixel_size)
        of_font_line_px(f, pixel_size, &info->ascender_26_6, &info->descender_26_6, &info->height_26_6);
}

int of_face_info(OFFace *face, OFFaceInfo *info)
{
    if (!face || !info)
        return OF_ERR_ARGUMENT;
    font_info(&face->font, face->pixel_size, info);
    return OF_OK;
}

uint32_t of_face_char_index(OFFace *face, uint32_t codepoint)
{
    return face ? of_font_glyph(&face->font, codepoint) : 0;
}

size_t of_face_cmap_ranges(OFFace *face, uint32_t *ranges, size_t cap)
{
    return face ? of_font_cmap_ranges(&face->font, ranges, cap) : 0;
}

/* One walk at a time: the faces are not shared between threads (the
 * library takes its lock around every call). */
static uint32_t walk_count;

/* Whether from's fallbacks, or theirs, lead to to. The graph has no loops
 * (of_face_add_fallback keeps it so), and each face is visited once. */
static int leads_to(OFFace *from, const OFFace *to, uint32_t walk)
{
    uint32_t i;
    if (from == to)
        return 1;
    if (from->walk == walk)
        return 0;
    from->walk = walk;
    for (i = 0; i < from->nfallbacks; ++i)
        if (leads_to(from->fallbacks[i], to, walk))
            return 1;
    return 0;
}

int of_face_add_fallback(OFFace *face, OFFace *fallback)
{
    if (!face || !fallback)
        return OF_ERR_ARGUMENT;
    if (++walk_count == 0)
        walk_count = 1;
    /* No loops, however long: each face holds its fallbacks, so a loop
     * would keep every face in it open after its owners close them. */
    if (leads_to(fallback, face, walk_count))
        return OF_ERR_ARGUMENT;
    if (face->nfallbacks >= OF_MAX_FALLBACKS)
        return OF_ERR_RANGE;
    fallback->refs++;
    face->fallbacks[face->nfallbacks++] = fallback;
    return OF_OK;
}

uint32_t of_face_fallback_count(const OFFace *face)
{
    return face ? face->nfallbacks : 0;
}

OFFace *of_face_fallback(OFFace *face, uint32_t n)
{
    if (!face)
        return NULL;
    if (n == 0)
        return face;
    if (n > face->nfallbacks)
        return NULL;
    /* shaping sets a fallback to the size of the face it serves; another
     * face it serves may have shaped since */
    of_face_set_pixel_size(face->fallbacks[n - 1], face->pixel_size);
    return face->fallbacks[n - 1];
}

/* ---- probing a face for the index: only the tables that describe it ---- */

#define PROBE_TAG(a, b, c, d) (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | ((uint32_t)(c) << 8) | (uint32_t)(d))

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

/* reads count bytes at offset; OF_ERR_FONT when they are past the file's
 * end (the font says so), OF_ERR_IO when the disk gives fewer */
static int read_exact(OFFile *file, uint32_t file_size, uint32_t offset, void *buf, uint32_t count)
{
    if (offset > file_size || count > file_size - offset)
        return OF_ERR_FONT;
    return of_io_read_at(file, offset, buf, count) == count ? OF_OK : OF_ERR_IO;
}

/* The face_index-th face's describing tables read into a small font of its
 * own (a header, a directory, the tables), so a large font (a CJK
 * collection) is never read whole. *faces: how many the file holds. */
static int read_describing_tables(OFFile *file, uint32_t file_size, uint32_t face_index,
                                  uint8_t **out, size_t *out_size, uint32_t *faces)
{
    static const uint32_t wanted[] = {
        PROBE_TAG('h', 'e', 'a', 'd'), PROBE_TAG('h', 'h', 'e', 'a'), PROBE_TAG('m', 'a', 'x', 'p'),
        PROBE_TAG('n', 'a', 'm', 'e'), PROBE_TAG('O', 'S', '/', '2'), PROBE_TAG('p', 'o', 's', 't')
    };
    /* tables whose presence alone says something (outlines, colour): kept
     * in the directory with 4 bytes each */
    static const uint32_t marks[] = {
        PROBE_TAG('g', 'l', 'y', 'f'), PROBE_TAG('C', 'F', 'F', ' '), PROBE_TAG('C', 'F', 'F', '2'),
        PROBE_TAG('C', 'O', 'L', 'R'), PROBE_TAG('C', 'B', 'D', 'T'), PROBE_TAG('s', 'b', 'i', 'x'),
        PROBE_TAG('S', 'V', 'G', ' ')
    };
    uint8_t head[12], *dir = NULL, *font = NULL;
    uint32_t dir_off = 0, n, i, k, keep = 0, at;
    size_t total;
    int rc;

    *out = NULL;
    *faces = 1;
    if ((rc = read_exact(file, file_size, 0, head, 12)) != OF_OK)
        return rc;
    if (be32(head) == PROBE_TAG('t', 't', 'c', 'f')) {
        uint8_t off[4];
        *faces = be32(head + 8);
        if (!*faces || face_index >= *faces)
            return OF_ERR_FONT;
        if ((rc = read_exact(file, file_size, 12 + 4 * face_index, off, 4)) != OF_OK)
            return rc;
        dir_off = be32(off);
        if ((rc = read_exact(file, file_size, dir_off, head, 12)) != OF_OK)
            return rc;
    } else if (face_index) {
        return OF_ERR_FONT;
    }
    n = ((uint32_t)head[4] << 8) | head[5];
    if (!n)
        return OF_ERR_FONT;
    dir = (uint8_t *)malloc(16 * n);
    if (!dir)
        return OF_ERR_MEMORY;
    if ((rc = read_exact(file, file_size, dir_off + 12, dir, 16 * n)) != OF_OK)
        goto done;
    /* what to keep, and how much room */
    total = 12;
    for (i = 0; i < n; ++i) {
        uint32_t tag = be32(dir + 16 * i), len = be32(dir + 16 * i + 12);
        for (k = 0; k < sizeof wanted / sizeof wanted[0]; ++k)
            if (tag == wanted[k]) {
                if (len > file_size) {
                    rc = OF_ERR_FONT;
                    goto done;
                }
                keep++;
                total += 16 + ((len + 3) & ~3u);
            }
        for (k = 0; k < sizeof marks / sizeof marks[0]; ++k)
            if (tag == marks[k] && len) {
                keep++;
                total += 16 + 4;
            }
    }
    font = (uint8_t *)calloc(1, total);
    if (!font) {
        rc = OF_ERR_MEMORY;
        goto done;
    }
    memcpy(font, head, 12);
    font[4] = (uint8_t)(keep >> 8);
    font[5] = (uint8_t)keep;
    at = 12 + 16 * keep;
    k = 0;
    for (i = 0; i < n; ++i) {
        const uint8_t *r = dir + 16 * i;
        uint32_t tag = be32(r), len = be32(r + 12), w, mark = 0, want = 0;
        for (w = 0; w < sizeof wanted / sizeof wanted[0]; ++w)
            if (tag == wanted[w])
                want = 1;
        for (w = 0; w < sizeof marks / sizeof marks[0]; ++w)
            if (tag == marks[w] && len)
                mark = 1;
        if (!want && !mark)
            continue;
        memcpy(font + 12 + 16 * k, r, 8);   /* tag, checksum */
        put32(font + 12 + 16 * k + 8, at);
        if (want) {
            put32(font + 12 + 16 * k + 12, len);
            if (len && (rc = read_exact(file, file_size, be32(r + 8), font + at, len)) != OF_OK)
                goto done;
            at += (len + 3) & ~3u;
        } else {
            put32(font + 12 + 16 * k + 12, 4);
            at += 4;
        }
        k++;
    }
    *out = font;
    *out_size = total;
    font = NULL;
    rc = OF_OK;
done:
    free(dir);
    free(font);
    return rc;
}

int of_probe_face(OFContext *ctx, const char *path, uint32_t face_index, OFFaceInfo *info)
{
    OFFile *file;
    OFFont font;
    uint8_t *data;
    size_t size;
    uint32_t file_size = 0, faces;
    int rc;

    if (!ctx || !path || !info)
        return OF_ERR_ARGUMENT;
    file = of_io_open(path, &file_size);
    if (!file)
        return OF_ERR_IO;
    rc = read_describing_tables(file, file_size, face_index, &data, &size, &faces);
    of_io_close(file);
    if (rc != OF_OK)
        return rc;
    rc = of_font_open_info(&font, data, size, 0);
    if (rc == OF_OK) {
        font_info(&font, 0, info);   /* no size: line metrics 0, as before */
        info->num_faces = faces;
    }
    of_font_close(&font);
    free(data);
    return rc;
}

/* ---- shaping, with fallback by cluster ---- */

typedef struct GlyphVec {
    OFGlyph *g;
    size_t n, cap;
    int failed;
} GlyphVec;

static void vec_push(GlyphVec *v, const OFGlyph *g)
{
    if (v->failed)
        return;
    if (v->n == v->cap) {
        size_t cap = v->cap ? v->cap * 2 : 64;
        OFGlyph *ng = (OFGlyph *)realloc(v->g, cap * sizeof(*ng));
        if (!ng) {
            v->failed = 1;
            return;
        }
        v->g = ng;
        v->cap = cap;
    }
    v->g[v->n++] = *g;
}

/* Shapes text[off, off + len) with face, the whole text as its context, and
 * appends the glyphs, each marked as from face_no. The run's script is
 * worked out the first time (*script OF_SCRIPT_UNKNOWN) and kept; the
 * fallbacks' pieces reuse it, since a piece of a run (an emoji, a mark)
 * can't tell its own. */
static int shape_item(OFFace *face, uint16_t face_no, const char *utf8, size_t bytes,
                      size_t off, size_t len, OFRunProps *props, GlyphVec *out)
{
    OFShBuf b;
    uint32_t i;
    int rc;

    memset(&b, 0, sizeof b);
    rc = of_shape_text(&face->font, face->pixel_size, utf8, bytes, off, len, props, &b);
    if (rc != OF_OK) {
        free(b.g);
        return rc;
    }
    for (i = 0; i < b.n; ++i) {
        OFGlyph g;
        g.glyph_id = b.g[i].gid;
        g.cluster = b.g[i].cluster;
        g.x_advance_26_6 = b.g[i].xa;
        g.y_advance_26_6 = b.g[i].ya;
        g.x_offset_26_6 = b.g[i].xo;
        g.y_offset_26_6 = b.g[i].yo;
        g.face = face_no;
        g.flags = (b.g[i].props & OF_SH_UNSAFE) ? OF_GLYPH_UNSAFE_TO_BREAK : 0;
        vec_push(out, &g);
    }
    free(b.g);
    return out->failed ? OF_ERR_MEMORY : OF_OK;
}

/* Appends glyphs, glyph 0 marked missing. */
static void push_marked(GlyphVec *out, const OFGlyph *g, size_t n)
{
    size_t k;
    for (k = 0; k < n; ++k) {
        OFGlyph x = g[k];
        if (!x.glyph_id)
            x.flags |= OF_GLYPH_MISSING;
        vec_push(out, &x);
    }
}

/* Shapes text[off, off + len) with the chain's level-th face. Every whole
 * cluster that face has no glyph for is shaped again with the next one;
 * what the last face can't show is the first face's own shaping of it (its
 * glyph 0, marked missing). */
static int shape_chain(OFFace *root, uint32_t level, const char *utf8, size_t bytes,
                       size_t off, size_t len, OFRunProps *props, GlyphVec *out)
{
    OFFace *face = level ? root->fallbacks[level - 1] : root;
    GlyphVec tmp;
    unsigned char *missing;
    size_t i, k;
    int rc;

    memset(&tmp, 0, sizeof tmp);
    if (level && of_face_set_pixel_size(face, root->pixel_size) != OF_OK)
        return OF_ERR_FONT;
    rc = shape_item(face, (uint16_t)level, utf8, bytes, off, len, props, &tmp);
    if (rc != OF_OK) {
        free(tmp.g);
        return rc;
    }
    if (!tmp.n) {
        free(tmp.g);
        return OF_OK;
    }
    missing = (unsigned char *)calloc(tmp.n, 1);
    if (!missing) {
        free(tmp.g);
        return OF_ERR_MEMORY;
    }
    /* a cluster is missing when any glyph of it is glyph 0; a cluster's
     * glyphs are next to each other in the run */
    for (i = 0; i < tmp.n; ++i) {
        if (tmp.g[i].glyph_id)
            continue;
        missing[i] = 1;
        for (k = i; k > 0 && tmp.g[k - 1].cluster == tmp.g[i].cluster; --k)
            missing[k - 1] = 1;
        for (k = i + 1; k < tmp.n && tmp.g[k].cluster == tmp.g[i].cluster; ++k)
            missing[k] = 1;
    }
    for (i = 0; i < tmp.n;) {
        size_t j;
        uint32_t cmin, cmax, end;
        if (!missing[i]) {
            vec_push(out, &tmp.g[i]);
            ++i;
            continue;
        }
        for (j = i; j < tmp.n && missing[j]; ++j)
            ;
        cmin = cmax = tmp.g[i].cluster;
        for (k = i; k < j; ++k) {
            if (tmp.g[k].cluster < cmin)
                cmin = tmp.g[k].cluster;
            if (tmp.g[k].cluster > cmax)
                cmax = tmp.g[k].cluster;
        }
        /* the run ends where the next cluster after it begins */
        end = (uint32_t)(off + len);
        for (k = 0; k < tmp.n; ++k)
            if (tmp.g[k].cluster > cmax && tmp.g[k].cluster < end)
                end = tmp.g[k].cluster;
        if (level < root->nfallbacks) {
            rc = shape_chain(root, level + 1, utf8, bytes, cmin, end - cmin, props, out);
            if (rc != OF_OK)
                break;
        } else if (level) {
            /* none of the chain has it: the first face's own glyphs for it,
             * so its glyph numbers, advances and pixels agree */
            GlyphVec first;
            memset(&first, 0, sizeof first);
            rc = shape_item(root, 0, utf8, bytes, cmin, end - cmin, props, &first);
            if (rc == OF_OK)
                push_marked(out, first.g, first.n);
            free(first.g);
            if (rc != OF_OK)
                break;
        } else {
            push_marked(out, tmp.g + i, j - i);
        }
        i = j;
    }
    free(missing);
    free(tmp.g);
    if (rc == OF_OK && out->failed)
        rc = OF_ERR_MEMORY;
    return rc;
}

/* Shapes utf8[off, off + len) with face and its fallbacks into a vector the
 * caller frees. */
static int shape_run(OFFace *face, const char *utf8, size_t bytes, size_t off, size_t len,
                     const OFShapeOptions *options, GlyphVec *v)
{
    OFRunProps props;
    memset(v, 0, sizeof *v);
    memset(&props, 0, sizeof props);
    props.script = OF_SCRIPT_UNKNOWN;
    if (options) {
        if (options->script) {
            unsigned sc = of_script_from_iso(options->script);
            if (sc != OF_SCRIPT_UNKNOWN)
                props.script = sc;
        }
        props.lang_tag = options->language;
        props.direction = options->direction <= OF_DIR_RTL ? options->direction : OF_DIR_AUTO;
        props.features = options->features;
        props.nfeatures = options->features ? options->num_features : 0;
    }
    if (off > bytes)
        return OF_ERR_ARGUMENT;
    if (len > bytes - off)
        len = bytes - off;
    if (!len)
        return OF_OK;
    return shape_chain(face, 0, utf8, bytes, off, len, &props, v);
}

static size_t copy_out(GlyphVec *v, OFGlyph *out, size_t capacity)
{
    size_t n = v->n;
    if (out && capacity)
        memcpy(out, v->g, (n < capacity ? n : capacity) * sizeof(*out));
    free(v->g);
    return n;
}

size_t of_shape_utf8(OFFace *face, const char *utf8, size_t bytes,
                     OFGlyph *out, size_t capacity)
{
    return of_shape(face, utf8, bytes, 0, bytes, NULL, out, capacity);
}

size_t of_shape(OFFace *face, const char *utf8, size_t bytes, size_t off, size_t len,
                const OFShapeOptions *options, OFGlyph *out, size_t capacity)
{
    GlyphVec v;
    if (!face || !utf8 || !bytes)
        return 0;
    if (shape_run(face, utf8, bytes, off, len, options, &v) != OF_OK) {
        free(v.g);
        return 0;
    }
    return copy_out(&v, out, capacity);
}

/* UTF-16 to UTF-8, with where each byte's character starts in the UTF-16
 * (map[byte], map[bytes] = units) and each unit's first byte (back[unit],
 * back[units] = bytes). A lone surrogate is U+FFFD. */
static char *utf16_to_utf8(const uint16_t *t, size_t units, size_t *bytes_out,
                           uint32_t **map_out, uint32_t **back_out)
{
    char *u = (char *)malloc(units * 3 + 1);
    uint32_t *map = (uint32_t *)malloc((units * 3 + 1) * sizeof(uint32_t));
    uint32_t *back = (uint32_t *)malloc((units + 1) * sizeof(uint32_t));
    size_t i = 0, n = 0;
    if (!u || !map || !back) {
        free(u);
        free(map);
        free(back);
        return NULL;
    }
    while (i < units) {
        uint32_t cp = t[i];
        size_t start = i, k, len;
        i++;
        if (cp >= 0xD800 && cp < 0xDC00 && i < units && t[i] >= 0xDC00 && t[i] < 0xE000) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (t[i] - 0xDC00);
            i++;
        } else if (cp >= 0xD800 && cp < 0xE000) {
            cp = 0xFFFD;
        }
        len = cp < 0x80 ? 1 : cp < 0x800 ? 2 : cp < 0x10000 ? 3 : 4;
        switch (len) {
        case 1:
            u[n] = (char)cp;
            break;
        case 2:
            u[n] = (char)(0xC0 | (cp >> 6));
            u[n + 1] = (char)(0x80 | (cp & 0x3F));
            break;
        case 3:
            u[n] = (char)(0xE0 | (cp >> 12));
            u[n + 1] = (char)(0x80 | ((cp >> 6) & 0x3F));
            u[n + 2] = (char)(0x80 | (cp & 0x3F));
            break;
        default:
            u[n] = (char)(0xF0 | (cp >> 18));
            u[n + 1] = (char)(0x80 | ((cp >> 12) & 0x3F));
            u[n + 2] = (char)(0x80 | ((cp >> 6) & 0x3F));
            u[n + 3] = (char)(0x80 | (cp & 0x3F));
            break;
        }
        for (k = 0; k < len; ++k)
            map[n + k] = (uint32_t)start;
        for (k = start; k < i; ++k)
            back[k] = (uint32_t)n;
        n += len;
    }
    map[n] = (uint32_t)units;
    back[units] = (uint32_t)n;
    *bytes_out = n;
    *map_out = map;
    *back_out = back;
    return u;
}

size_t of_shape_utf16(OFFace *face, const uint16_t *text, size_t units, size_t off, size_t len,
                      const OFShapeOptions *options, OFGlyph *out, size_t capacity)
{
    GlyphVec v;
    OFShapeOptions o;
    OFFeature features[OF_MAX_FEATURES];
    uint32_t *map, *back;
    size_t bytes, i, end;
    char *u;
    int rc;
    if (!face || !text || !units || off >= units)
        return 0;
    end = len > units - off ? units : off + len;
    u = utf16_to_utf8(text, units, &bytes, &map, &back);
    if (!u)
        return 0;
    memset(&o, 0, sizeof o);
    if (options) {
        o = *options;
        o.num_features = options->features ? (options->num_features < OF_MAX_FEATURES ? options->num_features : OF_MAX_FEATURES) : 0;
        for (i = 0; i < o.num_features; ++i) {
            features[i] = options->features[i];
            /* the features' ranges in bytes */
            if (!(features[i].start == 0 && features[i].end == OF_FEATURE_END)) {
                features[i].start = features[i].start >= units ? (uint32_t)bytes : back[features[i].start];
                features[i].end = features[i].end >= units ? (uint32_t)bytes : back[features[i].end];
            }
        }
        o.features = features;
    }
    rc = shape_run(face, u, bytes, back[off], back[end] - back[off], &o, &v);
    if (rc == OF_OK)
        for (i = 0; i < v.n; ++i)
            v.g[i].cluster = map[v.g[i].cluster];
    free(u);
    free(map);
    free(back);
    if (rc != OF_OK) {
        free(v.g);
        return 0;
    }
    return copy_out(&v, out, capacity);
}

int64_t of_measure_utf8(OFFace *face, const char *utf8, size_t bytes)
{
    GlyphVec v;
    size_t i;
    int64_t total = 0;
    if (!face || !utf8 || !bytes)
        return 0;
    if (shape_run(face, utf8, bytes, 0, bytes, NULL, &v) != OF_OK) {
        free(v.g);
        return 0;
    }
    for (i = 0; i < v.n; ++i)
        total += v.g[i].x_advance_26_6;
    free(v.g);
    return total;
}

int of_raster_glyph_a8(OFFace *face, uint32_t glyph_id, OFBitmap *out)
{
    int rc;
    int32_t left, top;
    if (!face || !out)
        return OF_ERR_ARGUMENT;
    memset(out, 0, sizeof(*out));
    if (glyph_id >= face->font.num_glyphs)
        return OF_ERR_FONT;
    of_path_reset(&face->path);
    rc = of_font_outline_px(&face->font, glyph_id, face->pixel_size, &face->path);
    if (rc != OF_OK)
        return rc;
    rc = of_raster_a8(&face->path, &out->width, &out->height, &left, &top, &out->pixels);
    if (rc != OF_OK)
        return rc;
    out->left = left;
    out->top = top;
    out->stride = out->width;
    /* the pen moves by the unrounded advance, as the shaping does */
    out->advance_x_26_6 = of_font_advance_px(&face->font, glyph_id, face->pixel_size);
    return OF_OK;
}

int of_raster_glyph_matrix_a8(OFFace *face, uint32_t glyph_id, const int32_t m[4], OFBitmap *out)
{
    int rc;
    int32_t left, top;
    int64_t adv;
    if (!face || !m || !out)
        return OF_ERR_ARGUMENT;
    memset(out, 0, sizeof(*out));
    if (glyph_id >= face->font.num_glyphs)
        return OF_ERR_FONT;
    of_path_reset(&face->path);
    rc = of_font_outline(&face->font, glyph_id, &face->path);
    if (rc != OF_OK)
        return rc;
    if (face->path.failed)
        return OF_ERR_MEMORY;
    rc = of_path_transform(&face->path, face->font.units_per_em, m);
    if (rc != OF_OK)
        return rc;
    rc = of_raster_a8(&face->path, &out->width, &out->height, &left, &top, &out->pixels);
    if (rc != OF_OK)
        return rc;
    out->left = left;
    out->top = top;
    out->stride = out->width;
    adv = (int64_t)of_font_advance_units(&face->font, glyph_id) * m[0];
    /* font units * 16.16 pixels to the em / units per em = 16.16 pixels; to 26.6 */
    out->advance_x_26_6 = (int32_t)(adv / ((int64_t)face->font.units_per_em << 10));
    return OF_OK;
}

int32_t of_face_glyph_advance(OFFace *face, uint32_t glyph_id)
{
    if (!face || glyph_id >= face->font.num_glyphs)
        return -1;
    return (int32_t)of_font_advance_units(&face->font, glyph_id);
}

/* The file and the face in it: two faces of one collection differ. Worked
 * out when a cache first asks, since it reads the whole file: a second on a
 * real 68040 for a large font, and most programs never need it. */
uint64_t of_face_source_fingerprint(const OFFace *face)
{
    OFFace *f = (OFFace *)face;
    if (!f || !f->data)
        return 0;
    if (!f->have_fingerprint) {
        f->source_fingerprint = fingerprint_bytes(f->data, f->data_size) ^
                                ((uint64_t)f->face_index * UINT64_C(0x9E3779B97F4A7C15));
        f->have_fingerprint = 1;
    }
    return f->source_fingerprint;
}

int of_cache_matches_face(const OFCache *cache, const OFFace *face)
{
    uint64_t cache_fp, face_fp;
    if (!cache || !face)
        return 0;
    cache_fp = of_cache_source_fingerprint(cache);
    face_fp = of_face_source_fingerprint(face);
    if (!cache_fp || !face_fp)
        return 0;
    return cache_fp == face_fp;
}

static void free_sizes(OFCacheSizeIn *ins, size_t size_count)
{
    size_t s, i;
    if (!ins)
        return;
    for (s = 0; s < size_count; ++s) {
        if (!ins[s].glyphs)
            continue;
        for (i = 0; i < ins[s].count; ++i)
            free((void *)ins[s].glyphs[i].pixels);
        free((void *)ins[s].glyphs);
    }
    free(ins);
}

int of_compile_cache(OFFace *face, const char *path,
                     const uint16_t *sizes, size_t size_count,
                     uint32_t first_codepoint, uint32_t last_codepoint)
{
    size_t span, s, i;
    OFCacheSizeIn *ins;
    int rc;

    if (!face || !path || !sizes || !size_count || first_codepoint > last_codepoint)
        return OF_ERR_ARGUMENT;
    if (!of_face_source_fingerprint(face))
        return OF_ERR_FONT;
    span = (size_t)last_codepoint - first_codepoint + 1;
    ins = (OFCacheSizeIn *)calloc(size_count, sizeof(*ins));
    if (!ins)
        return OF_ERR_MEMORY;

    for (s = 0; s < size_count; ++s) {
        OFCacheGlyphIn *gs = (OFCacheGlyphIn *)calloc(span, sizeof(*gs));
        if (!gs) {
            free_sizes(ins, size_count);
            return OF_ERR_MEMORY;
        }
        ins[s].glyphs = gs;
        ins[s].pixel_size = sizes[s];
        ins[s].count = span;
        if (of_face_set_pixel_size(face, sizes[s]) != OF_OK) {
            free_sizes(ins, size_count);
            return OF_ERR_FONT;
        }
        for (i = 0; i < span; ++i) {
            uint32_t cp = first_codepoint + (uint32_t)i;
            uint32_t gid = of_font_glyph(&face->font, cp);
            OFBitmap b;
            rc = of_raster_glyph_a8(face, gid, &b);
            if (rc != OF_OK) {
                free_sizes(ins, size_count);
                return rc;
            }
            gs[i].codepoint = cp;
            gs[i].glyph_id = gid;
            gs[i].left = b.left;
            gs[i].top = b.top;
            gs[i].advance_x_26_6 = b.advance_x_26_6;
            gs[i].width = b.width;
            gs[i].height = b.height;
            gs[i].pixels = b.pixels;
        }
    }

    rc = of_cache_write(path, of_face_source_fingerprint(face), first_codepoint,
                        last_codepoint, ins, size_count);
    free_sizes(ins, size_count);
    return rc;
}

const char *of_error_string(int code)
{
    switch (code) {
    case OF_OK: return "ok";
    case OF_ERR_ARGUMENT: return "bad argument";
    case OF_ERR_MEMORY: return "out of memory";
    case OF_ERR_IO: return "I/O error";
    case OF_ERR_FONT: return "font error";
    case OF_ERR_RANGE: return "range error";
    case OF_ERR_BUFFER: return "buffer too small";
    case OF_ERR_CACHE: return "invalid cache";
    case OF_ERR_EXEC: return "executor failed";
    default: return "unknown error";
    }
}

/* ---- what a layout engine (WebCore) asks of a face ---- */

static int32_t units_px(const OFFace *face, int32_t v)
{
    /* font units to 26.6 at the face's size, rounded half away from zero */
    int64_t x = (int64_t)v * (int64_t)face->pixel_size * 64;
    uint32_t upem = face->font.units_per_em ? face->font.units_per_em : 1000;
    return (int32_t)(x >= 0 ? (x + upem / 2) / upem : -((-x + upem / 2) / upem));
}

/* the top of a character's outline in font units, 0 when it has none */
static int32_t glyph_top_units(OFFace *face, uint32_t cp)
{
    uint32_t g = of_font_glyph(&face->font, cp), i;
    int32_t top = 0;
    of_path_reset(&face->path);
    if (!g || of_font_outline(&face->font, g, &face->path) != OF_OK || !face->path.npoints)
        return 0;
    for (i = 0; i < face->path.npoints; ++i)
        if (i == 0 || face->path.y[i] > top)
            top = face->path.y[i];
    return top >> 16;   /* 16.16 units */
}

int of_face_metrics(OFFace *face, OFFaceMetrics *m)
{
    const OFFont *f;
    int32_t a, d, h;
    uint32_t space;
    if (!face || !m)
        return OF_ERR_ARGUMENT;
    f = &face->font;
    memset(m, 0, sizeof *m);
    m->units_per_em = f->units_per_em;
    m->pixel_size = face->pixel_size;
    of_font_line_px(f, face->pixel_size, &a, &d, &h);
    m->ascender = a;
    m->descender = d;
    m->line_gap = h - (a - d);
    if (m->line_gap < 0)
        m->line_gap = 0;
    if (f->os2_version != 0xFFFF && f->os2_version >= 2 && f->os2.len >= 90) {
        m->x_height = units_px(face, of_s16(f->data + f->os2.off + 86));
        m->cap_height = units_px(face, of_s16(f->data + f->os2.off + 88));
    }
    if (!m->x_height)
        m->x_height = units_px(face, glyph_top_units(face, 'x'));
    if (!m->cap_height)
        m->cap_height = units_px(face, glyph_top_units(face, 'H'));
    if (f->post.len >= 12) {
        m->underline_position = units_px(face, of_s16(f->data + f->post.off + 8));
        m->underline_thickness = units_px(face, of_s16(f->data + f->post.off + 10));
    }
    if (f->os2_version != 0xFFFF && f->os2.len >= 30) {
        m->strikeout_thickness = units_px(face, of_s16(f->data + f->os2.off + 26));
        m->strikeout_position = units_px(face, of_s16(f->data + f->os2.off + 28));
    }
    if (f->hhea.len >= 12)
        m->max_advance = units_px(face, (int32_t)of_u16(f->data + f->hhea.off + 10));
    space = of_font_glyph(f, ' ');
    if (space)
        m->space_advance = of_font_advance_px(f, space, face->pixel_size);
    if (of_font_style(f) & OF_STYLE_ITALIC)
        m->flags |= OF_FACE_ITALIC;
    if (f->is_fixed_pitch)
        m->flags |= OF_FACE_FIXED;
    if (f->scalable)
        m->flags |= OF_FACE_SCALABLE;
    if (of_font_has_color(f))
        m->flags |= OF_FACE_COLOR;
    return OF_OK;
}

int32_t of_face_glyph_advance_px(OFFace *face, uint32_t glyph_id)
{
    if (!face || glyph_id >= face->font.num_glyphs)
        return -1;
    return of_font_advance_px(&face->font, glyph_id, face->pixel_size);
}

int of_face_glyph_extents(OFFace *face, uint32_t glyph_id, OFGlyphExtents *e)
{
    int32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    uint32_t i;
    int rc;
    if (!face || !e)
        return OF_ERR_ARGUMENT;
    memset(e, 0, sizeof *e);
    if (glyph_id >= face->font.num_glyphs)
        return OF_ERR_RANGE;
    if (!face->font.scalable) {
        /* a bitmap-only face: the bitmap's box */
        OFBitmap bm;
        if ((rc = of_raster_glyph_a8(face, glyph_id, &bm)) != OF_OK)
            return rc;
        e->x_bearing = bm.left * 64;
        e->y_bearing = bm.top * 64;
        e->width = (int32_t)bm.width * 64;
        e->height = (int32_t)bm.height * 64;
        of_bitmap_release(&bm);
        return OF_OK;
    }
    of_path_reset(&face->path);
    rc = of_font_outline_px(&face->font, glyph_id, face->pixel_size, &face->path);
    if (rc != OF_OK)
        return rc;
    if (face->path.failed)
        return OF_ERR_MEMORY;
    for (i = 0; i < face->path.npoints; ++i) {
        int32_t x = face->path.x[i], y = face->path.y[i];
        if (i == 0 || x < x0)
            x0 = x;
        if (i == 0 || x > x1)
            x1 = x;
        if (i == 0 || y < y0)
            y0 = y;
        if (i == 0 || y > y1)
            y1 = y;
    }
    if (!face->path.npoints)
        return OF_OK;
    e->x_bearing = x0;
    e->y_bearing = y1;
    e->width = x1 - x0;
    e->height = y1 - y0;
    return OF_OK;
}

/* Builds the verbs and points of an outline, counting what does not fit. */
typedef struct OutlineOut {
    uint8_t *verbs;
    size_t verb_cap, nverbs;
    int32_t *points;
    size_t point_cap, npoints;
} OutlineOut;

static void out_verb(OutlineOut *o, uint8_t v)
{
    if (o->verbs && o->nverbs < o->verb_cap)
        o->verbs[o->nverbs] = v;
    o->nverbs++;
}

static void out_point(OutlineOut *o, int32_t x, int32_t y)
{
    if (o->points && o->npoints < o->point_cap) {
        o->points[2 * o->npoints] = x;
        o->points[2 * o->npoints + 1] = y;
    }
    o->npoints++;
}

int of_face_glyph_outline(OFFace *face, uint32_t glyph_id,
                          uint8_t *verbs, size_t verb_capacity,
                          int32_t *points, size_t point_capacity,
                          size_t *nverbs, size_t *npoints)
{
    OutlineOut o;
    const OFPath *p;
    uint32_t c, start = 0;
    int rc;
    if (!face)
        return OF_ERR_ARGUMENT;
    if (glyph_id >= face->font.num_glyphs)
        return OF_ERR_RANGE;
    memset(&o, 0, sizeof o);
    o.verbs = verbs;
    o.verb_cap = verb_capacity;
    o.points = points;
    o.point_cap = point_capacity;
    of_path_reset(&face->path);
    rc = of_font_outline_px(&face->font, glyph_id, face->pixel_size, &face->path);
    if (rc != OF_OK)
        return rc;
    p = &face->path;
    if (p->failed)
        return OF_ERR_MEMORY;
    for (c = 0; c < p->ncontours; ++c) {
        uint32_t end = p->ends[c], n = end + 1 - start, k, first;
        int32_t sx, sy;
        if (end < start || end >= p->npoints) {
            start = end + 1;
            continue;
        }
        /* start on an on-curve point; a contour of quadratic control
         * points only starts between its first two */
        for (first = 0; first < n && p->tag[start + first] != OF_PT_ON; ++first)
            ;
        if (first == n) {
            sx = (p->x[start] + p->x[start + (n > 1 ? 1 : 0)]) / 2;
            sy = (p->y[start] + p->y[start + (n > 1 ? 1 : 0)]) / 2;
            first = 0;
        } else {
            sx = p->x[start + first];
            sy = p->y[start + first];
            first++;
        }
        out_verb(&o, OF_PATH_MOVE);
        out_point(&o, sx, sy);
        for (k = 0; k < n;) {
            uint32_t i = start + (first + k) % n;
            int last = k + 1 == n;
            if (p->tag[i] == OF_PT_ON) {
                if (!(last && p->x[i] == sx && p->y[i] == sy)) {
                    out_verb(&o, OF_PATH_LINE);
                    out_point(&o, p->x[i], p->y[i]);
                }
                k++;
            } else if (p->tag[i] == OF_PT_CUBIC) {
                uint32_t i2 = start + (first + k + 1) % n, i3 = start + (first + k + 2) % n;
                out_verb(&o, OF_PATH_CUBIC);
                out_point(&o, p->x[i], p->y[i]);
                out_point(&o, p->x[i2], p->y[i2]);
                if (k + 2 >= n)
                    out_point(&o, sx, sy);
                else
                    out_point(&o, p->x[i3], p->y[i3]);
                k += 3;
            } else {
                /* a quadratic control point: to the next on-curve point, or
                 * to the midpoint of two control points in a row */
                uint32_t j = start + (first + k + 1) % n;
                int32_t ex, ey;
                if (k + 1 >= n) {
                    ex = sx;
                    ey = sy;
                } else if (p->tag[j] == OF_PT_ON) {
                    ex = p->x[j];
                    ey = p->y[j];
                } else {
                    ex = (p->x[i] + p->x[j]) / 2;
                    ey = (p->y[i] + p->y[j]) / 2;
                }
                out_verb(&o, OF_PATH_QUAD);
                out_point(&o, p->x[i], p->y[i]);
                out_point(&o, ex, ey);
                k += (k + 1 < n && p->tag[j] == OF_PT_ON) ? 2 : 1;
            }
        }
        out_verb(&o, OF_PATH_CLOSE);
        start = end + 1;
    }
    if (nverbs)
        *nverbs = o.nverbs;
    if (npoints)
        *npoints = o.npoints;
    return (o.nverbs > verb_capacity || o.npoints > point_capacity) ? OF_ERR_BUFFER : OF_OK;
}

int of_face_color_png(OFFace *face, uint32_t glyph_id, uint32_t ppem, OFColorPng *out)
{
    if (!face || !out)
        return OF_ERR_ARGUMENT;
    if (glyph_id >= face->font.num_glyphs) {
        memset(out, 0, sizeof *out);
        return OF_ERR_RANGE;
    }
    return of_font_color_png(&face->font, glyph_id, ppem, out);
}

const uint8_t *of_face_table(OFFace *face, uint32_t tag, size_t *length)
{
    const OFFont *f;
    uint32_t n, i;
    if (length)
        *length = 0;
    if (!face)
        return NULL;
    f = &face->font;
    if ((size_t)f->dir + 12 > f->size)
        return NULL;
    n = of_u16(f->data + f->dir + 4);
    for (i = 0; i < n; ++i) {
        const uint8_t *r = f->data + f->dir + 12 + 16 * i;
        uint32_t off, len;
        if ((size_t)(r - f->data) + 16 > f->size)
            return NULL;
        if (of_u32(r) != tag)
            continue;
        off = of_u32(r + 8);
        len = of_u32(r + 12);
        if (off > f->size || len > f->size - off)
            return NULL;
        if (length)
            *length = len;
        return f->data + off;
    }
    return NULL;
}
