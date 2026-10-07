// Do the handoff's three typefaces actually work with THIS renderer?
//
// Section 3.0 of the handoff names three faces and warns that one of the
// supplied files is rejected outright by browsers. "A browser accepts it" and
// "stb_truetype accepts it" are different questions, and this client renders
// with stb_truetype -- so verify against the real thing before spending an
// embed cycle and a console launch on a font that will not load.
//
// Checks per face: the file parses, the codepoints the design actually uses are
// present, and glyphs rasterise to non-blank bitmaps at the sizes section 3.0
// specifies. A font that parses but renders blanks is the failure mode that
// would show up as an empty nav bar on a TV.
//
// Build/run:  make -f Makefile.host test_fonts && ./test_fonts

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

static int g_fail = 0;

static unsigned char *slurp(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0) { fclose(f); return NULL; }
    unsigned char *buf = (unsigned char *)malloc((size_t)n);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    if (got != (size_t)n) { free(buf); return NULL; }
    *len = got;
    return buf;
}

// Rasterise one codepoint and report how many pixels carry any coverage.
static int glyph_ink(stbtt_fontinfo *fi, int cp, float px) {
    float scale = stbtt_ScaleForPixelHeight(fi, px);
    int w = 0, h = 0, xo = 0, yo = 0;
    unsigned char *bm = stbtt_GetCodepointBitmap(fi, 0, scale, cp, &w, &h, &xo, &yo);
    if (!bm) return -1;
    int ink = 0;
    for (int i = 0; i < w * h; i++) if (bm[i] > 24) ink++;
    stbtt_FreeBitmap(bm, NULL);
    return ink;
}

static void check_face(const char *label, const char *path,
                       const char *sample, float px, bool expect_ok) {
    size_t len = 0;
    unsigned char *buf = slurp(path, &len);
    if (!buf) {
        printf("  %-26s MISSING  %s\n", label, path);
        if (expect_ok) g_fail++;
        return;
    }

    stbtt_fontinfo fi;
    int off = stbtt_GetFontOffsetForIndex(buf, 0);
    bool ok = (off >= 0) && stbtt_InitFont(&fi, buf, off);
    if (!ok) {
        printf("  %-26s REJECTED by stb_truetype (%zu bytes)%s\n",
               label, len, expect_ok ? "" : "  <-- expected");
        if (expect_ok) g_fail++;
        free(buf);
        return;
    }
    if (!expect_ok) {
        printf("  %-26s parsed, but was expected to FAIL\n", label);
        g_fail++;
        free(buf);
        return;
    }

    // Every codepoint the sample needs must map to a real glyph, and draw ink.
    int missing = 0, blank = 0, total = 0;
    for (const char *p = sample; *p; p++) {
        int cp = (unsigned char)*p;
        total++;
        if (stbtt_FindGlyphIndex(&fi, cp) == 0) { missing++; continue; }
        if (*p == ' ') continue;                       // space draws nothing
        int ink = glyph_ink(&fi, cp, px);
        if (ink <= 0) blank++;
    }

    int asc = 0, desc = 0, gap = 0;
    stbtt_GetFontVMetrics(&fi, &asc, &desc, &gap);

    printf("  %-26s ok  %6zu B  %2d/%2d glyphs  asc %5d desc %5d  @%gpx\n",
           label, len, total - missing, total, asc, desc, (double)px);

    if (missing) {
        printf("      FAIL %d codepoint(s) absent from the cmap\n", missing);
        g_fail++;
    }
    if (blank) {
        printf("      FAIL %d glyph(s) rasterised blank\n", blank);
        g_fail++;
    }
    free(buf);
}

// Rodin is about to become the face for EVERY string in the UI, including
// server-supplied library and media titles.  Those are not ASCII: "Amélie",
// "Das Boot", "Coração".  A face that silently drops accented glyphs turns a
// film title into a hole on the shelf, so count what it actually covers.
static void check_latin1_coverage(const char *path) {
    size_t len = 0;
    unsigned char *buf = slurp(path, &len);
    if (!buf) { printf("  (font missing)\n"); g_fail++; return; }

    stbtt_fontinfo fi;
    int off = stbtt_GetFontOffsetForIndex(buf, 0);
    if (off < 0 || !stbtt_InitFont(&fi, buf, off)) {
        printf("  FAIL could not parse for coverage\n"); g_fail++; free(buf); return;
    }

    // Latin-1 supplement letters -- the accented range real titles use.
    int have = 0, want = 0;
    char missing[256]; missing[0] = '\0';
    for (int cp = 0xC0; cp <= 0xFF; cp++) {
        if (cp == 0xD7 || cp == 0xF7) continue;          // multiply/divide signs
        want++;
        if (stbtt_FindGlyphIndex(&fi, cp)) have++;
        else if (strlen(missing) < 200) {
            char b[8]; snprintf(b, sizeof b, "%02X ", cp);
            strcat(missing, b);
        }
    }
    printf("  Latin-1 accented letters: %d/%d\n", have, want);
    if (have < want) {
        printf("      MISSING: %s\n", missing);
        printf("      FAIL accented titles would render with holes\n");
        g_fail++;
    }
    free(buf);
}

int main(void) {
    // The COPIES THE BUILD EMBEDS, not the design project originals.  This
    // used to read ../../design-import/assets/, which meant the test passed or
    // failed on whether a scratch import directory happened to be present, and
    // checked files that are not necessarily the ones compiled in.
    const char *DS = "../source/gfx/fonts/";
    char p[512];

    printf("handoff section 3.0 typefaces, against this client's stb_truetype\n");

    // --font-system: everything. Needs the full UI character set.
    snprintf(p, sizeof p, "%sSCE-PS3-RD-R-LATIN.ttf", DS);
    check_face("Rodin LATIN (system)", p,
               "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz"
               "0123456789 .,:;!?'\"()[]-/&%+", 18.0f, true);

    // --font-display: media titles and the clock only.
    snprintf(p, sizeof p, "%smicrogramma-web.ttf", DS);
    check_face("Microgramma (display)", p,
               "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz"
               "0123456789 :.-", 16.0f, true);

    // --font-spec: codec / quality values only, e.g. "DTS-HD MA 5.1".
    snprintf(p, sizeof p, "%sMichroma-Regular.ttf", DS);
    check_face("Michroma (spec)", p,
               "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 .-/·", 12.0f, true);

    // The lockup wordmark.  It draws ONE string, so the sample is that string
    // -- but check the whole alphabet anyway, because the next revision of the
    // design could reword the lockup and this is where that would surface.
    // 14px is the size v1.0 draws it at; a face that goes blank at the only
    // size it is ever used at is the failure this file exists to catch.
    snprintf(p, sizeof p, "%sMata-Bold.otf", DS);
    check_face("Mata Bold (lockup)", p,
               "JELLYFINABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789", 14.0f, true);

    printf("\naccented coverage, since Rodin becomes the face for every string:\n");
    snprintf(p, sizeof p, "%sSCE-PS3-RD-R-LATIN.ttf", DS);
    printf("  Rodin LATIN\n");
    check_latin1_coverage(p);
    printf("  OpenSans (the face it would replace)\n");
    check_latin1_coverage("../source/gfx/fonts/OpenSans-Regular.ttf");

    // The file section 3.0 calls malformed.
    //
    // Its stated reason does NOT apply to us: section 3.0 says "every browser
    // rejects the whole file as invalid font data", but stb_truetype is lenient
    // where a browser's OpenType validator is strict, so this client parses it
    // happily. The conclusion still holds, for a worse reason -- the cmap is a
    // 3/0 SYMBOL subtable, so ASCII does not resolve and the glyphs come back
    // wrong or empty rather than the file being refused. A browser fails loudly;
    // we would fail silently, which is why this check is by CODEPOINT LOOKUP
    // and not by whether InitFont succeeded.
    printf("\nthe file section 3.0 calls malformed:\n");
    snprintf(p, sizeof p, "%s../uploads/MICROGBE.TTF", DS);
    {
        size_t len = 0;
        unsigned char *buf = slurp(p, &len);
        if (!buf) {
            printf("  %-26s MISSING\n", "MICROGBE.TTF (original)");
        } else {
            stbtt_fontinfo fi;
            int off = stbtt_GetFontOffsetForIndex(buf, 0);
            bool parsed = (off >= 0) && stbtt_InitFont(&fi, buf, off);
            int ascii_ok = 0, symbol_ok = 0;
            if (parsed) {
                for (int cp = 'A'; cp <= 'Z'; cp++)
                    if (stbtt_FindGlyphIndex(&fi, cp)) ascii_ok++;
                for (int cp = 0xF041; cp <= 0xF05A; cp++)
                    if (stbtt_FindGlyphIndex(&fi, cp)) symbol_ok++;
            }
            printf("  %-26s parsed=%s  ASCII A-Z: %d/26  symbol F041-F05A: %d/26\n",
                   "MICROGBE.TTF (original)", parsed ? "yes" : "no",
                   ascii_ok, symbol_ok);
            // The point of the check: ASCII must NOT work here, or section 3.0's
            // advice to use the rebuilt file would be unnecessary.
            if (ascii_ok == 26) {
                printf("      NOTE ASCII resolves after all -- section 3.0's\n"
                       "           rebuild may no longer be needed. Re-check.\n");
            } else {
                printf("      confirmed unusable for ASCII text; "
                       "microgramma-web.ttf is required\n");
            }
            free(buf);
        }
    }

    if (g_fail) { printf("\ntest_fonts: %d FAILURES\n", g_fail); return 1; }
    printf("\ntest_fonts: all three faces load and rasterise\n");
    return 0;
}
