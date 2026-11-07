# GFOpenFont

GFOpenFont is the native modern font service for the GFOpenAmiga stack.

It gives Amiga applications one API for font discovery, shaping, measurement,
glyph rasterisation and compiled glyph caches. The production architecture
removes direct application dependencies on FreeType, HarfBuzz and fontconfig.
GFOpenFont reads, shapes and draws fonts with its own code, written from the
published specifications (the OpenType specification, the Unicode
Standard).

Current phase: **the resident library, a preview**. `gfopenfont.library` 1.5 is
GFOpenFont for AmigaOS 3.2 on the 68040, with its own font reader, rasteriser
and shaper inside it. It finds fonts by family through its own font index, and a
face falls back to others for the characters it has no glyphs for. Its calls
may still change until version 2. The same core builds on the x86 and ARM64
cores, where the tests run. Glyph lookup in a compiled cache does not walk the
file, and a range of glyphs can run on extra cores or, with no cores board, as
a plain loop on this CPU.

## Architecture

```text
GFOpenBrowser / GFOpenWrite / GFOpenLayout / native applications
                         |
                     GFOpenFont
       discovery | shaping | metrics | glyph cache
                         |
                     GFOpenGfx
                         |
                     GFOpenGPU
          68040 | AC090 | x86/ARM64 | GPU
```

GFOpenFont owns text semantics. GFOpenGfx paints positioned glyph runs. GFOpenGPU is
only the execution/acceleration layer and does not understand Unicode shaping.

## The core

`include/gfopenfont/gfopenfont.h` is the core the library is built from, and what
the host tests call:

- faces opened from a file or from memory, one face of a collection, counted
  so a face can be shared;
- pixel-size selection, and a face's names and metrics (`of_face_info`);
- UTF-8 shaping to positioned glyph runs, and measurement;
- fallback: up to eight faces after a face, each cluster it has no glyphs for
  shaped by the first that has them, and each glyph marked with the face it
  came from;
- A8 glyph rasterisation;
- a portable compiled bitmap-cache writer and reader.

### The reader and the rasteriser

[src/of_font.h](src/of_font.h) is GFOpenFont's own font reader and outline
rasteriser, written from the OpenType specification and Adobe's CFF and
Type 2 charstring specifications. No FreeType code is in them, copied or
translated.

- **Fonts:** TrueType and OpenType files and collections (`.ttf`, `.otf`,
  `.ttc`, `.otc`). Tables `cmap` (formats 0, 4, 6, 12, 13), `head`, `hhea`,
  `hmtx`, `maxp`, `loca`/`glyf` (simple and composite glyphs), `CFF `
  (Type 2 charstrings, subroutines, CID-keyed fonts), `name`, `OS/2` and
  `post`. A font without outlines it reads (CFF2, bitmaps only) still opens,
  for its cmap and metrics.
- **Rasteriser** ([src/of_raster.c](src/of_raster.c)): quadratic and cubic
  outlines to A8 coverage with the non-zero rule, exact area coverage at 256
  subpixels a pixel, curves cut into lines no more than 1/16 pixel from them.
  No hinting. Integer arithmetic only, the same to the bit on the host and the
  68040, and a pure function of an outline, so it can move to GFOpenGPU as it is.
- **Metrics:** advances and the line metrics in 26.6 at a pixel size, not
  rounded to whole pixels; a glyph's bitmap is its outline's control box
  rounded out.

Every read is checked against the file's end, and a damaged font gives
`OF_ERR_FONT`, never a crash: `test_reader` opens and draws fonts cut short
and with bytes changed, under the address and undefined-behaviour sanitisers
on the host.

Glyphs are unhinted: the pixels follow the outline exactly at every size. That
is a change from the hinted FreeType output before.

### The shaper

[src/of_shape.c](src/of_shape.c) and [src/of_layout.c](src/of_layout.c) turn
UTF-8 into positioned glyphs, written from the OpenType specification and the
Unicode Standard, with no HarfBuzz code:

- UTF-8 decoded with each byte that starts no valid sequence shown as one
  U+FFFD; clusters as byte offsets, marks and joiners in the cluster before;
- characters the font lacks decomposed into ones it has, marks in canonical
  order, base and marks composed again where the font has the result;
- spaces the font lacks drawn as its space, as wide as each should be;
- GSUB (all eight lookup types) with the default features: `ccmp`, `locl`,
  `rlig`, `calt`, `clig`, `liga`, `rclt`, and `frac`/`numr`/`dnom` around
  U+2044; GPOS (all nine) with `kern`, `mark`, `mkmk`, `curs`, `dist`,
  `abvm`, `blwm`, and the old `kern` table when GPOS has no kerning; GDEF
  classes and mark filtering; device tables at the pixel size;
- marks' advances zeroed and marks placed on their bases' anchors;
- the default ignorables (ZWJ, ZWNJ, soft hyphen...) hidden; ZWNJ keeps
  ligatures apart;
- the unsafe-to-break flags.

Since gfopenfont.library 1.3 (23 October 2026) it also takes what WebCore
asks for (`of_shape`, `of_shape_utf16`, `OF_ShapeA`):

- UTF-16 text, clusters in 16-bit units;
- a script (ISO 15924), a language system (`locl` and the font's own forms),
  a direction, and features turned on, off or to an alternate, over all of
  the text or part of it;
- right to left: mirrored characters take their mirrors, and the glyphs come
  in visual order, as HarfBuzz gives them;
- joining scripts (Arabic, Syriac, N'Ko, Mongolian...): `isol`, `fina`,
  `medi`, `init` from the joining types of ArabicShaping.txt, in their own
  stages before `rlig` and `calt`;
- Hebrew points and Arabic harakat in the order fonts are made for (shadda
  before the other harakat), not canonical order.

The Indic scripts are still shaped without their reordering.

The faces give WebCore what it measures and draws with: metrics (x and
capital height, underline, strikeout), glyph outlines as move/line/quad/cubic
verbs, ink boxes, raw tables (`MATH`, `OS/2`), faces from memory (web
fonts), and a face for a character the fonts in use don't have.

[tests/ref/compare_uharfbuzz.py](tests/ref/compare_uharfbuzz.py) compares
these with HarfBuzz through the uharfbuzz module (`pip install uharfbuzz`),
at the font's units per em so nothing is rounded:

```sh
python3 tests/ref/compare_uharfbuzz.py build/shapeopts FONT auto - - - tests/shape/rtl.txt
python3 tests/ref/compare_uharfbuzz.py build/shapeopts FONT auto - - -liga,salt=2 tests/shape/latin.txt utf16
```

On 23 October 2026 (uharfbuzz 0.56.4) [tests/shape/rtl.txt](tests/shape/rtl.txt)
(Hebrew with points, Arabic with harakat, lam-alef, Persian, Urdu, ZWJ and
ZWNJ, mirrored brackets) gave the same glyphs, clusters, advances and offsets
in DejaVu Sans, FreeSerif, FreeSans and FreeMono, and
[tests/shape/latin.txt](tests/shape/latin.txt) the same in DejaVu Sans,
Liberation Sans and Carlito with `-kern`, `-liga`, `liga` over part of each
line, `onum`, `onum` over part, `salt=1`, `salt=2`, `smcp`, `c2sc`, `ss01`,
`dlig`, `frac` and `-ccmp`, as UTF-8 and UTF-16, and with the Serbian
language system. The differences left: .notdef marks in fonts without GPOS
(HarfBuzz's own rule), and a 1/16 em space rounded the other way at
1000 units per em.

[src/of_unicode.h](src/of_unicode.h) gives the Unicode properties it needs:
general category, combining class, script, default ignorables, canonical
decomposition and composition. `src/of_unicode_data.c` is generated from the
Unicode Character Database 16.0.0 by
[tools/gen_unicode.py](tools/gen_unicode.py) (about 100 KB of tables); the
data is under the Unicode License v3
([LICENSES/Unicode-License-v3.txt](LICENSES/Unicode-License-v3.txt)).

[src/openfont_index.h](src/openfont_index.h) is the font index: the faces of
a few folders by family, style, weight and slant, kept in a text file and read
again only when a folder's date changes; aliases (`Arial` to Liberation Sans,
`monospace` to DejaVu Sans Mono, the user's own); and the default fallback
chain.

Files are read and folders listed through
[src/openfont_io.h](src/openfont_io.h): stdio and POSIX on the host,
dos.library in the library.

The cache writer is useful before anything else: common font sizes can be
compiled once so the 68k does no font parsing or rasterisation on a cache
hit.

## Build on the x86 or ARM64 cores

Requirements: CMake and a C compiler; nothing else.
The index test needs the DejaVu and Liberation fonts (`fonts-dejavu-core`,
`fonts-liberation2` on Debian and Ubuntu), and is left out without them.

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

The reference gate compares the reader and rasteriser with FreeType 2.14.3,
unhinted, when given a FreeType tree built in its own `build/` folder (only
for this test; nothing of FreeType's is linked into GFOpenFont):

```sh
cmake -S . -B build -DOPENFONT_FREETYPE_REF=/path/to/freetype-2.14.3
build/compare_freetype FONT...
```

For each font, face and size (9, 12, 16, 24 and 48 pixels) it must match
FreeType exactly in the cmap (every codepoint to U+2FFFF), the family and
style names, the bold, italic, fixed-pitch and colour flags, the line metrics
and every glyph's advance, bitmap size and place; and each pixel within 64 of
255, the mean difference within 2.5. The pixels differ because the two cut
curves into lines differently: measured against a near-exact raster (1/256
pixel), GFOpenFont is the closer, FreeType's own error reaching 57 in a pixel
and a mean of 2.6 on CFF fonts at 9 pixels.

On 17 October 2026 it passed on 63 font files of this PC: DejaVu and
Liberation (TrueType), the URW base 35 fonts (CFF), and the Noto CJK
collections (CID-keyed CFF, two faces of each), 335 font sizes.

The shaper's gate compares it with HarfBuzz 14.5.1, given HarfBuzz's release
sources (built for the test only):

```sh
cmake -S . -B build -DOPENFONT_HARFBUZZ_REF=/path/to/harfbuzz-14.5.1
build/compare_harfbuzz TEXTFILE FONT...
```

Each line of the text, and some broken UTF-8, at 12, 16 and 33 pixels:
HarfBuzz (its OpenType functions, GFOpenFont's advances, script and direction
guessed) and GFOpenFont must give the same glyphs, clusters, advances, offsets
and unsafe-to-break flags. Right-to-left runs are counted apart. One
difference is allowed: HarfBuzz places a .notdef mark in a font with no GPOS
by a rule of its own, which GFOpenFont does not copy.

On 17 October 2026 it passed on 58 fonts (DejaVu, Liberation, the URW
fonts, Noto Sans CJK) with [tests/shape/latin.txt](tests/shape/latin.txt)
(5,742 runs), [tests/shape/latin-random.txt](tests/shape/latin-random.txt)
(53,418 runs: letters, accents composed and decomposed, ligatures, kerning
pairs, fractions, hidden characters) and this README and DESIGN.md as text
(62,466 runs). The only differences were .notdef marks in Nimbus Mono PS,
which has no GPOS.

Compile common sizes:

```sh
./build/openfontcompile /path/to/font.ttf font.ofc 9 10 11 12 14 16 18 24 32
```

The file format is big-endian. Version 3 (what `openfontcompile` writes now)
carries a size directory, so a glyph is a couple of loads. Version 1 and 2
caches still open.

## Fast path: 68k fallback and GFOpenMulticore

`of_cache_fill_range` fills one `OFRangeSlot` per codepoint. The slot is a
multiple of 16 bytes, so it is a legal GFOpenMulticore slice: index `i` writes
only `slots[i]`. The worker does not call the OS and does not allocate.

- No `OFExec`, or fewer glyphs than the break-even (8): the worker runs in a
  loop on this CPU. That is the 68k fallback, and it is the reference.
- An `OFExec` runs the same worker. On Amiga, [src/openfont_omc.c](src/openfont_omc.c)
  passes it to `OMC_RunRange` (`gfopenmulticore.library` 0.3 or later). A cores
  board spreads the indices. No board, an older library, or a refused job falls
  back to the loop. The call does not set `OMCF_BOARD`, so a plain A1200 still
  gets the glyphs.

A cache hit does no rasterisation on either path. A glyph bigger than
`OF_RANGE_PIXELS` (4096 bytes) stays a per-slot `OF_ERR_BUFFER`;
`of_cache_get_a8` on this CPU still returns it. Shaping of one run stays on
this CPU. Independent ranges are what the extra cores are for.

The cache bytes and its index are one Fast RAM block (`of_cache_memory` in
the internal header). Grant that block read-only and the slots write. Leave
the job permissive.

## 68k build

[amiga/build.sh](amiga/build.sh) builds the range test with GFOpenAmigaGCC
**16.2.0-open2**, the compiler in the `os32-gcc16` stove. Nothing in the
GFOpenAmigaGCC releases is named 16.2.9-open2. The script looks in
`~/AmiCradle/stoves/os32-gcc16`, then `/opt/openamigagcc-16.2.0-open2-x86_64-linux`.
`CC` overrides that. It compiles the sources for the 68040 only: `-m68040`,
`-O2 -Wall -Wextra -Werror`, soft float through `toolchain/m68k-soft-m68040.specs`
(libnix's soft maths, not libm881), so it also runs on an LC or EC040.

It also builds `openfontreader-68040`, the reader and rasteriser's test
(`tests/test_reader.c`). It prints a fingerprint of every glyph's bitmap, place
and advance at 12 and 16 pixels for each font. On lab t4-a (OS 3.2.3, 17 Oct
2026) it printed the host's fingerprints for DejaVu Sans, Liberation Serif
Italic and Nimbus Sans (CFF), and `test_reader: ok`.

`src/openfont_omc.c` is not in that build unless `OMC_INC` points at a
directory that contains the NDK and `libraries/openmulticore.h`. The stove has
those. The compiler release tarball does not, so the script does not invent
them. With `OMC_INC` set it also builds `openfontomc-68040`. It opens
`gfopenmulticore.library`, checks the fallbacks, and runs the same eight glyphs
through `OMC_RunRange`.

On lab `openfont` (14 Oct 2026), a 68000 build of that program (before builds
became 68040 only, 17 Oct 2026), built with GFOpenAmigaGCC
16.2.0-open2 printed `OPENFONT_OMC PASS ver=0 rev=4 where=2`. Library 0.4,
worker on the cores board (`OMCW_BOARD`). A NULL base, a revision-2 library
and a count below the break-even still match the CPU loop.

## gfopenfont.library

The resident library is for AmigaOS 3.2 on a 68040 with its FPU, and only
that: it does not open on another CPU, or on a 68040 without its FPU (an
LC040). Later, AC090 leaves may take shaping,
glyph rasterisation and atlas building, with the same results.

It holds the core, the index, and GFOpenFont's reader, rasteriser and shaper,
about 200 KB. Inside it:

- one lock: calls from several tasks are taken one at a time;
- its own 64 KB stack, so callers need no more than their usual;
- one memory pool for everything the core allocates,
  freed whole when the library is expunged;
- no C startup and no stdio; files through dos.library, only for processes
  (a plain task gets `OFERR_NOTPROCESS`);
- no patches to the system.

### Fonts

`OF_FindFace` looks in `FONTS:`, `LIBS:WebBrowser/Fonts`, each folder listed
one a line in `ENVARC:GFOpenFont/FontDirs`, and folders a program adds. It keeps
what it found in `ENVARC:GFOpenFont/FontIndex`.

`ENVARC:GFOpenFont/Aliases` holds the user's aliases, one a line
(`Name=Family,Family`), before GFOpenFont's own. `ENVARC:GFOpenFont/Fallbacks` is
the default fallback chain (`DejaVu Sans,Noto Sans,Liberation Sans,DejaVu Sans
Mono,Droid Sans Fallback,NanumGothic` without it). Droid Sans Fallback (Chinese,
Japanese) and NanumGothic (Korean) are the CJK faces
([fonts/cjk](fonts/cjk/README.md)): packages put them in `FONTS:`.

### Using it

```c
#include <proto/gfopenfont.h>

struct Library *OpenFontBase = OpenLibrary("gfopenfont.library", 1);
struct OFFace *face = OF_FindFace("sans-serif", OFA_Size, 16, TAG_END);
struct OFGlyph glyphs[64];
ULONG n = OF_ShapeText(face, text, strlen(text), glyphs, 64);
/* glyphs[i].gl_Face is the face of the chain each glyph came from:
   OF_GetFallback(face, glyphs[i].gl_Face) draws it */
OF_CloseFace(face);
CloseLibrary(OpenFontBase);
```

[sdk](sdk) is what a program needs: the SFD, the C and assembler headers
(`Include_H`, `Include_I`), the FD file, and the autodoc
([sdk/Autodocs/gfopenfont.doc](sdk/Autodocs/gfopenfont.doc)). The varargs calls
`OF_OpenFace` and `OF_FindFace` are in `libgfopenfont.a`, which the build makes
from [sdk/stubs](sdk/stubs). Every call does its work on the library's stack.

Revision 1 (1.1) adds two calls for engines that make their own transforms,
such as our bullet.library (amicradle-guest `libraries/openbullet`), which
draws diskfont.library's outline fonts through GFOpenFont:
`OF_RasterGlyphMatrix` draws a glyph through a 2 x 2 matrix (a size, a
stretch, a shear, a rotation) and `OF_GlyphAdvance` gives its advance in font
units. Check `lib_Revision` before calling them.

Revision 2 (1.2) adds `OF_OpenTextFont`: the GFOpen apps' Amiga fonts, so they
never open diskfont.library or bullet.library. It gives the `TextFont` that
diskfont's `OpenDiskFont` would for the same `TextAttr`: from memory or
`FONTS:`, matched, scaled, and an outline font (an `.otag` beside its
`.font`) drawn at the size by GFOpenFont itself; close it with graphics'
`CloseFont`. The work is AmiCradle's GFOpenDiskFont and GFOpenBullet cores
(amicradle-guest `libraries/opendiskfont` and `libraries/openbullet`),
copied unchanged into [library/textfont](library/textfont), with their env in
[library/of_textfont.c](library/of_textfont.c): exec, dos and graphics for
the first, GFOpenFont's own functions for the second, inside this library. It
adds 25,580 bytes (1,028,488 to 1,054,068). Check `lib_Revision` against
`OPENFONT_REVISION_TEXTFONT` (2) before calling it.

GFOpenFont reads TrueType and OpenType outlines. An `.otag` whose outline it
can't read (OS 3.2's own CGTriumvirate, CGTimes and LetterGothic are Agfa
Intellifont `.type` files) goes, since 1.6, to the system's
`LIBS:bullet.library`, as diskfont.library would have done. Without one, or
when it can't read the outline either, the font comes from its bitmap sizes,
or the nearest size already in memory, as before; so a caller that needs the
size it asked for checks `tf_YSize`.

Since 1.7 an `.otag`'s `OT_YSizeFactor` is honoured, as diskfont.library does
it: the point height is the YSize times its high word over its low word
(diskfonttag.h's point height and black height terms), so a font whose
ascent and descent together are taller than its em (DejaVu's are 1.164 em)
fits the YSize instead of losing its descenders. An `.otag` without the tag
is drawn as before.

Since 1.8 the outline-font core is amicradle-guest's again (main 99d4a2b5;
`library/textfont/README.md`), so an outline font through the system's
bullet.library is laid out as OS 3.2.3's diskfont.library does it: a
character with a width but no picture (the space, U+00A0) takes its
`OT_WidthList` width instead of none, a character with neither gets
U+25A1, `OT_YSizeFactor` scales the DPI the engine draws at (CGTimes'
8782/10034 makes 72 63) instead of the point height, `OT_IsFixed` fonts
keep their glyphs to XSize, and a pixel is kept between glyphs at small
sizes. 1.7's CGTriumvirate and CGTimes came out with no space between words.

Revision 3 (1.3) adds what a layout engine such as WebCore asks:
`OF_ShapeA` (UTF-16, script, language, direction, features over ranges),
`OF_GetFaceMetrics`, `OF_GlyphOutline`, `OF_GlyphExtents`,
`OF_GetFontTable`, `OF_OpenFaceMemoryA` (web fonts; the face keeps its own
copy), `OF_FindFaceForCharA` and `OF_GlyphAdvancePx`, at -168 to -210. It is
also the first library with GFOpenFont's own shaper on main: HarfBuzz is gone
from it, and it went from 1,054,068 bytes (1.2) to 252,208. Check
`lib_Revision` against `OPENFONT_REVISION_LAYOUT` (3) before calling them.

Revision 4 (1.4) adds colour glyphs: `OF_GlyphColorBitmap` gives an emoji's
image from fonts that keep their emoji as PNG images, CBDT/CBLC (Google's,
such as Noto Color Emoji) and sbix (Apple's), from the strike nearest the
size asked, as ARGB pixels; `OF_FreeColorBitmap` frees them (-216 and
-222). The PNG is decoded through the PNG datatype (datatypes.library 44 or
newer): there is no PNG code in the library, and a program drawing emoji
needs no libpng either. The core's `of_face_color_png` gives the PNG itself;
the strike chosen and the image are HarfBuzz's for every glyph of Noto
Color Emoji and of HarfBuzz's own CBDT and sbix test fonts. A PNG datatype that reads only files is given the PNG through a
file in T:, and after `OFERR_EXEC` the result's `cb_Left` names the step
that failed (1 datatypes.library, 2 the picture object, 4 the pixel read)
with `IoErr()` in `cb_Top`. The library grows by 4,976 bytes (252,368 to
257,344). Check `lib_Revision` against
`OPENFONT_REVISION_COLOR` (4) before calling them.

Revision 5 (1.5) draws text through GFOpenGPU: `OF_DrawGlyphsA` (-228) draws a
run of glyphs (ids and 24.8 pen positions, a pixel size, a premultiplied
colour and a GFOpenGPU operator) onto a GFOpenGPU surface, inside a clip
rectangle and optionally an A8 clip mask, and returns when it is drawn. The
glyph cache is inside the library (`src/of_gpu.c`), shared by every caller:
each glyph is filled once into an A8 atlas page by GFOpenGPU's `PATH_FILL`
(per face, size, quarter-pixel x phase and anti-aliasing), then drawn at each
use as one `COMPOSITE_PD` of the colour through its cell; under a matrix, or
with `OFGR_NOCACHE`, each outline is filled where it lies. `OFGR_ALIASED`
needs a back end that says it takes `OGPU_PATH_ALIASED` (gfopengpu.library
0.24's `OGPU_QUERY_FLAG`). This is the glyph cache WebKit used cairo for: a
browser drawing through GFOpenGPU needs neither cairo nor a cache of its own.
`tests/test_gpu.c` draws runs on gfopenrtg's CPU core against cairo
filling the same outlines (configure with `-DOPENFONT_OPENAMIGARTG=` an
gfopenrtg checkout; it needs cairo's headers). The library opens
gfopengpu.library 0.18 or later on the first call (`OFERR_NOGPU` without it)
and is built with its headers from the GFOpen SDK (`OPENGPU_INCLUDE`, by
default `gfopensdk/Include` beside `NDK`). Check `lib_Revision` against
`OPENFONT_REVISION_GPU` (5) before calling it.

`OF_FindFaceForCharA` remembers which characters each indexed font has (its
character map's ranges, read the first time the font is opened), so a
character no font has, or one only a late font has, no longer opens every
font file again. On the PC, 200 characters no font has took 0.102 s across
20 faces before and 0.002 s now. Characters below U+0100 are always checked
in the font itself, for symbol fonts. The core's `of_face_cmap_ranges` gives
the ranges. `OF_AddFontDir` after the index file was read reads the file
again with the new folder, so a program's own folder is not read again
at every start of the library. Faces opened from the same file share one
copy of it while any of them is open, so a face found for a character in
a font the page already uses reads nothing from disk (50 such lookups:
51 file reads before, 1 now). The library grows by 1,552 bytes (257,344
to 258,896).

### Building it

[amiga/build-library.sh](amiga/build-library.sh) builds it with GFOpenAmigaGCC
16.2.0-open2 (the `os32-gcc16` stove's compiler), `-m68040 -m68881`:

```sh
NDK=/path/to/OpenNDK/Include_H sh amiga/build-library.sh
```

- `CC`: the compiler, when not in the stove or under `/opt`;
- `NDK`: AmigaOS headers, when the compiler has none of its own.

It builds from GFOpenFont's own sources alone; nothing upstream is fetched.

It writes `build/amiga/gfopenfont.library`, its link map, `libgfopenfont.a` and
`OpenFontLibTest`. It fails when the library leaves a symbol undefined (libnix
gives it only stateless functions: memory, strings, `qsort`), or
when its code does not start with `start()`. [build-amiga.sh](build-amiga.sh)
is the same build for a Cradle stove, which sets `STOVE` and `OUT`.

### Checking it

Copy `gfopenfont.library` to `LIBS:`, then, from a shell with a 4 KB stack:

```text
OpenFontLibTest FONT/A,FONTDIR/A,CACHE,OUTLINE/K,COLOR/K
```

`FONT` is `DejaVuSans.ttf`, `FONTDIR` a folder with the DejaVu and Liberation
fonts, and `CACHE` an `.ofc` made from `FONT` on the host. It shapes and
rasterises `FONT` at 16 pixels and expects the host's glyphs, advance and
pixels byte for byte (the host's `test_openfont` prints the same hash), then checks the
index, the aliases, the snowman from a fallback and `OFERR_NOTPROCESS` from a
plain task, then `OF_OpenTextFont` on topaz at 8, 11 and 23 (scaled), and on
`OUTLINE` (an outline `.font` in `FONTS:` whose `.otag` names a TrueType or
OpenType file) at 29. It ends with `OPENFONTLIB PASS`. Since 1.3 it also checks revision 3's
calls against the host's `OPENFONT LAYOUT` hash (tests/test_openfont.c). Since
1.4, given `COLOR` (Noto Color Emoji), it decodes U+1F600 through the PNG
datatype and prints the pixels' hash, which `tests/ref/color_png_hash.py`
works out on the host for the same font file. Since 1.5 it draws two runs
with `OF_DrawGlyphs` through gfopengpu.library and expects the pixels' hash
`tests/test_gpu.c` prints on the host (`LAB`). On
lab t4-a (17 Oct 2026) it stopped before its first line of output; the
library then had HarfBuzz inside, whose maths went through libnix's libm
stubs and their unopened math library bases (gfopenfont #14). With the
shaper our own, nothing in the library uses libm.

## Amiga bitmap fonts

[fonts/amiga](fonts/amiga) holds DejaVu Sans Mono as Amiga bitmap fonts for
the Shell: `DejaVuSansMono.font` with sizes 8 to 14, 16, 18, 20, 22 and 24
pixels high, fixed width, the Latin-1 characters, one bit a pixel. GFOpenUp
installs them in `FONTS:`, and the open ROM carries the 11 as its system
default text, the Shell's font where no Font prefs name another.
[tools/amigafont.py](tools/amigafont.py) makes them from the TrueType font,
in the AmigaOS diskfont format, checks them (`amigafont_check` in the host
tests), and writes a size as C data for a ROM. It draws the glyphs with FreeType's monochrome hinting through
Pillow, on the host, once: nothing of FreeType's is in the fonts or in
GFOpenFont. [fonts/amiga/README.md](fonts/amiga/README.md) has the sizes, the
previews and how they were made.

## Compatibility

FreeType/HarfBuzz compatibility headers will be provided for the subset needed
by existing ports, but they are compatibility front ends. New Open software
uses GFOpenFont directly.

MIT licensed AmiCradle Team code. The Unicode data tables in
`src/of_unicode_data.c` are generated from the Unicode Character Database and
are under the Unicode License v3
([LICENSES/Unicode-License-v3.txt](LICENSES/Unicode-License-v3.txt)); the
build copies that licence to `build/amiga/licences`, and it goes wherever the
library is copied. FreeType and HarfBuzz are no longer in GFOpenFont; the
reference tests build against separate trees of theirs. The fonts in
`fonts/amiga` are DejaVu Sans Mono's, under the Bitstream Vera licence with
DejaVu's changes in the public domain
([fonts/amiga/LICENSE-DejaVu.txt](fonts/amiga/LICENSE-DejaVu.txt)). The CJK
faces are not in this repository: Droid Sans Fallback Full (the Android Open
Source Project's, Apache License 2.0) and NanumGothic (NHN Corporation's, SIL
Open Font License 1.1); `fonts/cjk/fetch-cjk-font.sh` fetches them at build
time with their licences ([fonts/cjk](fonts/cjk/README.md)).
