#!/bin/sh
# Copyright (c) 2026 AmiCradle Team. OpenFont, MIT licence (LICENSE).
# Prints the folder that holds m68k-amigaos-gcc, or nothing when there is
# none, for amiga/build.sh and amiga/build-library.sh when CC is not set:
#   1. OS32_GCC16, the stove the caller names (its bin, then its prefix/bin):
#      when it is set, nowhere else is looked in, so a build never takes
#      another compiler than the one it was told to use
#   2. the os32-gcc16 stove where Cradle keeps it (~/AmiCradle/kitchen/stoves
#      since 16 November 2026, ~/AmiCradle/stoves before)
#   3. a release unpacked under /opt (given as arguments, first wins)
if [ -n "${OS32_GCC16:-}" ]; then
    set -- "$OS32_GCC16/bin" "$OS32_GCC16/prefix/bin"
else
    set -- "$HOME/AmiCradle/kitchen/stoves/os32-gcc16/bin" "$HOME/AmiCradle/kitchen/stoves/os32-gcc16/prefix/bin" \
        "$HOME/AmiCradle/stoves/os32-gcc16/bin" "$HOME/AmiCradle/stoves/os32-gcc16/prefix/bin" "$@"
fi
for d; do
    if [ -x "$d/m68k-amigaos-gcc" ]; then
        echo "$d"
        exit 0
    fi
done
exit 0
