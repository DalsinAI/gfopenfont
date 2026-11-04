# GFOpenFont's CJK faces

DejaVu and Liberation have no Chinese, Japanese or Korean, so programs that
draw with gfopenfont.library drew that text as boxes. Two faces cover it:

| Face | File | Covers | Licence |
| --- | --- | --- | --- |
| Droid Sans Fallback Full | `DroidSansFallbackFull.ttf`, 4,033,576 B | CJK ideographs, kana (3 of the 11,172 Hangul syllables) | Apache License 2.0, Android Open Source Project; `DroidSansFallback.NOTICE` |
| NanumGothic | `NanumGothic-Regular.ttf`, 2,054,744 B | all 11,172 Hangul syllables | SIL Open Font License 1.1, NHN Corporation; `NanumGothic.OFL.txt` |

Neither is kept in this repository. `fetch-cjk-font.sh [CACHE_DIR]` fetches
them at build time, Droid Sans Fallback from AOSP (`platform/frameworks/base`,
`data/fonts`, tag android-14.0.0_r1) and NanumGothic from google/fonts
(`ofl/nanumgothic`, commit 2fe1e8c6), checks each against a pinned sha256,
and prints the paths of the two fonts and their two licences, one a line.
`OF_CJK_FONT` and `OF_KOREAN_FONT` name local copies to use instead; they
must match the same sha256s.

Both are last in the default fallback chain
(`DejaVu Sans,Noto Sans,Liberation Sans,DejaVu Sans Mono,Droid Sans Fallback,NanumGothic`,
`src/openfont_index.c`). gfopenfont.library reads a face's whole file when it
opens it, so a CJK face is opened only for a character the faces before it
lack; text they cover never opens one.

Packages that carry gfopenfont.library (GFOpenBrowser's, for one) put all four
files in `FONTS:`, which gfopenfont.library searches, so every program that
draws with it has the faces.
