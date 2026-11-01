#include "openfont_internal.h"
#include "openfont_io.h"

#include <stdlib.h>
#include <string.h>

/* Compiled cache.
 *
 * v1 and v2 stay readable (a walk of the glyph records).
 * v3 adds a size directory so a glyph is a couple of loads.
 * The bytes and the directory index live in one block, so a job can
 * read the cache through a single grant.
 *
 * of_cache_fill_range is the fast path. Each index writes only its own
 * OFRangeSlot. With no executor, or below the break-even, the worker
 * runs in a loop on this CPU. That loop is the 68k fallback. An executor
 * (GFOpenMulticore's OMC_RunRange on Amiga) calls the same worker.
 */

#define OF_CACHE_MAX_SIZES 64
#define OF_CACHE_MAX_SPAN  65536

typedef char of_slot_is_slice[(sizeof(OFRangeSlot) % 16) == 0 ? 1 : -1];

struct OFSizeIndex {
    uint16_t pixel_size;
    const uint8_t **recs; /* one per codepoint in [first, last], or NULL */
};

struct OFCache {
    uint16_t version;
    uint32_t size_count;
    uint32_t first_codepoint;
    uint32_t last_codepoint;
    uint64_t source_fingerprint;
    size_t block_size;
    size_t length;
    uint8_t *bytes;
    struct OFSizeIndex *sizes;
};

struct RangeJob {
    const OFCache *cache;
    uint16_t pixel_size;
    uint32_t first;
    OFRangeSlot *slots;
};

static size_t align8(size_t n)
{
    return (n + 7u) & ~(size_t)7u;
}

static uint16_t get16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static uint32_t get32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) |
           ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) |
           (uint32_t)p[3];
}

static uint64_t get64(const uint8_t *p)
{
    return ((uint64_t)get32(p) << 32) | get32(p + 4);
}

static int rec_ok(const uint8_t *p, const uint8_t *end, const uint8_t **next)
{
    uint32_t width, height, bitmap_bytes;

    if ((size_t)(end - p) < 32)
        return 0;
    width = get32(p + 20);
    height = get32(p + 24);
    bitmap_bytes = get32(p + 28);
    if (width && height > UINT32_MAX / width)
        return 0;
    if (bitmap_bytes != width * height)
        return 0;
    if ((size_t)(end - (p + 32)) < bitmap_bytes)
        return 0;
    if (next)
        *next = p + 32 + bitmap_bytes;
    return 1;
}

static OFCache *cache_block(size_t file_len, uint32_t size_count, uint32_t span)
{
    size_t index_bytes, ptr_bytes, header, total;
    uint8_t *block;
    OFCache *cache;
    uint32_t s;
    const uint8_t **ptrs;

    if (!size_count || size_count > OF_CACHE_MAX_SIZES || !span || span > OF_CACHE_MAX_SPAN)
        return NULL;
    if (span > SIZE_MAX / size_count)
        return NULL;
    header = align8(sizeof(OFCache));
    index_bytes = align8(size_count * sizeof(struct OFSizeIndex));
    if ((size_t)size_count * span > SIZE_MAX / sizeof(const uint8_t *))
        return NULL;
    ptr_bytes = align8(size_count * span * sizeof(const uint8_t *));
    if (header > SIZE_MAX - index_bytes || header + index_bytes > SIZE_MAX - ptr_bytes)
        return NULL;
    if (header + index_bytes + ptr_bytes > SIZE_MAX - file_len)
        return NULL;
    total = header + index_bytes + ptr_bytes + file_len;
    block = (uint8_t *)calloc(1, total);
    if (!block)
        return NULL;
    cache = (OFCache *)block;
    cache->block_size = total;
    cache->length = file_len;
    cache->size_count = size_count;
    cache->sizes = (struct OFSizeIndex *)(block + header);
    ptrs = (const uint8_t **)(block + header + index_bytes);
    cache->bytes = block + header + index_bytes + ptr_bytes;
    for (s = 0; s < size_count; ++s)
        cache->sizes[s].recs = ptrs + (size_t)s * span;
    return cache;
}

static int header_of(const uint8_t *p, size_t length, OFCache *out, size_t *payload)
{
    memset(out, 0, sizeof(*out));
    if (length < 20 || memcmp(p, "OFBC", 4))
        return 0;
    out->version = get16(p + 4);
    if (out->version != 1 && out->version != 2 && out->version != 3)
        return 0;
    out->size_count = get32(p + 8);
    out->first_codepoint = get32(p + 12);
    out->last_codepoint = get32(p + 16);
    if (out->first_codepoint > out->last_codepoint || !out->size_count ||
        out->size_count > OF_CACHE_MAX_SIZES)
        return 0;
    if ((uint64_t)out->last_codepoint - out->first_codepoint + 1 > OF_CACHE_MAX_SPAN)
        return 0;
    if (out->version == 1) {
        *payload = 20;
        return 1;
    }
    if (length < 28)
        return 0;
    out->source_fingerprint = get64(p + 20);
    if (out->version == 2) {
        *payload = 28;
        return 1;
    }
    if (length < 36)
        return 0;
    *payload = 36;
    return 1;
}

static int index_v2(OFCache *cache)
{
    const uint8_t *p, *end, *next;
    uint32_t s, i, span;

    span = cache->last_codepoint - cache->first_codepoint + 1;
    p = cache->bytes + (cache->version == 1 ? 20 : 28);
    end = cache->bytes + cache->length;
    for (s = 0; s < cache->size_count; ++s) {
        uint32_t count;
        if ((size_t)(end - p) < 8)
            return 0;
        cache->sizes[s].pixel_size = get16(p);
        count = get32(p + 4);
        p += 8;
        if (count > span)
            return 0;
        for (i = 0; i < count; ++i) {
            uint32_t cp;
            if (!rec_ok(p, end, &next))
                return 0;
            cp = get32(p);
            if (cp < cache->first_codepoint || cp > cache->last_codepoint)
                return 0;
            if (cache->sizes[s].recs[cp - cache->first_codepoint])
                return 0;
            cache->sizes[s].recs[cp - cache->first_codepoint] = p;
            p = next;
        }
    }
    return p == end;
}

static int index_v3(OFCache *cache)
{
    const uint8_t *dir, *end, *file;
    uint32_t dir_off, span, s, i, count_dir;

    file = cache->bytes;
    end = file + cache->length;
    dir_off = get32(file + 28);
    if (dir_off < 36 || dir_off >= cache->length || (size_t)(end - (file + dir_off)) < 4)
        return 0;
    dir = file + dir_off;
    count_dir = get32(dir);
    dir += 4;
    if (count_dir != cache->size_count)
        return 0;
    span = cache->last_codepoint - cache->first_codepoint + 1;
    for (s = 0; s < cache->size_count; ++s) {
        uint32_t count, region;
        const uint8_t *table;
        const uint8_t *payload_end = file + dir_off;
        if ((size_t)(end - dir) < 8)
            return 0;
        cache->sizes[s].pixel_size = get16(dir);
        count = get32(dir + 4);
        dir += 8;
        if (count != span)
            return 0;
        if ((size_t)(end - dir) < (size_t)count * 4)
            return 0;
        table = dir;
        dir += (size_t)count * 4;
        region = 0;
        for (i = 0; i < count; ++i) {
            uint32_t off = get32(table + (size_t)i * 4);
            const uint8_t *rec;
            if (off < 36 || off >= dir_off)
                return 0;
            rec = file + off;
            if (!rec_ok(rec, payload_end, NULL))
                return 0;
            if (get32(rec) != cache->first_codepoint + i)
                return 0;
            if (i && off <= region)
                return 0;
            region = off;
            cache->sizes[s].recs[i] = rec;
        }
    }
    return dir == end;
}

void of_cache_memory(const OFCache *cache, const void **addr, size_t *length)
{
    if (addr)
        *addr = cache;
    if (length)
        *length = cache ? cache->block_size : 0;
}

OFCache *of_cache_open(const char *path)
{
    size_t n = 0;
    uint8_t *file = NULL;
    size_t payload = 0;
    OFCache scratch, *cache = NULL;
    uint32_t span;

    if (!path)
        return NULL;
    if (of_io_read_all(path, &file, &n) != OF_OK)
        return NULL;
    if (!n || !header_of(file, n, &scratch, &payload)) {
        free(file);
        return NULL;
    }
    span = scratch.last_codepoint - scratch.first_codepoint + 1;
    cache = cache_block((size_t)n, scratch.size_count, span);
    if (!cache) {
        free(file);
        return NULL;
    }
    cache->version = scratch.version;
    cache->first_codepoint = scratch.first_codepoint;
    cache->last_codepoint = scratch.last_codepoint;
    cache->source_fingerprint = scratch.source_fingerprint;
    memcpy(cache->bytes, file, (size_t)n);
    free(file);
    if (cache->version == 3) {
        if (!index_v3(cache)) {
            of_cache_close(cache);
            return NULL;
        }
    } else if (!index_v2(cache)) {
        of_cache_close(cache);
        return NULL;
    }
    return cache;
}

void of_cache_close(OFCache *cache)
{
    free(cache);
}

void of_bitmap_release(OFBitmap *bitmap)
{
    if (!bitmap)
        return;
    free(bitmap->pixels);
    memset(bitmap, 0, sizeof(*bitmap));
}

uint64_t of_cache_source_fingerprint(const OFCache *cache)
{
    return cache ? cache->source_fingerprint : 0;
}

static const uint8_t *find_rec(const OFCache *cache, uint16_t pixel_size, uint32_t codepoint)
{
    uint32_t s;
    if (!cache || codepoint < cache->first_codepoint || codepoint > cache->last_codepoint)
        return NULL;
    for (s = 0; s < cache->size_count; ++s) {
        if (cache->sizes[s].pixel_size == pixel_size)
            return cache->sizes[s].recs[codepoint - cache->first_codepoint];
    }
    return NULL;
}

static int size_present(const OFCache *cache, uint16_t pixel_size)
{
    uint32_t s;
    if (!cache)
        return 0;
    for (s = 0; s < cache->size_count; ++s) {
        if (cache->sizes[s].pixel_size == pixel_size)
            return 1;
    }
    return 0;
}

static void load_rec(const uint8_t *rec, uint32_t *glyph_id, OFBitmap *out, const uint8_t **pix)
{
    out->left = (int32_t)get32(rec + 8);
    out->top = (int32_t)get32(rec + 12);
    out->advance_x_26_6 = (int32_t)get32(rec + 16);
    out->width = get32(rec + 20);
    out->height = get32(rec + 24);
    out->stride = out->width;
    out->pixels = NULL;
    if (glyph_id)
        *glyph_id = get32(rec + 4);
    if (pix)
        *pix = rec + 32;
}

int of_cache_get_a8(OFCache *cache, uint16_t pixel_size, uint32_t codepoint,
                    uint32_t *glyph_id, OFBitmap *out)
{
    const uint8_t *rec, *pix;
    uint32_t bytes;

    if (!cache || !out)
        return OF_ERR_ARGUMENT;
    memset(out, 0, sizeof(*out));
    if (!size_present(cache, pixel_size))
        return OF_ERR_RANGE;
    rec = find_rec(cache, pixel_size, codepoint);
    if (!rec)
        return OF_ERR_RANGE;
    load_rec(rec, glyph_id, out, &pix);
    bytes = out->width * out->height;
    if (!bytes)
        return OF_OK;
    out->pixels = (uint8_t *)malloc(bytes);
    if (!out->pixels) {
        memset(out, 0, sizeof(*out));
        return OF_ERR_MEMORY;
    }
    memcpy(out->pixels, pix, bytes);
    return OF_OK;
}

int of_cache_fill_slot(const OFCache *cache, uint16_t pixel_size,
                       uint32_t codepoint, OFRangeSlot *slot)
{
    const uint8_t *rec, *pix;
    OFBitmap b;
    uint32_t bytes;

    if (!slot)
        return OF_ERR_ARGUMENT;
    memset(slot, 0, sizeof(*slot));
    if (!cache) {
        slot->status = OF_ERR_ARGUMENT;
        return OF_ERR_ARGUMENT;
    }
    if (!size_present(cache, pixel_size)) {
        slot->status = OF_ERR_RANGE;
        return OF_ERR_RANGE;
    }
    rec = find_rec(cache, pixel_size, codepoint);
    if (!rec) {
        slot->status = OF_ERR_RANGE;
        return OF_ERR_RANGE;
    }
    load_rec(rec, &slot->glyph_id, &b, &pix);
    bytes = b.width * b.height;
    slot->left = b.left;
    slot->top = b.top;
    slot->advance_x_26_6 = b.advance_x_26_6;
    slot->width = b.width;
    slot->height = b.height;
    slot->npixels = bytes;
    if (bytes > OF_RANGE_PIXELS) {
        slot->status = OF_ERR_BUFFER;
        return OF_ERR_BUFFER;
    }
    if (bytes)
        memcpy(slot->pixels, pix, bytes);
    slot->status = OF_OK;
    return OF_OK;
}

static int range_one(uint32_t index, void *arg)
{
    struct RangeJob *job = (struct RangeJob *)arg;
    int st = of_cache_fill_slot(job->cache, job->pixel_size,
                                job->first + index, &job->slots[index]);
    if (st == OF_OK || st == OF_ERR_RANGE || st == OF_ERR_BUFFER)
        return 0;
    return 1;
}

static int cpu0_range(OFIndexFn fn, uint32_t count, void *arg, void *user)
{
    uint32_t i;
    (void)user;
    for (i = 0; i < count; ++i) {
        if (fn(i, arg))
            return 1;
    }
    return 0;
}

int of_cache_fill_range(const OFCache *cache, uint16_t pixel_size,
                        uint32_t first_codepoint, uint32_t count,
                        OFRangeSlot *slots, const OFExec *exec)
{
    struct RangeJob job;
    uint32_t break_even = OF_RANGE_BREAK_EVEN;
    int (*run)(OFIndexFn, uint32_t, void *, void *) = NULL;
    void *user = NULL;
    int rc;

    if (!cache || !slots || !pixel_size)
        return OF_ERR_ARGUMENT;
    if (!count)
        return OF_OK;
    if (first_codepoint > UINT32_MAX - (count - 1))
        return OF_ERR_ARGUMENT;
    if (!size_present(cache, pixel_size))
        return OF_ERR_RANGE;

    if (exec && exec->run_range) {
        if (exec->break_even)
            break_even = exec->break_even;
        if (count >= break_even) {
            run = exec->run_range;
            user = exec->user;
        }
    }

    job.cache = cache;
    job.pixel_size = pixel_size;
    job.first = first_codepoint;
    job.slots = slots;
    rc = run ? run(range_one, count, &job, user)
             : cpu0_range(range_one, count, &job, NULL);
    return rc ? OF_ERR_EXEC : OF_OK;
}

/* ---- writer (v3) ---- */

struct Buf {
    uint8_t *p;
    size_t n, cap;
};

static int breserve(struct Buf *b, size_t more)
{
    size_t need, cap;
    uint8_t *np;
    if (more > SIZE_MAX - b->n)
        return 0;
    need = b->n + more;
    if (need <= b->cap)
        return 1;
    cap = b->cap ? b->cap : 256;
    while (cap < need) {
        if (cap > SIZE_MAX / 2)
            return 0;
        cap *= 2;
    }
    np = (uint8_t *)realloc(b->p, cap);
    if (!np)
        return 0;
    b->p = np;
    b->cap = cap;
    return 1;
}

static int bput(struct Buf *b, const void *src, size_t n)
{
    if (!breserve(b, n))
        return 0;
    if (n)
        memcpy(b->p + b->n, src, n);
    b->n += n;
    return 1;
}

static int b16(struct Buf *b, uint16_t v)
{
    uint8_t d[2];
    d[0] = (uint8_t)(v >> 8);
    d[1] = (uint8_t)v;
    return bput(b, d, 2);
}

static int b32(struct Buf *b, uint32_t v)
{
    uint8_t d[4];
    d[0] = (uint8_t)(v >> 24);
    d[1] = (uint8_t)(v >> 16);
    d[2] = (uint8_t)(v >> 8);
    d[3] = (uint8_t)v;
    return bput(b, d, 4);
}

static int b64(struct Buf *b, uint64_t v)
{
    return b32(b, (uint32_t)(v >> 32)) && b32(b, (uint32_t)v);
}

int of_cache_write(const char *path, uint64_t fingerprint,
                   uint32_t first_codepoint, uint32_t last_codepoint,
                   const OFCacheSizeIn *sizes, size_t size_count)
{
    struct Buf body, dir, rec_off;
    size_t s, i, span;
    int rc = OF_ERR_MEMORY;

    if (!path || !sizes || !size_count || size_count > OF_CACHE_MAX_SIZES ||
        first_codepoint > last_codepoint || !fingerprint)
        return OF_ERR_ARGUMENT;
    span = (size_t)last_codepoint - first_codepoint + 1;
    if (span > OF_CACHE_MAX_SPAN)
        return OF_ERR_ARGUMENT;

    memset(&body, 0, sizeof body);
    memset(&dir, 0, sizeof dir);
    memset(&rec_off, 0, sizeof rec_off);
    if (!b32(&dir, (uint32_t)size_count))
        goto done;

    for (s = 0; s < size_count; ++s) {
        const OFCacheSizeIn *sz = &sizes[s];
        if (!sz->pixel_size || !sz->glyphs || sz->count != span) {
            rc = OF_ERR_ARGUMENT;
            goto done;
        }
        if (!b16(&body, sz->pixel_size) || !b16(&body, 0) || !b32(&body, (uint32_t)span))
            goto done;
        rec_off.n = 0;
        for (i = 0; i < span; ++i) {
            const OFCacheGlyphIn *g = &sz->glyphs[i];
            uint32_t bytes, off;
            if (g->codepoint != first_codepoint + (uint32_t)i) {
                rc = OF_ERR_ARGUMENT;
                goto done;
            }
            if (g->width && g->height > UINT32_MAX / g->width) {
                rc = OF_ERR_ARGUMENT;
                goto done;
            }
            bytes = g->width * g->height;
            if (bytes && !g->pixels) {
                rc = OF_ERR_ARGUMENT;
                goto done;
            }
            off = (uint32_t)(36 + body.n);
            if (!b32(&rec_off, off) ||
                !b32(&body, g->codepoint) || !b32(&body, g->glyph_id) ||
                !b32(&body, (uint32_t)g->left) || !b32(&body, (uint32_t)g->top) ||
                !b32(&body, (uint32_t)g->advance_x_26_6) ||
                !b32(&body, g->width) || !b32(&body, g->height) || !b32(&body, bytes) ||
                (bytes && !bput(&body, g->pixels, bytes)))
                goto done;
        }
        if (!b16(&dir, sz->pixel_size) || !b16(&dir, 0) || !b32(&dir, (uint32_t)span) ||
            !bput(&dir, rec_off.p, rec_off.n))
            goto done;
    }

    if (body.n > UINT32_MAX - 36) {
        rc = OF_ERR_ARGUMENT;
        goto done;
    }

    {
        struct Buf file;
        uint32_t dir_off;
        memset(&file, 0, sizeof file);
        dir_off = (uint32_t)(36 + body.n);
        if (!bput(&file, "OFBC", 4) || !b16(&file, 3) || !b16(&file, 0) ||
            !b32(&file, (uint32_t)size_count) ||
            !b32(&file, first_codepoint) || !b32(&file, last_codepoint) ||
            !b64(&file, fingerprint) || !b32(&file, dir_off) || !b32(&file, 0) ||
            !bput(&file, body.p, body.n) || !bput(&file, dir.p, dir.n)) {
            free(file.p);
            goto done;
        }
        rc = of_io_write_all(path, file.p, file.n);
        free(file.p);
    }

done:
    free(body.p);
    free(dir.p);
    free(rec_off.p);
    return rc;
}
