// Do the translations fit the screen and the fonts?
//
//   * Glyph coverage: every character of every translation exists in the face chain the renderer
//     uses (Rodin, then Noto Sans JP).  A missing one would draw as nothing, silently.
//   * Kana, kanji and the Latin accents rasterise to ink, and the Japanese face is sized to the
//     same em as the Latin one.
//   * Layout: Settings rows and their help text, measured with the real faces at the 720p sizes
//     ui_settings.cpp draws them at, stay inside their boxes in every language.
//
// Build/run:  make -f Makefile.host test_i18n_text && ./test_i18n_text
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

#include "rodin_latin.h"
#include "notosansjp.h"
#include "i18n.h"
#include "settings_model.h"

static int s_checks = 0, s_failed = 0;
#define CHECK(cond) do { s_checks++; if (!(cond)) { s_failed++; \
    printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

static stbtt_fontinfo s_rodin, s_jp;
static float s_jp_k;     // the same factor ui_text.cpp computes at startup

static int next_cp(const char **p) {
    const unsigned char *s = (const unsigned char *)*p;
    int cp, n;
    if (s[0] < 0x80)      { cp = s[0]; n = 1; }
    else if (s[0] < 0xE0) { cp = s[0] & 0x1F; n = 2; }
    else if (s[0] < 0xF0) { cp = s[0] & 0x0F; n = 3; }
    else                  { cp = s[0] & 0x07; n = 4; }
    for (int i = 1; i < n; i++) cp = (cp << 6) | (s[i] & 0x3F);
    *p += n;
    return cp;
}

static bool covered(int cp) {
    return stbtt_FindGlyphIndex(&s_rodin, cp) || stbtt_FindGlyphIndex(&s_jp, cp);
}

// Pixel width of a run at `px`, the way the renderer walks it: Rodin where it has the glyph, the
// Japanese face otherwise, each at its own scale.
static float width_of(const char *t, float px) {
    const float sr = stbtt_ScaleForPixelHeight(&s_rodin, px);
    const float sj = px * s_jp_k;
    float w = 0;
    while (*t) {
        const int cp = next_cp(&t);
        int adv = 0;
        if (stbtt_FindGlyphIndex(&s_rodin, cp)) { stbtt_GetCodepointHMetrics(&s_rodin, cp, &adv, NULL); w += adv * sr; }
        else                                    { stbtt_GetCodepointHMetrics(&s_jp, cp, &adv, NULL);    w += adv * sj; }
    }
    return w;
}

static int ink(stbtt_fontinfo *fi, int cp, float scale) {
    int w = 0, h = 0, xo = 0, yo = 0;
    unsigned char *bm = stbtt_GetCodepointBitmap(fi, 0, scale, cp, &w, &h, &xo, &yo);
    if (!bm) return -1;
    int n = 0;
    for (int i = 0; i < w * h; i++) if (bm[i] > 24) n++;
    stbtt_FreeBitmap(bm, NULL);
    return n;
}

static void coverage(void) {
    printf("- glyph coverage\n");
    int missing = 0;
    for (int l = LANG_JA; l < LANG__COUNT; l++) {
        for (int i = 0; i < i18n_catalog_size(); i++) {
            const char *t = i18n_catalog_text((i18n_lang)l, i);
            for (const char *p = t; *p; ) {
                const int cp = next_cp(&p);
                if (cp == '\n' || cp < 0x20) continue;
                if (!covered(cp)) {
                    if (missing < 12) printf("  no glyph U+%04X in \"%.40s\" [%s]\n", cp, t, i18n_code((i18n_lang)l));
                    missing++;
                }
            }
        }
    }
    CHECK(missing == 0);
}

static void rasterise(void) {
    printf("- kana, kanji and accents draw\n");
    const float px = 18.0f;
    const char *kana = "ひらがなカタカナ日本語再生設定音量字幕";
    for (const char *p = kana; *p; ) {
        const int cp = next_cp(&p);
        CHECK(stbtt_FindGlyphIndex(&s_jp, cp) != 0);
        CHECK(ink(&s_jp, cp, px * s_jp_k) > 8);
    }
    const char *acc = "éèêàçñüöäßãõíóú";
    for (const char *p = acc; *p; ) {
        const int cp = next_cp(&p);
        CHECK(stbtt_FindGlyphIndex(&s_rodin, cp) != 0);
        CHECK(ink(&s_rodin, cp, stbtt_ScaleForPixelHeight(&s_rodin, px)) > 8);
    }
    // A kana em is close to a Latin em: the same 18 px asks for about the same width.
    const float jp = width_of("日", 18.0f), la = width_of("M", 18.0f);
    printf("  18 px: \"日\" %.1f px, \"M\" %.1f px\n", jp, la);
    CHECK(jp > la * 0.9f && jp < la * 1.6f);
}

// The 720p geometry of ui_settings.cpp: XMB_LIST_W = 780; the help panel text starts 24 px in.
#define LIST_W   780
#define HELP_W   (LIST_W - 48)

static void settings_layout_fits(void) {
    printf("- Settings rows fit in every language\n");
    for (int l = LANG_EN; l < LANG__COUNT; l++) {
        i18n_set((i18n_lang)l);
        float worst_help = 0, worst_label = 0;
        for (int i = 0; i < settings_count(); i++) {
            const setting_row *r = settings_row(i);
            const float lw = width_of(tr(r->label), 18.0f);
            if (lw > worst_label) worst_label = lw;
            // the label sits right of the 52 px icon column; the value (up to ~200 px) is right-aligned
            if (lw > 420.0f) printf("  label too wide (%.0f px) [%s]: %s\n", lw, i18n_code((i18n_lang)l), tr(r->label));
            CHECK(lw <= 420.0f);
            const char *t = tr(r->help);
            for (int ln = 0; ln < 2 && t && *t; ln++) {
                const char *nl = strchr(t, '\n');
                char line[400];
                size_t n = nl ? (size_t)(nl - t) : strlen(t);
                if (n > sizeof line - 1) n = sizeof line - 1;
                memcpy(line, t, n);
                line[n] = '\0';
                const float w = width_of(line, 15.0f);
                if (w > worst_help) worst_help = w;
                if (w > HELP_W) printf("  help line too wide (%.0f px) [%s]: %s\n", w, i18n_code((i18n_lang)l), line);
                CHECK(w <= HELP_W);
                t = nl ? nl + 1 : NULL;
            }
        }
        for (int s = 0; s < SEC__COUNT; s++) {
            // header: 13 px, 2 px tracking per character, upper-cased
            const char *t = tr(settings_section_label((setting_section)s));
            int chars = 0;
            for (const char *p = t; *p; ) { next_cp(&p); chars++; }
            const float w = width_of(t, 13.0f) + 2.0f * chars;
            CHECK(w <= 240.0f);
        }
        printf("  %-5s widest label %.0f px, widest help line %.0f px (limits 420 / %d)\n",
               i18n_code((i18n_lang)l), worst_label, worst_help, HELP_W);
    }
    i18n_set(LANG_EN);
}

int main(void) {
    CHECK(stbtt_InitFont(&s_rodin, (unsigned char *)Rodin_Latin_ttf, 0));
    CHECK(stbtt_InitFont(&s_jp, (unsigned char *)NotoSansJP_otf, 0));
    int asc = 0, desc = 0;
    stbtt_GetFontVMetrics(&s_rodin, &asc, &desc, NULL);
    const float rodin_upem = 1.0f / stbtt_ScaleForMappingEmToPixels(&s_rodin, 1.0f);
    const float jp_upem    = 1.0f / stbtt_ScaleForMappingEmToPixels(&s_jp, 1.0f);
    s_jp_k = (rodin_upem / (float)(asc - desc)) / jp_upem;
    printf("Rodin upem %.0f asc-desc %d; Noto JP upem %.0f; k=%.6f\n", rodin_upem, asc - desc, jp_upem, s_jp_k);

    coverage();
    rasterise();
    settings_layout_fits();
    printf("%d checks, %d failed\n", s_checks, s_failed);
    return s_failed ? 1 : 0;
}
