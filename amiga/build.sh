#!/bin/sh
# Copyright (c) 2026 AmigaChrome Team. OpenFont, MIT licence (LICENSE).
# The 68k range test, built with GFOpenAmigaGCC 16.2.0-open2.
# That is the compiler in the os32-gcc16 stove. There is no 16.2.9-open2.
#
#   amiga/build.sh [OUT_DIR]
#
# CC overrides the compiler. Otherwise the script looks for the stove, then
# the unpacked release under /opt.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/.." && pwd)
# Soft float: the specs file links libnix's soft libm020 (maths through the
# mathieee libraries), not libm881, so one build runs on every 68040.
SPECS="-specs=$ROOT/toolchain/m68k-soft-m68040.specs"
if [ -z "${CC:-}" ]; then
    if [ -x "$HOME/AmiCradle/stoves/os32-gcc16/bin/m68k-amigaos-gcc" ]; then
        CC=$HOME/AmiCradle/stoves/os32-gcc16/bin/m68k-amigaos-gcc
    elif [ -x "$HOME/AmiCradle/stoves/os32-gcc16/prefix/bin/m68k-amigaos-gcc" ]; then
        CC=$HOME/AmiCradle/stoves/os32-gcc16/prefix/bin/m68k-amigaos-gcc
    elif [ -x /opt/openamigagcc-16.2.0-open2-x86_64-linux/bin/m68k-amigaos-gcc ]; then
        CC=/opt/openamigagcc-16.2.0-open2-x86_64-linux/bin/m68k-amigaos-gcc
    else
        echo "m68k-amigaos-gcc not found. Set CC, or install the os32-gcc16 stove." >&2
        exit 1
    fi
fi
OUT=${1:-$ROOT/build}
mkdir -p "$OUT"
"$CC" --version | head -1
for cpu in 68040; do
    "$CC" -m"$cpu" $SPECS -O2 -Wall -Wextra -Werror \
        -I "$ROOT/include" -I "$ROOT/src" \
        -o "$OUT/openfontrange-$cpu" \
        "$ROOT/tests/test_range.c" "$ROOT/src/openfont_cache.c" "$ROOT/src/openfont_io_stdio.c" "$ROOT/src/openfont_io_join.c"
    echo "$OUT/openfontrange-$cpu ($(wc -c < "$OUT/openfontrange-$cpu") bytes)"
done

# GFOpenFont's own reader, rasteriser and shaper, and their tests: the same
# fingerprints as on the host, the same bitmaps and glyphs to the bit.
for cpu in 68040; do
    "$CC" -m"$cpu" $SPECS -O2 -Wall -Wextra -Werror \
        -I "$ROOT/include" -I "$ROOT/src" \
        -o "$OUT/openfontreader-$cpu" \
        "$ROOT/tests/test_reader.c" "$ROOT/src/of_font.c" "$ROOT/src/of_cff.c" "$ROOT/src/of_raster.c"
    echo "$OUT/openfontreader-$cpu ($(wc -c < "$OUT/openfontreader-$cpu") bytes)"
    # the shaper and the Unicode properties: the host's fingerprints too
    "$CC" -m"$cpu" $SPECS -O2 -Wall -Wextra -Werror \
        -I "$ROOT/include" -I "$ROOT/src" \
        -o "$OUT/openfontshape-$cpu" \
        "$ROOT/tests/test_shape.c" "$ROOT/src/of_font.c" "$ROOT/src/of_cff.c" "$ROOT/src/of_layout.c" \
        "$ROOT/src/of_shape.c" "$ROOT/src/of_unicode_data.c"
    echo "$OUT/openfontshape-$cpu ($(wc -c < "$OUT/openfontshape-$cpu") bytes)"
done

# The GFOpenMulticore path needs the NDK and openamigamulticore/include.
# OMC_INC is that include directory. Without it, only the range test is built.
# Builds are for the 68040 only (the target, 68040 + FPU).
if [ -n "${OMC_INC:-}" ]; then
    for cpu in 68040; do
        "$CC" -m"$cpu" $SPECS -O2 -Wall -Wextra -Werror \
            -I "$ROOT/include" -I "$ROOT/src" -I "$OMC_INC" \
            -o "$OUT/openfontomc-$cpu" \
            "$ROOT/tests/test_omc.c" "$ROOT/src/openfont_omc.c" "$ROOT/src/openfont_cache.c" "$ROOT/src/openfont_io_stdio.c" "$ROOT/src/openfont_io_join.c"
        echo "$OUT/openfontomc-$cpu ($(wc -c < "$OUT/openfontomc-$cpu") bytes)"
    done
fi
