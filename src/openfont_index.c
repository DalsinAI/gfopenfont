/* GFOpenFont's font index: see openfont_index.h.
 *
 * Written for the library as much as for the host: no stdio, no sprintf,
 * no locale. Memory comes from malloc, which gfopenfont.library supplies.
 *
 * Copyright (c) 2026 AmiCradle Team. OpenFont, MIT licence (LICENSE). */
#include "openfont_index.h"
#include "openfont_internal.h"
#include "openfont_io.h"

#include <stdlib.h>
#include <string.h>

/* GFOpenFont's own aliases, after the user's. Liberation's families keep the
 * metrics of the fonts they stand in for. */
static const char *const builtin_aliases[] = {
    "sans-serif=DejaVu Sans,Liberation Sans,Noto Sans",
    "sans=DejaVu Sans,Liberation Sans,Noto Sans",
    "system-ui=DejaVu Sans,Liberation Sans",
    "serif=DejaVu Serif,Liberation Serif,Noto Serif",
    "monospace=DejaVu Sans Mono,Liberation Mono,Noto Sans Mono",
    "mono=DejaVu Sans Mono,Liberation Mono",
    "cursive=DejaVu Serif,Liberation Serif",
    "fantasy=DejaVu Sans,Liberation Sans",
    "Arial=Liberation Sans,Arimo",
    "Helvetica=Liberation Sans,Arimo",
    "Helvetica Neue=Liberation Sans,Arimo",
    "Times New Roman=Liberation Serif,Tinos",
    "Times=Liberation Serif,Tinos",
    "Courier New=Liberation Mono,Cousine",
    "Courier=Liberation Mono,Cousine",
    "Verdana=DejaVu Sans",
    "Bitstream Vera Sans=DejaVu Sans",
    "Bitstream Vera Serif=DejaVu Serif",
    "Bitstream Vera Sans Mono=DejaVu Sans Mono",
    NULL
};

/* Droid Sans Fallback last: Chinese, Japanese and Korean (fonts/cjk), a
 * large face opened only for what the others lack */
static const char default_fallbacks[] = "DejaVu Sans,Noto Sans,Liberation Sans,DejaVu Sans Mono,Droid Sans Fallback";

/* ---- small string helpers (ASCII case only, as font names are) ---- */

static int lower(int c)
{
    return (c >= 'A' && c <= 'Z') ? c + 32 : c;
}

static int ci_eqn(const char *a, const char *b, size_t n)
{
    size_t i;
    for (i = 0; i < n; ++i) {
        if (lower((unsigned char)a[i]) != lower((unsigned char)b[i]))
            return 0;
        if (!a[i])
            return 1;
    }
    return 1;
}

static int ci_eq(const char *a, const char *b)
{
    while (*a && *b) {
        if (lower((unsigned char)*a) != lower((unsigned char)*b))
            return 0;
        ++a;
        ++b;
    }
    return *a == *b;
}

static void copy_str(char *out, size_t size, const char *in, size_t n)
{
    if (n >= size)
        n = size - 1;
    memcpy(out, in, n);
    out[n] = 0;
}

static int font_file(const char *path)
{
    size_t n = strlen(path);
    const char *e;
    if (n < 5)
        return 0;
    e = path + n - 4;
    return ci_eq(e, ".ttf") || ci_eq(e, ".otf") || ci_eq(e, ".ttc") || ci_eq(e, ".otc");
}

/* ---- the index itself ---- */

void of_index_init(OFIndex *ix)
{
    memset(ix, 0, sizeof(*ix));
    copy_str(ix->fallbacks, sizeof ix->fallbacks, default_fallbacks, strlen(default_fallbacks));
}

/* Forgets every entry's coverage: the entries changed. */
static void drop_coverage(OFIndex *ix)
{
    size_t i;
    for (i = 0; i < ix->coverage_count; ++i)
        free(ix->coverage[i].ranges);
    free(ix->coverage);
    ix->coverage = NULL;
    ix->coverage_count = 0;
}

void of_index_free(OFIndex *ix)
{
    if (!ix)
        return;
    drop_coverage(ix);
    free(ix->entries);
    ix->entries = NULL;
    ix->count = ix->cap = 0;
}

void of_index_configure(OFIndex *ix, const char *aliases, const char *fallbacks)
{
    if (!ix)
        return;
    if (aliases)
        copy_str(ix->aliases, sizeof ix->aliases, aliases, strlen(aliases));
    if (fallbacks)
        copy_str(ix->fallbacks, sizeof ix->fallbacks, fallbacks, strlen(fallbacks));
}

int of_index_add_dir(OFIndex *ix, const char *dir)
{
    size_t i, n;
    if (!ix || !dir || !*dir)
        return OF_ERR_ARGUMENT;
    n = strlen(dir);
    if (n >= OF_INDEX_PATH)
        return OF_ERR_BUFFER;
    for (i = 0; i < ix->ndirs; ++i)
        if (ci_eq(ix->dirs[i].path, dir))
            return OF_OK;
    if (ix->ndirs >= OF_INDEX_MAX_DIRS)
        return OF_ERR_RANGE;
    memset(&ix->dirs[ix->ndirs], 0, sizeof ix->dirs[0]);
    memcpy(ix->dirs[ix->ndirs].path, dir, n + 1);
    ix->ndirs++;
    return OF_OK;
}

static int add_entry(OFIndex *ix, const OFIndexEntry *e)
{
    if (ix->count == ix->cap) {
        size_t cap = ix->cap ? ix->cap * 2 : 32;
        OFIndexEntry *ne = (OFIndexEntry *)realloc(ix->entries, cap * sizeof(*ne));
        if (!ne)
            return OF_ERR_MEMORY;
        ix->entries = ne;
        ix->cap = cap;
    }
    ix->entries[ix->count++] = *e;
    return OF_OK;
}

/* the entries under dir: its own files, not those of its sub-folders */
static int entry_in_dir(const OFIndexEntry *e, const char *dir)
{
    size_t d = strlen(dir), n = strlen(e->path), i;
    if (n <= d || !ci_eqn(e->path, dir, d))
        return 0;
    i = d;
    if (dir[d - 1] != '/' && dir[d - 1] != ':') {
        if (e->path[i] != '/')
            return 0;
        ++i;
    }
    for (; i < n; ++i)
        if (e->path[i] == '/')
            return 0;
    return 1;
}

int of_index_stale(const OFIndex *ix)
{
    size_t i;
    if (!ix)
        return 0;
    for (i = 0; i < ix->ndirs; ++i) {
        uint32_t st[3];
        int present = of_io_stamp(ix->dirs[i].path, st) == OF_OK;
        if (present != ix->dirs[i].present)
            return 1;
        if (present && memcmp(st, ix->dirs[i].stamp, sizeof st))
            return 1;
    }
    return 0;
}

struct ScanArg {
    OFIndex *ix;
    OFContext *ctx;
    int rc;
    int incomplete;             /* a file of this folder could not be read */
};

static int scan_file(const char *path, void *arg)
{
    struct ScanArg *a = (struct ScanArg *)arg;
    uint32_t face_index = 0, num_faces = 1;
    if (!font_file(path) || strlen(path) >= OF_INDEX_PATH)
        return 0;
    while (face_index < num_faces) {
        OFFaceInfo info;
        OFIndexEntry e;
        int rc = of_probe_face(a->ctx, path, face_index, &info);
        if (rc == OF_ERR_MEMORY) {
            a->rc = rc;
            return 1;
        }
        if (rc == OF_ERR_IO) {
            /* in use (being copied in, say) or unreadable just now: the
             * folder is read again next time */
            a->incomplete = 1;
            break;
        }
        if (rc != OF_OK)
            break;              /* not a font after all: skip it, keep going */
        num_faces = info.num_faces ? info.num_faces : 1;
        memset(&e, 0, sizeof e);
        copy_str(e.path, sizeof e.path, path, strlen(path));
        e.face_index = face_index;
        memcpy(e.family, info.family, sizeof e.family);
        memcpy(e.style, info.style, sizeof e.style);
        e.weight = info.weight;
        e.width = info.width;
        e.flags = info.flags;
        memcpy(e.unicode_ranges, info.unicode_ranges, sizeof e.unicode_ranges);
        if (add_entry(a->ix, &e) != OF_OK) {
            a->rc = OF_ERR_MEMORY;
            return 1;
        }
        face_index++;
    }
    return 0;
}

int of_index_scan(OFIndex *ix, OFContext *ctx)
{
    size_t i;
    struct ScanArg a;
    if (!ix || !ctx)
        return OF_ERR_ARGUMENT;
    drop_coverage(ix);
    ix->count = 0;
    a.ix = ix;
    a.ctx = ctx;
    a.rc = OF_OK;
    for (i = 0; i < ix->ndirs; ++i) {
        OFIndexDir *d = &ix->dirs[i];
        int rc;
        d->present = of_io_stamp(d->path, d->stamp) == OF_OK;
        if (!d->present) {
            memset(d->stamp, 0, sizeof d->stamp);
            continue;
        }
        a.incomplete = 0;
        rc = of_io_list_dir(d->path, scan_file, &a);
        if (a.rc != OF_OK) {
            /* stopped part way: nothing counts as up to date, so the next
             * look scans again */
            size_t k;
            for (k = 0; k < ix->ndirs; ++k) {
                ix->dirs[k].present = 0;
                memset(ix->dirs[k].stamp, 0, sizeof ix->dirs[k].stamp);
            }
            return a.rc;
        }
        if (rc != OF_OK || a.incomplete) {
            /* there, but not listed (or not all of it, or a font not read):
             * keep what the folders gave, and mark this one so the next
             * look reads it again rather than taking it as it is */
            d->present = 0;
            memset(d->stamp, 0, sizeof d->stamp);
        }
    }
    return OF_OK;
}

/* ---- the file: a few lines of text ----
 *   OFIX 2
 *   D <stamp0> <stamp1> <stamp2> <present>	<folder>
 *   F <face> <weight> <width> <flags> <range0..3>	<family>	<style>	<path>
 *   E <how many F lines>
 * Numbers in hex. Tabs never occur in names; a name with one is cut there.
 * A file without its E line, or with the wrong count, was cut short and is
 * refused (version 1 had no E line). */

struct Out {
    uint8_t *p;
    size_t n, cap;
    int failed;
};

static void put(struct Out *o, const char *s, size_t n)
{
    if (o->failed)
        return;
    if (o->n + n > o->cap) {
        size_t cap = o->cap ? o->cap * 2 : 4096;
        uint8_t *np;
        while (cap < o->n + n)
            cap *= 2;
        np = (uint8_t *)realloc(o->p, cap);
        if (!np) {
            o->failed = 1;
            return;
        }
        o->p = np;
        o->cap = cap;
    }
    memcpy(o->p + o->n, s, n);
    o->n += n;
}

static void puts_(struct Out *o, const char *s)
{
    size_t n = 0;
    while (s[n] && s[n] != '\t' && s[n] != '\n')
        ++n;
    put(o, s, n);
}

static void puthex(struct Out *o, uint32_t v)
{
    char b[9];
    int i;
    for (i = 7; i >= 0; --i) {
        b[i] = "0123456789abcdef"[v & 15];
        v >>= 4;
    }
    b[8] = ' ';
    put(o, b, 9);
}

int of_index_save(const OFIndex *ix, const char *path)
{
    struct Out o;
    size_t i;
    int k, rc;
    if (!ix || !path)
        return OF_ERR_ARGUMENT;
    memset(&o, 0, sizeof o);
    put(&o, "OFIX 2\n", 7);
    for (i = 0; i < ix->ndirs; ++i) {
        put(&o, "D ", 2);
        for (k = 0; k < 3; ++k)
            puthex(&o, ix->dirs[i].stamp[k]);
        puthex(&o, (uint32_t)ix->dirs[i].present);
        put(&o, "\t", 1);
        puts_(&o, ix->dirs[i].path);
        put(&o, "\n", 1);
    }
    for (i = 0; i < ix->count; ++i) {
        const OFIndexEntry *e = &ix->entries[i];
        put(&o, "F ", 2);
        puthex(&o, e->face_index);
        puthex(&o, e->weight);
        puthex(&o, e->width);
        puthex(&o, e->flags);
        for (k = 0; k < 4; ++k)
            puthex(&o, e->unicode_ranges[k]);
        put(&o, "\t", 1);
        puts_(&o, e->family);
        put(&o, "\t", 1);
        puts_(&o, e->style);
        put(&o, "\t", 1);
        puts_(&o, e->path);
        put(&o, "\n", 1);
    }
    /* the last line, with the count of F lines: a file cut short is refused */
    put(&o, "E ", 2);
    puthex(&o, (uint32_t)ix->count);
    put(&o, "\n", 1);
    if (o.failed) {
        free(o.p);
        return OF_ERR_MEMORY;
    }
    rc = of_io_write_all(path, o.p, o.n);
    free(o.p);
    return rc;
}

/* parses up to n hex words separated by spaces; returns the position after */
static const char *gethex(const char *s, const char *end, uint32_t *out, int n)
{
    int k;
    for (k = 0; k < n; ++k) {
        uint32_t v = 0;
        int digits = 0;
        while (s < end && *s == ' ')
            ++s;
        while (s < end) {
            int c = *s, d;
            if (c >= '0' && c <= '9')
                d = c - '0';
            else if (c >= 'a' && c <= 'f')
                d = c - 'a' + 10;
            else
                break;
            v = (v << 4) | (uint32_t)d;
            ++s;
            ++digits;
        }
        if (!digits)
            return NULL;
        out[k] = v;
    }
    while (s < end && *s == ' ')
        ++s;
    return s;
}

/* the next tab-separated field into out; returns the position after it */
static const char *getfield(const char *s, const char *end, char *out, size_t size)
{
    const char *f = s;
    while (s < end && *s != '\t')
        ++s;
    copy_str(out, size, f, (size_t)(s - f));
    return s < end ? s + 1 : s;
}

int of_index_load(OFIndex *ix, const char *path)
{
    uint8_t *data = NULL;
    size_t size = 0;
    const char *p, *end;
    OFIndexDir dirs[OF_INDEX_MAX_DIRS];
    size_t ndirs = 0, i;
    uint32_t flines = 0;
    int rc, ended = 0;

    if (!ix || !path)
        return OF_ERR_ARGUMENT;
    rc = of_io_read_all(path, &data, &size);
    if (rc != OF_OK)
        return rc;
    p = (const char *)data;
    end = p + size;
    if (size < 7 || memcmp(p, "OFIX 2\n", 7)) {
        free(data);
        return OF_ERR_CACHE;
    }
    p += 7;
    drop_coverage(ix);
    ix->count = 0;
    while (p < end) {
        const char *line = p, *eol = p;
        while (eol < end && *eol != '\n')
            ++eol;
        p = eol < end ? eol + 1 : eol;
        if (eol - line < 2 || line[1] != ' ')
            continue;
        if (line[0] == 'E') {
            uint32_t v;
            ended = gethex(line + 2, eol, &v, 1) != NULL && v == flines;
            break;
        }
        if (line[0] == 'F')
            flines++;
        if (line[0] == 'D' && ndirs < OF_INDEX_MAX_DIRS) {
            uint32_t v[4];
            const char *q = gethex(line + 2, eol, v, 4);
            if (!q || q >= eol || *q != '\t')
                continue;
            memset(&dirs[ndirs], 0, sizeof dirs[0]);
            getfield(q + 1, eol, dirs[ndirs].path, sizeof dirs[ndirs].path);
            memcpy(dirs[ndirs].stamp, v, sizeof dirs[ndirs].stamp);
            dirs[ndirs].present = v[3] != 0;
            ndirs++;
        } else if (line[0] == 'F') {
            uint32_t v[8];
            OFIndexEntry e;
            const char *q = gethex(line + 2, eol, v, 8);
            if (!q || q >= eol || *q != '\t')
                continue;
            memset(&e, 0, sizeof e);
            e.face_index = v[0];
            e.weight = (uint16_t)v[1];
            e.width = (uint16_t)v[2];
            e.flags = (uint16_t)v[3];
            memcpy(e.unicode_ranges, v + 4, sizeof e.unicode_ranges);
            q = getfield(q + 1, eol, e.family, sizeof e.family);
            q = getfield(q, eol, e.style, sizeof e.style);
            getfield(q, eol, e.path, sizeof e.path);
            if (!e.path[0])
                continue;
            if (add_entry(ix, &e) != OF_OK) {
                free(data);
                return OF_ERR_MEMORY;
            }
        }
    }
    free(data);
    if (!ended) {
        /* cut short, or not ours: none of it counts */
        ix->count = 0;
        return OF_ERR_CACHE;
    }
    /* the folders the caller asked for are the list: take each one's stamp
     * from the file when it was indexed, and drop entries of others */
    for (i = 0; i < ix->ndirs; ++i) {
        size_t k;
        ix->dirs[i].present = 0;
        memset(ix->dirs[i].stamp, 0, sizeof ix->dirs[i].stamp);
        for (k = 0; k < ndirs; ++k)
            if (ci_eq(dirs[k].path, ix->dirs[i].path)) {
                ix->dirs[i] = dirs[k];
                memcpy(ix->dirs[i].path, dirs[k].path, sizeof dirs[k].path);
                break;
            }
        if (k == ndirs)
            ix->dirs[i].present = -1;   /* never indexed: stale */
    }
    {
        size_t w = 0, k;
        for (k = 0; k < ix->count; ++k) {
            int keep = 0;
            for (i = 0; i < ix->ndirs && !keep; ++i)
                keep = entry_in_dir(&ix->entries[k], ix->dirs[i].path);
            if (keep)
                ix->entries[w++] = ix->entries[k];
        }
        ix->count = w;
    }
    return OF_OK;
}

/* ---- finding ---- */

static const OFIndexEntry *best_of_family(const OFIndex *ix, const char *family, size_t n,
                                          uint16_t weight, int italic)
{
    const OFIndexEntry *best = NULL;
    long best_score = 0;
    size_t i;
    for (i = 0; i < ix->count; ++i) {
        const OFIndexEntry *e = &ix->entries[i];
        long score;
        if (strlen(e->family) != n || !ci_eqn(e->family, family, n))
            continue;
        score = (long)e->weight - (long)weight;
        if (score < 0)
            score = -score;
        if (((e->flags & OF_FACE_ITALIC) != 0) != (italic != 0))
            score += 1000;
        score += 50L * (e->width > 5 ? e->width - 5 : 5 - e->width);
        if (!best || score < best_score) {
            best = e;
            best_score = score;
        }
    }
    return best;
}

/* tries each family of a comma-separated list */
static const OFIndexEntry *best_of_list(const OFIndex *ix, const char *list, const char *end,
                                        uint16_t weight, int italic)
{
    while (list < end) {
        const char *f = list, *fe;
        const OFIndexEntry *e;
        while (list < end && *list != ',')
            ++list;
        fe = list;
        if (list < end)
            ++list;
        while (f < fe && *f == ' ')
            ++f;
        while (fe > f && fe[-1] == ' ')
            --fe;
        if (fe > f && (e = best_of_family(ix, f, (size_t)(fe - f), weight, italic)) != NULL)
            return e;
    }
    return NULL;
}

/* the expansion of name in a block of "name=list" lines */
static const OFIndexEntry *best_of_alias(const OFIndex *ix, const char *lines, const char *name,
                                         uint16_t weight, int italic)
{
    size_t n = strlen(name);
    const char *p = lines;
    while (p && *p) {
        const char *eol = p, *eq;
        while (*eol && *eol != '\n')
            ++eol;
        for (eq = p; eq < eol && *eq != '='; ++eq)
            ;
        if (eq < eol) {
            const char *ke = eq;
            while (ke > p && ke[-1] == ' ')
                --ke;
            if ((size_t)(ke - p) == n && ci_eqn(p, name, n)) {
                const OFIndexEntry *e = best_of_list(ix, eq + 1, eol, weight, italic);
                if (e)
                    return e;
            }
        }
        p = *eol ? eol + 1 : eol;
    }
    return NULL;
}

static const OFIndexEntry *best_of_builtin(const OFIndex *ix, const char *name,
                                           uint16_t weight, int italic)
{
    int i;
    size_t n = strlen(name);
    for (i = 0; builtin_aliases[i]; ++i) {
        const char *a = builtin_aliases[i], *eq = strchr(a, '=');
        if ((size_t)(eq - a) == n && ci_eqn(a, name, n))
            return best_of_list(ix, eq + 1, eq + strlen(eq), weight, italic);
    }
    return NULL;
}

const OFIndexEntry *of_index_find_exact(const OFIndex *ix, const char *family,
                                        uint16_t weight, int italic)
{
    const OFIndexEntry *e;
    if (!ix || !ix->count || !family || !*family)
        return NULL;
    if (!weight)
        weight = 400;
    if ((e = best_of_family(ix, family, strlen(family), weight, italic)) != NULL)
        return e;
    if ((e = best_of_alias(ix, ix->aliases, family, weight, italic)) != NULL)
        return e;
    return best_of_builtin(ix, family, weight, italic);
}

const OFIndexEntry *of_index_find(const OFIndex *ix, const char *family,
                                  uint16_t weight, int italic)
{
    const OFIndexEntry *e;
    if (!ix || !ix->count)
        return NULL;
    if (!weight)
        weight = 400;
    if ((e = of_index_find_exact(ix, family, weight, italic)) != NULL)
        return e;
    if ((e = best_of_alias(ix, ix->aliases, "sans-serif", weight, italic)) != NULL)
        return e;
    if ((e = best_of_builtin(ix, "sans-serif", weight, italic)) != NULL)
        return e;
    return &ix->entries[0];
}

OFFace *of_index_open(const OFIndex *ix, OFContext *ctx, const char *family,
                      uint16_t weight, int italic, int with_fallbacks, int *error)
{
    return of_index_open_family(ix, ctx, family, weight, italic, with_fallbacks, 0, error);
}

OFFace *of_index_open_family(const OFIndex *ix, OFContext *ctx, const char *family,
                             uint16_t weight, int italic, int with_fallbacks, int exact,
                             int *error)
{
    const OFIndexEntry *e = exact ? of_index_find_exact(ix, family, weight, italic)
                                  : of_index_find(ix, family, weight, italic);
    OFFace *face;
    const char *p, *end;
    int dummy;
    if (!error)
        error = &dummy;
    *error = OF_OK;
    if (!ctx) {
        *error = OF_ERR_ARGUMENT;
        return NULL;
    }
    if (!e) {
        *error = OF_ERR_FONT;
        return NULL;
    }
    face = of_face_open_file(ctx, e->path, e->face_index, error);
    if (!face || !with_fallbacks)
        return face;
    p = ix->fallbacks;
    end = p + strlen(p);
    while (p < end && of_face_fallback_count(face) < OF_MAX_FALLBACKS) {
        const char *f = p, *fe;
        const OFIndexEntry *fb;
        while (p < end && *p != ',')
            ++p;
        fe = p;
        if (p < end)
            ++p;
        while (f < fe && *f == ' ')
            ++f;
        while (fe > f && fe[-1] == ' ')
            --fe;
        if (fe == f)
            continue;
        fb = best_of_family(ix, f, (size_t)(fe - f), weight, italic);
        if (!fb || (ci_eq(fb->path, e->path) && fb->face_index == e->face_index))
            continue;
        {
            OFFace *ff = of_face_open_index(ctx, fb->path, fb->face_index);
            if (ff) {
                of_face_add_fallback(face, ff);
                of_face_close(ff);          /* the chain holds it now */
            }
        }
    }
    return face;
}

/* opens e and keeps it when it has a glyph for codepoint */
static int covered(const OFCoverage *c, uint32_t codepoint)
{
    uint32_t lo = 0, hi = c->count;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (codepoint < c->ranges[2 * mid])
            hi = mid;
        else if (codepoint > c->ranges[2 * mid + 1])
            lo = mid + 1;
        else
            return 1;
    }
    return 0;
}

/* Keeps the characters face maps as entry i's coverage. */
static void learn_coverage(OFIndex *ix, size_t i, OFFace *face)
{
    OFCoverage *c;
    size_t n;
    if (!ix->coverage) {
        ix->coverage = (OFCoverage *)calloc(ix->count, sizeof *ix->coverage);
        if (!ix->coverage)
            return;
        ix->coverage_count = ix->count;
    }
    if (i >= ix->coverage_count)
        return;
    c = &ix->coverage[i];
    n = of_face_cmap_ranges(face, NULL, 0);
    if (!n || n > 0xFFFF)
        return;
    c->ranges = (uint32_t *)malloc(n * 2 * sizeof(uint32_t));
    if (!c->ranges)
        return;
    c->count = (uint32_t)of_face_cmap_ranges(face, c->ranges, n);
    c->known = 1;
}

/* Entry e's face when it has the character. A face is opened (the whole
 * file read) only the first time, or when its coverage says it may have
 * it; characters below U+0100 always look, for Symbol fonts' stand-ins. */
static OFFace *open_if_has(OFIndex *ix, OFContext *ctx, const OFIndexEntry *e, uint32_t codepoint)
{
    size_t i = (size_t)(e - ix->entries);
    OFFace *face;
    if (codepoint >= 0x100 && i < ix->coverage_count && ix->coverage[i].known
        && !covered(&ix->coverage[i], codepoint))
        return NULL;
    face = of_face_open_index(ctx, e->path, e->face_index);
    if (!face)
        return NULL;
    if (i >= ix->coverage_count || !ix->coverage[i].known)
        learn_coverage(ix, i, face);
    if (!of_face_char_index(face, codepoint)) {
        of_face_close(face);
        face = NULL;
    }
    return face;
}

OFFace *of_index_open_char(OFIndex *ix, OFContext *ctx, uint32_t codepoint,
                           uint16_t weight, int italic, int *error)
{
    const char *p, *end;
    size_t i;
    int dummy;
    if (!error)
        error = &dummy;
    *error = OF_OK;
    if (!ctx) {
        *error = OF_ERR_ARGUMENT;
        return NULL;
    }
    /* the default chain first, in its order */
    p = ix->fallbacks;
    end = p + strlen(p);
    while (p < end) {
        const char *f = p, *fe;
        const OFIndexEntry *fb;
        OFFace *face;
        while (p < end && *p != ',')
            ++p;
        fe = p;
        if (p < end)
            ++p;
        while (f < fe && *f == ' ')
            ++f;
        while (fe > f && fe[-1] == ' ')
            --fe;
        if (fe == f)
            continue;
        fb = best_of_family(ix, f, (size_t)(fe - f), weight, italic);
        if (fb && (face = open_if_has(ix, ctx, fb, codepoint)) != NULL)
            return face;
    }
    /* then every face: the right slant and the nearest weight first */
    for (i = 0; i < ix->count; ++i) {
        const OFIndexEntry *e = &ix->entries[i];
        const OFIndexEntry *best;
        OFFace *face;
        size_t k;
        int seen = 0;
        /* one face a family: its best for weight and slant */
        for (k = 0; k < i && !seen; ++k)
            seen = ci_eq(ix->entries[k].family, e->family);
        if (seen)
            continue;
        best = best_of_family(ix, e->family, strlen(e->family), weight, italic);
        if (best && (face = open_if_has(ix, ctx, best, codepoint)) != NULL)
            return face;
    }
    *error = OF_ERR_FONT;
    return NULL;
}
