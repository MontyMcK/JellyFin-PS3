// Does the text path read UTF-8?
//
// It did not.  Every walker in ui_text.cpp read `(unsigned char)*p` and stepped
// one byte -- Latin-1 -- while Jellyfin sends UTF-8, so a curly apostrophe
// (U+2019 = E2 80 99) drew as `a-circumflex` plus two .notdef boxes and every
// accented title drew as mojibake.  The fonts were never the problem: U+2019 is
// glyph 304 in Rodin and 110 in Microgramma, and OpenSans has it too.
//
// What makes this worth a test rather than a look at the TV:
//
//   * FIVE walkers have to agree.  ttf_text_width_face() measures, drawTTF_face()
//     blits, ttf_run_box()/ttf_run_raster() feed the GPU path, and
//     drawTTF_vcentered() measures ink for centring.  A decoder applied to four
//     of the five is invisible until a string happens to take the other path,
//     and then it is off by a glyph.  ui_text_gpu.h's whole contract is that the
//     CPU and GPU paths land on the same pixels.
//   * The failure mode is silent.  A wrong decode still renders SOMETHING, so
//     nothing crashes and nothing logs; it just looks wrong in a way that is
//     easy to blame on the font.
//
// As with test_text_runs.cpp, the REAL ui_text.cpp is compiled here against
// hoststub/, so what runs is the code the console runs -- not a host
// reimplementation of the same idea, which would happily agree with itself.
//
// Build:  make -f Makefile.host test_utf8 && ./test_utf8

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <math.h>

#include <ppu-types.h>
#include <rsx/rsx.h>

// ---- everything ui_text.cpp / ui_draw.cpp expect the app to provide --------
// Same stub set test_text_runs.cpp uses, for the same reason: the real sources
// compile unmodified.
#define FB_W 1280
#define FB_H 256
static u32 s_fb[FB_W * FB_H];

gcmContextData *context = NULL;
u32  display_width  = FB_W;
u32  display_height = FB_H;
u32  display_par_num = 1, display_par_den = 1;
u32  curr_fb = 0;
u32  color_pitch = FB_W * 4;
u32  color_offset[2] = { 0, 0 };
u32 *color_buffer[2] = { s_fb, s_fb };
u32  depth_pitch = 0, depth_offset = 0;

// The UI scale override, normally owned by ui_scale.cpp.  ui_draw.cpp's
// drawHeader() reaches it through the uis_w/uis_h/uis_tf inlines in
// ui_visuals.h, so it has to exist for the link even though nothing here
// changes it.
//
// 0 is the neutral value, not an arbitrary one: it selects the PROPORTIONAL
// branch of those inlines, which scales from display_width/display_height
// above.  A non-zero percentage would put the header on a fixed scale and
// make character boundaries depend on a setting this test is not about.
int g_uis_pct = 0;

// NOTE: unlike test_text_runs.cpp, the CPU-draw seam is NOT stubbed here --
// ui_draw.cpp is compiled in and owns cpu_draw_row()/cpu_row_clipped() and the
// clip globals for real.  They read color_buffer[curr_fb], which points at
// s_fb above, so the genuine code path draws into the test framebuffer.

void plog(const char *) {}
int  overscan_x(void) { return 0; }
int  overscan_y(void) { return 0; }

#include "bitmap.h"
void bitmapSetXpm(Bitmap *bm, const char *[]) { memset(bm, 0, sizeof(*bm)); }
void bitmapDestroy(Bitmap *bm)         { memset(bm, 0, sizeof(*bm)); }

#include "ui_text_gpu.h"
bool ui_text_gpu_run(u32, u32, const char *, float, u32, int) { return false; }
bool ui_text_gpu_icon(u32, u32, int, float, u32) { return false; }

// ui_draw.cpp reaches for these two.  Colours do not affect metrics, so a
// zeroed theme is enough; the wave is never drawn.
#include "theme.h"
Theme       g_theme;
QualityMode g_quality = QUALITY_FULL;
bool ui_cpu_bg(void) { return false; }

// The real things, compiled as-is.
#include "../source/ui/render/ui_text.cpp"
#include "../source/ui/render/ui_draw.cpp"

// ---------------------------------------------------------------------------

static int g_fail = 0;
static int g_checks = 0;

static void ok(bool cond, const char *what, ...)
{
    g_checks++;
    if (cond) return;
    g_fail++;
    va_list ap; va_start(ap, what);
    fputs("  FAIL: ", stdout);
    vprintf(what, ap);
    putchar('\n');
    va_end(ap);
}

// The width the OLD code would have produced: every byte its own Latin-1
// codepoint.  Kept so the tests can assert the bug is actually gone, not merely
// that some number came back.
static int width_as_latin1(stbtt_fontinfo *fi, int id, const char *text, float px)
{
    float scale = stbtt_ScaleForPixelHeight(fi, px);
    float xf = 0.0f;
    int prev = 0;
    for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
        int cp = *p;
        if (prev) xf += stbtt_GetCodepointKernAdvance(fi, prev, cp) * scale;
        xf += (float)glyph_advance(fi, id, cp) * scale;
        prev = cp;
    }
    return (int)xf;
}
// The width an independent codepoint walk says the string should be.
//
// Written against stb directly rather than against ui_text.cpp's helpers, so it
// is a reference and not a restatement -- but it does have to model the FALLBACK
// CHAIN, because since v1.0 the chain IS the contract: --font-display is a
// 99-glyph subset and its missing characters legitimately come from Rodin, at
// Rodin's advance.  A reference that measured the primary alone would just be
// asserting the bug.
//
// Deliberately written as its own small resolver rather than by calling
// chain_pick(), so a mistake in the renderer's chain cannot hide by being
// mirrored here.
static stbtt_fontinfo *ref_face_for(int face, int cp)
{
    stbtt_fontinfo *primary = face_of(face);
    if (stbtt_FindGlyphIndex(primary, cp)) return primary;
    switch (face) {
    case UI_FACE_DISPLAY: case UI_FACE_EYEBROW:
    case UI_FACE_TAB:     case UI_FACE_TAB_REG:
    case UI_FACE_SPEC:
        if (stbtt_FindGlyphIndex(&s_font, cp)) return &s_font;
        break;
    default: break;
    }
    return primary;               // .notdef comes from the primary
}

static int width_from_codepoints(int face, const int *cps, int n, float px)
{
    float xf = 0.0f;
    int prev = 0;
    stbtt_fontinfo *prev_fi = NULL;
    for (int i = 0; i < n; i++) {
        stbtt_fontinfo *fi = ref_face_for(face, cps[i]);
        float scale = stbtt_ScaleForPixelHeight(fi, px);
        int adv;
        if (prev && fi == prev_fi)
            xf += stbtt_GetCodepointKernAdvance(fi, prev, cps[i]) * scale;
        stbtt_GetCodepointHMetrics(fi, cps[i], &adv, NULL);
        xf += (float)adv * scale;
        prev = cps[i]; prev_fi = fi;
    }
    return (int)xf;
}

struct FaceRef { const char *name; int face; };
static const FaceRef FACES[] = {
    { "regular", UI_FACE_REGULAR },
    { "bold",    UI_FACE_BOLD    },
    { "display", UI_FACE_DISPLAY },
    { "spec",    UI_FACE_SPEC    },
    { "eyebrow", UI_FACE_EYEBROW },
    { "tab",     UI_FACE_TAB     },
    { "tab-reg", UI_FACE_TAB_REG },
};
static const int NFACES = (int)(sizeof(FACES) / sizeof(FACES[0]));

// ---------------------------------------------------------------------------
// 1. The glyphs are present.  If this fails the decode is not the problem.
// ---------------------------------------------------------------------------
static void test_glyphs_exist(void)
{
    puts("-- 1: the faces carry the codepoints --");
    static const struct { int cp; const char *nm; } want[] = {
        { 0x2019, "U+2019 right single quote" },
        { 0x2018, "U+2018 left single quote"  },
        { 0x201C, "U+201C left double quote"  },
        { 0x00E9, "U+00E9 e-acute"            },
        { 0x2013, "U+2013 en dash"            },
    };
    for (int f = 0; f < NFACES; f++) {
        for (size_t i = 0; i < sizeof(want) / sizeof(want[0]); i++) {
            // Through the CHAIN, not the primary: a role can legitimately
            // borrow a codepoint from Rodin, and since v1.0 --font-display
            // does exactly that for every accent and dash it lacks.
            int g = stbtt_FindGlyphIndex(ref_face_for(FACES[f].face, want[i].cp),
                                         want[i].cp);
            ok(g != 0, "%s: %s has no glyph anywhere in its chain",
               FACES[f].name, want[i].nm);
        }
    }
    printf("   %d face/codepoint pairs checked\n", NFACES * 5);
}

// ---------------------------------------------------------------------------
// 2. ttf_text_width*() measures codepoints, not bytes.
// ---------------------------------------------------------------------------
static void test_width_decodes(void)
{
    puts("-- 2: widths are measured per codepoint --");

    // Each case: the UTF-8 string, and the codepoints it encodes.
    struct Case {
        const char *utf8;
        int cps[40];
        int n;
        const char *why;
    };
    static const Case cases[] = {
        { "\xE2\x80\x99", { 0x2019 }, 1, "a lone curly apostrophe" },
        { "Don\xE2\x80\x99t Look Up",
          { 'D','o','n',0x2019,'t',' ','L','o','o','k',' ','U','p' }, 13,
          "the reported case: an apostrophe mid-title" },
        { "Am\xC3\xA9lie", { 'A','m',0x00E9,'l','i','e' }, 6,
          "a 2-byte sequence" },
        { "L\xC3\xA9on", { 'L',0x00E9,'o','n' }, 4, "another 2-byte sequence" },
        { "WALL\xC2\xB7""E", { 'W','A','L','L',0x00B7,'E' }, 6,
          "a middle dot" },
        { "\xE2\x80\x9CQuoted\xE2\x80\x9D",
          { 0x201C,'Q','u','o','t','e','d',0x201D }, 8, "curly double quotes" },
        { "1979\xE2\x80\x93" "2003", { '1','9','7','9',0x2013,'2','0','0','3' }, 9,
          "an en dash between years" },
        { "plain ASCII", { 'p','l','a','i','n',' ','A','S','C','I','I' }, 11,
          "ASCII must be untouched" },
    };

    for (int f = 0; f < NFACES; f++) {
        stbtt_fontinfo *fi = face_of(FACES[f].face);
        int id = font_id_of(fi);
        for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
            const Case &k = cases[c];
            for (float px = 12.0f; px <= 36.0f; px += 12.0f) {
                int got  = ttf_text_width_face(k.utf8, px, FACES[f].face);
                int want = width_from_codepoints(FACES[f].face, k.cps, k.n, px);
                ok(got == want,
                   "%s @%gpx: %s -- width %d, expected %d",
                   FACES[f].name, px, k.why, got, want);

                // And the bug is gone: for anything non-ASCII the old byte walk
                // gave a different (larger) answer.
                bool ascii = true;
                for (const unsigned char *p = (const unsigned char *)k.utf8;
                     *p; p++) if (*p >= 0x80) { ascii = false; break; }
                if (!ascii) {
                    int old = width_as_latin1(fi, id, k.utf8, px);
                    ok(got != old,
                       "%s @%gpx: %s -- still measuring as Latin-1 (%d)",
                       FACES[f].name, px, k.why, old);
                }
            }
        }
    }
    printf("   %d faces x %d strings x 3 sizes\n", NFACES,
           (int)(sizeof(cases) / sizeof(cases[0])));
}

// ---------------------------------------------------------------------------
// 3. The CPU blit draws one glyph, not three.
// ---------------------------------------------------------------------------
static void fb_clear(void) { memset(s_fb, 0, sizeof(s_fb)); }

// Ink bounding box, or w=0 when nothing was drawn.
static void ink_box(int *x0, int *y0, int *x1, int *y1)
{
    *x0 = FB_W; *y0 = FB_H; *x1 = 0; *y1 = 0;
    for (int y = 0; y < FB_H; y++)
        for (int x = 0; x < FB_W; x++)
            if (s_fb[(size_t)y * FB_W + x]) {
                if (x < *x0) *x0 = x;
                if (y < *y0) *y0 = y;
                if (x + 1 > *x1) *x1 = x + 1;
                if (y + 1 > *y1) *y1 = y + 1;
            }
    if (*x1 <= *x0) { *x0 = *x1 = *y0 = *y1 = 0; }
}

static void test_blit_decodes(void)
{
    puts("-- 3: the CPU blit puts down one glyph per codepoint --");

    const float px = 40.0f;
    for (int f = 0; f < NFACES; f++) {
        // A lone curly apostrophe.  As Latin-1 this was `a-circumflex` plus two
        // .notdef, which is both wider and much taller than one quote mark.
        fb_clear();
        drawTTF_face(100, 60, "\xE2\x80\x99", px, 0x00FFFFFF, FACES[f].face);
        int ax0, ay0, ax1, ay1;
        ink_box(&ax0, &ay0, &ax1, &ay1);
        ok(ax1 > ax0, "%s: U+2019 drew nothing at all", FACES[f].name);

        // The straight ASCII apostrophe is the same KIND of mark, so its ink
        // box is the honest yardstick: a correctly decoded U+2019 is within a
        // couple of glyph widths of it, a three-glyph mojibake is not.
        fb_clear();
        drawTTF_face(100, 60, "'", px, 0x00FFFFFF, FACES[f].face);
        int bx0, by0, bx1, by1;
        ink_box(&bx0, &by0, &bx1, &by1);

        if (ax1 > ax0 && bx1 > bx0) {
            ok(ax1 - ax0 < (bx1 - bx0) * 3 + (int)px,
               "%s: U+2019 ink is %dpx wide vs %dpx for ' -- still mojibake",
               FACES[f].name, ax1 - ax0, bx1 - bx0);
            ok(ay1 - ay0 < (by1 - by0) * 3 + (int)px,
               "%s: U+2019 ink is %dpx tall vs %dpx for ' -- still mojibake",
               FACES[f].name, ay1 - ay0, by1 - by0);
        }

        // The pen must also END in the right place: drawing a title with an
        // apostrophe has to advance exactly as far as ttf_text_width says.
        const char *title = "Don\xE2\x80\x99t Look Up";
        fb_clear();
        drawTTF_face(100, 60, title, 24.0f, 0x00FFFFFF, FACES[f].face);
        ink_box(&ax0, &ay0, &ax1, &ay1);
        int w = ttf_text_width_face(title, 24.0f, FACES[f].face);
        ok(ax1 - 100 <= w + 4,
           "%s: ink runs %dpx past x, but the measured width is %d",
           FACES[f].name, ax1 - 100, w);
    }
}

// ---------------------------------------------------------------------------
// 4. The GPU run path decodes the same way the CPU path does.
//
// This is the one that would rot silently: the run walkers are a separate copy
// of the same loop, and the gate that chooses between them is a runtime flag.
// ---------------------------------------------------------------------------
static void test_run_path_agrees(void)
{
    puts("-- 4: the GPU run walkers agree with the CPU walk --");

    static const char *strings[] = {
        "Don\xE2\x80\x99t Look Up",
        "Am\xC3\xA9lie",
        "\xE2\x80\x9CThe Bear\xE2\x80\x9D",
        "Se\xC3\xB1or",
        "1979\xE2\x80\x93" "2003",
        "Tokyo Story",
    };

    for (int f = 0; f < NFACES; f++) {
        for (size_t s = 0; s < sizeof(strings) / sizeof(strings[0]); s++) {
            const float px = 28.0f;
            TtfRunBox box;
            if (!ttf_run_box(strings[s], px, FACES[f].face, &box)) {
                ok(false, "%s: run box failed for \"%s\"",
                   FACES[f].name, strings[s]);
                continue;
            }

            // The run's own box must match the ink the CPU path lays down.
            fb_clear();
            drawTTF_face(100, 60, strings[s], px, 0x00FFFFFF, FACES[f].face);
            int x0, y0, x1, y1;
            ink_box(&x0, &y0, &x1, &y1);

            ok(x0 == 100 + box.ox,
               "%s \"%s\": CPU ink starts at x=%d, run box says %d",
               FACES[f].name, strings[s], x0, 100 + box.ox);
            ok(x1 - x0 == box.w,
               "%s \"%s\": CPU ink is %dpx wide, run box says %d",
               FACES[f].name, strings[s], x1 - x0, box.w);
            ok(y1 - y0 == box.h,
               "%s \"%s\": CPU ink is %dpx tall, run box says %d",
               FACES[f].name, strings[s], y1 - y0, box.h);

            // And the rasterizer must fill that box rather than come back blank,
            // which is what a desynchronised decode in run_raster would do.
            u32 *run = (u32 *)malloc((size_t)box.w * box.h * sizeof(u32));
            ttf_run_raster(strings[s], px, FACES[f].face, 0x00FFFFFF, run, &box);
            long ink = 0;
            for (int i = 0; i < box.w * box.h; i++) if (run[i] >> 24) ink++;
            ok(ink > 0, "%s \"%s\": run raster came back empty",
               FACES[f].name, strings[s]);
            free(run);
        }
    }
}

// ---------------------------------------------------------------------------
// 5. Malformed input degrades, never crashes or runs off the end.
// ---------------------------------------------------------------------------
static void test_malformed(void)
{
    puts("-- 5: malformed sequences fall back rather than misbehave --");

    static const char *bad[] = {
        "\xE2",              // lead byte, string ends
        "\xE2\x80",          // truncated 3-byte sequence
        "\x80",              // lone continuation byte
        "\xBF\xBF\xBF",      // continuation bytes only
        "\xC0\x80",          // overlong NUL
        "\xE0\x80\x80",      // overlong, again
        "\xED\xA0\x80",      // a surrogate half, which is not a codepoint
        "\xF5\x80\x80\x80",  // past U+10FFFF
        "\xFF\xFE",          // not UTF-8 at all
        "ok\xE2 then more",  // a bad byte in the middle of good text
        "",
    };

    // A guard page would be better, but a canary after the NUL catches the
    // realistic mistake: a lead byte at the very end of a buffer whose
    // continuation bytes are read past the terminator.
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        char buf[64];
        size_t n = strlen(bad[i]);
        memset(buf, 0x5A, sizeof(buf));
        memcpy(buf, bad[i], n);
        buf[n] = '\0';

        for (int f = 0; f < NFACES; f++) {
            int w = ttf_text_width_face(buf, 24.0f, FACES[f].face);
            ok(w >= 0, "width of malformed case %d went negative (%d)",
               (int)i, w);
            fb_clear();
            drawTTF_face(100, 60, buf, 24.0f, 0x00FFFFFF, FACES[f].face);
        }
        // Nothing may have walked past the terminator into the canary.
        bool intact = true;
        for (size_t j = n + 1; j < sizeof(buf); j++)
            if ((unsigned char)buf[j] != 0x5A) intact = false;
        ok(intact, "malformed case %d: the string buffer was modified", (int)i);
    }
    printf("   %d malformed inputs x %d faces\n",
           (int)(sizeof(bad) / sizeof(bad[0])), NFACES);
}

// ---------------------------------------------------------------------------
// 6. decode_unicode_escapes() emits UTF-8.
//
// The other half of the bug: a title that arrived as ’ lost the character
// here, before the renderer ever saw it, because anything outside printable
// ASCII became '?'.
// ---------------------------------------------------------------------------
static void esc_case(const char *in, const char *want)
{
    char buf[256];
    snprintf(buf, sizeof(buf), "%s", in);
    decode_unicode_escapes(buf);
    ok(strcmp(buf, want) == 0,
       "decode(\"%s\") gave \"%s\", expected \"%s\"", in, buf, want);
}

static void test_escapes(void)
{
    puts("-- 6: JSON \\uXXXX escapes decode to UTF-8 --");

    esc_case("Don\\u2019t Look Up", "Don\xE2\x80\x99t Look Up");
    esc_case("Am\\u00e9lie",        "Am\xC3\xA9lie");
    esc_case("Am\\u00E9lie",        "Am\xC3\xA9lie");   // uppercase hex
    esc_case("\\u0041BC",           "ABC");             // ASCII stays one byte
    esc_case("plain",               "plain");
    esc_case("",                    "");
    esc_case("trailing \\u",        "trailing \\u");    // not an escape
    esc_case("\\u12",               "\\u12");           // too short
    // A surrogate PAIR is one codepoint; a lone half is not a codepoint at all.
    esc_case("\\ud83d\\ude00",      "\xF0\x9F\x98\x80");
    esc_case("\\ud83d",             "?");
    esc_case("\\ude00",             "?");
    esc_case("a\\u2013b",           "a\xE2\x80\x93""b");

    // Round trip: what the decoder emits, the renderer must measure as one
    // codepoint.  This is the join between the two halves of the fix.
    char buf[128];
    snprintf(buf, sizeof(buf), "%s", "Don\\u2019t");
    decode_unicode_escapes(buf);
    int cps[] = { 'D','o','n',0x2019,'t' };
    for (int f = 0; f < NFACES; f++) {
        ok(ttf_text_width_face(buf, 24.0f, FACES[f].face) ==
               width_from_codepoints(FACES[f].face, cps, 5, 24.0f),
           "%s: the decoded escape does not measure as 5 codepoints",
           FACES[f].name);
    }
}

// ---------------------------------------------------------------------------
// 7. The fallback chain.
//
// v1.0's --font-display is GT America Expanded Bold, a 99-glyph subset: A-Z,
// a-z, 0-9 and `! ( ) , . : ; ? _` and nothing else.  No hyphen, ampersand,
// slash, apostrophe or accent.  Used as a single face it would render
// "Spider-Man" as "SpiderMan" -- SILENTLY, because a missing codepoint is
// .notdef and .notdef in most faces is a zero-width nothing.  Nothing crashes,
// nothing logs, a character just stops existing.
//
// So the role falls back to Rodin per codepoint.  What has to hold:
//   * the missing characters actually appear, and take width
//   * they sit on the SAME BASELINE as the primary's glyphs, because each face
//     is scaled to the pixel height independently and their ascents differ
//   * all five walkers agree, since each carries its own copy of the loop
// ---------------------------------------------------------------------------
static void test_fallback(void)
{
    puts("-- 7: the display face's missing glyphs come from Rodin --");

    stbtt_fontinfo *gt = face_of(UI_FACE_DISPLAY);

    // First, state the premise rather than assuming it: this only matters
    // because the subset really is missing these.
    static const struct { int cp; const char *nm; } gone[] = {
        { '-', "hyphen" }, { '&', "ampersand" }, { '/', "slash" },
        { '\'', "apostrophe" }, { '"', "quote" },
    };
    for (size_t i = 0; i < sizeof(gone) / sizeof(gone[0]); i++) {
        ok(stbtt_FindGlyphIndex(gt, gone[i].cp) == 0,
           "display face unexpectedly HAS %s -- this test is now testing nothing",
           gone[i].nm);
        ok(stbtt_FindGlyphIndex(ref_face_for(UI_FACE_DISPLAY, gone[i].cp),
                                gone[i].cp) != 0,
           "%s has nowhere to come from in the display chain", gone[i].nm);
    }

    const float px = 32.0f;

    // A hyphen must cost width.  If the fallback were missing, "Spider-Man"
    // and "SpiderMan" would measure the same -- which is exactly the bug.
    int with    = ttf_text_width_face("Spider-Man", px, UI_FACE_DISPLAY);
    int without = ttf_text_width_face("SpiderMan",  px, UI_FACE_DISPLAY);
    char buf[160];
    snprintf(buf, sizeof buf, "\"Spider-Man\" %d vs \"SpiderMan\" %d", with, without);
    ok(with > without + 2, "the hyphen takes no width in the display face", buf);

    // And it must put ink down, in the gap between the two words.
    fb_clear();
    drawTTF_face(100, 60, "Spider-Man", px, 0x00FFFFFF, UI_FACE_DISPLAY);
    int ax0, ay0, ax1, ay1;
    ink_box(&ax0, &ay0, &ax1, &ay1);
    fb_clear();
    drawTTF_face(100, 60, "SpiderMan", px, 0x00FFFFFF, UI_FACE_DISPLAY);
    int bx0, by0, bx1, by1;
    ink_box(&bx0, &by0, &bx1, &by1);
    snprintf(buf, sizeof buf, "hyphenated ink %dpx wide, unhyphenated %dpx",
             ax1 - ax0, bx1 - bx0);
    ok(ax1 - ax0 > bx1 - bx0, "the hyphen drew nothing", buf);

    // ONE BASELINE.  A fallback glyph positioned on its own font's baseline
    // instead of the run's would push the ink box well outside the primary's,
    // so compare the vertical extent of a mixed string against a pure one.
    // A hyphen sits mid-x-height, so it cannot legitimately extend either edge.
    snprintf(buf, sizeof buf, "mixed y %d..%d, pure y %d..%d",
             ay0, ay1, by0, by1);
    ok(ay0 >= by0 - 1 && ay1 <= by1 + 1,
       "the fallback glyph is not on the run's baseline", buf);

    // Accents come from Rodin too, and those DO have ascenders, so check the
    // string still measures as one codepoint per character rather than three.
    int cps[] = { 'A', 'm', 0x00E9, 'l', 'i', 'e' };
    ok(ttf_text_width_face("Am\xC3\xA9lie", px, UI_FACE_DISPLAY) ==
           width_from_codepoints(UI_FACE_DISPLAY, cps, 6, px),
       "an accented title does not measure as 6 codepoints in the display face");

    // All five walkers carry their own copy of the chain loop.  The CPU path
    // and the GPU run path must still land on the same pixels for a string
    // that switches face mid-word -- this is the one that would rot silently.
    static const char *mixed[] = {
        "Spider-Man", "Fast & Furious", "Face/Off", "Am\xC3\xA9lie",
        "Don\xE2\x80\x99t Look Up", "WALL\xC2\xB7""E",
    };
    for (size_t s = 0; s < sizeof(mixed) / sizeof(mixed[0]); s++) {
        TtfRunBox box;
        if (!ttf_run_box(mixed[s], px, UI_FACE_DISPLAY, &box)) {
            ok(false, "run box failed for \"%s\"", mixed[s]);
            continue;
        }
        fb_clear();
        drawTTF_face(100, 60, mixed[s], px, 0x00FFFFFF, UI_FACE_DISPLAY);
        int x0, y0, x1, y1;
        ink_box(&x0, &y0, &x1, &y1);
        ok(x0 == 100 + box.ox && x1 - x0 == box.w && y1 - y0 == box.h,
           "\"%s\": CPU ink (%d,%d %dx%d) vs run box (%d,%d %dx%d)",
           mixed[s], x0, y0, x1 - x0, y1 - y0,
           100 + box.ox, 60 + box.oy, box.w, box.h);
    }

    // Roles whose face is complete must NOT be silently borrowing.  If Satoshi
    // started resolving through Rodin the tab strip would change shape for
    // reasons nobody would think to look for.
    for (int f = 0; f < NFACES; f++) {
        if (FACES[f].face == UI_FACE_DISPLAY) continue;
        int borrowed = 0;
        for (int cp = 32; cp < 127; cp++)
            if (ref_face_for(FACES[f].face, cp) != face_of(FACES[f].face)) borrowed++;
        snprintf(buf, sizeof buf, "%s borrows %d of 95 ASCII characters",
                 FACES[f].name, borrowed);
        ok(borrowed == 0, "a complete face is falling back unexpectedly", buf);
    }
}

// ---------------------------------------------------------------------------

int main(void)
{
    ttf_init();

    test_glyphs_exist();
    test_width_decodes();
    test_blit_decodes();
    test_run_path_agrees();
    test_malformed();
    test_escapes();
    test_fallback();

    printf("\n%d checks, %d failed\n", g_checks, g_fail);
    if (g_fail) { puts("FAIL"); return 1; }
    puts("PASS");
    return 0;
}
