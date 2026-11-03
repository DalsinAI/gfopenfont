#!/bin/sh
# fetch-cjk-font.sh [CACHE_DIR]: GFOpenFont's CJK font, fetched at build
# time and never kept in git. Prints the font's path.
#
# DejaVu and Liberation have no Chinese, Japanese or Korean, so every
# program drawing with gfopenfont.library drew that text as boxes. Droid
# Sans Fallback Full (Android Open Source Project, Apache License 2.0; see
# README.md here) covers the CJK ideographs, kana and Hangul in one
# TrueType face of about 4 MB, and is last in the default fallback chain
# (src/openfont_index.c). gfopenfont.library reads a face's whole file when
# it opens it, so only text the other faces lack opens this one. Packages
# that carry gfopenfont.library put it in FONTS:, with its NOTICE.
#
# OF_CJK_FONT: a copy of the same file to use instead (it must match the
# pinned sha256).
#
# Copyright (c) 2026 AmiCradle Team. OpenFont, MIT licence (LICENSE).
set -eu
CACHE=${1:-"${XDG_CACHE_HOME:-$HOME/.cache}/gfopenfont/fonts"}
NAME=DroidSansFallbackFull.ttf
# AOSP's copy, at a fixed tag; googlesource gives files base64 with ?format=TEXT
TAG=android-14.0.0_r1
BASE="https://android.googlesource.com/platform/frameworks/base/+/refs/tags/$TAG/data/fonts"
SHA256=${CJK_FONT_SHA256:-PINNED_ON_FIRST_FETCH}
NOTICE_SHA256=${CJK_NOTICE_SHA256:-PINNED_ON_FIRST_FETCH}

sha() {
    if command -v sha256sum >/dev/null 2>&1; then sha256sum "$1" | cut -c1-64; else shasum -a 256 "$1" | cut -c1-64; fi
}
check() {  # FILE [SHA256]
    want=${2:-$SHA256}
    got=$(sha "$1")
    [ "$got" = "$want" ] || { echo "fetch-cjk-font.sh: $1 has sha256 $got, not $want" >&2; return 1; }
}
fetch() {  # NAME SHA256 DEST
    curl -fsSL "$BASE/$1?format=TEXT" | base64 -d > "$3.tmp"
    check "$3.tmp" "$2" || { rm -f "$3.tmp"; exit 1; }
    mv "$3.tmp" "$3"
}

mkdir -p "$CACHE"
F="$CACHE/$NAME"
if [ -n "${OF_CJK_FONT:-}" ]; then
    check "$OF_CJK_FONT"
    cp "$OF_CJK_FONT" "$F.tmp" && mv "$F.tmp" "$F"
elif ! [ -f "$F" ] || ! check "$F" 2>/dev/null; then
    fetch "$NAME" "$SHA256" "$F"
fi
# the licence (Apache License 2.0, AOSP's NOTICE for these fonts) travels
# with the font: packages put it beside it as DroidSansFallback.NOTICE
N="$CACHE/DroidSansFallback.NOTICE"
if ! [ -f "$N" ] || ! check "$N" "$NOTICE_SHA256" 2>/dev/null; then
    fetch NOTICE "$NOTICE_SHA256" "$N"
fi
echo "$F"
