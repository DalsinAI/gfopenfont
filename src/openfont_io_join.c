/* of_io_join: one folder and one name, the way AmigaOS and POSIX both read
 * them ("FONTS:" and "dir/" take no separator). Copyright (c) 2026
 * AmiCradle Team. GFOpenFont, MIT licence (LICENSE). */
#include "gfopenfont/gfopenfont.h"
#include "openfont_io.h"

#include <string.h>

int of_io_join(char *out, size_t size, const char *dir, const char *name)
{
    size_t d, n;
    int sep;
    if (!out || !dir || !name)
        return OF_ERR_ARGUMENT;
    d = strlen(dir);
    n = strlen(name);
    /* "FONTS:" and "dir/" need no separator */
    sep = d && dir[d - 1] != '/' && dir[d - 1] != ':';
    if (d + (size_t)sep + n + 1 > size)
        return OF_ERR_BUFFER;
    memcpy(out, dir, d);
    if (sep)
        out[d++] = '/';
    memcpy(out + d, name, n + 1);
    return OF_OK;
}
