// Does the GPU text-run path put ink on exactly the same pixels as the
// per-glyph path it replaces?
//
// ui_text_gpu.cpp turns a whole string into one texture that the RSX
// composites, instead of blending each glyph's coverage against video memory.
// That is only a safe trade if the two produce the same picture, and "it looked
// right on the TV" does not distinguish a correct run from one that is a pixel
// to the left, drops a kerned overlap, or loses the last glyph of a string.
//
// The two paths are exactly comparable for WHITE TEXT ON BLACK, by
// construction rather than by luck:
//
//   per-glyph:  aa_blend(a, 255, 0) = l2g[(a*g2l[255] + 0)/255] = l2g[a]
//   run:        stored alpha l2g[a], blended (l2g[a]*255 + 0)/255 = l2g[a]
//
// -- which is the same algebra that makes baking l2g into the stored alpha the
// right gamma fix in the first place (see ui_text_gpu.h).  So this test
// composites the run texture with the RSX's fixed-function blend formula and
// demands the result match the CPU blit.
//
// The only place they may legitimately differ by a rounding step is a pixel
// two glyph boxes SHARE: the CPU path composites the second glyph over the
// first through the gamma LUTs, the run path combines coverage first and maps
// once.  Those are the same quantity up to integer rounding, so the test
// reports how many such pixels exist and caps the deviation rather than
// pretending it is zero.  Which pixels have ink at all must match exactly.
//
// Build:  make -f Makefile.host test_text_runs && ./test_text_runs

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include <ppu-types.h>
#include <rsx/rsx.h>

// ---- the framebuffer ui_text.cpp draws into --------------------------------
#define FB_W 1280
#define FB_H 256
static u32 s_fb[FB_W * FB_H];

// ---- everything ui_text.cpp expects the rest of the app to provide ---------
// Declared in ui.h / rsxutil.h / ui_visuals.h; defined here so the real source
// compiles unmodified.  None of it is stubbed behaviour the test relies on:
// the pieces that matter (cpu_draw_row and friends) are the genuine contract,
// pointed at s_fb instead of video memory.
gcmContextData *context = NULL;
u32  display_width  = FB_W;
u32  display_height = FB_H;
u32  display_par_num = 1, display_par_den = 1;
u32  curr_fb = 0;
u32  color_pitch = FB_W * 4;
u32  color_offset[2] = { 0, 0 };
u32 *color_buffer[2] = { s_fb, s_fb };
u32  depth_pitch = 0, depth_offset = 0;

int g_cpu_clip_top = 0;
int g_cpu_clip_bot = 0;

bool  cpu_rt_on(void)        { return false; }
u32   cpu_draw_w(void)       { return FB_W; }
u32   cpu_draw_h(void)       { return FB_H; }
u32  *cpu_draw_row(u32 y)    { return s_fb + (size_t)y * FB_W; }
bool  cpu_row_clipped(int sy) {
    if (sy < g_cpu_clip_top || sy >= (int)FB_H) return true;
    if (g_cpu_clip_bot && sy >= g_cpu_clip_bot) return true;
    return false;
}

void plog(const char *) {}
int  overscan_x(void) { return 0; }
int  overscan_y(void) { return 0; }

#include "bitmap.h"
void bitmapSetXpm(Bitmap *bm, const char *[]) { memset(bm, 0, sizeof(*bm)); }
void bitmapDestroy(Bitmap *bm)         { memset(bm, 0, sizeof(*bm)); }

// The collecting window is never open here: this test is comparing the CPU
// path against the run path directly, so drawTTF must take the CPU path.
#include "ui_text_gpu.h"
bool ui_text_gpu_run(u32, u32, const char *, float, u32, int) { return false; }

// Likewise drawIcon(): ui_text.cpp calls it unconditionally, so the link needs
// it whether or not a test case draws an icon.  Returning false keeps drawIcon
// on its own per-glyph blit -- which is the path this file compares against,
// and which is NOT the gamma-corrected one the run path uses.
bool ui_text_gpu_icon(u32, u32, int, float, u32) { return false; }

// The real thing, compiled as-is.
#include "../source/ui/render/ui_text.cpp"

// ---------------------------------------------------------------------------

struct Result {
    long inked_mismatch, delta_px, max_delta, total_ink;
    long bucket_px[8], bucket_max[8];   // deltas bucketed by pixel brightness
};
static int bucket_of(u32 v) { int b = (int)(v >> 5); return b > 7 ? 7 : b; }

static void fb_clear(u32 c) { for (int i = 0; i < FB_W * FB_H; i++) s_fb[i] = c; }

// Composite a run texture the way the RSX does: straight SRC_ALPHA /
// ONE_MINUS_SRC_ALPHA, 8-bit, no gamma anywhere -- which is precisely the
// hardware behaviour the baked alpha is designed to cancel out.
static void blend_run(u32 *fb, const u32 *run, const TtfRunBox *box,
                      int x, int y)
{
    for (int r = 0; r < box->h; r++) {
        int dy = y + box->oy + r;
        if (dy < 0 || dy >= FB_H) continue;
        for (int c = 0; c < box->w; c++) {
            int dx = x + box->ox + c;
            if (dx < 0 || dx >= FB_W) continue;
            u32 s = run[(size_t)r * box->w + c];
            u32 a = s >> 24;
            if (!a) continue;
            u32 d = fb[(size_t)dy * FB_W + dx];
            u32 o = 0;
            for (int sh = 0; sh <= 16; sh += 8) {
                u32 sc = (s >> sh) & 0xFF, dc = (d >> sh) & 0xFF;
                o |= (((sc * a + dc * (255 - a)) / 255) & 0xFF) << sh;
            }
            fb[(size_t)dy * FB_W + dx] = o;
        }
    }
}

static void compare(const u32 *a, const u32 *b, u32 bg, Result *res)
{
    for (int i = 0; i < FB_W * FB_H; i++) {
        bool ia = (a[i] != bg), ib = (b[i] != bg);
        if (ia) res->total_ink++;
        if (ia != ib) { res->inked_mismatch++; continue; }
        if (!ia) continue;
        long worst = 0;
        for (int sh = 0; sh <= 16; sh += 8) {
            long d = (long)((a[i] >> sh) & 0xFF) - (long)((b[i] >> sh) & 0xFF);
            if (d < 0) d = -d;
            if (d > worst) worst = d;
        }
        if (worst) {
            res->delta_px++;
            if (worst > res->max_delta) res->max_delta = worst;
            int bk = bucket_of(a[i] & 0xFF);
            res->bucket_px[bk]++;
            if (worst > res->bucket_max[bk]) res->bucket_max[bk] = worst;
        }
    }
}

static const char *kStrings[] = {
    "Continue Watching",
    "AVATAR",                  // AV, TA -- the kerning pairs that overlap
    "To Wong Foo",
    "Taxi Driver",
    "Jellyfin",
    "PS3",
    "13/6  21:34",
    "1080p  DTS-HD MA 7.1",
    "  leading and trailing  ",
    "W",
    "i",
    ".",
    " ",                        // no ink at all -- must be rejected cleanly
    "",
    "The Lord of the Rings: The Fellowship of the Ring (Extended Edition)",
    "AVAVAVAVAVAVAVAVAVAVAVAVAV",
    "jjjjgggpppyyyqqq",          // deep descenders
    "\xC3\xA9\xC3\xA8\xC3\xBC",  // Latin-1 high bytes, as drawTTF treats them
};
static const float kSizes[] = { 13.f, 14.f, 15.f, 18.f, 19.f, 21.f, 22.f, 26.f, 30.f };
static const int   kFaces[] = { UI_FACE_REGULAR, UI_FACE_BOLD };

static u32 fb_glyph[FB_W * FB_H];
static u32 run[1920 * 96];

// Run one (string, size, face, x) case through both paths.  Returns false on a
// hard structural failure; the pixel differences accumulate into *res.
static bool run_case(const char *s, float px, int fc, int x, Result *res,
                     long *no_ink)
{
    const u32 y = 60;

    // Path A: the per-glyph CPU blit, white on black.
    fb_clear(0x00000000);
    drawTTF_face((u32)x, y, s, px, 0x00FFFFFF, fc);
    memcpy(fb_glyph, s_fb, sizeof(s_fb));

    // Path B: one run texture, composited the RSX's way.
    TtfRunBox box;
    if (!ttf_run_box(s, px, fc, &box)) {
        // No ink.  Then the CPU path must not have drawn any either, or the
        // run path is silently losing text.
        (*no_ink)++;
        for (int i = 0; i < FB_W * FB_H; i++)
            if (fb_glyph[i] != 0) {
                printf("FAIL: ttf_run_box said no ink but the glyph path drew "
                       "some: \"%s\" %.0fpx face=%d\n", s, (double)px, fc);
                return false;
            }
        return true;
    }
    if (box.w > 1920 || box.h > 96) {
        printf("FAIL: run %dx%d exceeds the staging cap: \"%s\"\n",
               box.w, box.h, s);
        return false;
    }
    ttf_run_raster(s, px, fc, 0x00FFFFFF, run, &box);

    fb_clear(0x00000000);
    blend_run(s_fb, run, &box, x, (int)y);

    compare(fb_glyph, s_fb, 0x00000000, res);
    return true;
}

int main(void)
{
    ttf_init();

    // ---- phase 1: single glyphs ------------------------------------------
    // A one-character run has no glyph boxes to overlap, so nothing can round
    // differently and the two paths must agree BIT FOR BIT.  That is the
    // strong claim -- it pins the pen position, the bitmap box, the l2g
    // identity and the blend formula all at once -- and it is what makes the
    // multi-glyph deltas below attributable to overlap rather than to a bug.
    Result one;
    memset(&one, 0, sizeof(one));
    long one_cases = 0, one_no_ink = 0;
    for (int cp = 32; cp < 127; cp++) {
        char s[2] = { (char)cp, 0 };
        for (size_t zi = 0; zi < sizeof(kSizes) / sizeof(*kSizes); zi++)
            for (size_t fi = 0; fi < sizeof(kFaces) / sizeof(*kFaces); fi++)
                for (int x = 0; x < 5; x++) {
                    if (!run_case(s, kSizes[zi], kFaces[fi], x, &one, &one_no_ink))
                        return 1;
                    one_cases++;
                }
    }
    printf("-- phase 1: single glyphs (no overlap possible) --\n");
    printf("cases           : %ld\n", one_cases);
    printf("inked pixels    : %ld\n", one.total_ink);
    printf("ink-set mismatch: %ld\n", one.inked_mismatch);
    printf("pixels differing: %ld, max channel delta %ld\n",
           one.delta_px, one.max_delta);
    if (one.inked_mismatch || one.delta_px) {
        printf("FAIL: a single glyph must be bit-exact through either path\n");
        return 1;
    }
    printf("PASS (bit-exact)\n\n");

    // ---- phase 2: real strings -------------------------------------------
    printf("-- phase 2: multi-glyph strings --\n");
    Result res;
    memset(&res, 0, sizeof(res));
    long cases = 0, no_ink = 0;

    for (size_t si = 0; si < sizeof(kStrings) / sizeof(*kStrings); si++) {
        for (size_t zi = 0; zi < sizeof(kSizes) / sizeof(*kSizes); zi++) {
            for (size_t fi = 0; fi < sizeof(kFaces) / sizeof(*kFaces); fi++) {
                for (int x = 0; x < 5; x++) {
                    const char *s  = kStrings[si];
                    float       px = kSizes[zi];
                    int         fc = kFaces[fi];
                    if (!run_case(s, px, fc, x, &res, &no_ink)) return 1;
                    cases++;
                }
            }
        }
    }

    printf("cases           : %ld (%ld of them correctly had no ink)\n",
           cases + no_ink, no_ink);
    printf("inked pixels    : %ld\n", res.total_ink);
    printf("ink-set mismatch: %ld   <-- must be 0\n", res.inked_mismatch);
    printf("pixels differing: %ld (%.4f%%), max channel delta %ld\n",
           res.delta_px,
           res.total_ink ? 100.0 * (double)res.delta_px / (double)res.total_ink : 0.0,
           res.max_delta);
    printf("  by pixel brightness (the CPU path's value):\n");
    for (int b = 0; b < 8; b++)
        printf("    %3d-%3d : %6ld px, max delta %ld\n",
               b * 32, b * 32 + 31, res.bucket_px[b], res.bucket_max[b]);

    if (res.inked_mismatch) {
        printf("FAIL: the two paths ink different pixels\n");
        return 1;
    }

    // Phase 1 proved a lone glyph is bit-exact, so every difference counted
    // here belongs to a pixel two glyph boxes SHARE -- and on those the two
    // paths are not merely different, they are differently ACCURATE.
    //
    // The CPU path composites the second glyph onto the first through the
    // gamma LUTs, so it reads back a value it has already quantised: it
    // computes g2l[l2g[a1]], and that round trip is lossy (l2g is a square
    // root rounded to 8 bits, g2l squares and floors).  The run path combines
    // a1 and a2 in coverage space and maps through l2g once, at the end, so it
    // never makes that round trip at all.
    //
    // The residue is therefore expected, bounded by the LUT round trip, and in
    // the run path's favour.  What must not happen is a LARGE divergence,
    // which would mean the coverage combine itself is wrong rather than that a
    // LUT rounded -- so the budget is a few steps out of 255, not zero and not
    // unbounded.
    const long BUDGET = 8;
    if (res.max_delta > BUDGET) {
        printf("FAIL: max channel delta %ld exceeds the %ld the gamma LUT "
               "round trip can account for\n", res.max_delta, BUDGET);
        return 1;
    }
    if (res.total_ink && res.delta_px * 100 > res.total_ink) {
        printf("FAIL: %.2f%% of pixels differ -- too many to be glyph overlap\n",
               100.0 * (double)res.delta_px / (double)res.total_ink);
        return 1;
    }
    printf("PASS\n");
    return 0;
}
