/* The C library pieces that GFOpenFont's core uses
 * inside gfopenfont.library: memory from one Exec pool. Every call into the
 * core holds the library's lock, so the pool needs no lock of its own.
 * The library runs no C startup and keeps no stdio: the rest of what the
 * core calls (memcpy, strlen, qsort) is libnix's, which keeps no state
 * that needs one.
 *
 * Copyright (c) 2026 AmigaChrome Team. OpenFont, MIT licence (LICENSE). */
#include <exec/types.h>
#include <exec/memory.h>
#include <proto/exec.h>

#include <stddef.h>
#include <string.h>

#include "of_runtime.h"

static APTR of_pool;

int of_runtime_init(void)
{
    /* puddles of 64 KB; a block of 16 KB or more gets its own */
    of_pool = CreatePool(MEMF_ANY, 65536, 16384);
    return of_pool != NULL;
}

void of_runtime_exit(void)
{
    if (of_pool)
        DeletePool(of_pool);    /* frees every block still in it */
    of_pool = NULL;
}

/* Each block carries its size in front, 8 bytes so doubles stay aligned. */
void *malloc(size_t n)
{
    ULONG *p;
    if (!of_pool || n > 0x7FFFFFF0UL)
        return NULL;
    p = (ULONG *)AllocPooled(of_pool, (ULONG)n + 8);
    if (!p)
        return NULL;
    p[0] = (ULONG)n;
    p[1] = 0x4F464D45UL;        /* "OFME": checked on free */
    return p + 2;
}

void free(void *ptr)
{
    ULONG *p;
    if (!ptr)
        return;
    p = (ULONG *)ptr - 2;
    if (p[1] != 0x4F464D45UL)
        return;                 /* not ours: never free what we didn't hand out */
    p[1] = 0;
    FreePooled(of_pool, p, p[0] + 8);
}

void *calloc(size_t n, size_t m)
{
    void *p;
    if (m && n > (size_t)0x7FFFFFF0UL / m)
        return NULL;
    p = malloc(n * m);
    if (p)
        memset(p, 0, n * m);
    return p;
}

void *realloc(void *ptr, size_t n)
{
    void *np;
    size_t old;
    if (!ptr)
        return malloc(n);
    if (!n) {
        free(ptr);
        return NULL;
    }
    old = ((ULONG *)ptr)[-2];
    if (n <= old && old - n < 256)
        return ptr;             /* shrinking by a little: keep the block */
    np = malloc(n);
    if (!np)
        return NULL;
    memcpy(np, ptr, n < old ? n : old);
    free(ptr);
    return np;
}
