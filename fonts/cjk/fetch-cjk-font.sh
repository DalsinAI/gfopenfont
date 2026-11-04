#!/bin/sh
# fetch-cjk-font.sh [CACHE_DIR]: GFOpenFont's CJK faces and their licences,
# fetched at build time and never kept in git. Prints the four files' paths,
# one a line: copy them all to FONTS:.
#
# DejaVu and Liberation have no Chinese, Japanese or Korean, so every
# program drawing with gfopenfont.library drew that text as boxes.
# - Droid Sans Fallback Full (Android Open Source Project, Apache License
#   2.0, NOTICE as DroidSansFallback.NOTICE): the CJK ideographs and kana,
#   one TrueType face of 4,033,576 B. It has 3 of the 11,172 Hangul
#   syllables.
# - NanumGothic (NHN Corporation, SIL Open Font License 1.1, as
#   NanumGothic.OFL.txt): all 11,172 Hangul syllables, 2,054,744 B.
# Both are last in the default fallback chain (src/openfont_index.c).
# gfopenfont.library reads a face's whole file when it opens it, so only
# text the other faces lack opens one of these.
#
# OF_CJK_FONT, OF_KOREAN_FONT: local copies of the same files to use
# instead (they must match the pinned sha256s).
#
# Copyright (c) 2026 AmiCradle Team. OpenFont, MIT licence (LICENSE).
set -eu
CACHE=${1:-"${XDG_CACHE_HOME:-$HOME/.cache}/gfopenfont/fonts"}
# AOSP's copy, at a fixed tag; googlesource gives files base64 with ?format=TEXT
AOSP="https://android.googlesource.com/platform/frameworks/base/+/refs/tags/android-14.0.0_r1/data/fonts"
# google/fonts, at a fixed commit
GFONTS="https://raw.githubusercontent.com/google/fonts/2fe1e8c6b137fb13041a7cfca6fdcd8a910ea4dc/ofl/nanumgothic"

sha() {
    if command -v sha256sum >/dev/null 2>&1; then sha256sum "$1" | cut -c1-64; else shasum -a 256 "$1" | cut -c1-64; fi
}
check() {  # FILE SHA256
    got=$(sha "$1")
    [ "$got" = "$2" ] || { echo "fetch-cjk-font.sh: $1 has sha256 $got, not $2" >&2; return 1; }
}
# URL DEST SHA256 [base64] [LOCAL_COPY]: DEST from the cache, the local copy
# or the URL. Hosts answer 503 now and then: four tries, 2, 4 and 8 s apart.
get() {
    if [ -n "${5:-}" ]; then
        check "$5" "$3"
        cp "$5" "$2.tmp" && mv "$2.tmp" "$2"
    elif ! [ -f "$2" ] || ! check "$2" "$3" 2>/dev/null; then
        n=0
        until curl -fsSL "$1" > "$2.dl"; do
            n=$((n + 1))
            [ $n -lt 4 ] || { rm -f "$2.dl"; echo "fetch-cjk-font.sh: cannot fetch $1" >&2; exit 1; }
            sleep $((1 << n))
        done
        if [ "${4:-}" = base64 ]; then
            base64 -d < "$2.dl" > "$2.tmp" && rm -f "$2.dl"
        else
            mv "$2.dl" "$2.tmp"
        fi
        check "$2.tmp" "$3" || { rm -f "$2.tmp"; exit 1; }
        mv "$2.tmp" "$2"
    fi
    echo "$2"
}

mkdir -p "$CACHE"
get "$AOSP/DroidSansFallbackFull.ttf?format=TEXT" "$CACHE/DroidSansFallbackFull.ttf" \
    "${CJK_FONT_SHA256:-2392015530438bafc48edfc4aee6d9de2387f627a6134d8ab3dfcc99d21c8240}" base64 "${OF_CJK_FONT:-}"
get "$AOSP/NOTICE?format=TEXT" "$CACHE/DroidSansFallback.NOTICE" \
    "${CJK_NOTICE_SHA256:-38751245389e1e23f73e6f5384b5cbe7fa972cc4410c5adc9c04b082a0b9561a}" base64
get "$GFONTS/NanumGothic-Regular.ttf" "$CACHE/NanumGothic-Regular.ttf" \
    "${KOREAN_FONT_SHA256:-76f45ef4a6bcff344c837c95a7dcc26e017e38b5846d5ae0cdcb5b86be2e2d31}" plain "${OF_KOREAN_FONT:-}"
get "$GFONTS/OFL.txt" "$CACHE/NanumGothic.OFL.txt" \
    "${KOREAN_OFL_SHA256:-eeacf16032901d0ed0456876ec77b8f0fda6b3fecec7d972f8543eb602e6c30f}" plain
