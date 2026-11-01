# OF_OpenTextFont's cores

Copied unchanged from amigachrome-guest; change them there first, then copy
them here again:

| File | From |
| --- | --- |
| `diskfont_core.cpp`, `.h` | `libraries/opendiskfont` (main d2e10cce) |
| `bullet_core.cpp`, `.h` | `libraries/openbullet` (PR #450, 8a38ee38) |

Their tests are there (`tests/m68k/test_opendiskfont.sh`,
`tests/m68k/test_openbullet.sh`, the latter on GFOpenFont's own reader and
rasteriser). [../of_textfont.c](../of_textfont.c) is their env here.
