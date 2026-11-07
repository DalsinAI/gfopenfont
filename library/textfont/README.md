# OF_OpenTextFont's cores

Copied unchanged from amicradle-guest; change them there first, then copy
them here again:

| File | From |
| --- | --- |
| `diskfont_core.cpp`, `.h` | `libraries/opendiskfont` (main 99d4a2b5) |
| `bullet_core.cpp`, `.h` | `libraries/openbullet` (main 99d4a2b5) |

Their tests are there (`tests/m68k/test_opendiskfont.sh`,
`tests/m68k/test_openbullet.sh`, the latter on GFOpenFont's own reader and
rasteriser). [../of_textfont.c](../of_textfont.c) is their env here.
