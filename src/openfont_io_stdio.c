/* openfont_io.h's files through C's stdio: for the host, and for 68k
 * programs linked with libnix. Copyright (c) 2026 AmiCradle Team. MIT. */
#include "gfopenfont/gfopenfont.h"
#include "openfont_io.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int of_io_read_all(const char *path, uint8_t **data, size_t *size)
{
    FILE *f;
    long n;
    uint8_t *buf;

    if (!path || !data || !size)
        return OF_ERR_ARGUMENT;
    *data = NULL;
    *size = 0;
    f = fopen(path, "rb");
    if (!f)
        return OF_ERR_IO;
    if (fseek(f, 0, SEEK_END) || (n = ftell(f)) < 0 || fseek(f, 0, SEEK_SET)) {
        fclose(f);
        return OF_ERR_IO;
    }
    buf = (uint8_t *)malloc(n ? (size_t)n : 1);
    if (!buf) {
        fclose(f);
        return OF_ERR_MEMORY;
    }
    if (n && fread(buf, 1, (size_t)n, f) != (size_t)n) {
        fclose(f);
        free(buf);
        return OF_ERR_IO;
    }
    fclose(f);
    *data = buf;
    *size = (size_t)n;
    return OF_OK;
}

int of_io_write_all(const char *path, const uint8_t *data, size_t size)
{
    FILE *f;
    if (!path || (size && !data))
        return OF_ERR_ARGUMENT;
    f = fopen(path, "wb");
    if (!f)
        return OF_ERR_IO;
    if (size && fwrite(data, 1, size, f) != size) {
        fclose(f);
        return OF_ERR_IO;
    }
    return fclose(f) ? OF_ERR_IO : OF_OK;
}

struct OFFile {
    FILE *f;
};

OFFile *of_io_open(const char *path, uint32_t *size)
{
    OFFile *h;
    long n;
    if (!path)
        return NULL;
    h = (OFFile *)malloc(sizeof(*h));
    if (!h)
        return NULL;
    h->f = fopen(path, "rb");
    if (!h->f) {
        free(h);
        return NULL;
    }
    if (fseek(h->f, 0, SEEK_END) || (n = ftell(h->f)) < 0) {
        fclose(h->f);
        free(h);
        return NULL;
    }
    if (size)
        *size = (uint32_t)n;
    return h;
}

uint32_t of_io_read_at(OFFile *h, uint32_t offset, void *buf, uint32_t count)
{
    if (!h || (count && !buf) || fseek(h->f, (long)offset, SEEK_SET))
        return 0;
    return count ? (uint32_t)fread(buf, 1, count, h->f) : 0;
}

void of_io_close(OFFile *h)
{
    if (!h)
        return;
    fclose(h->f);
    free(h);
}
