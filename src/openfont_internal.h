#ifndef OPENFONT_INTERNAL_H
#define OPENFONT_INTERNAL_H

#include "gfopenfont/gfopenfont.h"

/* One compiled glyph. pixels may be NULL when width and height are 0.
 * Codepoints in a size are first..last, in order, with no gaps. */
typedef struct OFCacheGlyphIn {
    uint32_t codepoint;
    uint32_t glyph_id;
    int32_t left;
    int32_t top;
    int32_t advance_x_26_6;
    uint32_t width;
    uint32_t height;
    const uint8_t *pixels;
} OFCacheGlyphIn;

typedef struct OFCacheSizeIn {
    uint16_t pixel_size;
    const OFCacheGlyphIn *glyphs;
    size_t count;
} OFCacheSizeIn;

int of_cache_write(const char *path, uint64_t fingerprint,
                   uint32_t first_codepoint, uint32_t last_codepoint,
                   const OFCacheSizeIn *sizes, size_t size_count);

/* What the index needs of a face in a file: only the tables that describe
 * it are read. OF_OK and info filled (num_faces too). */
int of_probe_face(OFContext *ctx, const char *path, uint32_t face_index, OFFaceInfo *info);

/* The one Fast RAM block a job must be granted to read the cache. */
void of_cache_memory(const OFCache *cache, const void **addr, size_t *length);

#endif
