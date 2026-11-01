/* gfopenfont.library's Amiga fonts: OF_OpenTextFont's work (of_textfont.c).
 * Copyright (c) 2026 AmigaChrome Team. OpenFont, MIT licence (LICENSE). */
#ifndef OF_TEXTFONT_H
#define OF_TEXTFONT_H

#include <exec/semaphores.h>
#include <graphics/text.h>

#include "gfopenfont/gfopenfont.h"
#include "textfont/diskfont_core.h"

struct OFTextFonts {
    struct SignalSemaphore sem;     /* the fonts this library loaded: ctx.loaded */
    struct odf_ctx ctx;
    OFContext *of;                  /* GFOpenFont's context, for the outlines */
};

/* lib_init: graphics.library, and the font list; 0 when graphics won't open */
int of_textfont_setup(struct OFTextFonts *t);
/* lib_free */
void of_textfont_cleanup(void);
/* GFOpenDiskFont's work on FONTS: (a process only, under the library's lock
 * and on its stack): an Amiga font, bitmap or outline, as a TextFont
 * closed with graphics' CloseFont. Outline fonts are drawn by GFOpenFont. */
struct TextFont *of_textfont_open(struct OFTextFonts *t, OFContext *of, const struct TextAttr *ta);
/* the fonts nobody has open go; non-zero when none of ours is left */
int of_textfont_flush(struct OFTextFonts *t);

#endif
