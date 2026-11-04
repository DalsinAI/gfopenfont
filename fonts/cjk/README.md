# GFOpenFont's CJK face

DejaVu and Liberation have no Chinese, Japanese or Korean, so programs that
draw with gfopenfont.library drew that text as boxes. The CJK face is
**Droid Sans Fallback Full** (`DroidSansFallbackFull.ttf`), from the Android
Open Source Project, under the Apache License 2.0. It is TrueType, about 4 MB,
and covers the CJK ideographs, kana and Hangul.

It is not kept in this repository. `fetch-cjk-font.sh [CACHE_DIR]` fetches it
at build time from AOSP (`platform/frameworks/base`, `data/fonts`, at a fixed
tag), checks it against a pinned sha256, puts its `NOTICE` beside it as
`DroidSansFallback.NOTICE`, and prints the font's path. `OF_CJK_FONT` names a
local copy to use instead; it must match the same sha256.

The face is last in the default fallback chain
(`DejaVu Sans,Noto Sans,Liberation Sans,DejaVu Sans Mono,Droid Sans Fallback`,
`src/openfont_index.c`). gfopenfont.library reads a face's whole file when it
opens it, so the CJK face is opened only for a character the faces before
it lack; text they cover never opens it.

Packages that carry gfopenfont.library (GFOpenBrowser's, for one) put the font
and its `DroidSansFallback.NOTICE` in `FONTS:`, which gfopenfont.library
searches, so every program that draws with it has the face.
