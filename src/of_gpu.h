/*
 * Glyph runs drawn through GFOpenGPU, with a glyph cache (of_gpu.c): what
 * gfopenfont.library's OF_DrawGlyphsA does. Each glyph's outline is filled
 * once into an A8 atlas page by GFOpenGPU's PATH_FILL (per face, pixel size,
 * quarter-pixel x phase and anti-aliasing), then every use of it is one
 * COMPOSITE_PD of the run's colour through that cell. Glyphs under a
 * transform other than a scale are filled from their outlines each time.
 * Plain C over a submit function, so the host tests run it on
 * openamigartg's CPU core. MIT, Copyright (c) 2026 AmiCradle Team.
 * SPDX-License-Identifier: MIT
 */
#ifndef OF_GPU_H
#define OF_GPU_H

#include <stdint.h>
#include <stddef.h>

#include "gfopenfont/gfopenfont.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Where a run is drawn: a GFOpenGPU surface (stream.h's formats with
 * colour: ARGB32, XRGB32, RGB565 and the rest), the clip (a rectangle, and
 * optionally an A8 mask covering mask_x, mask_y, mask_w, mask_h whose
 * coverage multiplies what is drawn). The same layout as the SDK's struct
 * OFGPUTarget (sdk/Include_H/libraries/gfopenfont.h). */
typedef struct OFGpuTarget {
    uint32_t address;
    int32_t bpr;
    uint16_t width, height;
    uint32_t format;
    int16_t clip_x, clip_y, clip_w, clip_h;
    uint32_t mask_address;          /* 0: no mask */
    int32_t mask_bpr;
    int16_t mask_x, mask_y, mask_w, mask_h;
} OFGpuTarget;

/* OFGpuRun.flags */
#define OF_GPU_ALIASED  0x0001      /* no anti-aliasing: each pixel by its centre (GFOpenGPU 0.23) */
#define OF_GPU_NOCACHE  0x0002      /* fill each glyph's outline, past the cache */

/* A run: glyphs of the face at pixel_size, each at a pen position (24.8
 * target pixels, the baseline's origin, y down), in a premultiplied ARGB32
 * colour by a GFOpenGPU operator (OGPU_PD_*, or a blend mode), optionally
 * under a 2 x 2 matrix (16.16; 0 for none) about each pen position. The
 * same layout as the SDK's struct OFGlyphRun. */
typedef struct OFGpuRun {
    const uint32_t *glyphs;
    const int32_t *positions;       /* x, y pairs */
    uint32_t count;
    uint32_t pixel_size;
    uint32_t color;
    uint32_t op;
    const int32_t *matrix;
    uint32_t flags;
} OFGpuRun;

/* Memory the back end reaches: bytes (aligned on 4) and its address there. */
typedef void *(*of_gpu_alloc_fn)(void *user, size_t bytes, uint32_t *address);
typedef void (*of_gpu_free_fn)(void *user, void *block);
/* Sends words of stream and waits until they are drawn: 0, or GFOpenGPU's error. */
typedef long (*of_gpu_submit_fn)(void *user, const uint8_t *stream, long words);

typedef struct OFGpu OFGpu;

/* A drawer with its own cache; NULL without memory. aliased_ok: the back
 * end takes OGPU_PATH_ALIASED (OGPU_QUERY_FLAG); without it a run with
 * OF_GPU_ALIASED is refused (OF_ERR_ARGUMENT). */
OFGpu *of_gpu_create(of_gpu_alloc_fn alloc, of_gpu_free_fn release, of_gpu_submit_fn submit, void *user, int aliased_ok);
void of_gpu_destroy(OFGpu *g);

/* GFOpenGPU failed a batch (of_gpu_error says how). gfopenfont.library's
 * OFERR_NOTPROCESS is -9 and OFERR_NOGPU -10. */
#define OF_ERR_GPU      -11

/* Draws a run: the glyphs drawn (spaces and glyphs with no outline count),
 * or an OF_ERR_ code. Waits until it is drawn. The face's pixel size is
 * set to the run's while it draws and put back after. */
int of_gpu_draw(OFGpu *g, OFFace *face, const OFGpuTarget *t, const OFGpuRun *r);

/* GFOpenGPU's error (OGPU_ERR_) for the last batch sent, 0 when it went well. */
long of_gpu_error(const OFGpu *g);

/* The cache's counts, for tests and OF_DrawGlyphs' tags: glyphs filled into
 * the atlas, glyphs drawn from it, glyphs filled straight onto the target,
 * and times the atlas was emptied for room. */
typedef struct OFGpuStats {
    uint32_t rasterised, hits, direct, resets;
} OFGpuStats;
void of_gpu_stats(const OFGpu *g, OFGpuStats *s);

#ifdef __cplusplus
}
#endif

#endif
