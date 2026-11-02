/* GFOpenFont on Amiga: the same fill worker, on a cores board or the main CPU.
 *
 * Not part of the x86/ARM64 bootstrap. Build it with the os32 stove, next to
 * a checkout of gfopenmulticore:
 *
 *   m68k-amigaos-gcc -O2 -Iinclude -Isrc -I../gfopenmulticore/include \
 *       -c src/openfont_omc.c
 *
 * OMC_RunRange runs of_omc_fill_one once per codepoint. On a cores board that
 * is every free core. With no board, no 0.3 library, or a refused job, the
 * call falls back to of_cache_fill_range, which loops on this CPU. The worker
 * does not call the OS, and it writes only slots[index].
 *
 * Put the slots in memory from OMC_AllocGrant so the written grant does not
 * share a 4 KB page with this library's code. The cache itself is one Fast
 * RAM block (of_cache_memory). Leave the job permissive: the default. Do not
 * set OMCF_BOARD. A machine with no cores board still gets the glyphs.
 */

#include "gfopenfont/gfopenfont.h"
#include "openfont_internal.h"

#include <libraries/openmulticore.h>
#include <proto/openmulticore.h>

/* First argument is the index (D0 as well). The rest are the job's omj_Args,
 * the same convention as OMC_SubmitRange's square() example. */
LONG of_omc_fill_one(ULONG index, ULONG pixel_size, ULONG first,
                     ULONG cache_bits, ULONG slot_bits)
{
    OFCache *cache = (OFCache *)(uintptr_t)cache_bits;
    OFRangeSlot *slots = (OFRangeSlot *)(uintptr_t)slot_bits;
    int st = of_cache_fill_slot(cache, (uint16_t)pixel_size,
                                first + index, &slots[index]);
    if (st == OF_OK || st == OF_ERR_RANGE || st == OF_ERR_BUFFER)
        return 0;
    return 1;
}

int of_omc_fill_range(struct Library *OpenMulticoreBase,
                      OFCache *cache, uint16_t pixel_size,
                      uint32_t first, uint32_t count, OFRangeSlot *slots)
{
    struct OMCJob job;
    ULONG args[4];
    ULONG failed = 0;
    const void *mem = NULL;
    size_t mem_len = 0;
    LONG st;

    if (!cache || !slots || !pixel_size)
        return OF_ERR_ARGUMENT;
    if (!count)
        return OF_OK;
    if (count > (ULONG)~0 / (ULONG)sizeof(OFRangeSlot))
        return OF_ERR_ARGUMENT;
    if (!OpenMulticoreBase || !OMC_HAS_RANGE(OpenMulticoreBase) ||
        count < OF_RANGE_BREAK_EVEN)
        return of_cache_fill_range(cache, pixel_size, first, count, slots, NULL);

    of_cache_memory(cache, &mem, &mem_len);
    OMC_JobInit(&job);
    job.omj_Entry = (APTR)of_omc_fill_one;
    job.omj_Target = OMC_ANY;
    args[0] = pixel_size;
    args[1] = first;
    args[2] = (ULONG)(uintptr_t)cache;
    args[3] = (ULONG)(uintptr_t)slots;
    job.omj_Args = args;
    job.omj_NArgs = 4;
    if (OMC_AddGrant(&job, (APTR)mem, (ULONG)mem_len, OMCG_READ) != OMCERR_OK ||
        OMC_AddGrant(&job, slots, OMC_GRANTSIZE(count * (ULONG)sizeof(OFRangeSlot)),
                     OMCG_READ | OMCG_WRITE) != OMCERR_OK)
        return of_cache_fill_range(cache, pixel_size, first, count, slots, NULL);

    st = OMC_RunRange(&job, count, (ULONG)sizeof(OFRangeSlot), &failed);
    if (st != OMCERR_OK)
        return of_cache_fill_range(cache, pixel_size, first, count, slots, NULL);
    return OF_OK;
}
