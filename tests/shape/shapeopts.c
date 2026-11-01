/* shapeopts: each line of a text file shaped by GFOpenFont with a direction,
 * a script, a language and features, glyph by glyph, for comparing with
 * another shaper (tests/ref/compare_uharfbuzz.py).
 *
 *   shapeopts FONT PIXELS DIRECTION SCRIPT LANGUAGE FEATURES TEXTFILE [utf16]
 *
 * DIRECTION is auto, ltr or rtl; SCRIPT an ISO 15924 code or "-"; LANGUAGE
 * an OpenType language tag or "-"; FEATURES "-" or comma-separated
 * "tag" (on), "-tag" (off), "tag=N" (value N) or "tag[A:B]=N" (over
 * [A, B) of each line). With "utf16" the lines are shaped as UTF-16 and the
 * clusters are 16-bit units.
 *
 * One line per run: glyph/cluster/x advance/y advance/x offset/y offset,
 * 26.6 pixels.
 *
 * Copyright (c) 2026 AmigaChrome Team. MIT licence (see LICENSE). */
#include "gfopenfont/gfopenfont.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t tag4(const char *s)
{
    char t[4] = { ' ', ' ', ' ', ' ' };
    size_t i;
    for (i = 0; i < 4 && s[i] && s[i] != '=' && s[i] != ',' && s[i] != '['; ++i)
        t[i] = s[i];
    return OF_MAKE_TAG(t[0], t[1], t[2], t[3]);
}

static uint32_t parse_features(const char *s, OFFeature *out)
{
    uint32_t n = 0;
    if (!strcmp(s, "-"))
        return 0;
    while (*s && n < OF_MAX_FEATURES) {
        OFFeature *f = &out[n++];
        int off = *s == '-';
        if (off)
            s++;
        f->tag = tag4(s);
        f->value = off ? 0 : 1;
        f->start = 0;
        f->end = OF_FEATURE_END;
        while (*s && *s != ',' && *s != '=' && *s != '[')
            s++;
        if (*s == '[') {
            f->start = (uint32_t)strtoul(s + 1, (char **)&s, 10);
            if (*s == ':')
                f->end = (uint32_t)strtoul(s + 1, (char **)&s, 10);
            if (*s == ']')
                s++;
        }
        if (*s == '=')
            f->value = (uint32_t)strtoul(s + 1, (char **)&s, 10);
        if (*s == ',')
            s++;
    }
    return n;
}

static size_t to_utf16(const unsigned char *s, size_t len, uint16_t *out)
{
    size_t i = 0, n = 0;
    while (i < len) {
        uint32_t c = s[i];
        size_t k = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
        if (k > 1)
            c &= 0xFF >> (k + 1);
        for (size_t j = 1; j < k && i + j < len; ++j)
            c = (c << 6) | (s[i + j] & 0x3F);
        i += k;
        if (c >= 0x10000) {
            out[n++] = (uint16_t)(0xD800 + ((c - 0x10000) >> 10));
            out[n++] = (uint16_t)(0xDC00 + ((c - 0x10000) & 0x3FF));
        } else {
            out[n++] = (uint16_t)c;
        }
    }
    return n;
}

int main(int argc, char **argv)
{
    OFContext *ctx = of_context_create();
    OFFace *face;
    OFShapeOptions o;
    OFFeature features[OF_MAX_FEATURES];
    FILE *f;
    char line[8192];
    int utf16;
    if (argc < 8 || !ctx)
        return 2;
    utf16 = argc > 8 && !strcmp(argv[8], "utf16");
    face = of_face_open(ctx, argv[1]);
    if (!face || of_face_set_pixel_size(face, (uint32_t)atoi(argv[2])) != OF_OK)
        return 2;
    memset(&o, 0, sizeof o);
    o.direction = !strcmp(argv[3], "rtl") ? OF_DIR_RTL : !strcmp(argv[3], "ltr") ? OF_DIR_LTR : OF_DIR_AUTO;
    o.script = strcmp(argv[4], "-") ? tag4(argv[4]) : 0;
    o.language = strcmp(argv[5], "-") ? tag4(argv[5]) : 0;
    o.num_features = parse_features(argv[6], features);
    o.features = features;
    f = fopen(argv[7], "rb");
    if (!f)
        return 2;
    while (fgets(line, sizeof line, f)) {
        size_t len = strcspn(line, "\r\n"), n, i;
        OFGlyph *g;
        uint16_t u[8192];
        size_t units = 0;
        if (!len) {
            printf("\n");
            continue;
        }
        if (utf16) {
            units = to_utf16((const unsigned char *)line, len, u);
            n = of_shape_utf16(face, u, units, 0, units, &o, NULL, 0);
        } else {
            n = of_shape(face, line, len, 0, len, &o, NULL, 0);
        }
        g = (OFGlyph *)malloc((n ? n : 1) * sizeof(*g));
        if (utf16)
            n = of_shape_utf16(face, u, units, 0, units, &o, g, n);
        else
            n = of_shape(face, line, len, 0, len, &o, g, n);
        for (i = 0; i < n; ++i)
            printf("%s%u/%u/%d/%d/%d/%d", i ? " " : "", g[i].glyph_id, g[i].cluster, g[i].x_advance_26_6,
                   g[i].y_advance_26_6, g[i].x_offset_26_6, g[i].y_offset_26_6);
        printf("\n");
        free(g);
    }
    fclose(f);
    of_face_close(face);
    of_context_destroy(ctx);
    return 0;
}
