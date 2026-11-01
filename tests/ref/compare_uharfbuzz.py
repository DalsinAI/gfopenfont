#!/usr/bin/env python3
"""GFOpenFont's shaper against HarfBuzz (the uharfbuzz module), for the
directions, scripts, languages and features WebCore asks for.

    compare_uharfbuzz.py SHAPEOPTS FONT DIRECTION SCRIPT LANGUAGE FEATURES TEXTFILE [utf16]

The arguments after SHAPEOPTS are shapeopts' (tests/shape/shapeopts.c),
less its pixel size: both shape at the font's units per em, so positions
are in font units and nothing is rounded. Prints each line that differs
and a count; exits 1 when any did. Only for testing: nothing of HarfBuzz's
is in GFOpenFont.

Copyright (c) 2026 AmigaChrome Team. MIT licence (see LICENSE).
"""
import subprocess
import sys

import uharfbuzz as hb


def hb_features(spec):
    out = {}
    if spec == "-":
        return out
    for item in spec.split(","):
        out_item = item
        if item.startswith("-"):
            out_item = item[1:] + "=0"
        tag, _, val = out_item.partition("=")
        rng = None
        if "[" in tag:
            tag, r = tag.split("[")
            a, b = r.rstrip("]").split(":")
            rng = (int(a), int(b))
        v = int(val) if val else 1
        if rng:
            out.setdefault(tag, []).append((rng[0], rng[1], v))
        else:
            out[tag] = v
    return out


def main():
    shapeopts, font, direction, script, language, features, textfile = sys.argv[1:8]
    utf16 = len(sys.argv) > 8 and sys.argv[8] == "utf16"
    data = open(font, "rb").read()
    face = hb.Face(data)
    upem = face.upem
    hfont = hb.Font(face)
    hfont.scale = (upem, upem)
    ours = subprocess.run([shapeopts, font, str(upem), direction, script, language, features, textfile]
                          + (["utf16"] if utf16 else []),
                          capture_output=True, check=True).stdout.decode().split("\n")
    lines = open(textfile, encoding="utf-8").read().split("\n")
    feats = hb_features(features)
    bad = 0
    for no, text in enumerate(lines):
        if not text:
            continue
        buf = hb.Buffer()
        if utf16:
            buf.add_utf16(text.encode("utf-16-be").decode("utf-16-be")) if False else buf.add_str(text)
        else:
            buf.add_utf8(text.encode("utf-8"))
        if direction != "auto":
            buf.direction = direction
        if script != "-":
            buf.script = script
        if language != "-":
            buf.language = hb.ot_tag_to_language(language) if hasattr(hb, "ot_tag_to_language") else language
        buf.guess_segment_properties()
        hb.shape(hfont, buf, feats)
        if utf16:
            # clusters in UTF-16 units
            offs, u = [], 0
            for ch in text:
                offs.append(u)
                u += 2 if ord(ch) > 0xFFFF else 1
            cl = [offs[i.cluster] for i in buf.glyph_infos]
        else:
            cl = [i.cluster for i in buf.glyph_infos]
        theirs = ["%d/%d/%d/%d/%d/%d" % (i.codepoint, c, p.x_advance, p.y_advance, p.x_offset, p.y_offset)
                  for i, c, p in zip(buf.glyph_infos, cl, buf.glyph_positions)]
        mine = []
        for g in ours[no].split():
            gid, c, xa, ya, xo, yo = (int(x) for x in g.split("/"))
            mine.append("%d/%d/%d/%d/%d/%d" % (gid, c, round(xa / 64), round(ya / 64), round(xo / 64), round(yo / 64)))
        if mine != theirs:
            bad += 1
            print("line %d: %s" % (no + 1, text))
            print("  openfont: " + " ".join(mine))
            print("  harfbuzz: " + " ".join(theirs))
    print("%s: %d of %d lines differ" % (textfile, bad, sum(1 for t in lines if t)))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
