// Host test for the embedded icon font (source/ui/fonts/tabler_icons.h): every
// ICON_* codepoint in source/ui/icons.h has a glyph with ink at the sizes the UI
// draws, and no two icons that the UI means to be distinct are the same shape.
//
// stb_truetype is the rasteriser the app uses (ui_text.cpp), so a glyph that
// draws here draws on the console.
//
//   make -f Makefile.host test_icons && ./test_icons
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

#include "fonts/tabler_icons.h"

static int s_checks = 0, s_failed = 0;
#define CHECK(cond) do { s_checks++; if (!(cond)) { s_failed++; \
    printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

struct Icon { char name[32]; int cp; };

static int read_icons(const char *path, Icon *out, int max) {
    FILE *f = fopen(path, "r");
    if (!f) { printf("cannot open %s\n", path); exit(2); }
    char line[256];
    int n = 0;
    while (fgets(line, sizeof line, f) && n < max) {
        char name[48]; unsigned cp;
        if (sscanf(line, "#define ICON_%47s 0x%x", name, &cp) == 2) {
            snprintf(out[n].name, sizeof out[n].name, "%s", name);
            out[n].cp = (int)cp;
            n++;
        }
    }
    fclose(f);
    return n;
}

// A small fingerprint of a glyph's picture at `px`: its box and a sum of its pixels.
static bool ink(const stbtt_fontinfo *fi, int cp, float px, int *w, int *h, unsigned *sum) {
    const float sc = stbtt_ScaleForPixelHeight(fi, px);
    int x0, y0, x1, y1;
    stbtt_GetCodepointBitmapBox(fi, cp, sc, sc, &x0, &y0, &x1, &y1);
    *w = x1 - x0; *h = y1 - y0;
    if (*w <= 0 || *h <= 0) return false;
    unsigned char *bm = (unsigned char *)calloc((size_t)(*w) * (size_t)(*h), 1);
    stbtt_MakeCodepointBitmap(fi, bm, *w, *h, *w, sc, sc, cp);
    unsigned s = 0, any = 0;
    for (int i = 0; i < *w * *h; i++) { s = s * 31u + bm[i]; any |= bm[i]; }
    free(bm);
    *sum = s;
    return any != 0;
}

int main() {
    stbtt_fontinfo fi;
    CHECK(stbtt_InitFont(&fi, (unsigned char *)TablerIcons_ttf, 0));
    CHECK(TablerIcons_ttf_len > 1000);

    Icon icons[64];
    const int n = read_icons("../source/ui/icons.h", icons, 64);
    CHECK(n >= 25);

    unsigned sums[64];
    for (int i = 0; i < n; i++) {
        const float sizes[3] = { 14.0f, 20.0f, 36.0f };    // the sizes the UI draws icons at
        for (int s = 0; s < 3; s++) {
            int w = 0, h = 0; unsigned sum = 0;
            const bool has = ink(&fi, icons[i].cp, sizes[s], &w, &h, &sum);
            if (!has) printf("  ICON_%s (0x%04X) has no ink at %.0f px\n", icons[i].name, icons[i].cp, (double)sizes[s]);
            CHECK(has);
            if (s == 2) sums[i] = sum;
        }
    }

    // The Settings categories each have a shape of their own.
    const char *cats[] = { "PLAY", "VOLUME", "CAPTIONS", "DOWNLOAD", "DESKTOP", "SETTINGS" };
    unsigned cs[6]; int found = 0;
    for (int c = 0; c < 6; c++)
        for (int i = 0; i < n; i++)
            if (!strcmp(icons[i].name, cats[c])) { cs[c] = sums[i]; found++; }
    CHECK(found == 6);
    for (int a = 0; a < 6; a++)
        for (int b = a + 1; b < 6; b++) CHECK(cs[a] != cs[b]);

    printf("%d icons, %d checks, %d failed\n", n, s_checks, s_failed);
    return s_failed ? 1 : 0;
}
