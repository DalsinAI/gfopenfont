#include "gfopenfont/gfopenfont.h"
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
    OFContext *ctx;
    OFFace *face;
    uint16_t sizes[64];
    size_t count = 0;
    int rc, i;
    if (argc < 4) {
        fprintf(stderr, "usage: %s FONT OUTPUT SIZE [SIZE ...]\n", argv[0]);
        return 2;
    }
    for (i = 3; i < argc && count < 64; ++i) {
        long n = strtol(argv[i], NULL, 10);
        if (n < 1 || n > 512) {
            fprintf(stderr, "invalid size: %s\n", argv[i]);
            return 2;
        }
        sizes[count++] = (uint16_t)n;
    }
    ctx = of_context_create();
    face = ctx ? of_face_open(ctx, argv[1]) : NULL;
    if (!face) {
        fprintf(stderr, "cannot open font: %s\n", argv[1]);
        of_context_destroy(ctx);
        return 1;
    }
    rc = of_compile_cache(face, argv[2], sizes, count, 0x20, 0xff);
    if (rc != OF_OK)
        fprintf(stderr, "compile failed: %s\n", of_error_string(rc));
    else
        printf("OPENFONT_CACHE %s sizes=%lu range=U+0020..U+00FF\n",
               argv[2], (unsigned long)count);
    of_face_close(face);
    of_context_destroy(ctx);
    return rc == OF_OK ? 0 : 1;
}
