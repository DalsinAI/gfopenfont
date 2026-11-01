/* What GFOpenFont's core asks of the system it runs on: whole files in and
 * out, the names in a folder, and when a file or folder last changed.
 * Programs (the host, and 68k programs with libnix) use openfont_io_stdio.c
 * and openfont_io_posixdir.c; gfopenfont.library uses library/of_io_dos.c,
 * which goes through dos.library and owns no C library state.
 *
 * Copyright (c) 2026 AmiCradle Team. OpenFont, MIT licence (LICENSE). */
#ifndef OPENFONT_IO_H
#define OPENFONT_IO_H

#include <stddef.h>
#include <stdint.h>

/* The whole file into a malloc-owned buffer. OF_OK, OF_ERR_IO, OF_ERR_MEMORY. */
int of_io_read_all(const char *path, uint8_t **data, size_t *size);

/* Writes the whole file, replacing it. OF_OK or OF_ERR_IO. */
int of_io_write_all(const char *path, const uint8_t *data, size_t size);

/* Calls fn with each plain file's full path in dir, in no set order. A
 * non-zero answer from fn stops the walk and is returned. OF_ERR_IO when
 * dir can't be read. */
typedef int (*OFDirFn)(const char *path, void *arg);
int of_io_list_dir(const char *dir, OFDirFn fn, void *arg);

/* When path last changed, as three words that compare equal only when
 * nothing changed (days, minutes, ticks on the Amiga; seconds and
 * nanoseconds on the host). OF_OK or OF_ERR_IO. */
int of_io_stamp(const char *path, uint32_t stamp[3]);

/* A file opened for reading at any offset (the index reads only
 * the tables that describe a face this way). */
typedef struct OFFile OFFile;
OFFile *of_io_open(const char *path, uint32_t *size);
/* Reads count bytes at offset into buf. Returns the bytes read. */
uint32_t of_io_read_at(OFFile *f, uint32_t offset, void *buf, uint32_t count);
void of_io_close(OFFile *f);

/* dir and name joined with the system's separator, into out (size bytes).
 * OF_OK, or OF_ERR_BUFFER when it doesn't fit. */
int of_io_join(char *out, size_t size, const char *dir, const char *name);

#endif
