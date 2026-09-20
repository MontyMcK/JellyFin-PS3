// Does the circular headshot blit stay inside its circle?
//
// v1.0 makes the cast headshots round. They are drawn over the animated wave,
// so "round" is not a look — it is a contract: every pixel outside the circle
// must be left exactly as it was found, or the background tears around six
// faces on the detail screen and it will read as a wave bug, not a blit bug.
//
// The same contract is what keeps `bpx` at zero. render/circle_blit.h does its
// anti-aliasing against the SOURCE IMAGE rather than the framebuffer, so it
// never reads video memory — see the header. That is only sound if it also
// never writes where it should not, which is what this file checks.
//
// The REAL rasteriser is compiled here (it is header-only scalar C with no PS3
// headers), so this is not a host reimplementation agreeing with itself.
//
// Build/run:  make -f Makefile.host test_circle_blit && ./test_circle_blit

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdarg.h>

#include "../source/ui/render/circle_blit.h"

#define W 160
#define H 120
#define SENTINEL 0x00DEAD01u

static unsigned int g_fb[W * H];
static int g_fail = 0;
static int g_checks = 0;

static void ok(int cond, const char *fmt, ...)
{
    g_checks++;
    if (cond) return;
    g_fail++;
    va_list ap;
    va_start(ap, fmt);
    fputs("  FAIL: ", stdout);
    vprintf(fmt, ap);
    putchar('\n');
    va_end(ap);
}

static void fb_reset(void)
{
    for (int i = 0; i < W * H; i++) g_fb[i] = SENTINEL;
}

static CbSurface surf(int clip_top, int clip_bot)
{
    CbSurface s;
    s.px = g_fb; s.w = W; s.h = H; s.pitch = W;
    s.clip_top = clip_top; s.clip_bot = clip_bot;
    return s;
}

// A source image whose every pixel is distinguishable from the sentinel and
// from the rim, so "where did this pixel come from" is always answerable.
#define SRC_W 40
#define SRC_H 60
static unsigned int g_src[SRC_W * SRC_H];
static void src_init(void)
{
    for (int y = 0; y < SRC_H; y++)
        for (int x = 0; x < SRC_W; x++)
            g_src[y * SRC_W + x] = 0x00100000u | (unsigned)(y << 8) | (unsigned)x;
}
static int is_src_pixel(unsigned int p) { return (p & 0xFF0000u) == 0x100000u; }

// ---------------------------------------------------------------------------
// The invariant: nothing outside the circle changes.
// ---------------------------------------------------------------------------
static void check_outside_untouched(const char *what, int dx, int dy, int d)
{
    const float r = (float)d * 0.5f;
    const float cx = (float)dx + r, cy = (float)dy + r;
    int bad = 0, first_x = -1, first_y = -1;
    float worst = 0.0f;

    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            if (g_fb[y * W + x] == SENTINEL) continue;      // untouched: fine
            float ddx = (float)x + 0.5f - cx, ddy = (float)y + 0.5f - cy;
            float dist = sqrtf(ddx * ddx + ddy * ddy);
            if (dist > r) {                                  // written OUTSIDE
                if (!bad) { first_x = x; first_y = y; }
                if (dist - r > worst) worst = dist - r;
                bad++;
            }
        }
    }
    ok(bad == 0,
       "%s: %d pixel(s) written outside the circle, first at (%d,%d), "
       "worst %.2fpx past the radius", what, bad, first_x, first_y, (double)worst);
}

static void check_inside_filled(const char *what, int dx, int dy, int d,
                                int expect_source)
{
    // Well inside the rim, every pixel must have been written, and from the
    // image rather than left as background.
    const float r = (float)d * 0.5f;
    const float cx = (float)dx + r, cy = (float)dy + r;
    const float safe = r - 2.0f;
    if (safe <= 1.0f) return;
    int holes = 0, wrong_src = 0, tested = 0;

    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            float ddx = (float)x + 0.5f - cx, ddy = (float)y + 0.5f - cy;
            if (ddx * ddx + ddy * ddy > safe * safe) continue;
            tested++;
            if (g_fb[y * W + x] == SENTINEL) { holes++; continue; }
            if (expect_source && !is_src_pixel(g_fb[y * W + x])) wrong_src++;
        }
    }
    ok(tested > 0, "%s: the safe interior was empty, test proves nothing", what);
    ok(holes == 0, "%s: %d unwritten hole(s) inside the circle", what, holes);
    if (expect_source)
        ok(wrong_src == 0, "%s: %d interior pixel(s) did not come from the image",
           what, wrong_src);
}

// ---------------------------------------------------------------------------

static void test_basic(void)
{
    puts("-- a circle in the middle of the surface --");
    for (int d = 4; d <= 64; d += 4) {
        int dx = (W - d) / 2, dy = (H - d) / 2;
        char nm[64];
        snprintf(nm, sizeof nm, "d=%d blit", d);

        fb_reset();
        CbSurface s = surf(0, 0);
        cb_blit_circle(&s, g_src, SRC_W, SRC_H, dx, dy, d, 0x00FFFFFFu);
        check_outside_untouched(nm, dx, dy, d);
        check_inside_filled(nm, dx, dy, d, 1);

        snprintf(nm, sizeof nm, "d=%d disc", d);
        fb_reset();
        cb_fill_circle(&s, dx, dy, d, 0x00203040u, 0x00FFFFFFu);
        check_outside_untouched(nm, dx, dy, d);
        check_inside_filled(nm, dx, dy, d, 0);
    }
    printf("   16 diameters, blit and disc\n");
}

// The area written should be close to pi*r^2.  This is what catches a rasteriser
// that is "inside the circle" but only fills half of it -- every pixel would be
// legal and the picture would still be wrong.
static void test_area(void)
{
    puts("-- the disc is actually round, and actually full --");
    for (int d = 8; d <= 64; d += 8) {
        int dx = (W - d) / 2, dy = (H - d) / 2;
        fb_reset();
        CbSurface s = surf(0, 0);
        cb_fill_circle(&s, dx, dy, d, 0x00203040u, 0x00FFFFFFu);
        long written = 0;
        for (int i = 0; i < W * H; i++) if (g_fb[i] != SENTINEL) written++;
        double want = 3.14159265358979 * (d * 0.5) * (d * 0.5);
        double err  = (double)written / want;
        ok(err > 0.90 && err < 1.10,
           "d=%d: filled %ld px, a circle of that diameter is %.0f (%.0f%%)",
           d, written, want, err * 100.0);
    }
}

// Every edge, and the corners, and entirely off-surface.
static void test_clipping(void)
{
    puts("-- clipped against all four edges, and off-surface entirely --");
    const int d = 32;
    struct { int x, y; const char *nm; } pos[] = {
        { -d / 2,        H / 2,        "half off the left"    },
        { W - d / 2,     H / 2,        "half off the right"   },
        { W / 2,         -d / 2,       "half off the top"     },
        { W / 2,         H - d / 2,    "half off the bottom"  },
        { -d + 1,        -d + 1,       "top-left corner"      },
        { W - 1,         H - 1,        "bottom-right corner"  },
        { -d - 8,        H / 2,        "entirely off the left"},
        { W + 8,         H / 2,        "entirely off the right"},
        { W / 2,         -d - 8,       "entirely above"       },
        { W / 2,         H + 8,        "entirely below"       },
    };

    for (size_t i = 0; i < sizeof(pos) / sizeof(pos[0]); i++) {
        fb_reset();
        CbSurface s = surf(0, 0);
        cb_blit_circle(&s, g_src, SRC_W, SRC_H, pos[i].x, pos[i].y, d, 0x00FFFFFFu);
        check_outside_untouched(pos[i].nm, pos[i].x, pos[i].y, d);

        // Fully off-surface must write nothing at all.
        int off = (pos[i].x + d <= 0) || (pos[i].x >= W)
               || (pos[i].y + d <= 0) || (pos[i].y >= H);
        if (off) {
            long written = 0;
            for (int k = 0; k < W * H; k++) if (g_fb[k] != SENTINEL) written++;
            ok(written == 0, "%s: wrote %ld px despite being off-surface",
               pos[i].nm, written);
        }

        fb_reset();
        cb_fill_circle(&s, pos[i].x, pos[i].y, d, 0x00203040u, 0x00FFFFFFu);
        check_outside_untouched(pos[i].nm, pos[i].x, pos[i].y, d);
    }
}

// The scissor the CPU phase uses for scrolling screens.
static void test_row_clip(void)
{
    puts("-- the CPU-phase row scissor is honoured --");
    const int d = 40;
    const int dx = (W - d) / 2, dy = (H - d) / 2;
    const int top = dy + 8, bot = dy + d - 8;

    fb_reset();
    CbSurface s = surf(top, bot);
    cb_blit_circle(&s, g_src, SRC_W, SRC_H, dx, dy, d, 0x00FFFFFFu);

    int above = 0, below = 0;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++)
            if (g_fb[y * W + x] != SENTINEL) {
                if (y < top) above++;
                if (y >= bot) below++;
            }
    ok(above == 0, "%d pixel(s) written above clip_top", above);
    ok(below == 0, "%d pixel(s) written below clip_bot", below);
    check_outside_untouched("row-clipped", dx, dy, d);

    // clip_bot == 0 means "to the bottom edge", not "clip everything".
    fb_reset();
    CbSurface s2 = surf(0, 0);
    cb_blit_circle(&s2, g_src, SRC_W, SRC_H, dx, dy, d, 0x00FFFFFFu);
    long written = 0;
    for (int i = 0; i < W * H; i++) if (g_fb[i] != SENTINEL) written++;
    ok(written > 0, "clip_bot == 0 suppressed the whole circle");
}

// Source images smaller and larger than the circle, and degenerate inputs.
static void test_source_sizes(void)
{
    puts("-- source images of every shape, and degenerate inputs --");
    const int d = 48;
    const int dx = (W - d) / 2, dy = (H - d) / 2;
    struct { int w, h; } sz[] = { {1,1}, {3,7}, {40,60}, {48,48}, {200,40}, {40,200}, {512,512} };

    for (size_t i = 0; i < sizeof(sz) / sizeof(sz[0]); i++) {
        int n = sz[i].w * sz[i].h;
        unsigned int *src = (unsigned int *)malloc((size_t)n * sizeof(unsigned int));
        for (int k = 0; k < n; k++) src[k] = 0x00100000u | (unsigned)(k & 0xFFFF);
        fb_reset();
        CbSurface s = surf(0, 0);
        cb_blit_circle(&s, src, sz[i].w, sz[i].h, dx, dy, d, 0x00FFFFFFu);
        char nm[64];
        snprintf(nm, sizeof nm, "source %dx%d", sz[i].w, sz[i].h);
        check_outside_untouched(nm, dx, dy, d);
        check_inside_filled(nm, dx, dy, d, 1);
        free(src);
    }

    // None of these may write anything or crash.
    fb_reset();
    CbSurface s = surf(0, 0);
    cb_blit_circle(&s, g_src, SRC_W, SRC_H, 10, 10, 0, 0x00FFFFFFu);
    cb_blit_circle(&s, g_src, SRC_W, SRC_H, 10, 10, -5, 0x00FFFFFFu);
    cb_blit_circle(&s, NULL, SRC_W, SRC_H, 10, 10, 20, 0x00FFFFFFu);
    cb_blit_circle(&s, g_src, 0, 0, 10, 10, 20, 0x00FFFFFFu);
    cb_fill_circle(&s, 10, 10, 0, 0x00203040u, 0x00FFFFFFu);
    cb_fill_circle(&s, 10, 10, -5, 0x00203040u, 0x00FFFFFFu);
    long written = 0;
    for (int i = 0; i < W * H; i++) if (g_fb[i] != SENTINEL) written++;
    ok(written == 0, "a degenerate call wrote %ld pixel(s)", written);
}

// The rim exists, and it is on the OUTSIDE.
static void test_rim(void)
{
    puts("-- the rim is a rim --");
    const int d = 64;
    const int dx = (W - d) / 2, dy = (H - d) / 2;
    const unsigned int RIM = 0x00FF0000u;

    // A source that is uniformly black, so anything non-black came from the rim.
    unsigned int *black = (unsigned int *)calloc(SRC_W * SRC_H, sizeof(unsigned int));
    fb_reset();
    CbSurface s = surf(0, 0);
    cb_blit_circle(&s, black, SRC_W, SRC_H, dx, dy, d, RIM);

    const float r = (float)d * 0.5f;
    const float cx = (float)dx + r, cy = (float)dy + r;
    long tinted_outer = 0, tinted_inner = 0;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            unsigned int p = g_fb[y * W + x];
            if (p == SENTINEL || p == 0) continue;
            float ddx = (float)x + 0.5f - cx, ddy = (float)y + 0.5f - cy;
            float dist = sqrtf(ddx * ddx + ddy * ddy);
            if (dist > r - 1.5f) tinted_outer++;
            else                 tinted_inner++;
        }
    ok(tinted_outer > 0, "no rim was drawn at all");
    ok(tinted_inner == 0,
       "%ld rim-tinted pixel(s) landed more than 1.5px inside the edge",
       tinted_inner);
    free(black);
}

int main(void)
{
    src_init();
    test_basic();
    test_area();
    test_clipping();
    test_row_clip();
    test_source_sizes();
    test_rim();

    printf("\n%d checks, %d failed\n", g_checks, g_fail);
    if (g_fail) { puts("FAIL"); return 1; }
    puts("PASS: the circle blit stays inside its circle");
    return 0;
}
