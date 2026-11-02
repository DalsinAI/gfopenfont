/* GFOpenMulticore range against the CPU loop. Amiga only.
 * Build: amiga/build.sh with OMC_INC pointing at gfopenmulticore/include
 * and the NDK. */
#include "gfopenfont/gfopenfont.h"
#include "openfont_internal.h"

#include <libraries/openmulticore.h>
#include <proto/openmulticore.h>

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern struct Library *SysBase;

LONG of_omc_fill_one(ULONG index, ULONG pixel_size, ULONG first,
                     ULONG cache_bits, ULONG slot_bits);
int of_omc_fill_range(struct Library *OpenMulticoreBase, OFCache *cache,
                      uint16_t pixel_size, uint32_t first, uint32_t count,
                      OFRangeSlot *slots);

static struct Library *open_library(const char *name, unsigned long version)
{
    const char *nm = name;
    unsigned long ver = version;
    {
        register struct Library *re __asm("d0");
        register struct Library *const bn __asm("a6") = SysBase;
        register const char *nname __asm("a1") = nm;
        __asm volatile ("jsr a6@(-552:W)"
            : "=r" (re), "+r" (nname)
            : "r" (bn), "0" (ver)
            : "d1", "a0", "cc", "memory");
        return re;
    }
}

static void close_library(struct Library *lib)
{
    struct Library *l = lib;
    {
        register struct Library *const bn __asm("a6") = SysBase;
        register struct Library *n1 __asm("a1") = l;
        __asm volatile ("jsr a6@(-414:W)"
            : "+r" (n1)
            : "r" (bn)
            : "d0", "d1", "a0", "cc", "memory");
    }
}

static int fail(const char *msg)
{
    printf("OPENFONT_OMC FAIL %s\n", msg);
    return 1;
}

static void *align16(void *p)
{
    uintptr_t u = (uintptr_t)p;
    u = (u + 15u) & ~(uintptr_t)15u;
    return (void *)u;
}

static int make_cache(OFCache **out)
{
    enum { N = 8 };
    OFCacheGlyphIn glyphs[N];
    OFCacheSizeIn sz;
    uint8_t pix[N];
    uint32_t i;
    const char *path = "ram:omc.ofc";

    memset(glyphs, 0, sizeof glyphs);
    for (i = 0; i < N; ++i) {
        pix[i] = (uint8_t)(0x10 + i);
        glyphs[i].codepoint = 32 + i;
        glyphs[i].glyph_id = 200 + i;
        glyphs[i].left = (int32_t)i - 3;
        glyphs[i].top = 9;
        glyphs[i].advance_x_26_6 = 40 + (int32_t)i;
        glyphs[i].width = 1;
        glyphs[i].height = 1;
        glyphs[i].pixels = &pix[i];
    }
    sz.pixel_size = 16;
    sz.glyphs = glyphs;
    sz.count = N;
    if (of_cache_write(path, UINT64_C(0x0c0ffee), 32, 39, &sz, 1) != OF_OK)
        return fail("write");
    *out = of_cache_open(path);
    remove(path);
    if (!*out)
        return fail("open");
    return 0;
}

static int slots_match(const OFRangeSlot *a, const OFRangeSlot *b, uint32_t n)
{
    return memcmp(a, b, (size_t)n * sizeof(OFRangeSlot)) == 0;
}

int main(void)
{
    enum { N = 8 };
    OFCache *cache = NULL;
    OFRangeSlot *cpu, *via;
    void *cpu_mem, *via_mem;
    struct Library *OpenMulticoreBase;
    struct Library old;
    struct OMCJob job;
    ULONG args[4];
    ULONG failed = 0;
    const void *mem = NULL;
    size_t mem_len = 0;
    LONG st;
    int rc;

    if (offsetof(struct Library, lib_Version) != 20 ||
        offsetof(struct Library, lib_Revision) != 22)
        return fail("library layout");
    if (sizeof(OFRangeSlot) % 16)
        return fail("slot");

    if (make_cache(&cache))
        return 1;

    cpu_mem = malloc((size_t)N * sizeof(OFRangeSlot) + 16);
    via_mem = malloc((size_t)N * sizeof(OFRangeSlot) + 16);
    if (!cpu_mem || !via_mem)
        return fail("malloc");
    cpu = align16(cpu_mem);
    via = align16(via_mem);

    if (of_cache_fill_range(cache, 16, 32, N, cpu, NULL) != OF_OK)
        return fail("cpu");
    if (cpu[0].pixels[0] != 0x10 || cpu[7].glyph_id != 207)
        return fail("cpu bytes");

    memset(via, 0x5a, (size_t)N * sizeof(OFRangeSlot));
    if (of_omc_fill_range(NULL, cache, 16, 32, N, via) != OF_OK)
        return fail("null base");
    if (!slots_match(cpu, via, N))
        return fail("null base bytes");

    memset(&old, 0, sizeof old);
    old.lib_Revision = 2;
    memset(via, 0x5a, (size_t)N * sizeof(OFRangeSlot));
    if (of_omc_fill_range(&old, cache, 16, 32, N, via) != OF_OK)
        return fail("old library");
    if (!slots_match(cpu, via, N))
        return fail("old library bytes");

    OpenMulticoreBase = open_library("gfopenmulticore.library", 0);
    if (!OpenMulticoreBase)
        return fail("OpenLibrary");
    if (!OMC_HAS_RANGE(OpenMulticoreBase)) {
        printf("OPENFONT_OMC FAIL no-range ver=%u rev=%u\n",
               (unsigned)OpenMulticoreBase->lib_Version,
               (unsigned)OpenMulticoreBase->lib_Revision);
        close_library(OpenMulticoreBase);
        return 1;
    }

    memset(via, 0x5a, sizeof(OFRangeSlot));
    if (of_omc_fill_range(OpenMulticoreBase, cache, 16, 32, 1, via) != OF_OK)
        return fail("below break-even");
    if (memcmp(cpu, via, sizeof(OFRangeSlot)))
        return fail("below break-even bytes");

    of_cache_memory(cache, &mem, &mem_len);
    memset(via, 0x5a, (size_t)N * sizeof(OFRangeSlot));
    OMC_JobInit(&job);
    job.omj_Entry = (APTR)of_omc_fill_one;
    job.omj_Target = OMC_ANY;
    args[0] = 16;
    args[1] = 32;
    args[2] = (ULONG)(uintptr_t)cache;
    args[3] = (ULONG)(uintptr_t)via;
    job.omj_Args = args;
    job.omj_NArgs = 4;
    if (OMC_AddGrant(&job, (APTR)mem, (ULONG)mem_len, OMCG_READ) != OMCERR_OK)
        return fail("grant cache");
    if (OMC_AddGrant(&job, via, (ULONG)(N * sizeof(OFRangeSlot)),
                     OMCG_READ | OMCG_WRITE) != OMCERR_OK)
        return fail("grant slots");
    st = OMC_RunRange(&job, N, (ULONG)sizeof(OFRangeSlot), &failed);
    if (st != OMCERR_OK) {
        printf("OPENFONT_OMC FAIL run=%ld where=%lu fault=%lu pc=%lu failed=%lu\n",
               (long)st, (unsigned long)job.omj_Where,
               (unsigned long)job.omj_FaultVector, (unsigned long)job.omj_FaultPC,
               (unsigned long)failed);
        close_library(OpenMulticoreBase);
        return 1;
    }
    if (!slots_match(cpu, via, N))
        return fail("runrange bytes");

    memset(via, 0x5a, (size_t)N * sizeof(OFRangeSlot));
    rc = of_omc_fill_range(OpenMulticoreBase, cache, 16, 32, N, via);
    if (rc != OF_OK)
        return fail("fill_range");
    if (!slots_match(cpu, via, N))
        return fail("fill_range bytes");

    printf("OPENFONT_OMC PASS ver=%u rev=%u where=%lu\n",
           (unsigned)OpenMulticoreBase->lib_Version,
           (unsigned)OpenMulticoreBase->lib_Revision,
           (unsigned long)job.omj_Where);
    close_library(OpenMulticoreBase);
    of_cache_close(cache);
    free(cpu_mem);
    free(via_mem);
    return 0;
}
