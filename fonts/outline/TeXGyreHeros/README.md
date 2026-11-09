# AmiSans (TeX Gyre Heros): the open stand-in for CGTriumvirate

`texgyreheros-regular.otf` is TeX Gyre Heros Regular 2.004, an OpenType font
with CFF outlines, as GUST's e-foundry publishes it: unmodified, byte for
byte the file in `qhv2.004otf.zip` from
<https://www.gust.org.pl/projects/e-foundry/tex-gyre/heros> (the whole
family's `tg2_501otf.zip` has the same file).

    sha256 6ae1a09d5a940367b7aaaa91ee8bd8a2c333bfe193e7096e23f931357d62081f  texgyreheros-regular.otf

## Why this font

OS 3.2.3's look on a tall Workbench (Instance-32) is CGTriumvirate 13, Agfa's
outline Helvetica, in 24-pixel bars. CGTriumvirate is OS 3.2.3's and can't
be on an open disk, and DejaVu Sans, the fallback until now, is a Verdana-like
humanist design that is wider and looks unlike it. Of the free Helvetica-style
fonts:

| Font | Design | Licence | |
| --- | --- | --- | --- |
| **TeX Gyre Heros** | Helvetica (URW's Nimbus Sans L, extended by GUST) | GUST Font License (LPPL 1.3c) | taken |
| URW Nimbus Sans (urw-base35, 2017 and later) | Helvetica | AGPL 3.0 with a font exception | not taken: AGPL |
| Liberation Sans / Arimo | Arial's metrics, its own shapes | SIL OFL 1.1 (Liberation Sans an RFN) | not taken: Arial, not Helvetica (R, G, t, a differ) |

TeX Gyre Heros is Helvetica's design (the R's straight leg, the
horizontal-cut terminals of a, c, e, s, the G's spur), the closest to
CGTriumvirate, and its licence lets us bundle it in the OS image.

## The licence

[GUST-FONT-LICENSE.txt](GUST-FONT-LICENSE.txt), version 1.0 (22 June 2009),
as GUST publishes it at
<https://www.gust.org.pl/projects/e-foundry/licenses/GUST-FONT-LICENSE.txt>:
the font may be distributed and modified under the LaTeX Project Public
License 1.3c or later; GUST asks (not as a legal condition) that a modified
font be given new font and file names. The file here is not modified, so it
keeps its own names. The bitmap sizes drawn from it (below) are a derived
work, so, as GUST asks, the Amiga font is called **AmiSans**, not TeX Gyre
Heros. The font file's own notice (name ID 0): "Copyright 2006, 2009 for TeX
Gyre extensions by B. Jackowski and J.M. Nowacki (on behalf of TeX users
groups). This work is released under the GUST Font License". There is no
Reserved Font Name (the GUST licence has none; it is not the OFL).

## AmiSans: how it goes on the Amiga

[`../AmiSans.font`](../AmiSans.font) is an outline font's contents file
(`OFCH_ID` `$0F03`) listing two bitmap sizes, [`../AmiSans/12`](../AmiSans)
and `13`, the sizes the look uses (13 for the screen and its menus, 12 for
the screen's and windows' titles). They are drawn once, on the host, from
this file with FreeType's hinting (Pillow 13.0.0, FreeType 2.14.3) by
[`tools/amigapropfont.py`](../../../tools/amigapropfont.py):

    tools/amigapropfont.py build fonts/outline/TeXGyreHeros/texgyreheros-regular.otf AmiSans fonts/outline \
        --sizes 12 13 --ppem-factor 8782/10519 --preview fonts/outline/preview

Why bitmaps: GFOpenBullet draws outlines unhinted, a pixel set where its
coverage is half or more, and at 10 and 11 pixels an em TeX Gyre Heros's
thin stems fall between pixels: in lab t5font-heros (9 November 2026) the
title bar drawn from the outline lost the stem of 1, the comma, the bottom
of U and the +. The hinted bitmaps keep every stroke. Every other size still
comes from the outline, through `AmiSans.otag`.

GFOpenUp's GFOpenFont part (gfopenup `tools/build_pack.py`, `amisans`)
installs, beside DejaVu Sans:

- `FONTS:AmiSans.font` and `FONTS:AmiSans/12`, `13`, from here;
- `FONTS:AmiSans.otag`: `OT_FileIdent`, `OT_Engine` "bullet",
  `OT_Spec1 | OT_Indirect` `_otf/texgyreheros-regular.otf`, and
  `OT_YSizeFactor` 8782/10519, CGTriumvirate.otag's own, so a size has the
  same em as CGTriumvirate's (13: 11 pixels, 12: 10), as the bitmaps do;
- `FONTS:_otf/texgyreheros-regular.otf`, this file, unmodified;
- the licence in `SYS:Utilities/GFOpenUp/Licences/AmiSans.txt`.

The GFOpenKick ROM's tall Workbench (720 lines and up, no Font prefs) tries
CGTriumvirate 13, then AmiSans 13, then DejaVu Sans 13, all in the same
24-pixel bars (amicradle-guest `screen_core.h`, `OI_TALL_FONT_OPEN`).
GFOpenPrefs' Look "Standard" font is CGTriumvirate 13 where FONTS: has it,
else AmiSans 13.
