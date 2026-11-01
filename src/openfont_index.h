/* GFOpenFont's font index, in place of fontconfig: what fonts are in the
 * folders GFOpenFont looks in, found by family, weight and slant, with the
 * aliases and generic families (sans-serif, serif, monospace) in one place.
 *
 * The index is kept in a small text file, rebuilt when a folder's date says
 * something in it changed. Faces are read through a stream, so indexing a
 * large font reads only its header and the tables the index needs.
 *
 * Copyright (c) 2026 AmigaChrome Team. OpenFont, MIT licence (LICENSE). */
#ifndef OPENFONT_INDEX_H
#define OPENFONT_INDEX_H

#include "gfopenfont/gfopenfont.h"

#define OF_INDEX_MAX_DIRS 16
#define OF_INDEX_PATH 256

typedef struct OFIndexEntry {
    char path[OF_INDEX_PATH];
    uint32_t face_index;            /* in a collection */
    char family[64];
    char style[64];
    uint16_t weight;
    uint16_t width;
    uint16_t flags;                 /* OF_FACE_* */
    uint16_t pad;
    uint32_t unicode_ranges[4];
} OFIndexEntry;

typedef struct OFIndexDir {
    char path[OF_INDEX_PATH];
    uint32_t stamp[3];
    int present;                    /* could be read when last scanned */
} OFIndexDir;

/* The characters an entry's face maps, worked out the first time a
 * character lookup needs them, so later lookups open only faces that may
 * have the character. Kept in memory only. */
typedef struct OFCoverage {
    uint32_t *ranges;               /* [first, last] pairs, ascending */
    uint32_t count;
    int known;
} OFCoverage;

typedef struct OFIndex {
    OFIndexEntry *entries;
    size_t count, cap;
    OFCoverage *coverage;           /* one per entry, or NULL */
    size_t coverage_count;
    OFIndexDir dirs[OF_INDEX_MAX_DIRS];
    size_t ndirs;
    char aliases[2048];             /* "name=family,family\n" lines, the user's own */
    char fallbacks[256];            /* "family,family": the default chain */
} OFIndex;

void of_index_init(OFIndex *ix);
void of_index_free(OFIndex *ix);

/* Adds a folder to look in. OF_ERR_RANGE when there are OF_INDEX_MAX_DIRS. */
int of_index_add_dir(OFIndex *ix, const char *dir);

/* Reads the index file. OF_OK, or OF_ERR_IO / OF_ERR_CACHE when there is
 * none or it can't be used (then rescan). Folders already added stay; the
 * file's entries are kept only for folders still in the list. */
int of_index_load(OFIndex *ix, const char *path);
int of_index_save(const OFIndex *ix, const char *path);

/* 1 when a folder's date differs from the one indexed, or it was never
 * scanned: of_index_scan then brings the index up to date. */
int of_index_stale(const OFIndex *ix);

/* Rescans every folder with ctx (GFOpenFont reads the faces' tables). */
int of_index_scan(OFIndex *ix, OFContext *ctx);

/* The best face for a family, weight (100-900) and slant: the family
 * itself, then the aliases (the user's first, then GFOpenFont's own), then
 * the generic family it belongs to, then sans-serif. NULL with no fonts. */
const OFIndexEntry *of_index_find(const OFIndex *ix, const char *family,
                                  uint16_t weight, int italic);

/* The family itself or one of its aliases, as of_index_find tries them
 * first; NULL when neither is installed, so that a program with its own
 * list of families (CSS font-family) can go on to the next. */
const OFIndexEntry *of_index_find_exact(const OFIndex *ix, const char *family,
                                        uint16_t weight, int italic);

/* Sets the user's aliases ("Name=Family,Family" lines) and the default
 * fallback chain ("Family,Family"). Either may be NULL to keep it. */
void of_index_configure(OFIndex *ix, const char *aliases, const char *fallbacks);

/* Opens the best face for family and adds the default fallback chain to
 * it, skipping the face itself and families not installed. When it fails,
 * *error (if error is not NULL) says why: OF_ERR_FONT with no fonts
 * indexed, or what opening the face gave. */
OFFace *of_index_open(const OFIndex *ix, OFContext *ctx, const char *family,
                      uint16_t weight, int italic, int with_fallbacks, int *error);

/* of_index_open, and with exact set only the family or its aliases
 * (of_index_find_exact): *error OF_ERR_FONT when neither is installed. */
OFFace *of_index_open_family(const OFIndex *ix, OFContext *ctx, const char *family,
                             uint16_t weight, int italic, int with_fallbacks, int exact,
                             int *error);

/* Opens a face that has a glyph for codepoint: the default fallback
 * chain's families first, then every family's best face for weight and
 * slant, in the index's order. NULL (*error OF_ERR_FONT) when none has it. */
OFFace *of_index_open_char(OFIndex *ix, OFContext *ctx, uint32_t codepoint,
                           uint16_t weight, int italic, int *error);

#endif
