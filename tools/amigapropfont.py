#!/usr/bin/env python3
"""Amiga proportional bitmap sizes for an outline font, beside its .otag.

    tools/amigapropfont.py build FONT NAME OUTDIR --sizes 12 13 [--ppem-factor 8782/10519]
                                 [--render mono|threshold:N] [--preview DIR]
    tools/amigapropfont.py check OUTDIR/NAME.font
    tools/amigapropfont.py preview OUTDIR/NAME.font SIZE OUT.png [--scale N] [--text TEXT]

build writes OUTDIR/NAME.font and OUTDIR/NAME/<size>: proportional Amiga
bitmap fonts (FPF_PROPORTIONAL | FPF_DESIGNED | FPF_DISKFONT) of the Latin-1
characters 32 to 255, for the sizes an outline font is used at most, so
diskfont.library takes the designed size, drawn here once with FreeType's
hinting, where it would otherwise draw the outline unhinted (GFOpenBullet's
coverage set at 128: thin stems fall between pixels at 10 or 11 pixels an
em). The contents file is an outline one (OFCH_ID $0F03) listing the sizes,
so every other size still comes from the font's .otag, as OS 3.2's Fountain
cached sizes beside an outline font.

- The size: ppem = YSize x the factor, rounded, the factor being the
  .otag's OT_YSizeFactor (point height over YSize), so a size here has the
  em diskfont would draw the outline at.
- The line: tf_YSize rows; the descent is the deepest Latin-1 glyph's (the
  accented capitals aside, whose accents are brought down into the line as
  amigafont.py's --accents squeeze does), tf_Baseline the row above it.
- Each glyph: its black box packed in tf_CharData (tf_CharLoc: its first bit
  and its width), tf_CharKern its left edge from the pen, tf_CharSpace the
  hinted advance less that: Text() moves the pen by kern + space.
  tf_XSize is the advance of 'n'.
- The characters the font hasn't got take the default glyph, a box the
  capitals' height.

Glyphs are drawn by FreeType through Pillow (pip install pillow), as
amigafont.py draws them. The sizes keep the font's licence; they are drawings
of it, so GUST's request for derived works (a new name) is met by NAME.

MIT, Copyright (c) 2026 AmiCradle Team.
"""
import argparse
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import amigafont as af  # noqa: E402

OFCH_ID = 0x0F03
FLAGS = af.FPF_PROPORTIONAL | af.FPF_DESIGNED | af.FPF_DISKFONT


def glyphs_at(path: str, ppem: int, chars, render: str):
    from PIL import ImageFont
    font = ImageFont.truetype(path, ppem, layout_engine=ImageFont.Layout.BASIC)
    out = {}
    for c in chars:
        g = af.draw_glyph(font, chr(c), ppem, render)
        adv = font.getlength(chr(c), mode="1" if render == "mono" else "L")
        out[c] = (g, int(round(adv)))
    return out


def size_file(name: str, ysize: int, xsize: int, baseline: int, glyphs: list) -> bytes:
    """glyphs: HI - LO + 2 of (rows ysize high, width, kern, space), the last the default glyph; None takes it."""
    nglyph = af.HI - af.LO + 2
    default = glyphs[-1]
    glyphs = [default if g is None else g for g in glyphs]
    bits, locs = 0, []
    for rows, w, _, _ in glyphs:
        locs.append((bits, w))
        bits += w
    modulo = (bits + 15) // 16 * 2
    plane = bytearray(modulo * ysize)
    for (rows, w, _, _), (o, _) in zip(glyphs, locs):
        for y, row in enumerate(rows):
            for x in range(w):
                if row[x]:
                    b = o + x
                    plane[y * modulo + b // 8] |= 0x80 >> (b & 7)
    o_data = af.TFO + af.TF_SIZE
    o_loc = o_data + len(plane)
    o_space = o_loc + 4 * nglyph
    o_kern = o_space + 2 * nglyph
    end = o_kern + 2 * nglyph
    hsize = (end + 3) & ~3
    h = bytearray(hsize)
    h[0:4] = af.be32(0x70FF4E75)
    relocs = []

    def ptr(at, to):
        h[at:at + 4] = af.be32(to)
        relocs.append(at)

    nm = (name + ".font").encode("latin-1")
    if len(nm) >= af.MAXFONTNAME:
        raise SystemExit(f"{name}.font: a font's name is at most {af.MAXFONTNAME - 1} characters")
    D, T = af.DFH, af.TFO
    h[D + 8] = af.NT_FONT
    ptr(D + 10, D + 22)
    h[D + 14:D + 16] = af.be16(af.DFH_ID)
    h[D + 22:D + 22 + len(nm)] = nm
    h[T + 8] = af.NT_FONT
    ptr(T + 10, D + 22)
    h[T + 20:T + 22] = af.be16(ysize)
    h[T + 22] = 0
    h[T + 23] = FLAGS
    h[T + 24:T + 26] = af.be16(xsize)
    h[T + 26:T + 28] = af.be16(baseline)
    h[T + 28:T + 30] = af.be16(1)
    h[T + 32] = af.LO
    h[T + 33] = af.HI
    ptr(T + 34, o_data)
    h[T + 38:T + 40] = af.be16(modulo)
    ptr(T + 40, o_loc)
    ptr(T + 44, o_space)
    ptr(T + 48, o_kern)
    h[o_data:o_data + len(plane)] = plane
    for i, ((o, w), (_, _, kern, space)) in enumerate(zip(locs, glyphs)):
        h[o_loc + 4 * i:o_loc + 4 * i + 4] = af.be32((o << 16) | w)
        h[o_space + 2 * i:o_space + 2 * i + 2] = af.be16(space)
        h[o_kern + 2 * i:o_kern + 2 * i + 2] = af.be16(kern)
    out = bytearray()
    out += af.be32(af.HUNK_HEADER) + af.be32(0) + af.be32(1) + af.be32(0) + af.be32(0) + af.be32(hsize // 4)
    out += af.be32(af.HUNK_CODE) + af.be32(hsize // 4) + h
    out += af.be32(af.HUNK_RELOC32) + af.be32(len(relocs)) + af.be32(0)
    for r in sorted(relocs):
        out += af.be32(r)
    out += af.be32(0) + af.be32(af.HUNK_END)
    return bytes(out)


def contents_file(name: str, sizes: list) -> bytes:
    out = bytearray(af.be16(OFCH_ID) + af.be16(len(sizes)))
    for y in sizes:
        path = f"{name}/{y}".encode("latin-1")
        out += path + bytes(af.MAXFONTPATH - len(path)) + af.be16(y) + bytes([0, FLAGS])
    return bytes(out)


class PropFont(af.AmigaFont):
    """A proportional size file read back: amigafont's reader with tf_CharSpace and tf_CharKern."""

    def __init__(self, data: bytes, where: str = "font"):
        h = data[32:]
        g32 = lambda o: struct.unpack(">I", h[o:o + 4])[0]
        n = (g32(af.TFO + 44) and 1) + (g32(af.TFO + 48) and 1)
        # amigafont's reader wants the four fixed-width relocations: check the two more here
        relocs_at = 32 + 4 * struct.unpack(">I", data[20:24])[0]
        k = struct.unpack(">I", data[relocs_at + 4:relocs_at + 8])[0]
        relocs = [struct.unpack(">I", data[relocs_at + 12 + 4 * i:relocs_at + 16 + 4 * i])[0] for i in range(k)]
        if n != 2 or sorted(relocs) != sorted({af.DFH + 10, af.TFO + 10, af.TFO + 34, af.TFO + 40, af.TFO + 44, af.TFO + 48}):
            raise ValueError(f"{where}: not a proportional size file (tf_CharSpace, tf_CharKern and six relocations)")
        fixed = bytearray(data)
        # read through amigafont with tf_CharSpace/tf_CharKern's relocations left out
        keep = [r for r in relocs if r not in (af.TFO + 44, af.TFO + 48)]
        fixed[relocs_at + 4:relocs_at + 8] = af.be32(len(keep))
        body = b"".join(af.be32(r) for r in keep)
        fixed[relocs_at + 12:] = body + af.be32(0) + af.be32(af.HUNK_END)
        super().__init__(bytes(fixed), where)
        self.space = [struct.unpack(">h", h[self.charspace + 2 * i:self.charspace + 2 * i + 2])[0] for i in range(self.nglyph)]
        self.kern = [struct.unpack(">h", h[self.charkern + 2 * i:self.charkern + 2 * i + 2])[0] for i in range(self.nglyph)]

    def text_image(self, text: str, fg=(0, 0, 0), bg=(255, 255, 255), pad=4):
        from PIL import Image
        x, ops = pad, []
        for ch in text:
            gi = self.glyph_of(ord(ch) if ord(ch) <= 255 else 0x80)
            ops.append((x + self.kern[gi], gi))
            x += self.kern[gi] + self.space[gi]
        im = Image.new("RGB", (x + pad, self.ysize), bg)
        px = im.load()
        for x0, gi in ops:
            for y in range(self.ysize):
                for xx in range(self.locs[gi][1]):
                    if self.pixel(gi, xx, y) and 0 <= x0 + xx < im.width:
                        px[x0 + xx, y] = fg
        return im


def build(font: Path, name: str, outdir: Path, sizes, factor, render: str, previews) -> int:
    data = font.read_bytes()
    have = af.cmap_latin1(data)
    chars = [c for c in range(af.LO, af.HI + 1) if c in have]
    (outdir / name).mkdir(parents=True, exist_ok=True)
    for ysize in sizes:
        ppem = int(ysize * factor[0] / factor[1] + 0.5)
        gl = glyphs_at(str(font), ppem, chars, render)
        drawn = [g for c, (g, _) in gl.items() if g.rows and c not in af.CAPITAL_ACCENTED]
        below = max(g.bottom for g in drawn)
        top = ysize - below                       # rows above the baseline, the baseline row included
        baseline = top - 1
        cap = -gl[ord("H")][0].top
        squeezed = []
        out = []
        for c in range(af.LO, af.HI + 1):
            if c not in gl:
                out.append(None)
                continue
            g, adv = gl[c]
            if c in af.CAPITAL_ACCENTED and -g.top > top:
                g = af.squeeze_accent(g, top, cap)
                squeezed.append(chr(c))
            if not g.rows:
                out.append(([[0]] * ysize, 0, 0, adv))
                continue
            rows = [[0] * g.width for _ in range(ysize)]
            for y, row in enumerate(g.rows):
                cy = top + g.top + y
                if 0 <= cy < ysize:
                    rows[cy] = list(row)
            out.append((rows, g.width, g.left, adv - g.left))
        # the default glyph: a box the capitals' height, as wide as 'n'
        nadv = gl[ord("n")][1]
        w = max(3, nadv - 2)
        box = [[0] * w for _ in range(ysize)]
        for y in range(top - cap, top):
            for x in range(w):
                if y in (top - cap, top - 1) or x in (0, w - 1):
                    box[y][x] = 1
        out.append((box, w, 1, nadv - 1))
        f = size_file(name, ysize, nadv, baseline, out)
        (outdir / name / str(ysize)).write_bytes(f)
        print(f"{name}/{ysize}: {ppem} pixels an em, baseline {baseline}, 'n' {nadv} wide, {len(f)} bytes"
              + (f"; accents brought down: {''.join(squeezed)}" if squeezed else ""))
    (outdir / f"{name}.font").write_bytes(contents_file(name, sizes))
    if previews:
        previews.mkdir(parents=True, exist_ok=True)
        for y in sizes:
            o = previews / f"{name}-{y}.png"
            preview(outdir / f"{name}.font", y, o, 3, None)
    return check(outdir / f"{name}.font")


def read_contents(path: Path):
    data = path.read_bytes()
    fid, n = struct.unpack(">HH", data[:4])
    if fid != OFCH_ID:
        raise ValueError(f"{path}: not OFCH_ID ($0F03) but ${fid:04X}")
    if len(data) != 4 + n * 260:
        raise ValueError(f"{path}: {len(data)} bytes, not 4 + {n} x 260")
    out = []
    for i in range(n):
        e = data[4 + 260 * i:4 + 260 * (i + 1)]
        out.append((e[:af.MAXFONTPATH].split(b"\0")[0].decode("latin-1"),) + struct.unpack(">HBB", e[af.MAXFONTPATH:]))
    return out


def check(path: Path) -> int:
    entries = read_contents(path)
    name = path.name[:-5]
    bad = 0
    print(f"{path.name}: OFCH_ID, {len(entries)} bitmap sizes beside the outline")
    for fname, y, st, fl in entries:
        f = path.parent / fname
        try:
            if fname != f"{name}/{y}":
                raise ValueError(f"{fname} is not {name}/{y}")
            a = PropFont(f.read_bytes(), str(f))
            if a.ysize != y or a.style != st or a.flags != fl or fl != FLAGS:
                raise ValueError(f"the contents file says {y}/{st}/${fl:02X}, the size file {a.ysize}/{a.style}/${a.flags:02X}")
            if a.baseline >= a.ysize:
                raise ValueError("tf_Baseline is not inside the line")
            if a.name != name + ".font":
                raise ValueError(f"dfh_Name is {a.name!r}")
            if any(k + s < 0 for k, s in zip(a.kern, a.space)):
                raise ValueError("a glyph moves the pen back")
            print(f"  {fname}: {y} high, baseline {a.baseline}, 'n' {a.xsize}, characters {a.lo}-{a.hi} and the default,"
                  f" modulo {a.modulo}, {f.stat().st_size} bytes: ok")
        except (OSError, ValueError) as e:
            print(f"  {fname}: {e}")
            bad += 1
    return 1 if bad else 0


SAMPLE = ["OpenBench 0.1.44 - Chip 8,167 KB, Fast 515,114 KB, GPU = 1, CPU = 1 (m68k) + 10",
          "The quick brown fox jumps over the lazy dog. 0123456789",
          "Sphinx of black quartz, judge my vow! Workbench Window Icons Tools",
          "Grüße aus Ærøskøbing: £50 ±1 ½ ¼ ¾ µ ¿¡ «© ®» ÀÉÎÕÜ"]


def preview(path: Path, size: int, out: Path, scale: int, text) -> int:
    from PIL import Image
    entries = {y: f for f, y, _, _ in read_contents(path)}
    a = PropFont((path.parent / entries[size]).read_bytes())
    lines = [a.text_image(t) for t in ([text] if text else SAMPLE)]
    im = Image.new("RGB", (max(l.width for l in lines), sum(l.height + 2 for l in lines)), (255, 255, 255))
    y = 0
    for l in lines:
        im.paste(l, (0, y))
        y += l.height + 2
    im.resize((im.width * scale, im.height * scale), Image.NEAREST).save(out)
    print(out)
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    b = sub.add_parser("build")
    b.add_argument("font", type=Path)
    b.add_argument("name")
    b.add_argument("outdir", type=Path)
    b.add_argument("--sizes", type=int, nargs="+", required=True)
    b.add_argument("--ppem-factor", default="1/1", help="N/D: pixels an em = YSize x N / D (the .otag's OT_YSizeFactor)")
    b.add_argument("--render", default="mono")
    b.add_argument("--preview", type=Path)
    c = sub.add_parser("check")
    c.add_argument("font", type=Path)
    p = sub.add_parser("preview")
    p.add_argument("font", type=Path)
    p.add_argument("size", type=int)
    p.add_argument("out", type=Path)
    p.add_argument("--scale", type=int, default=3)
    p.add_argument("--text")
    a = ap.parse_args()
    if a.cmd == "build":
        n, d = (int(v) for v in a.ppem_factor.split("/"))
        return build(a.font, a.name, a.outdir, a.sizes, (n, d), a.render, a.preview)
    if a.cmd == "check":
        return check(a.font)
    return preview(a.font, a.size, a.out, a.scale, a.text)


if __name__ == "__main__":
    sys.exit(main())
