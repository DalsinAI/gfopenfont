#include "gfopenfont/gfopenfont.h"
#include "openfont_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fail(const char *msg)
{
    fprintf(stderr, "FAIL %s\n", msg);
    return 1;
}

static void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static void put64(uint8_t *p, uint64_t v)
{
    put32(p, (uint32_t)(v >> 32));
    put32(p + 4, (uint32_t)v);
}

/* A version-2 cache: one size, one 1x1 glyph. Proves old files still open. */
static int write_v2(const char *path)
{
    uint8_t b[28 + 8 + 32 + 1];
    FILE *f;
    memset(b, 0, sizeof b);
    memcpy(b, "OFBC", 4);
    put16(b + 4, 2);
    put32(b + 8, 1);
    put32(b + 12, 65);
    put32(b + 16, 65);
    put64(b + 20, UINT64_C(0x1111222233334444));
    put16(b + 28, 12);
    put32(b + 32, 1);
    put32(b + 36, 65);
    put32(b + 40, 7);
    put32(b + 44, (uint32_t)-2);
    put32(b + 48, 11);
    put32(b + 52, 64);
    put32(b + 56, 1);
    put32(b + 60, 1);
    put32(b + 64, 1);
    b[68] = 0xab;
    f = fopen(path, "wb");
    if (!f || fwrite(b, 1, sizeof b, f) != sizeof b || fclose(f))
        return 0;
    return 1;
}

static int test_v2(void)
{
    const char *path = "old-v2.ofc";
    OFCache *cache;
    OFBitmap bm;
    OFRangeSlot slot;
    uint32_t gid = 0;
    if (!write_v2(path))
        return fail("write v2");
    cache = of_cache_open(path);
    remove(path);
    if (!cache)
        return fail("open v2");
    if (of_cache_source_fingerprint(cache) != UINT64_C(0x1111222233334444))
        return fail("v2 fingerprint");
    if (of_cache_get_a8(cache, 12, 65, &gid, &bm) != OF_OK)
        return fail("v2 get");
    if (gid != 7 || bm.width != 1 || bm.height != 1 || bm.left != -2 ||
        bm.top != 11 || bm.advance_x_26_6 != 64 || !bm.pixels || bm.pixels[0] != 0xab) {
        of_bitmap_release(&bm);
        return fail("v2 bytes");
    }
    of_bitmap_release(&bm);
    if (of_cache_fill_slot(cache, 12, 65, &slot) != OF_OK || slot.pixels[0] != 0xab)
        return fail("v2 slot");
    if (of_cache_get_a8(cache, 13, 65, NULL, &bm) != OF_ERR_RANGE)
        return fail("v2 missing size");
    of_cache_close(cache);
    return 0;
}

struct Seen {
    int calls;
    uint8_t hit[8];
};

static int reverse_range(OFIndexFn fn, uint32_t count, void *arg, void *user)
{
    struct Seen *seen = (struct Seen *)user;
    uint32_t i;
    seen->calls++;
    if (count > 8)
        return 1;
    for (i = count; i-- > 0; ) {
        seen->hit[i] = 1;
        if (fn(i, arg))
            return 1;
    }
    return 0;
}

static int test_v3_and_range(void)
{
    OFCacheGlyphIn glyphs[2][4];
    OFCacheSizeIn sizes[2];
    uint8_t pix[2][4][8];
    OFCache *cache;
    OFRangeSlot cpu[4], fast[4];
    OFExec exec;
    struct Seen seen;
    const void *mem;
    size_t mem_len;
    const char *path = "range-v3.ofc";
    uint32_t s, i;
    OFBitmap bm;
    uint32_t gid = 0;

    memset(glyphs, 0, sizeof glyphs);
    memset(pix, 0, sizeof pix);
    for (s = 0; s < 2; ++s) {
        for (i = 0; i < 4; ++i) {
            uint32_t n = i + 1;
            uint32_t p;
            glyphs[s][i].codepoint = 65 + i;
            glyphs[s][i].glyph_id = 100 + s * 10 + i;
            glyphs[s][i].left = (int32_t)i - 1;
            glyphs[s][i].top = 10 + (int32_t)s;
            glyphs[s][i].advance_x_26_6 = 20 + (int32_t)i;
            glyphs[s][i].width = n;
            glyphs[s][i].height = 2;
            glyphs[s][i].pixels = pix[s][i];
            for (p = 0; p < n * 2; ++p)
                pix[s][i][p] = (uint8_t)(0x40 + s * 16 + i * 3 + p);
        }
        sizes[s].pixel_size = (uint16_t)(12 + s * 4);
        sizes[s].glyphs = glyphs[s];
        sizes[s].count = 4;
    }
    /* One empty glyph, still in order. */
    glyphs[0][2].width = 0;
    glyphs[0][2].height = 0;
    glyphs[0][2].pixels = NULL;

    if (of_cache_write(path, UINT64_C(0xabcdef), 65, 68, sizes, 2) != OF_OK)
        return fail("write v3");
    cache = of_cache_open(path);
    remove(path);
    if (!cache)
        return fail("open v3");
    of_cache_memory(cache, &mem, &mem_len);
    if (mem != cache || mem_len < 36)
        return fail("one block");

    if (of_cache_get_a8(cache, 16, 68, &gid, &bm) != OF_OK)
        return fail("v3 get last");
    if (gid != 113 || bm.width != 4 || bm.height != 2 || !bm.pixels ||
        bm.pixels[0] != pix[1][3][0] || bm.pixels[7] != pix[1][3][7]) {
        of_bitmap_release(&bm);
        return fail("v3 indexed bytes");
    }
    of_bitmap_release(&bm);

    if (of_cache_fill_range(cache, 12, 65, 4, cpu, NULL) != OF_OK)
        return fail("cpu0 range");
    if (cpu[2].status != OF_OK || cpu[2].npixels != 0)
        return fail("empty glyph");
    if (cpu[0].glyph_id != 100 || cpu[3].pixels[0] != pix[0][3][0])
        return fail("cpu0 slots");

    memset(&seen, 0, sizeof seen);
    memset(&exec, 0, sizeof exec);
    exec.run_range = reverse_range;
    exec.user = &seen;
    exec.break_even = 100;
    if (of_cache_fill_range(cache, 12, 65, 4, fast, &exec) != OF_OK)
        return fail("below break-even");
    if (seen.calls != 0)
        return fail("break-even still called the executor");
    if (memcmp(cpu, fast, sizeof cpu))
        return fail("break-even mismatch");

    exec.break_even = 1;
    memset(fast, 0, sizeof fast);
    if (of_cache_fill_range(cache, 12, 65, 4, fast, &exec) != OF_OK)
        return fail("reverse range");
    if (seen.calls != 1 || memcmp(seen.hit, "\1\1\1\1", 4))
        return fail("reverse did not cover every index");
    if (memcmp(cpu, fast, sizeof cpu))
        return fail("68k and executor slots differ");

    memset(fast, 0x5a, sizeof fast[0]);
    if (of_cache_fill_range(cache, 99, 65, 1, fast, NULL) != OF_ERR_RANGE)
        return fail("missing size");
    if (((unsigned char *)fast)[0] != 0x5a)
        return fail("missing size wrote a slot");

    if (of_cache_fill_range(cache, 12, 64, 2, fast, NULL) != OF_OK)
        return fail("partial span");
    if (fast[0].status != OF_ERR_RANGE || fast[1].status != OF_OK || fast[1].glyph_id != 100)
        return fail("per-glyph miss");

    of_cache_close(cache);
    return 0;
}

static int test_buffer(void)
{
    OFCacheGlyphIn g;
    OFCacheSizeIn sz;
    uint8_t *pix;
    OFRangeSlot slot;
    OFCache *cache;
    OFBitmap bm;
    const char *path = "big.ofc";
    uint32_t bytes = 70u * 70u;
    int rc;

    pix = (uint8_t *)malloc(bytes);
    if (!pix)
        return fail("malloc");
    memset(pix, 0x3c, bytes);
    memset(&g, 0, sizeof g);
    g.codepoint = 65;
    g.glyph_id = 1;
    g.width = 70;
    g.height = 70;
    g.pixels = pix;
    g.advance_x_26_6 = 100;
    sz.pixel_size = 48;
    sz.glyphs = &g;
    sz.count = 1;
    rc = of_cache_write(path, 99, 65, 65, &sz, 1);
    free(pix);
    if (rc != OF_OK)
        return fail("write big");
    cache = of_cache_open(path);
    remove(path);
    if (!cache)
        return fail("open big");
    if (of_cache_fill_slot(cache, 48, 65, &slot) != OF_ERR_BUFFER ||
        slot.npixels != bytes || slot.status != OF_ERR_BUFFER)
        return fail("slot buffer");
    if (of_cache_get_a8(cache, 48, 65, NULL, &bm) != OF_OK || bm.width != 70) {
        of_bitmap_release(&bm);
        return fail("get still has the big glyph");
    }
    of_bitmap_release(&bm);
    of_cache_close(cache);
    return 0;
}

int main(void)
{
    if (sizeof(OFRangeSlot) % 16)
        return fail("slot is not a 16-byte slice");
    if (test_v2() || test_v3_and_range() || test_buffer())
        return 1;
    printf("OPENFONT_RANGE PASS slot=%lu\n", (unsigned long)sizeof(OFRangeSlot));
    return 0;
}
