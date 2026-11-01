#!/usr/bin/env python3
"""The pixel hash OpenFontLibTest prints for COLOR (U+1F600 at 20 pixels
through OF_GlyphColorBitmap), worked out on the host: the same PNG as
GFOpenFont picks (HarfBuzz's strike choice is the same), decoded with Pillow
to ARGB, hashed with FNV-1a over the bytes A, R, G, B of each pixel.

    tests/ref/color_png_hash.py /usr/share/fonts/truetype/noto/NotoColorEmoji.ttf

Needs uharfbuzz and Pillow (pip install uharfbuzz pillow); only for testing.

Copyright (c) 2026 AmiCradle Team. OpenFont, MIT licence (LICENSE)."""
import io
import sys

import uharfbuzz as hb
from PIL import Image


def main(path, codepoint=0x1F600, ppem=20):
    font = hb.Font(hb.Face(hb.Blob.from_file_path(path)))
    font.ppem = (ppem, ppem)
    glyph = font.get_nominal_glyph(codepoint)
    png = font.get_glyph_color_png(glyph).data
    image = Image.open(io.BytesIO(png)).convert("RGBA")
    h = 2166136261
    inked = 0
    rgba = image.tobytes()
    for i in range(0, len(rgba), 4):
        r, g, b, a = rgba[i:i + 4]
        for byte in (a, r, g, b):
            h = ((h ^ byte) * 16777619) & 0xFFFFFFFF
        inked += a != 0
    print("U+%04X: %dx%d, pixels hash=%08x, %d not transparent" % (codepoint, image.width, image.height, h, inked))


if __name__ == "__main__":
    main(sys.argv[1])
