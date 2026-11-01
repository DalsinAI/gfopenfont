# OpenFont design

Version 0.3 — 17 October 2026 (0.2: the resident library; 0.1, 8 October:
§1–§5, and the tests)

## 1. Decision

OpenFont is a first-class Amiga system library. It replaces direct
application-level use of FreeType, HarfBuzz and fontconfig in the Open stack.

The public dependency direction is:

```text
application -> openfont.library -> OpenGfx -> OpenGPU
```

OpenFont's own code replaces FreeType and HarfBuzz inside it, in stages, each
from the published specifications only (the OpenType specification, the
Unicode Standard), never from their code:

1. the font reader and the rasteriser (FreeType out): done, 17 October 2026;
2. the shaper for Latin (cmap, GPOS pair kerning and `kern`, GSUB ligatures,
   simple mark attachment), HarfBuzz out: done, 17 October 2026 (on main
   with openfont.library 1.3, 23 October 2026); scripts, languages, right to
   left, joining and features for WebCore: done, 23 October 2026; colour
   glyphs from CBDT and sbix PNG images, decoded through the PNG datatype
   (libpng out): openfont.library 1.4, 23 October 2026; glyph runs drawn
   through OpenGPU from a glyph cache in the library (cairo's glyph cache
   out of WebKit): openfont.library 1.5, 1 November 2026;
3. the text functions: UTF-8/16, Unicode properties from the UCD, line breaks
   (UAX #14), grapheme and word boundaries (UAX #29), NFC and IDNA, so
   OpenFont is the one place for them and can later take ICU's place for
   WebKit.

Their APIs are not the architecture: the core API stays as it is.

## 2. Responsibilities

OpenFont owns font discovery and substitution, TTF/OTF and Amiga font
integration, Unicode fallback, shaping, metrics, variable-font coordinates,
colour glyph selection, memory caches and persistent compiled caches.

OpenFont does not own drawing state, clipping, layers or compositing. Those
belong to OpenGfx. It does not own GPU policy. OpenGfx/OpenGPU decide execution.

## 3. Compiled font instances

A compiled instance is keyed by face identity, variation coordinates, pixel
size and raster flags. It can contain character-to-glyph mapping, metrics,
position-independent glyph metrics and A8/MONO1/RGBA glyph bitmaps.

Common sizes may be precompiled by `OpenFontCompile`. Uncommon sizes are
created lazily and may be persisted. Cache files are endian-stable and include
a source checksum so stale font data is never reused.

## 4. Acceleration

The reference implementation runs as ordinary 68040 C/C++. AC090 may replace
high-value operations with host leaves while preserving identical results:

- shape a UTF-8 run;
- rasterise A8 glyph;
- rasterise mono glyph;
- rasterise colour glyph;
- build/copy an atlas.

Shaping is not a GPU operation. Positioned glyph runs are handed to OpenGfx,
which may batch their masks through OpenGPU.

## 5. OS 3.2.3 integration

OpenFont patches **nothing** in AmigaOS.

OpenGfx is the sole owner of the four Open-stack `graphics.library` patches: `Text`, `RectFill`, `BltBitMap` and `ScrollRaster`.

When the OpenGfx `Text` patch runs, it calls OpenFont for shaping, metrics and cached MONO1/A8/RGBA glyph data, then OpenGfx performs the actual drawing. OpenFont never installs a `Text`, `TextLength`, `TextExtent` or `TextFit` patch.

Modern applications call OpenFont directly when they need shaped measurement. Classic measurement calls remain OS-owned.

## 6. The resident library

`openfont.library` is built for AmigaOS 3.2 on the 68040 with its FPU, with
OpenAmigaGCC 16.2.0-open2 (`-m68040 -m68881`), and does not open unless exec
reports both the CPU and its FPU: not on another CPU, nor on an LC040. Version 1 is a
preview: its calls and structures may change. Version 2 freezes them, once the
tests in §7 pass on a lab and on real 68040 hardware.

**One lock.** The core keeps state in the faces and the library, so
every call takes one SignalSemaphore and runs alone. The semaphore nests, so a
task already inside the library does not wait for itself.

**Its own stack.** Shaping and rasterising need more stack
than an Amiga program is given. Each outermost call swaps (`StackSwap`) to a
64 KB stack the library owns, does its work, and swaps back, so callers need no
more than their usual.

**One pool.** Everything the core allocates comes from
one Exec pool, with an eight-byte header per block for its size and a check
word. Expunging the library deletes the pool, so nothing it allocated
outlives it. The library has no C startup, no stdio and no constructors: libnix
gives it only stateless functions.

**Files only for processes.** The calls that read files (opening a face or a
cache, finding a face, adding a font folder, rescanning) use dos.library, so
they need a process. A
plain task gets `OFERR_NOTPROCESS`. Shaping, metrics and rasterising use only
memory, and any task may call them. A face's font file is read whole into
memory when it opens.

**The font index.** Finding a face by family needs every installed face's
names, which means opening each font. The index does that once and keeps the
answer in `ENVARC:OpenFont/FontIndex`, a text file of folders (with their
dates) and faces (path, face index, family, style, weight, width, slant),
ending with a count of the faces: a file cut short is not used. When a
folder's date differs from the index's (a font added or taken away), the
folders are read again. A folder that could not be listed whole, or a font in
it that could not be opened just then (being copied in), leaves the index
stale, so the next look reads the folders again; running out of memory leaves
it all stale. Finding a face tries the family, then the user's
aliases (`ENVARC:OpenFont/Aliases`), then OpenFont's own, then `sans-serif`.
Among a family's faces the right slant counts most, then the nearest weight,
and a wide or narrow face is marked down.

**Fallback by cluster.** A face holds a chain of up to eight others. Shaping
runs the face over the whole run. A cluster is missing when any of its glyphs
is glyph 0; each stretch of missing clusters is shaped again with the next face
of the chain, given the whole run as context and the script and direction
guessed for the whole run (a lone emoji or mark can't tell its own), and so on
down the chain. What no face has is the first face's own shaping of it, its
glyph 0 marked missing. Fallbacks may not form a loop, however long: each face
holds its fallbacks open. Each glyph says which face of the chain it came from.
A chain is one level deep: a fallback's own fallbacks are not used. A fallback
shapes at the pixel size of the face it serves, and is set to it again when a
program asks for it (`OF_GetFallback`) to draw its glyphs, since one face may
serve several.

**The cache's fingerprint.** A compiled cache names the font it was compiled
from by a hash of its bytes. The hash is worked out the first time a face is
checked against a cache, not when it opens.

## 7. Testing

The reference gate includes:

- glyph-for-glyph HarfBuzz comparison (`compare_harfbuzz`): glyphs,
  clusters, advances, offsets and unsafe-to-break flags exact on Latin text;
- the reader and rasteriser against FreeType 2.14.3 unhinted
  (`compare_freetype`): cmap, names, flags, line metrics, advances and bitmap
  boxes exact, pixels within 64 of 255 and a mean within 2.5;
- the reader on damaged fonts (`test_reader`, under the sanitisers), and its
  bitmaps the same to the bit on the 68040 as on the host;
- cache round-trip and corruption tests;
- big-endian cache parsing;
- UTF-8, bidi, combining marks and fallback cases;
- the font index: aliases, weights, generic families and fallback, on the host
  (`test_index`);
- resident-library ABI tests on OS 3.2.3 (`OpenFontLibTest`: the host's
  glyphs and pixels byte for byte, the structures' sizes, the index, fallback,
  and a plain task refused);
- AC090 accelerated/reference differential tests.

Correctness is a prerequisite for acceleration.
