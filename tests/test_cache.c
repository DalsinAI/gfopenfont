#include "gfopenfont/gfopenfont.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int same_bitmap(const OFBitmap *a, const OFBitmap *b)
{
    size_t bytes;
    if (a->width != b->width || a->height != b->height ||
        a->left != b->left || a->top != b->top ||
        a->advance_x_26_6 != b->advance_x_26_6 ||
        a->stride != b->stride)
        return 0;
    bytes = (size_t)a->width * a->height;
    return !bytes || !memcmp(a->pixels, b->pixels, bytes);
}

int main(int argc, char **argv)
{
    static const uint16_t sizes[] = {9, 12, 16, 24};
    static const uint32_t cps[] = {'A', 'g', 0x00e9};
    OFContext *ctx = NULL;
    OFFace *face = NULL;
    OFCache *cache = NULL;
    int rc = 1;
    size_t si, ci;

    if (argc != 3) {
        fprintf(stderr, "usage: %s FONT CACHE\n", argv[0]);
        return 2;
    }

    ctx = of_context_create();
    face = ctx ? of_face_open(ctx, argv[1]) : NULL;
    if (!face) {
        fprintf(stderr, "cannot open font\n");
        goto done;
    }

    if (of_compile_cache(face, argv[2], sizes,
                         sizeof sizes / sizeof sizes[0],
                         0x20, 0xff) != OF_OK) {
        fprintf(stderr, "compile cache failed\n");
        goto done;
    }

    cache = of_cache_open(argv[2]);
    if (!cache) {
        fprintf(stderr, "open cache failed\n");
        goto done;
    }
    if (!of_cache_source_fingerprint(cache) ||
        !of_cache_matches_face(cache, face)) {
        fprintf(stderr, "cache fingerprint mismatch\n");
        goto done;
    }

    for (si = 0; si < sizeof sizes / sizeof sizes[0]; ++si) {
        if (of_face_set_pixel_size(face, sizes[si]) != OF_OK)
            goto done;
        for (ci = 0; ci < sizeof cps / sizeof cps[0]; ++ci) {
            OFGlyph shaped[8];
            OFBitmap live, cached;
            size_t n;
            uint32_t gid = 0;

            {
                char utf8[3];
                size_t len;
                if (cps[ci] < 0x80) {
                    utf8[0] = (char)cps[ci];
                    len = 1;
                } else {
                    utf8[0] = (char)(0xc0 | (cps[ci] >> 6));
                    utf8[1] = (char)(0x80 | (cps[ci] & 0x3f));
                    len = 2;
                }
                n = of_shape_utf8(face, utf8, len, shaped, 8);
            }
            if (n != 1) {
                fprintf(stderr, "unexpected shape count\n");
                goto done;
            }
            if (of_raster_glyph_a8(face, shaped[0].glyph_id, &live) != OF_OK)
                goto done;
            if (of_cache_get_a8(cache, sizes[si], cps[ci],
                                &gid, &cached) != OF_OK) {
                of_bitmap_release(&live);
                fprintf(stderr, "cache miss for size/codepoint\n");
                goto done;
            }
            if (gid != shaped[0].glyph_id || !same_bitmap(&live, &cached)) {
                of_bitmap_release(&live);
                of_bitmap_release(&cached);
                fprintf(stderr, "cache/live mismatch\n");
                goto done;
            }
            of_bitmap_release(&live);
            of_bitmap_release(&cached);
        }
    }

    {
        OFBitmap missing;
        if (of_cache_get_a8(cache, 13, 'A', NULL, &missing) != OF_ERR_RANGE) {
            fprintf(stderr, "missing size did not fail\n");
            goto done;
        }
    }

    printf("OPENFONT_CACHE PASS fingerprint=%016llx\n",
           (unsigned long long)of_cache_source_fingerprint(cache));
    rc = 0;

done:
    of_cache_close(cache);
    of_face_close(face);
    of_context_destroy(ctx);
    remove(argv[2]);
    return rc;
}
