#!/bin/sh
# Copyright (c) 2026 AmiCradle Team. OpenFont, MIT licence (LICENSE).
# For Cradle's stove builds: Cradle sets STOVE (the os32-gcc16 stove) and
# OUT, and runs this. It builds gfopenfont.library, the SDK's link library and
# the lab test with the stove's GFOpenAmigaGCC 16.2 (CI builds with open4), for the 68040 only
# (amiga/build-library.sh).
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
# the stove: STOVE, else the one OS32_GCC16 names, else Cradle's (in
# ~/AmiCradle/kitchen/stoves since 16 November 2026, ~/AmiCradle/stoves before)
if [ -z "${STOVE:-}" ]; then
    STOVE=${OS32_GCC16:-$HOME/AmiCradle/kitchen/stoves/os32-gcc16}
    [ -d "$STOVE" ] || [ -n "${OS32_GCC16:-}" ] || STOVE=$HOME/AmiCradle/stoves/os32-gcc16
fi
OUT=${OUT:-$HERE/build/amiga}
BIN=
for d in "$STOVE/bin" "$STOVE/prefix/bin"; do
    if [ -x "$d/m68k-amigaos-gcc" ]; then BIN=$d; break; fi
done
[ -n "$BIN" ] || { echo "no m68k-amigaos-gcc in $STOVE" >&2; exit 1; }
CC=$BIN/m68k-amigaos-gcc sh "$HERE/amiga/build-library.sh" "$OUT"
