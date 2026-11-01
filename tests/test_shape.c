/* test_shape: GFOpenFont's Unicode properties and its shaper.
 *
 *   test_shape                 the Unicode properties
 *   test_shape FONT [TEXT]     and, when FONT is DejaVu Sans (DejaVuSans.ttf),
 *                              shaping with it:
 *                              ligatures, kerning, composed and decomposed
 *                              accents, marks on their bases, broken UTF-8,
 *                              hidden characters and spaces the font lacks;
 *                              then every line of TEXT shaped with the font
 *                              cut short and with bytes changed, which must
 *                              fail cleanly
 *
 * It prints a fingerprint of TEXT's shaping at 12 and 16 pixels: the
 * shaper is integers only, so the host and the 68040 must print the same.
 *
 * Copyright (c) 2026 AmiCradle Team. MIT licence (see LICENSE). */
#include "gfopenfont/gfopenfont.h"
#include "of_font.h"
#include "of_layout.h"
#include "of_unicode.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

#define CHECK(cond, ...) do { if (!(cond)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); failures++; } } while (0)

static void unicode_tests(void)
{
    uint32_t a, b;
    CHECK(of_uni_category('A') == OF_GC_LU && of_uni_category('a') == OF_GC_LL, "letters");
    CHECK(of_uni_category(0x0301) == OF_GC_MN && of_uni_combining_class(0x0301) == 230, "acute");
    CHECK(of_uni_combining_class(0x0323) == 220, "dot below");
    CHECK(of_uni_category(0x3000) == OF_GC_ZS && of_uni_category(0x200B) == OF_GC_CF, "spaces");
    CHECK(of_uni_default_ignorable(0x00AD) && of_uni_default_ignorable(0x200D) && !of_uni_default_ignorable('a'),
          "ignorables");
    CHECK(of_uni_category(0xE0001) == OF_GC_CF && of_uni_default_ignorable(0xE0001), "plane 14");
    CHECK(of_uni_category(0xF0000) == OF_GC_CO && of_uni_category(0x10FFFD) == OF_GC_CO, "private use");
    CHECK(of_uni_category(0x50000) == OF_GC_CN && of_uni_category(0x110000) == OF_GC_CN, "unassigned");
    CHECK(of_uni_category(0x20000) == OF_GC_LO && of_uni_script(0x20000) == OF_SCRIPT_HAN, "CJK B");
    CHECK(of_uni_script('A') == OF_SCRIPT_LATIN && of_uni_script(0x03B1) == OF_SCRIPT_GREEK &&
          of_uni_script(0x0416) == OF_SCRIPT_CYRILLIC && of_uni_script(' ') == OF_SCRIPT_COMMON &&
          of_uni_script(0x0301) == OF_SCRIPT_INHERITED, "scripts");
    CHECK(of_uni_decompose(0x00E9, &a, &b) == 2 && a == 'e' && b == 0x0301, "e acute");
    CHECK(of_uni_decompose(0x1EC7, &a, &b) == 2 && a == 0x1EB9 && b == 0x0302, "e dot below circumflex");
    CHECK(of_uni_decompose(0x212B, &a, &b) == 1 && a == 0x00C5, "angstrom");
    CHECK(of_uni_decompose('a', &a, &b) == 0, "a");
    CHECK(of_uni_compose('e', 0x0301) == 0x00E9 && of_uni_compose(0x00C5, 0x0301) == 0x01FA, "compose");
    CHECK(of_uni_compose('A', 0x030A) == 0x00C5 && of_uni_compose(0x0B47, 0x0B3E) == 0x0B4B, "compose 2");
    CHECK(of_uni_compose(0x0915, 0x093C) == 0, "excluded");
    CHECK(of_uni_decompose(0xAC01, &a, &b) == 2 && a == 0xAC00 && b == 0x11A8, "hangul LVT");
    CHECK(of_uni_compose(0x1100, 0x1161) == 0xAC00 && of_uni_compose(0xAC00, 0x11A8) == 0xAC01, "hangul");
}

static unsigned char *read_file(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb");
    unsigned char *d;
    long n;
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    d = (unsigned char *)malloc(n + 1);
    if (d && fread(d, 1, n, f) != (size_t)n) {
        free(d);
        d = NULL;
    }
    fclose(f);
    if (d)
        d[n] = 0;
    *size = (size_t)n;
    return d;
}

static uint32_t shape(const OFFont *f, uint32_t ppem, const char *s, OFShBuf *b)
{
    OFRunProps props = { OF_SCRIPT_UNKNOWN, 0, 0, OF_DIR_AUTO, NULL, 0 };
    if (of_shape_text(f, ppem, s, strlen(s), 0, strlen(s), &props, b) != OF_OK)
        return 0;
    return b->n;
}

static void dejavu_tests(const OFFont *f)
{
    OFShBuf b;
    uint32_t n, i;
    int32_t sum;
    memset(&b, 0, sizeof b);

    /* ligatures: "office" has fewer glyphs than letters, the ligature in
     * the cluster of its first letter */
    n = shape(f, 16, "office", &b);
    CHECK(n == 4 && b.g[1].cluster == 1 && b.g[2].cluster == 4, "office: %u glyphs", n);

    /* kerning: A then V closer than their advances */
    n = shape(f, 16, "AV", &b);
    CHECK(n == 2 && b.g[0].xa < of_font_advance_px(f, of_font_glyph(f, 'A'), 16), "AV kerned");
    CHECK(n == 2 && (b.g[1].props & OF_SH_UNSAFE), "AV: unsafe to break between");

    /* e + combining acute is the font's e acute */
    n = shape(f, 16, "e\xCC\x81", &b);
    CHECK(n == 1 && b.g[0].gid == of_font_glyph(f, 0xE9), "e acute composed");
    /* and e acute the font has stays one */
    n = shape(f, 16, "\xC3\xA9", &b);
    CHECK(n == 1 && b.g[0].gid == of_font_glyph(f, 0xE9), "e acute");

    /* q + acute: no composed form, so a mark, on q, in q's cluster */
    n = shape(f, 16, "q\xCC\x81", &b);
    CHECK(n == 2 && b.g[1].xa == 0 && (b.g[1].xo || b.g[1].yo) && b.g[1].cluster == 0, "q acute");

    /* marks in canonical order: dot below (220) before circumflex (230) */
    n = shape(f, 16, "a\xCC\x82\xCC\xA3", &b);
    CHECK(n == 1 && b.g[0].gid == of_font_glyph(f, 0x1EAD), "a circumflex dot below: %u", n);

    /* broken UTF-8: each byte that starts nothing valid is one U+FFFD */
    n = shape(f, 16, "a\xE2\x82" "b", &b);
    CHECK(n == 4 && b.g[1].gid == of_font_glyph(f, 0xFFFD) && b.g[2].gid == b.g[1].gid &&
          b.g[1].cluster == 1 && b.g[2].cluster == 2 && b.g[3].cluster == 3, "broken UTF-8");

    /* hidden: ZWJ joins the cluster before it and takes no room */
    n = shape(f, 16, "a\xE2\x80\x8D" "b", &b);
    CHECK(n == 3 && b.g[1].xa == 0 && b.g[1].cluster == 0 && b.g[1].gid == of_font_glyph(f, ' '), "ZWJ");
    /* ZWNJ keeps f and i apart */
    n = shape(f, 16, "f\xE2\x80\x8C" "i", &b);
    CHECK(n == 3 && b.g[0].gid == of_font_glyph(f, 'f'), "ZWNJ");

    /* a space the font lacks: the ideographic space is an em */
    n = shape(f, 16, "a\xE3\x80\x80" "b", &b);
    CHECK(n == 3 && b.g[1].gid == of_font_glyph(f, ' ') && b.g[1].xa == 16 * 64, "ideographic space");

    /* the advances add up to the text's */
    n = shape(f, 16, "Hello", &b);
    for (i = 0, sum = 0; i < n; ++i)
        sum += b.g[i].xa;
    CHECK(n == 5 && sum > 5 * 64 * 5 && sum < 5 * 64 * 16, "Hello: %d", sum);
    free(b.g);
}

static uint32_t mix(uint32_t h, uint32_t v)
{
    int k;
    for (k = 0; k < 4; ++k) {
        h ^= (v >> (8 * k)) & 0xFF;
        h *= 16777619u;
    }
    return h;
}

/* every line of text shaped: a fingerprint of the glyphs, or nothing but a
 * clean return for a damaged font */
static uint32_t shape_lines(const OFFont *f, const char *text, uint32_t *runs)
{
    static const uint32_t sizes[] = { 12, 16 };
    uint32_t hash = 2166136261u;
    OFShBuf b;
    unsigned s;
    memset(&b, 0, sizeof b);
    *runs = 0;
    for (s = 0; s < 2; ++s) {
        const char *p = text;
        while (*p) {
            const char *e = strchr(p, '\n');
            size_t len = e ? (size_t)(e - p) : strlen(p);
            OFRunProps props = { OF_SCRIPT_UNKNOWN, 0, 0, OF_DIR_AUTO, NULL, 0 };
            uint32_t i;
            if (of_shape_text(f, sizes[s], p, len, 0, len, &props, &b) == OF_OK)
                for (i = 0; i < b.n; ++i) {
                    hash = mix(hash, b.g[i].gid);
                    hash = mix(hash, b.g[i].cluster);
                    hash = mix(hash, (uint32_t)b.g[i].xa);
                    hash = mix(hash, (uint32_t)b.g[i].xo);
                    hash = mix(hash, (uint32_t)b.g[i].yo);
                    hash = mix(hash, b.g[i].props & OF_SH_UNSAFE);
                }
            (*runs)++;
            p += len + (e ? 1 : 0);
        }
    }
    free(b.g);
    return hash;
}

int main(int argc, char **argv)
{
    size_t size, tsize;
    unsigned char *data, *text = NULL, *bad;
    OFFont f;
    unicode_tests();
    if (argc > 1) {
        data = read_file(argv[1], &size);
        if (!data || of_font_open(&f, data, size, 0) != OF_OK) {
            printf("FAIL: can't open %s\n", argv[1]);
            return 1;
        }
        {
            char family[64], style[64];
            of_font_name(&f, 1, family, sizeof family);
            of_font_name(&f, 2, style, sizeof style);
            if (!strcmp(family, "DejaVu Sans") && !strcmp(style, "Book"))
                dejavu_tests(&f);
            else
                printf("%s: not DejaVu Sans, only the fingerprint and damaged copies\n", argv[1]);
        }
        if (argc > 2 && (text = read_file(argv[2], &tsize)) != NULL) {
            uint32_t runs, hash = shape_lines(&f, (const char *)text, &runs), i;
            unsigned seed = 7;
            printf("%s: %u runs, fingerprint %08lx\n", argv[2], runs, (unsigned long)hash);
            /* damaged copies: the layout tables with bytes changed */
            bad = (unsigned char *)malloc(size);
            for (i = 0; bad && i < 24; ++i) {
                OFFont g;
                uint32_t k, r;
                memcpy(bad, data, size);
                for (k = 0; k < 200; ++k) {
                    const OFTable *t = (k % 3 == 0) ? &f.gsub : (k % 3 == 1 ? &f.gpos : &f.gdef);
                    seed = seed * 1103515245u + 12345u;
                    if (t->len)
                        bad[t->off + (seed >> 4) % t->len] = (unsigned char)(seed >> 20);
                }
                if (of_font_open(&g, bad, size, 0) == OF_OK)
                    shape_lines(&g, (const char *)text, &r);
                of_font_close(&g);
            }
            free(bad);
            free(text);
        }
        of_font_close(&f);
        free(data);
    }
    printf(failures ? "test_shape: %d failures\n" : "test_shape: ok\n", failures);
    return failures ? 1 : 0;
}
