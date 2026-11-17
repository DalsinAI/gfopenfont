#!/bin/sh
# Copyright (c) 2026 AmiCradle Team. OpenFont, MIT licence (LICENSE).
# amiga/find-cc.sh with made-up stoves: OS32_GCC16 wins and is the only
# place looked in when set, then the kitchen's stove, then the old place,
# then /opt; and both build scripts take the compiler OS32_GCC16 names.
# (On 17 November 2026 a browser build stopped here: the stove had moved
# to ~/AmiCradle/kitchen/stoves and OS32_GCC16 was not read.)
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
T=$(mktemp -d "${TMPDIR:-/tmp}/of-findcc.XXXXXX")
trap 'rm -rf "$T"' EXIT
fails=0
check() { if [ "$1" = "$2" ]; then :; else fails=$((fails + 1)); echo "FAIL $3: got '$1', wanted '$2'"; fi; }
stove() {       # stove DIR NAME: a compiler that says NAME and stops the build
    mkdir -p "$1"
    printf '#!/bin/sh\necho "fake-gcc %s"\nexit 1\n' "$2" > "$1/m68k-amigaos-gcc"
    chmod +x "$1/m68k-amigaos-gcc"
}
find_cc() { HOME=$T/home sh "$ROOT/amiga/find-cc.sh" "$@"; }

mkdir -p "$T/home"
unset OS32_GCC16 CC CXX
check "$(find_cc "$T/opt/a/bin")" "" "nothing anywhere"
stove "$T/opt/b/bin" opt
check "$(find_cc "$T/opt/a/bin" "$T/opt/b/bin")" "$T/opt/b/bin" "/opt, the first that is there"
stove "$T/home/AmiCradle/stoves/os32-gcc16/prefix/bin" old
check "$(find_cc "$T/opt/b/bin")" "$T/home/AmiCradle/stoves/os32-gcc16/prefix/bin" "the old stove before /opt"
stove "$T/home/AmiCradle/kitchen/stoves/os32-gcc16/prefix/bin" kitchen
check "$(find_cc "$T/opt/b/bin")" "$T/home/AmiCradle/kitchen/stoves/os32-gcc16/prefix/bin" "the kitchen's stove before the old one"
stove "$T/home/AmiCradle/kitchen/stoves/os32-gcc16/bin" kitchen-bin
check "$(find_cc)" "$T/home/AmiCradle/kitchen/stoves/os32-gcc16/bin" "bin before prefix/bin"
stove "$T/named/prefix/bin" named
check "$(OS32_GCC16=$T/named find_cc "$T/opt/b/bin")" "$T/named/prefix/bin" "OS32_GCC16 first"
check "$(OS32_GCC16=$T/missing find_cc "$T/opt/b/bin")" "" "OS32_GCC16 set but empty: nowhere else"

# the build scripts: the first thing each prints is its compiler's version
for script in build.sh build-library.sh; do
    line=$(OS32_GCC16=$T/named HOME=$T/home sh "$ROOT/amiga/$script" "$T/out" 2>/dev/null | head -n 1 || true)
    check "$line" "fake-gcc named" "amiga/$script with OS32_GCC16"
    line=$(HOME=$T/home sh "$ROOT/amiga/$script" "$T/out" 2>/dev/null | head -n 1 || true)
    check "$line" "fake-gcc kitchen-bin" "amiga/$script with the kitchen's stove"
    if OS32_GCC16=$T/missing HOME=$T/home sh "$ROOT/amiga/$script" "$T/out" >/dev/null 2>"$T/err"; then
        fails=$((fails + 1)); echo "FAIL amiga/$script went on without a compiler"
    fi
    grep -q "not found" "$T/err" || { fails=$((fails + 1)); echo "FAIL amiga/$script: no 'not found' message"; }
done

[ "$fails" = 0 ] || { echo "test_find_cc: $fails FAILED"; exit 1; }
echo "test_find_cc: PASS"
