/* openfont_io.h through dos.library, for gfopenfont.library: files read and
 * written whole, read at an offset, a folder's names and its date. Every
 * call runs in the calling process (the library checks it is one).
 *
 * Copyright (c) 2026 AmiCradle Team. OpenFont, MIT licence (LICENSE). */
#include <exec/types.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <proto/exec.h>
#include <proto/dos.h>

#include <stdlib.h>
#include <string.h>

#include "gfopenfont/gfopenfont.h"
#include "openfont_io.h"

/* reads up to this much at a time, so a slow disk shows progress to others */
#define CHUNK (256 * 1024)

static LONG file_size(BPTR fh)
{
    struct FileInfoBlock *fib = (struct FileInfoBlock *)AllocDosObject(DOS_FIB, NULL);
    LONG n = -1;
    if (!fib)
        return -1;
    if (ExamineFH(fh, fib))
        n = fib->fib_Size;
    FreeDosObject(DOS_FIB, fib);
    return n;
}

int of_io_read_all(const char *path, uint8_t **data, size_t *size)
{
    BPTR fh;
    LONG n, done = 0;
    uint8_t *buf;

    if (!path || !data || !size)
        return OF_ERR_ARGUMENT;
    *data = NULL;
    *size = 0;
    fh = Open((CONST_STRPTR)path, MODE_OLDFILE);
    if (!fh)
        return OF_ERR_IO;
    n = file_size(fh);
    if (n < 0) {
        Close(fh);
        return OF_ERR_IO;
    }
    buf = (uint8_t *)malloc(n ? (size_t)n : 1);
    if (!buf) {
        Close(fh);
        return OF_ERR_MEMORY;
    }
    while (done < n) {
        LONG want = n - done > CHUNK ? CHUNK : n - done;
        LONG got = Read(fh, buf + done, want);
        if (got <= 0)
            break;
        done += got;
    }
    Close(fh);
    if (done != n) {
        free(buf);
        return OF_ERR_IO;
    }
    *data = buf;
    *size = (size_t)n;
    return OF_OK;
}

int of_io_write_all(const char *path, const uint8_t *data, size_t size)
{
    BPTR fh;
    LONG done = 0;
    if (!path || (size && !data))
        return OF_ERR_ARGUMENT;
    fh = Open((CONST_STRPTR)path, MODE_NEWFILE);
    if (!fh)
        return OF_ERR_IO;
    while ((size_t)done < size) {
        LONG want = (LONG)size - done > CHUNK ? CHUNK : (LONG)size - done;
        LONG put = Write(fh, (APTR)(data + done), want);
        if (put != want)
            break;
        done += put;
    }
    if (!Close(fh) || (size_t)done != size)
        return OF_ERR_IO;
    return OF_OK;
}

struct OFFile {
    BPTR fh;
    LONG pos;
};

OFFile *of_io_open(const char *path, uint32_t *size)
{
    OFFile *f;
    LONG n;
    if (!path)
        return NULL;
    f = (OFFile *)malloc(sizeof(*f));
    if (!f)
        return NULL;
    f->fh = Open((CONST_STRPTR)path, MODE_OLDFILE);
    if (!f->fh) {
        free(f);
        return NULL;
    }
    n = file_size(f->fh);
    if (n < 0) {
        Close(f->fh);
        free(f);
        return NULL;
    }
    f->pos = 0;
    if (size)
        *size = (uint32_t)n;
    return f;
}

uint32_t of_io_read_at(OFFile *f, uint32_t offset, void *buf, uint32_t count)
{
    LONG got;
    if (!f || !count || !buf)
        return 0;
    if ((LONG)offset != f->pos) {
        if (Seek(f->fh, (LONG)offset, OFFSET_BEGINNING) < 0)
            return 0;
        f->pos = (LONG)offset;
    }
    got = Read(f->fh, buf, (LONG)count);
    if (got < 0) {
        f->pos = -1;            /* unknown: seek next time */
        return 0;
    }
    f->pos += got;
    return (uint32_t)got;
}

void of_io_close(OFFile *f)
{
    if (!f)
        return;
    Close(f->fh);
    free(f);
}

int of_io_list_dir(const char *dir, OFDirFn fn, void *arg)
{
    BPTR lock;
    struct FileInfoBlock *fib;
    char path[512];
    int rc = OF_OK;

    if (!dir || !fn)
        return OF_ERR_ARGUMENT;
    lock = Lock((CONST_STRPTR)dir, SHARED_LOCK);
    if (!lock)
        return OF_ERR_IO;
    fib = (struct FileInfoBlock *)AllocDosObject(DOS_FIB, NULL);
    if (!fib) {
        UnLock(lock);
        return OF_ERR_MEMORY;
    }
    if (Examine(lock, fib) && fib->fib_DirEntryType > 0) {
        for (;;) {
            if (!ExNext(lock, fib)) {
                /* the end, or a fault part way: only the end is a full list */
                if (IoErr() != ERROR_NO_MORE_ENTRIES)
                    rc = OF_ERR_IO;
                break;
            }
            if (fib->fib_DirEntryType >= 0)
                continue;       /* a folder, or a link to one */
            if (of_io_join(path, sizeof path, dir, (const char *)fib->fib_FileName) != OF_OK)
                continue;
            rc = fn(path, arg);
            if (rc)
                break;
        }
    } else {
        rc = OF_ERR_IO;
    }
    FreeDosObject(DOS_FIB, fib);
    UnLock(lock);
    return rc;
}

int of_io_stamp(const char *path, uint32_t stamp[3])
{
    BPTR lock;
    struct FileInfoBlock *fib;
    int rc = OF_ERR_IO;
    if (!path || !stamp)
        return OF_ERR_ARGUMENT;
    lock = Lock((CONST_STRPTR)path, SHARED_LOCK);
    if (!lock)
        return OF_ERR_IO;
    fib = (struct FileInfoBlock *)AllocDosObject(DOS_FIB, NULL);
    if (fib) {
        if (Examine(lock, fib)) {
            stamp[0] = (uint32_t)fib->fib_Date.ds_Days;
            stamp[1] = (uint32_t)fib->fib_Date.ds_Minute;
            stamp[2] = (uint32_t)fib->fib_Date.ds_Tick;
            rc = OF_OK;
        }
        FreeDosObject(DOS_FIB, fib);
    }
    UnLock(lock);
    return rc;
}
