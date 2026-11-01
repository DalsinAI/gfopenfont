#!/usr/bin/env python3
"""Amiga bitmap fonts from a fixed-width TrueType font.

    tools/amigafont.py build TTF NAME OUTDIR [--sizes 8 9 10 11 12 13 14 16 18 20 22 24]
                       [--render mono|threshold:N] [--accents squeeze|fit] [--preview DIR]
    tools/amigafont.py check FONTDIR/NAME.font
    tools/amigafont.py preview FONTDIR/NAME.font SIZE OUT.png [--scale N]
    tools/amigafont.py c FONTDIR/NAME.font SIZE OUT.h [--prefix NAME]

build writes OUTDIR/NAME.font (the contents file) and OUTDIR/NAME/<size>, one
file a size, in the AmigaOS diskfont format, for the Latin-1 characters 32 to
255:

- NAME.font: a FontContentsHeader (FCH_ID, $0F00) and one FontContents a size
  (the file's name "NAME/<size>" in 256 bytes, the height, style 0, flags
  FPF_DESIGNED | FPF_DISKFONT).
- NAME/<size>: a load file of one code hunk. The hunk starts MOVEQ #-1,D0;
  RTS, then a DiskFontHeader (DFH_ID, $0F80) with its TextFont, then the
  glyphs (tf_CharData, one bit plane, tf_Modulo bytes a row) and tf_CharLoc
  (a long a character: the glyph's first bit in the row, its width). The
  font is fixed width: no tf_CharSpace, no tf_CharKern, so every character
  is tf_XSize wide, as console.device wants. HUNK_RELOC32 relocates the
  four pointers (the two node names, tf_CharData and tf_CharLoc).

The height is the font's tf_YSize: the whole line, with room for the
descenders (_ and |) below. For each height the largest pixel size whose
Latin-1 glyphs fit is taken. With --accents squeeze (the default) the
accented capitals (A grave to Y acute), the tallest glyphs, don't count: where
an accent then rises above the line, the blank rows between it and its letter
go first, then rows of the accent that repeat the row under them, then its
top rows; the letter itself never moves. That gives 11 pixels 6 x 11 cells
(10 pixels an em) where fitting every accent as drawn (--accents fit) gives
5 x 11 (8 an em). tf_XSize is the font's advance at that size, or the widest
glyph where one is wider (M and W at 7 pixels an em, the 8).

The glyphs are drawn by FreeType through Pillow (the only thing this needs:
Debian/Ubuntu python3-pil, or pip install pillow), one bit a pixel:

- mono (the default): FreeType's monochrome mode, which runs the font's own
  TrueType hinting instructions in full (no subpixel shortcuts), so stems
  land on whole pixels and are one or two pixels wide, the same in every
  glyph;
- threshold:N: the anti-aliased glyph, hinted, with a pixel set where its
  coverage is N (0-255) or more.

A glyph that sticks out of the cell on one side is moved into it; build says
which, and which accents it brought down. Characters the font hasn't got
(127 to 159, the C1 controls) take the font's default glyph, the last in
tf_CharLoc: a box the height of the capitals.

check reads a contents file and its size files back, checks every field this
writes, and prints them. preview draws text with a size file's own glyphs, as
graphics.library's Text() would, into a PNG. c writes one size as a C header
of constant data (the TextFont's fields as macros, tf_CharData and
tf_CharLoc as arrays), for a ROM that carries the font as graphics.library
carries topaz.

MIT, Copyright (c) 2026 AmiCradle Team. The fonts it makes keep their
font's licence (DejaVu: fonts/amiga/LICENSE-DejaVu.txt).
"""
import argparse
import struct
import sys
from pathlib import Path

LO, HI = 32, 255                     # Latin-1, the characters an Amiga font holds
FCH_ID = 0x0F00                      # FontContentsHeader: FontContents follow
DFH_ID = 0x0F80                      # DiskFontHeader
NT_FONT = 12
FPF_DISKFONT, FPF_PROPORTIONAL, FPF_DESIGNED = 0x02, 0x20, 0x40
MAXFONTPATH, MAXFONTNAME = 256, 32
HUNK_HEADER, HUNK_CODE, HUNK_RELOC32, HUNK_END = 0x3F3, 0x3E9, 0x3EC, 0x3F2
DFH = 4                              # the DiskFontHeader, past MOVEQ #-1,D0; RTS
TFO = DFH + 54                       # its TextFont (dfh_TF)
TF_SIZE = 52                         # struct TextFont


# ---- the TrueType font's character map (which Latin-1 characters it has) ----------------------

def cmap_latin1(ttf: bytes) -> set:
    """The codepoints 0-255 the font maps to a glyph, from its Unicode cmap
    (format 4 or 12, platform 3 or 0)."""
    n = struct.unpack(">H", ttf[4:6])[0]
    tables = {ttf[12 + 16 * i:16 + 16 * i]: struct.unpack(">II", ttf[20 + 16 * i:28 + 16 * i]) for i in range(n)}
    off, _ = tables[b"cmap"]
    count = struct.unpack(">H", ttf[off + 2:off + 4])[0]
    subs = {}
    for i in range(count):
        pid, eid, so = struct.unpack(">HHI", ttf[off + 4 + 8 * i:off + 12 + 8 * i])
        subs[(pid, eid)] = off + so
    have = set()
    for key in ((3, 10), (0, 4), (3, 1), (0, 3)):
        if key not in subs:
            continue
        s = subs[key]
        fmt = struct.unpack(">H", ttf[s:s + 2])[0]
        if fmt == 4:
            segx2 = struct.unpack(">H", ttf[s + 6:s + 8])[0]
            ends = struct.unpack(f">{segx2 // 2}H", ttf[s + 14:s + 14 + segx2])
            starts = struct.unpack(f">{segx2 // 2}H", ttf[s + 16 + segx2:s + 16 + 2 * segx2])
            deltas = struct.unpack(f">{segx2 // 2}h", ttf[s + 16 + 2 * segx2:s + 16 + 3 * segx2])
            ro_at = s + 16 + 3 * segx2
            ros = struct.unpack(f">{segx2 // 2}H", ttf[ro_at:ro_at + segx2])
            for k in range(segx2 // 2):
                for c in range(starts[k], min(ends[k], 255) + 1):
                    if ros[k] == 0:
                        g = (c + deltas[k]) & 0xFFFF
                    else:
                        at = ro_at + 2 * k + ros[k] + 2 * (c - starts[k])
                        g = struct.unpack(">H", ttf[at:at + 2])[0]
                        g = (g + deltas[k]) & 0xFFFF if g else 0
                    if g:
                        have.add(c)
            return have
        if fmt == 12:
            ngroups = struct.unpack(">I", ttf[s + 12:s + 16])[0]
            for k in range(ngroups):
                a, b, g = struct.unpack(">III", ttf[s + 16 + 12 * k:s + 28 + 12 * k])
                for c in range(a, min(b, 255) + 1):
                    if g + (c - a):
                        have.add(c)
            return have
    raise SystemExit("the font has no Unicode character map (cmap format 4 or 12)")


# ---- drawing the glyphs ------------------------------------------------------------------------

class Glyph:
    """One glyph's pixels: rows of 0/1, the left column and the top row
    relative to the pen at the baseline (top is negative above it)."""

    def __init__(self, rows, left, top):
        self.rows, self.left, self.top = rows, left, top
        self.width = len(rows[0]) if rows else 0

    @property
    def bottom(self):                 # rows below the baseline, the baseline row not counted
        return self.top + len(self.rows)


def draw_glyph(font, ch: str, ppem: int, render: str) -> Glyph:
    from PIL import Image, ImageDraw
    w = h = 4 * ppem
    ox, oy = ppem, 3 * ppem           # the pen, at the baseline
    if render == "mono":
        im = Image.new("1", (w, h), 0)
        d = ImageDraw.Draw(im)
        d.fontmode = "1"              # FreeType's monochrome mode: the font's hinting in full
        d.text((ox, oy), ch, font=font, fill=1, anchor="ls")
        px = lambda x, y: im.getpixel((x, y)) != 0
    else:
        level = int(render.split(":", 1)[1])
        im = Image.new("L", (w, h), 0)
        d = ImageDraw.Draw(im)
        d.fontmode = "L"
        d.text((ox, oy), ch, font=font, fill=255, anchor="ls")
        px = lambda x, y: im.getpixel((x, y)) >= level
    box = im.getbbox() if render == "mono" else im.point(lambda v: 255 if v >= level else 0).getbbox()
    if not box:
        return Glyph([], 0, 0)
    x0, y0, x1, y1 = box
    rows = [[1 if px(x, y) else 0 for x in range(x0, x1)] for y in range(y0, y1)]
    return Glyph(rows, x0 - ox, y0 - oy)


def advance(font) -> int:
    a = font.getlength("0", mode="1")
    if a != int(a):
        raise SystemExit(f"the hinted advance is {a}, not a whole number of pixels")
    return int(a)


# The accented capitals (A grave to Y acute, not the multiplication sign): the tallest glyphs of Latin-1. With
# --accents squeeze they don't set the pixel size; an accent that then sticks out above the line is brought
# down into it (squeeze_accent).
CAPITAL_ACCENTED = set(range(0xC0, 0xDE)) - {0xD7}


def fit_size(ttf_path: str, height: int, chars, render: str, accents: str = "squeeze"):
    """The largest pixel size at which the glyphs fit a line `height` pixels
    high (every glyph, or with accents "squeeze" every glyph but the accented
    capitals): (ppem, font, glyphs, rows above, rows below)."""
    from PIL import ImageFont
    for ppem in range(height, 3, -1):
        font = ImageFont.truetype(ttf_path, ppem, layout_engine=ImageFont.Layout.BASIC)
        glyphs = {c: draw_glyph(font, chr(c), ppem, render) for c in chars}
        drawn = [g for c, g in glyphs.items() if g.rows and (accents == "fit" or c not in CAPITAL_ACCENTED)]
        above = max(-g.top for g in drawn)
        below = max(g.bottom for g in drawn)
        if above + below <= height:
            return ppem, font, glyphs, above, below
    raise SystemExit(f"no pixel size fits {height} pixels")


def cell_width(font, glyphs) -> int:
    """tf_XSize: the font's advance, or the widest glyph when one is wider
    (DejaVu Sans Mono's M and W at 7 pixels an em are 5 pixels on an advance
    of 4), so no stroke is cut."""
    return max(advance(font), max(g.width for g in glyphs.values()))


def squeeze_accent(g: Glyph, room: int, cap: int) -> Glyph:
    """An accented capital whose accent rises more than `room` rows above the
    baseline, brought down: first the blank rows between the accent and the
    letter go (the lowest first), then rows of the accent that repeat the row
    under them (a ring's or an acute's sides: the shape stays), then, if it
    is still too tall, the accent's top rows. Nothing at or below the
    capitals' height (`cap` rows) moves."""
    over = -g.top - room
    if over <= 0:
        return g
    rows = [list(r) for r in g.rows]
    body = -g.top - cap                     # rows from here down are the letter (at or under the cap height)
    for y in range(min(body, len(rows)) - 1, -1, -1):
        if over and not any(rows[y]):
            del rows[y]
            body -= 1
            over -= 1
    for y in range(min(body, len(rows)) - 2, -1, -1):
        if over and rows[y] == rows[y + 1]:
            del rows[y]
            body -= 1
            over -= 1
    while over > 0 and rows:
        del rows[0]
        over -= 1
    return Glyph(rows, g.left, -room if rows else 0)


# ---- the Amiga files -----------------------------------------------------------------------------

def be16(v): return struct.pack(">H", v & 0xFFFF)
def be32(v): return struct.pack(">I", v & 0xFFFFFFFF)


def size_file(name: str, ysize: int, xsize: int, baseline: int, cells: list) -> bytes:
    """A size file. cells: HI - LO + 2 glyphs of xsize x ysize pixels (rows of
    0/1), the last the default glyph, or None for a character that takes it."""
    nglyph = HI - LO + 2
    unique, locs = [], []
    for c in cells:
        locs.append(None if c is None else len(unique))
        if c is not None:
            unique.append(c)
    default = len(unique) - 1
    locs = [default if v is None else v for v in locs]
    bits = len(unique) * xsize
    modulo = (bits + 15) // 16 * 2                    # a whole number of words a row
    plane = bytearray(modulo * ysize)
    for gi, cell in enumerate(unique):
        for y, row in enumerate(cell):
            for x, v in enumerate(row):
                if v:
                    b = gi * xsize + x
                    plane[y * modulo + b // 8] |= 0x80 >> (b & 7)
    o_data = TFO + TF_SIZE
    o_loc = o_data + len(plane)
    end = o_loc + 4 * nglyph
    hsize = (end + 3) & ~3
    h = bytearray(hsize)
    h[0:4] = be32(0x70FF4E75)                         # MOVEQ #-1,D0; RTS
    relocs = []

    def ptr(at, to):
        h[at:at + 4] = be32(to)
        relocs.append(at)

    nm = (name + ".font").encode("latin-1")
    if len(nm) >= MAXFONTNAME:
        raise SystemExit(f"{name}.font: a font's name is at most {MAXFONTNAME - 1} characters")
    # dfh_DF: ln_Succ, ln_Pred 0; ln_Type NT_FONT; ln_Pri 0; ln_Name -> dfh_Name
    h[DFH + 8] = NT_FONT
    ptr(DFH + 10, DFH + 22)
    h[DFH + 14:DFH + 16] = be16(DFH_ID)               # dfh_FileID; dfh_Revision 0, dfh_Segment 0
    h[DFH + 22:DFH + 22 + len(nm)] = nm               # dfh_Name (diskfont writes the name it opened here too)
    # dfh_TF: tf_Message (ln_Type NT_FONT, ln_Name -> dfh_Name, mn_ReplyPort 0, mn_Length 0)
    h[TFO + 8] = NT_FONT
    ptr(TFO + 10, DFH + 22)
    h[TFO + 20:TFO + 22] = be16(ysize)                # tf_YSize
    h[TFO + 22] = 0                                   # tf_Style
    h[TFO + 23] = FPF_DESIGNED | FPF_DISKFONT         # tf_Flags: fixed width
    h[TFO + 24:TFO + 26] = be16(xsize)                # tf_XSize
    h[TFO + 26:TFO + 28] = be16(baseline)             # tf_Baseline
    h[TFO + 28:TFO + 30] = be16(1)                    # tf_BoldSmear; tf_Accessors 0
    h[TFO + 32] = LO
    h[TFO + 33] = HI
    ptr(TFO + 34, o_data)                             # tf_CharData
    h[TFO + 38:TFO + 40] = be16(modulo)               # tf_Modulo
    ptr(TFO + 40, o_loc)                              # tf_CharLoc; tf_CharSpace, tf_CharKern NULL
    h[o_data:o_data + len(plane)] = plane
    for i, gi in enumerate(locs):
        h[o_loc + 4 * i:o_loc + 4 * i + 4] = be32(((gi * xsize) << 16) | xsize)
    out = bytearray()
    out += be32(HUNK_HEADER) + be32(0) + be32(1) + be32(0) + be32(0) + be32(hsize // 4)
    out += be32(HUNK_CODE) + be32(hsize // 4) + h
    out += be32(HUNK_RELOC32) + be32(len(relocs)) + be32(0)
    for r in sorted(relocs):
        out += be32(r)
    out += be32(0) + be32(HUNK_END)
    return bytes(out)


def contents_file(name: str, sizes: list) -> bytes:
    out = bytearray(be16(FCH_ID) + be16(len(sizes)))
    for y in sizes:
        path = f"{name}/{y}".encode("latin-1")
        out += path + bytes(MAXFONTPATH - len(path)) + be16(y) + bytes([0, FPF_DESIGNED | FPF_DISKFONT])
    return bytes(out)


# ---- reading them back ---------------------------------------------------------------------------

class AmigaFont:
    """A size file read back: the TextFont's fields and the glyph plane."""

    def __init__(self, data: bytes, where: str = "font"):
        def fail(why):
            raise ValueError(f"{where}: {why}")

        def u32(o):
            if o + 4 > len(data):
                fail("cut short")
            return struct.unpack(">I", data[o:o + 4])[0]
        if len(data) < 32 or u32(0) != HUNK_HEADER:
            fail("not a load file (HUNK_HEADER)")
        if u32(4) != 0 or u32(8) != 1 or u32(12) != 0 or u32(16) != 0:
            fail("not one hunk, 0 to 0")
        longs = u32(20)
        if u32(24) != HUNK_CODE or u32(28) != longs:
            fail("its hunk is not HUNK_CODE of the size the header gives")
        h = bytearray(data[32:32 + 4 * longs])
        if len(h) != 4 * longs:
            fail("cut short")
        at = 32 + 4 * longs
        if u32(at) != HUNK_RELOC32:
            fail("no HUNK_RELOC32 after the code")
        n = u32(at + 4)
        if u32(at + 8) != 0:
            fail("relocations against another hunk")
        self.relocs = [u32(at + 12 + 4 * i) for i in range(n)]
        at += 12 + 4 * n
        if u32(at) != 0 or u32(at + 4) != HUNK_END or at + 8 != len(data):
            fail("not ended by 0 and HUNK_END")
        if h[0:4] != be32(0x70FF4E75):
            fail("doesn't start MOVEQ #-1,D0; RTS")
        g16 = lambda o: struct.unpack(">H", h[o:o + 2])[0]
        g32 = lambda o: struct.unpack(">I", h[o:o + 4])[0]
        if g16(DFH + 14) != DFH_ID:
            fail("no DFH_ID")
        for r in self.relocs:
            if r + 4 > len(h) or g32(r) >= len(h):
                fail(f"a relocated pointer at {r} points outside the hunk")
        self.name = bytes(h[DFH + 22:DFH + 22 + MAXFONTNAME]).split(b"\0")[0].decode("latin-1")
        self.ysize, self.style, self.flags = g16(TFO + 20), h[TFO + 22], h[TFO + 23]
        self.xsize, self.baseline, self.boldsmear = g16(TFO + 24), g16(TFO + 26), g16(TFO + 28)
        self.lo, self.hi = h[TFO + 32], h[TFO + 33]
        self.chardata, self.modulo = g32(TFO + 34), g16(TFO + 38)
        self.charloc, self.charspace, self.charkern = g32(TFO + 40), g32(TFO + 44), g32(TFO + 48)
        need = {DFH + 10, TFO + 10, TFO + 34, TFO + 40}
        if set(self.relocs) != need:
            fail(f"relocations {sorted(self.relocs)}, not {sorted(need)}")
        if g32(DFH + 10) != DFH + 22 or g32(TFO + 10) != DFH + 22:
            fail("the node names don't point at dfh_Name")
        if h[DFH + 8] != NT_FONT or h[TFO + 8] != NT_FONT:
            fail("the nodes are not NT_FONT")
        if self.hi < self.lo or not self.ysize or self.modulo & 1:
            fail("LoChar/HiChar, YSize or an odd Modulo")
        if self.chardata + self.modulo * self.ysize > len(h):
            fail("tf_CharData runs past the hunk")
        self.nglyph = self.hi - self.lo + 2
        if self.charloc + 4 * self.nglyph > len(h):
            fail("tf_CharLoc runs past the hunk")
        self.locs = [(g32(self.charloc + 4 * i) >> 16, g32(self.charloc + 4 * i) & 0xFFFF) for i in range(self.nglyph)]
        for off, w in self.locs:
            if off + w > self.modulo * 8:
                fail("a glyph runs past the end of the row")
        self.plane = bytes(h[self.chardata:self.chardata + self.modulo * self.ysize])

    def pixel(self, glyph: int, x: int, y: int) -> int:
        off, w = self.locs[glyph]
        if x >= w:
            return 0
        b = off + x
        return 1 if self.plane[y * self.modulo + b // 8] & (0x80 >> (b & 7)) else 0

    def glyph_of(self, ch: int) -> int:
        return ch - self.lo if self.lo <= ch <= self.hi else self.nglyph - 1


def read_contents(path: Path):
    data = path.read_bytes()
    fid, n = struct.unpack(">HH", data[:4])
    if fid != FCH_ID:
        raise ValueError(f"{path}: not FCH_ID ($0F00) but ${fid:04X}")
    if len(data) != 4 + n * 260:
        raise ValueError(f"{path}: {len(data)} bytes, not 4 + {n} x 260")
    out = []
    for i in range(n):
        e = data[4 + 260 * i:4 + 260 * (i + 1)]
        fname = e[:MAXFONTPATH].split(b"\0")[0].decode("latin-1")
        y, st, fl = struct.unpack(">HBB", e[MAXFONTPATH:])
        out.append((fname, y, st, fl))
    return out


def check(path: Path) -> int:
    entries = read_contents(path)
    name = path.name[:-5]
    bad = 0
    print(f"{path.name}: FCH_ID, {len(entries)} sizes")
    for fname, y, st, fl in entries:
        f = path.parent / fname
        try:
            if not fname.startswith(name + "/"):
                raise ValueError(f"{fname} is not in the {name} drawer")
            a = AmigaFont(f.read_bytes(), str(f))
            if a.ysize != y or a.style != st or a.flags != fl:
                raise ValueError(f"the contents file says {y}/{st}/${fl:02X}, the size file {a.ysize}/{a.style}/${a.flags:02X}")
            if a.flags & FPF_PROPORTIONAL or a.charspace or a.charkern:
                raise ValueError("not fixed width (FPF_PROPORTIONAL, tf_CharSpace or tf_CharKern)")
            if any(w != a.xsize for _, w in a.locs):
                raise ValueError("a glyph not tf_XSize wide")
            if a.baseline >= a.ysize:
                raise ValueError("tf_Baseline is not inside the line")
            if a.name != name + ".font":
                raise ValueError(f"dfh_Name is {a.name!r}")
            print(f"  {fname}: {a.xsize} x {a.ysize}, baseline {a.baseline}, characters {a.lo}-{a.hi} and the default,"
                  f" modulo {a.modulo}, {f.stat().st_size} bytes: ok")
        except (OSError, ValueError) as e:
            print(f"  {fname}: {e}")
            bad += 1
    return 1 if bad else 0


# ---- previews ----------------------------------------------------------------------------------

SHELL_SAMPLE = [
    "1.SYS:> Dir",
    "     C (dir)                       Classes (dir)",
    "     Devs (dir)                    Fonts (dir)",
    "     Libs (dir)                    Prefs (dir)",
    "  Disk.info                        Prefs.info",
    "1.SYS:> List Fonts/#?.font",
    "DejaVuSansMono.font         1304 ----rwed Today     10:50:00",
    "topaz.font                   264 ----rwed 01-May-21 12:00:00",
    "1.SYS:> Echo \"Grüße aus Ærøskøbing: £50 ±1 ½ ¼ ¾ µ ¿¡ «© ®»\"",
]


def table_lines():
    rows = []
    for start in (32, 64, 96, 128, 160, 192, 224):
        rows.append(f"{start:3d} " + "".join(chr(c) for c in range(start, start + 32)))
    return rows


def render_text(font: AmigaFont, lines, fg=(0, 0, 0), bg=(255, 255, 255)):
    from PIL import Image
    w = max(len(l) for l in lines) * font.xsize
    im = Image.new("RGB", (w, len(lines) * font.ysize), bg)
    px = im.load()
    for row, text in enumerate(lines):
        for col, ch in enumerate(text):
            code = ord(ch) if ord(ch) <= 255 else 0x80   # outside Latin-1: the default glyph
            gi = font.glyph_of(code)
            for y in range(font.ysize):
                for x in range(font.xsize):
                    if font.pixel(gi, x, y):
                        px[col * font.xsize + x, row * font.ysize + y] = fg
    return im


def preview_image(font: AmigaFont, title: str, scale: int = 2):
    from PIL import Image
    body = render_text(font, [title, ""] + table_lines() + [""] + SHELL_SAMPLE)
    big = body.resize((body.width * scale, body.height * scale), Image.NEAREST)
    return big


def preview(path: Path, size: int, out: Path, scale: int) -> int:
    entries = {y: f for f, y, _, _ in read_contents(path)}
    if size not in entries:
        raise SystemExit(f"{path} has no size {size}")
    a = AmigaFont((path.parent / entries[size]).read_bytes())
    preview_image(a, f"{path.name[:-5]}/{size}: {a.xsize} x {a.ysize}, baseline {a.baseline}", scale).save(out)
    print(f"{out}")
    return 0


# ---- build -------------------------------------------------------------------------------------

def build(ttf: Path, name: str, outdir: Path, sizes, render: str, previews: Path | None,
          accents: str = "squeeze") -> int:
    data = ttf.read_bytes()
    have = cmap_latin1(data)
    chars = [c for c in range(LO, HI + 1) if c in have]
    missing = [c for c in range(LO, HI + 1) if c not in have]
    (outdir / name).mkdir(parents=True, exist_ok=True)
    report = []
    for height in sizes:
        ppem, font, glyphs, above, below = fit_size(str(ttf), height, chars, render, accents)
        xsize = cell_width(font, glyphs)
        spare = height - above - below
        top = above + (spare + 1) // 2             # a spare row goes above the capitals' accents first
        baseline = top - 1                          # tf_Baseline: the row the letters stand on
        moved, cut, squeezed = [], [], []
        cap = -glyphs[ord("H")].top
        for c in sorted(CAPITAL_ACCENTED & set(glyphs)):
            if -glyphs[c].top > top:
                glyphs[c] = squeeze_accent(glyphs[c], top, cap)
                squeezed.append(chr(c))

        def cell(g: Glyph, c: int):
            rows = [[0] * xsize for _ in range(height)]
            left = g.left
            if left < 0 or left + g.width > xsize:
                if g.width <= xsize:
                    left = min(max(left, 0), xsize - g.width)
                    moved.append(chr(c))
                else:
                    left = 0                        # its left edge stays, the right is cut
                    cut.append(chr(c))
            for y, row in enumerate(g.rows):
                for x, v in enumerate(row):
                    cx, cy = left + x, top + g.top + y
                    if v and 0 <= cx < xsize and 0 <= cy < height:
                        rows[cy][cx] = 1
            return rows

        cells = [cell(glyphs[c], c) if c in glyphs else None for c in range(LO, HI + 1)]
        # the default glyph: a box from the capitals' height to the baseline
        box = [[0] * xsize for _ in range(height)]
        y0, y1 = top - cap, top - 1
        for y in range(y0, y1 + 1):
            for x in range(1, xsize - 1):
                if y in (y0, y1) or x in (1, xsize - 2):
                    box[y][x] = 1
        cells.append(box)
        f = size_file(name, height, xsize, baseline, cells)
        (outdir / name / str(height)).write_bytes(f)
        report.append((height, ppem, xsize, baseline, len(f), moved, cut))
        print(f"{name}/{height}: {xsize} x {height} from {ppem} pixels an em, baseline {baseline}, {len(f)} bytes"
              + (f"; moved into the cell: {''.join(moved)}" if moved else "")
              + (f"; cut to the cell: {''.join(cut)}" if cut else "")
              + (f"; accents brought down: {''.join(squeezed)}" if squeezed else ""))
    (outdir / f"{name}.font").write_bytes(contents_file(name, sizes))
    print(f"{name}.font: {len(sizes)} sizes; the default glyph for {len(missing)} characters the font hasn't got "
          f"({', '.join(str(c) for c in missing[:3])}{'...' if len(missing) > 3 else ''})")
    if previews:
        previews.mkdir(parents=True, exist_ok=True)
        for height, ppem, xsize, baseline, _, _, _ in report:
            a = AmigaFont((outdir / name / str(height)).read_bytes())
            out = previews / f"{name}-{height}.png"
            preview_image(a, f"{name}/{height}: {xsize} x {height} ({ppem} pixels an em), baseline {baseline}").save(out)
            print(f"  {out}")
    return check(outdir / f"{name}.font")


def c_header(path: Path, size: int, prefix: str, out: Path) -> int:
    """One size as a C header of constant data, for a ROM that carries the
    font as graphics.library carries topaz: the TextFont's fields as macros,
    tf_CharData and tf_CharLoc as arrays."""
    import hashlib
    entries = {y: f for f, y, _, _ in read_contents(path)}
    if size not in entries:
        raise SystemExit(f"{path} has no size {size}")
    src = path.parent / entries[size]
    raw = src.read_bytes()
    a = AmigaFont(raw, str(src))
    P = prefix.upper()
    lines = [
        f"/* {path.name} {size} as constant data, made by openamigafont's tools/amigafont.py:",
        f" *   amigafont.py c fonts/amiga/{path.name} {size} OUT --prefix {prefix}",
        f" * from fonts/amiga/{entries[size]} (sha256 {hashlib.sha256(raw).hexdigest()}).",
        f" * The font is DejaVu Sans Mono, Font Software under the Bitstream Vera licence with",
        f" * DejaVu's changes in the public domain: its notice goes with every copy",
        f" * (openamigafont's fonts/amiga/LICENSE-DejaVu.txt). */",
        f"#define {P}_NAME \"{a.name}\"",
        f"#define {P}_YSIZE {a.ysize}",
        f"#define {P}_XSIZE {a.xsize}",
        f"#define {P}_BASELINE {a.baseline}",
        f"#define {P}_BOLDSMEAR {a.boldsmear}",
        f"#define {P}_LOCHAR {a.lo}",
        f"#define {P}_HICHAR {a.hi}",
        f"#define {P}_MODULO {a.modulo}",
        f"/* tf_CharData: {a.ysize} rows of {a.modulo} bytes */",
        f"static const unsigned char {prefix}_data[{len(a.plane)}] = {{",
    ]
    for i in range(0, len(a.plane), 16):
        lines.append("    " + ", ".join(f"0x{b:02x}" for b in a.plane[i:i + 16]) + ",")
    lines += ["};", f"/* tf_CharLoc: for each character {a.lo} to {a.hi} and the default glyph, its first bit"
                    " (high word) and width (low word) */",
              f"static const unsigned long {prefix}_loc[{a.nglyph}] = {{"]
    locs = [(o << 16) | w for o, w in a.locs]
    for i in range(0, len(locs), 8):
        lines.append("    " + ", ".join(f"0x{v:08x}" for v in locs[i:i + 8]) + ",")
    lines.append("};")
    out.write_text("\n".join(lines) + "\n", encoding="ascii")
    print(f"{out}: {len(a.plane)} bytes of glyphs, {4 * a.nglyph} of locations")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    b = sub.add_parser("build", help="make NAME.font and its sizes from a TrueType font")
    b.add_argument("ttf", type=Path)
    b.add_argument("name")
    b.add_argument("outdir", type=Path)
    b.add_argument("--sizes", type=int, nargs="+", default=[8, 9, 10, 11, 12, 13, 14, 16, 18, 20, 22, 24])
    b.add_argument("--render", default="mono", help="mono (the font's hinting, one bit) or threshold:N (0-255)")
    b.add_argument("--accents", default="squeeze", choices=["squeeze", "fit"],
                   help="squeeze (the default): the accented capitals don't set the pixel size, their accents "
                        "are brought down into the line; fit: every glyph fits as drawn")
    b.add_argument("--preview", type=Path, help="also a PNG a size in this folder")
    h = sub.add_parser("c", help="one size as a C header of constant data (for a ROM)")
    h.add_argument("font", type=Path)
    h.add_argument("size", type=int)
    h.add_argument("out", type=Path)
    h.add_argument("--prefix", default="romfont")
    c = sub.add_parser("check", help="read a .font and its sizes back and check them")
    c.add_argument("font", type=Path)
    p = sub.add_parser("preview", help="text in a size's own glyphs, as a PNG")
    p.add_argument("font", type=Path)
    p.add_argument("size", type=int)
    p.add_argument("out", type=Path)
    p.add_argument("--scale", type=int, default=2)
    a = ap.parse_args()
    if a.cmd == "build":
        if a.render != "mono" and not a.render.startswith("threshold:"):
            ap.error("--render: mono or threshold:N")
        return build(a.ttf, a.name, a.outdir, a.sizes, a.render, a.preview, a.accents)
    if a.cmd == "check":
        return check(a.font)
    if a.cmd == "c":
        return c_header(a.font, a.size, a.prefix, a.out)
    return preview(a.font, a.size, a.out, a.scale)


if __name__ == "__main__":
    sys.exit(main())
