#!/bin/sh
# Copyright (c) 2026 AmigaChrome Team. OpenFont, MIT licence (LICENSE).
# openfont.library for AmigaOS 3.2 on the 68040, built with OpenAmigaGCC
# 16.2.0-open2 (the os32-gcc16 stove's compiler). Everything in it is
# OpenFont's own code: no upstream sources are fetched.
#
#   amiga/build-library.sh [OUT_DIR]          (default build/amiga)
#
#   CC, CXX    the compilers (default: the os32-gcc16 stove, then the
#              release unpacked under /opt)
#   NDK        a folder of AmigaOS headers, when the compiler has none of
#              its own: OpenNDK's Include_H (openamigasdk/OpenNDK/Include_H)
#   OPENGPU_INCLUDE  opengpu.library's headers (opengpu/stream.h,
#              proto/opengpu.h), for OF_DrawGlyphsA: the Open SDK's Include
#              (default: next to NDK, openamigasdk/Include, then
#              ../openamigasdk/Include beside this checkout)
#
# It builds in OUT_DIR/work.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/.." && pwd)
OUT=${1:-$ROOT/build/amiga}
WORK=$OUT/work
mkdir -p "$OUT" "$WORK"

find_cc() {
    for d in "$HOME/AmigaChrome/stoves/os32-gcc16/bin" "$HOME/AmigaChrome/stoves/os32-gcc16/prefix/bin" \
             /opt/openamigagcc-16.2.0-open2-x86_64-linux/bin; do
        if [ -x "$d/m68k-amigaos-gcc" ]; then echo "$d"; return; fi
    done
}
if [ -z "${CC:-}" ]; then
    BIN=$(find_cc)
    [ -n "$BIN" ] || { echo "m68k-amigaos-gcc not found: set CC and CXX, or install the os32-gcc16 stove" >&2; exit 1; }
    CC=$BIN/m68k-amigaos-gcc
    CXX=$BIN/m68k-amigaos-g++
fi
CXX=${CXX:-$(dirname "$CC")/m68k-amigaos-g++}
NM=$(dirname "$CC")/m68k-amigaos-nm
"$CC" --version | head -1

NDKFLAGS=
[ -n "${NDK:-}" ] && NDKFLAGS="-I$NDK"
if [ -z "${OPENGPU_INCLUDE:-}" ]; then
    for d in ${NDK:+"$NDK/../../Include"} "$ROOT/../openamigasdk/Include"; do
        if [ -f "$d/opengpu/stream.h" ] && [ -f "$d/proto/opengpu.h" ]; then OPENGPU_INCLUDE=$(cd "$d" && pwd); break; fi
    done
fi
[ -n "${OPENGPU_INCLUDE:-}" ] && [ -f "$OPENGPU_INCLUDE/opengpu/stream.h" ] || {
    echo "opengpu.library's headers not found: set OPENGPU_INCLUDE to the Open SDK's Include" >&2; exit 1; }
# after the NDK's, so its exec and proto headers come first
NDKFLAGS="$NDKFLAGS -idirafter $OPENGPU_INCLUDE"

CPU="-m68040 -m68881"   # the 68040 and its own FPU (-m68040 alone gives soft float)
COMMON="$CPU -O2 -fno-delete-null-pointer-checks -fno-common -ffunction-sections -fdata-sections -DNDEBUG"
OBJ=$WORK/obj
rm -rf "$OBJ"
mkdir -p "$OBJ"

# ---- OpenFont: the core, the index, and the library around them ----
OFFLAGS="-std=gnu99 -Wall -Wextra -Werror -I$ROOT/include -I$ROOT/src -I$ROOT/library"
for f in src/openfont src/of_font src/of_cff src/of_raster src/of_layout src/of_shape src/of_unicode_data \
         src/openfont_cache src/openfont_index src/openfont_io_join src/of_gpu; do
    "$CC" $COMMON $NDKFLAGS $OFFLAGS -c "$ROOT/$f.c" -o "$OBJ/of_$(basename "$f").o"
done
# The library's own files include the OS headers. devices/timer.h defines
# struct timeval itself and sets _TIMEVAL_DEFINED, the guard libnix's
# sys/_timeval.h shares, so whichever comes first defines it; defining the
# guard here would leave struct timerequest with no struct timeval at all.
LIBFLAGS="$OFFLAGS"
for f in library/of_runtime library/of_stack library/of_io_dos library/of_textfont; do
    "$CC" $COMMON $NDKFLAGS $LIBFLAGS -c "$ROOT/$f.c" -o "$OBJ/of_$(basename "$f").o"
done
# OF_OpenTextFont's two cores (library/textfont, from amigachrome-guest's
# OpenDiskFont and OpenBullet): lean C++, library profile, integer only
LEAN="-std=gnu++23 -fno-exceptions -fno-rtti -fno-threadsafe-statics -fno-asynchronous-unwind-tables \
 -fno-unwind-tables -nostdlib++ -fno-builtin -fno-tree-loop-distribute-patterns -Wall -Wextra -Werror -Wno-unused-parameter"
for f in diskfont_core bullet_core; do
    "$CXX" $COMMON $LEAN -c "$ROOT/library/textfont/$f.cpp" -o "$OBJ/of_$f.o"
done
bad=$("$NM" "$OBJ/of_diskfont_core.o" "$OBJ/of_bullet_core.o" | awk '{print $NF}' |
      grep -E '^(__cxa_|__gxx_personality|_ZTI|_ZTS|_ZTV|_GLOBAL__sub_I_|_ZGV|_Znw|_Zna|_ZdlPv|_ZdaPv)' || true)
[ -z "$bad" ] || { echo "the textfont cores hold what the lean profile forbids: $bad" >&2; exit 1; }
# the library's file first, its top level in order: the code starts with start()
"$CC" $COMMON $NDKFLAGS $LIBFLAGS -fno-toplevel-reorder -fno-function-sections -fno-data-sections -c "$ROOT/library/openfont_lib.c" -o "$OBJ/aa_openfont_lib.o"

# ---- linking: no C startup; libnix only for its stateless functions ----
"$CC" $CPU -nostartfiles -Wl,--gc-sections -Wl,-Map,"$OUT/openfont.library.map" \
    -o "$OUT/openfont.library" \
    "$OBJ/aa_openfont_lib.o" "$OBJ"/of_*.o \
    -lc -lm -lgcc
"$NM" -u "$OUT/openfont.library" > "$OUT/undefined.txt" || true
# the first code in the file must be start(), which returns at once when
# the library is run as a program
if [ "$("$NM" "$OUT/openfont.library" | awk '$3 == "_start" {print $1}')" != 00000000 ]; then
    echo "start() is not at the beginning of the code" >&2
    exit 1
fi
if [ -s "$OUT/undefined.txt" ]; then
    echo "undefined symbols:" >&2
    cat "$OUT/undefined.txt" >&2
    exit 1
fi
echo "$OUT/openfont.library ($(wc -c < "$OUT/openfont.library") bytes)"
sha256sum "$OUT/openfont.library"
# the licences that go with the library wherever it is copied
mkdir -p "$OUT/licences"
cp "$ROOT/LICENSE" "$OUT/licences/OpenFont.txt"
cp "$ROOT/LICENSES/Unicode-License-v3.txt" "$OUT/licences/Unicode.txt"

# ---- the SDK's link library (the varargs calls), and the lab test ----
SDKFLAGS="$CPU -O2 -Wall -Wextra -Werror -fno-delete-null-pointer-checks $NDKFLAGS -I$ROOT/sdk/Include_H"
STUBS=$WORK/stubs
rm -rf "$STUBS"; mkdir -p "$STUBS"
for f in "$ROOT"/sdk/stubs/*.c; do
    "$CC" $SDKFLAGS -c "$f" -o "$STUBS/$(basename "$f" .c).o"
done
rm -f "$OUT/libopenfont.a"
"$(dirname "$CC")/m68k-amigaos-ar" rcs "$OUT/libopenfont.a" "$STUBS"/*.o
"$CC" $SDKFLAGS -o "$OUT/OpenFontLibTest" "$ROOT/tests/amiga/OpenFontLibTest.c" -L"$OUT" -lopenfont
echo "$OUT/OpenFontLibTest ($(wc -c < "$OUT/OpenFontLibTest") bytes)"
