// Host test for stage C of the audio-reactive wave,
// source/ui/render/wave_layers.h, and for the WHOLE pipeline end to end:
//
//   PCM -> wave_audio -> wave_motion -> wave_kernel -> wave_spline
//                                    -> wave_layers -> wave_ribbon -> vertices
//
// Invariants, not golden values (.clinerules rule 9).
//
// THE TEST THIS FILE EXISTS FOR IS test_amplitude_budget().
//
// wave_layers.h's THE AMPLITUDE BUDGET comment claims every layer's crest
// stays inside the band by CONSTRUCTION -- that wl_curve's clamp is a
// tripwire, not a working part.  That claim is the difference between a wave
// that is composed and a wave that is merely clipped, and it is the sort of
// claim that quietly stops being true the first time a constant is retuned.
// So the test does not check that the output is in range (the clamp
// guarantees that trivially and would pass against a badly-sized layer).  It
// checks the MARGIN: how close the crest came to the clamp over a long run of
// deliberately hostile audio.  If a retune ever eats the margin, this fails
// while the picture still looks fine, which is the only time it is cheap to
// fix.
//
// The second thing here that is not a boundedness check is
// test_crossings(): the layers must actually cross each other.  The XMB
// wave's signature is not that curves move, it is that several curves cross
// at shallow angles.  Four parallel ribbons satisfy every other assertion in
// this file and look like a bar chart.
//
// libm is used freely HERE; the kernels may not use it.

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <time.h>

#include "../source/ui/render/wave_audio.h"
#include "../source/ui/render/wave_motion.h"
#include "../source/ui/render/wave_layers.h"
#include "../source/ui/render/wave_kernel.h"
#include "../source/ui/render/wave_spline.h"
#include "../source/ui/render/wave_ribbon.h"
#include "../source/ui/render/wave_field.h"
#include "../source/ui/render/wave_render_map.h"

#define NODES   96
#define SAMPLES 72
#define RATE    48000.0f
#define BLK     800
#define DT      (1.0f / 60.0f)
#define STEP_DT 2.0f                /* wave-spec.md's TIMESTEP base */

static int failures = 0;

#define CHECK(cond, ...) do {                                   \
    if (!(cond)) {                                              \
        printf("  FAIL %s:%d: ", __FILE__, __LINE__);           \
        printf(__VA_ARGS__);                                    \
        printf("\n");                                           \
        failures++;                                             \
    }                                                           \
} while (0)

// --- the whole pipeline in one object ------------------------------------

typedef struct {
    wa_state s;
    wm_state m;
    wk_chain c;
    float    y[NODES], z[NODES], vy[NODES], vz[NODES];
    float    ox[SAMPLES], oy[SAMPLES], oz[SAMPLES];
    float    ly[WL_LAYERS][SAMPLES];
    wr_vert  vb[2 * SAMPLES];
} pipe;

static void pipe_init(pipe *p)
{
    memset(p, 0, sizeof *p);
    wa_init(&p->s, RATE);
    wm_init(&p->m);
    wk_init(&p->c, p->y, p->z, p->vy, p->vz, NODES, 0xC0FFEEu);
}

// One UI frame: analyse, respond, solve, spline, build every layer.
static void pipe_frame(pipe *p, const float *pcm, int frames)
{
    wa_features f;
    int i;

    if (pcm) wa_push(&p->s, pcm, frames, 1);
    wa_frame(&p->s, DT, &f);
    wm_update(&p->m, &f, DT);

    // The three audio-derived scalars going into the EXISTING solver through
    // arguments it already takes.  This line is the entire coupling between
    // the audio system and the geometry pipeline.
    wk_step(&p->c, STEP_DT * p->m.p.timescale, p->m.p.perturb, p->m.p.drive);

    ws_build(p->y, p->z, NODES, -1.0f, 1.0f, p->ox, p->oy, p->oz, SAMPLES);
    for (i = 0; i < WL_LAYERS; i++)
        wl_curve(p->oy, SAMPLES, &p->m.p, i, p->ly[i]);
}

// --- signal generators ----------------------------------------------------

static void gen_silence(float *buf, int n, long t)
{
    (void)t;
    memset(buf, 0, (size_t)n * sizeof *buf);
}

// Loud broadband music with beats: the worst case the geometry has to survive.
static void gen_loud(float *buf, int n, long t)
{
    int i;
    for (i = 0; i < n; i++, t++) {
        double tt   = (double)t / (double)RATE;
        double beat = tt - floor(tt / 0.5) * 0.5;
        float  v;
        v  = 0.30f * sinf(2.0f * (float)M_PI * 55.0f  * (float)tt);
        v += 0.25f * sinf(2.0f * (float)M_PI * 220.0f * (float)tt);
        v += 0.20f * sinf(2.0f * (float)M_PI * 880.0f * (float)tt);
        v += 0.15f * sinf(2.0f * (float)M_PI * 4400.0f * (float)tt);
        v += 0.90f * expf(-(float)beat * 35.0f)
                   * sinf(2.0f * (float)M_PI * 68.0f * (float)beat);
        v += 0.40f * expf(-(float)beat * 110.0f)
                   * sinf(2.0f * (float)M_PI * 6000.0f * (float)beat);
        buf[i] = (v > 1.0f) ? 1.0f : ((v < -1.0f) ? -1.0f : v);
    }
}

// Full-scale square: the loudest thing a decoder can produce.
static void gen_square(float *buf, int n, long t)
{
    int i;
    for (i = 0; i < n; i++, t++) buf[i] = ((t % 64) < 32) ? 1.0f : -1.0f;
}

// --- wl_bump --------------------------------------------------------------

static void test_bump(void)
{
    int   entry = failures;
    float d;
    CHECK(fabsf(wl_bump(0.0f, 0.1f) - 1.0f) < 1e-6f,
          "bump at the centre should be 1, got %f", wl_bump(0.0f, 0.1f));

    // COMPACT SUPPORT is the reason this is not a Gaussian: a pulse must touch
    // only the samples within its own width.  If this ever stops holding the
    // cost estimate in the spec stops holding with it.
    CHECK(wl_bump(0.1f,  0.1f) == 0.0f, "bump should be exactly 0 at the edge");
    CHECK(wl_bump(0.15f, 0.1f) == 0.0f, "bump should be exactly 0 outside");
    CHECK(wl_bump(-0.15f,0.1f) == 0.0f, "bump should be exactly 0 outside, left");
    CHECK(wl_bump(5.0f,  0.1f) == 0.0f, "bump should be 0 far outside");
    CHECK(wl_bump(0.0f,  0.0f) == 0.0f, "zero width should give 0, not a divide");
    CHECK(wl_bump(0.0f, -1.0f) == 0.0f, "negative width should give 0");

    for (d = -0.3f; d <= 0.3f; d += 0.0005f) {
        float v = wl_bump(d, 0.1f);
        CHECK(v >= 0.0f && v <= 1.0f, "bump(%f) = %f out of [0,1]", d, v);
        if (failures > entry) return;
        // Symmetric.
        CHECK(fabsf(v - wl_bump(-d, 0.1f)) < 1e-6f, "bump is not symmetric at %f", d);
        if (failures > entry) return;
    }
    // C1 at the edge: the slope must reach zero there, or a pulse entering a
    // sample would show a crease.
    //
    // This checks the ORDER of the one-sided difference, not its size, and
    // that distinction is the point.  (1-t^2)^2 near t = 1 is O(eps^2), so the
    // difference quotient over a step h is O(h): at h = 1e-4 against a width
    // of 0.1 it reads 0.04, which looks like a kink and is not one.  A REAL
    // kink gives a quotient that does not shrink with h at all.  Halving h
    // must therefore halve the quotient, and that is what separates the two
    // cases -- the size alone cannot.
    {
        float h1 = 1e-4f, h2 = 5e-5f;
        float s1 = (wl_bump(0.1f, 0.1f) - wl_bump(0.1f - h1, 0.1f)) / h1;
        float s2 = (wl_bump(0.1f, 0.1f) - wl_bump(0.1f - h2, 0.1f)) / h2;
        printf("  bump: one-sided slope at the edge %.5f at h, %.5f at h/2 "
               "(ratio %.2f, want ~2)\n", s1, s2, s1 / s2);
        CHECK(s2 != 0.0f && s1 / s2 > 1.7f && s1 / s2 < 2.3f,
              "bump kinks at the edge: %.5f -> %.5f is not first order", s1, s2);
        CHECK(fabsf(s2) < 0.03f, "bump edge slope %.5f too large even at h/2", s2);
    }
}

// --- the measured constant ------------------------------------------------

static void test_base_max(void)
{
    static float y[NODES], z[NODES], vy[NODES], vz[NODES];
    static float ox[SAMPLES], oy[SAMPLES], oz[SAMPLES];
    wk_chain c;
    long  f;
    int   k;
    float peak = 0.0f;

    // WL_BASE_MAX is a pinned measurement, so it has to be re-measured or it
    // is just a number somebody typed.  Drive the solver at exactly the
    // maximum the motion stage can ever ask for and see what comes out.
    wk_init(&c, y, z, vy, vz, NODES, 0x5EEDu);
    for (f = 0; f < 60 * 300; f++) {              /* 5 min at 60 fps */
        wk_step(&c, STEP_DT * WM_MAX_TIME, WM_MAX_PERTURB, WM_MAX_DRIVE);
        ws_build(y, z, NODES, -1.0f, 1.0f, ox, oy, oz, SAMPLES);
        for (k = 0; k < SAMPLES; k++) {
            float a = fabsf(oy[k]);
            if (a > peak) peak = a;
        }
    }
    printf("  solver peak at the motion maximum: %.4f "
           "(WL_BASE_MAX %.2f, WK_KNEE %.2f)\n", peak, WL_BASE_MAX, WK_KNEE);

    CHECK(peak <= WL_BASE_MAX,
          "the solver exceeds WL_BASE_MAX: %.4f > %.2f -- every layer's "
          "amplitude budget is now wrong", peak, WL_BASE_MAX);
    // And not so far under that the band is being wasted.
    CHECK(peak > WL_BASE_MAX * 0.80f,
          "WL_BASE_MAX %.2f is stale and over-generous: the solver only "
          "reaches %.4f", WL_BASE_MAX, peak);

    // The solver must stay out of its OWN soft clip.  A drive high enough to
    // sit inside WK_KNEE compresses the top of the dynamic range, so loud
    // passages stop growing; that is what pinned WM_MAX_DRIVE at 1.10.
    CHECK(peak < WK_KNEE,
          "the solver reaches %.4f, inside its own soft clip at %.2f -- "
          "WM_MAX_DRIVE is too high", peak, WK_KNEE);
}

// --- THE amplitude budget -------------------------------------------------

static void run_budget(void (*gen)(float *, int, long), long frames,
                       const char *what, float *worst_lo, float *worst_hi)
{
    int entry = failures;      /* local: an earlier failure elsewhere must not
                                  skip this run entirely */
    static float buf[BLK];
    static pipe  p;
    long f;
    long t = 0;
    int  i, k;

    *worst_lo = 1e9f;
    *worst_hi = 1e9f;
    pipe_init(&p);

    for (f = 0; f < frames; f++) {
        gen(buf, BLK, t);
        t += BLK;
        pipe_frame(&p, buf, BLK);

        for (i = 0; i < WL_LAYERS; i++) {
            for (k = 0; k < SAMPLES; k++) {
                float v  = p.ly[i][k];
                float lo = v - WL_BAND_BOT;       /* room below */
                float hi = WL_BAND_TOP - v;       /* room above */
                CHECK(v == v, "%s: NaN in layer %d sample %d at frame %ld",
                      what, i, k, f);
                if (failures > entry) return;
                if (lo < *worst_lo) *worst_lo = lo;
                if (hi < *worst_hi) *worst_hi = hi;
            }
        }
    }
}

static void test_amplitude_budget(void)
{
    struct { void (*gen)(float *, int, long); const char *name; long frames; }
    cases[3] = {
        { gen_silence, "silence",     3600 },
        { gen_loud,    "loud music", 10800 },
        { gen_square,  "square wave", 3600 },
    };
    int j;

    for (j = 0; j < 3; j++) {
        float lo, hi;
        int   entry = failures;
        run_budget(cases[j].gen, cases[j].frames, cases[j].name, &lo, &hi);
        if (failures > entry) return;
        printf("  %-12s over %5.0f s: closest to the floor %.4f, "
               "to the ceiling %.4f\n",
               cases[j].name, (float)cases[j].frames * DT, lo, hi);

        // The clamp must never have engaged.  Not "the output is in range" --
        // the clamp guarantees that for free and would pass against a badly
        // sized layer.  A real margin is the claim wave_layers.h makes.
        CHECK(lo > 0.002f,
              "%s: a crest came within %.5f of the band floor -- wl_curve's "
              "clamp is doing real work, so THE AMPLITUDE BUDGET is wrong",
              cases[j].name, lo);
        CHECK(hi > 0.002f,
              "%s: a crest came within %.5f of the band ceiling -- same",
              cases[j].name, hi);
    }
}

// --- depth, crossings, liveliness ----------------------------------------

static void test_depth_ordering(void)
{
    int i;
    // Vertical order, back to front.  If two layers ever swap the depth
    // reading inverts and the whole image flattens.
    for (i = 1; i < WL_LAYERS; i++)
        CHECK(WL_BASE_Y[i] > WL_BASE_Y[i - 1],
              "layer %d sits below layer %d", i, i - 1);

    // Thickness RISES with distance: near things are thin and sharp.
    for (i = 1; i < WL_LAYERS; i++)
        CHECK(wl_half(i) < wl_half(i - 1),
              "layer %d is thicker than layer %d", i, i - 1);

    // Spatial frequency rises with proximity, and so does the pulse response.
    for (i = 1; i < WL_LAYERS; i++) {
        CHECK(WL_SEC_K[i] > WL_SEC_K[i - 1],
              "layer %d has a lower wavenumber than layer %d", i, i - 1);
        CHECK(WL_PULSE_AMP[i] > WL_PULSE_AMP[i - 1],
              "layer %d responds to pulses less than layer %d", i, i - 1);
        CHECK(WM_PHASE_RATE[i] > WM_PHASE_RATE[i - 1],
              "layer %d drifts slower than layer %d", i, i - 1);
    }

    // The furthest layer carries no fine detail at all.
    CHECK(WL_RIP_AMP[0] == 0.0f, "the furthest layer has a ripple term");

    CHECK(wl_half(-1) == 0.0f && wl_half(WL_LAYERS) == 0.0f,
          "wl_half should reject an out-of-range layer");
}

static void test_crossings(void)
{
    static float buf[BLK];
    static pipe  p;
    long f, t = 0;
    int  i, k, pair;
    int  crossings[WL_LAYERS - 1];

    for (i = 0; i < WL_LAYERS - 1; i++) crossings[i] = 0;
    pipe_init(&p);

    // Four minutes of real music.  A crossing is a sign change in the
    // difference between two adjacent layers' crests along the ribbon.
    for (f = 0; f < 60 * 240; f++) {
        gen_loud(buf, BLK, t);
        t += BLK;
        pipe_frame(&p, buf, BLK);

        for (pair = 0; pair < WL_LAYERS - 1; pair++) {
            int sign = 0;
            for (k = 0; k < SAMPLES; k++) {
                float d = p.ly[pair][k] - p.ly[pair + 1][k];
                int   s = (d > 0.0f) ? 1 : ((d < 0.0f) ? -1 : 0);
                if (s == 0) continue;
                if (sign != 0 && s != sign) crossings[pair]++;
                sign = s;
            }
        }
    }

    printf("  crossings over 240 s (adjacent layer pairs):");
    for (i = 0; i < WL_LAYERS - 1; i++) printf(" %d-%d:%d", i, i + 1, crossings[i]);
    printf("\n");

    // THE assertion.  Four parallel ribbons pass everything else in this file.
    for (i = 0; i < WL_LAYERS - 1; i++)
        CHECK(crossings[i] > 0,
              "layers %d and %d never crossed in 240 s -- the wave is a bar "
              "chart, not an XMB wave", i, i + 1);
}

static void test_idle_is_alive(void)
{
    static float buf[BLK];
    static pipe  p;
    float prev[WL_LAYERS][SAMPLES];
    float total = 0.0f, worst_step = 0.0f;
    long  f;
    int   i, k;

    // Silence must produce a calm idle state, not a dead screen.  Two things
    // have to hold at once and they pull against each other: the geometry has
    // to KEEP MOVING, and it has to move SLOWLY.
    pipe_init(&p);
    for (f = 0; f < 600; f++) { gen_silence(buf, BLK, 0); pipe_frame(&p, buf, BLK); }
    memcpy(prev, p.ly, sizeof prev);

    for (f = 0; f < 3600; f++) {                  /* 60 s */
        gen_silence(buf, BLK, 0);
        pipe_frame(&p, buf, BLK);
        for (i = 0; i < WL_LAYERS; i++) {
            for (k = 0; k < SAMPLES; k++) {
                float d = fabsf(p.ly[i][k] - prev[i][k]);
                total += d;
                if (d > worst_step) worst_step = d;
            }
        }
        memcpy(prev, p.ly, sizeof prev);
    }

    printf("  idle over 60 s: mean per-frame movement %.3e clip units, "
           "largest single step %.3e\n",
           total / (float)(3600 * WL_LAYERS * SAMPLES), worst_step);

    // ALIVE.
    CHECK(total > 0.0f, "the idle wave is completely static");
    CHECK(total / (float)(3600 * WL_LAYERS * SAMPLES) > 1e-6f,
          "the idle wave barely moves -- silence reads as a crashed screen");
    // CALM.  0.01 clip units is ~5 px at 1080p in one frame.
    CHECK(worst_step < 0.01f,
          "the idle wave jumped %.4f clip units in one frame", worst_step);
}

static void test_pulses_move_geometry(void)
{
    static pipe p;
    wm_params   q;
    float       flat[SAMPLES], with[SAMPLES], without[SAMPLES];
    int         k, lifted = 0;
    float       peak_at = 0.0f, peak = -1e9f;

    // A pulse has to actually deform the ribbon, and it has to do it LOCALLY
    // -- that is the whole difference between a gesture travelling through the
    // wave and the wave flashing.
    pipe_init(&p);
    memset(&q, 0, sizeof q);
    q.drive = WM_IDLE_DRIVE; q.perturb = WM_IDLE_PERTURB;
    q.timescale = 1.0f; q.hue = 0.5f; q.bright = 0.5f;
    for (k = 0; k < WL_LAYERS; k++) q.amp[k] = 0.5f;
    for (k = 0; k < SAMPLES; k++) flat[k] = 0.0f;

    wl_curve(flat, SAMPLES, &q, WL_FILAMENT, without);

    q.pulse[0].live  = 1;
    q.pulse[0].x     = 0.5f;
    q.pulse[0].amp   = 1.0f;
    q.pulse[0].width = 0.12f;
    wl_curve(flat, SAMPLES, &q, WL_FILAMENT, with);

    for (k = 0; k < SAMPLES; k++) {
        float d = with[k] - without[k];
        float u = (float)k / (float)(SAMPLES - 1);
        if (d > 1e-6f) lifted++;
        if (d > peak) { peak = d; peak_at = u; }
        // Outside the pulse's support nothing may move at all.
        if (fabsf(u - 0.5f) >= 0.12f)
            CHECK(fabsf(d) < 1e-6f,
                  "a pulse at u=0.5 width 0.12 moved the curve at u=%.3f by %.2e",
                  u, d);
    }
    printf("  pulse: lifted %d of %d samples, peak %.4f at u = %.3f\n",
           lifted, SAMPLES, peak, peak_at);
    CHECK(lifted > 2, "a full-amplitude pulse moved only %d samples", lifted);
    CHECK(lifted < SAMPLES / 2,
          "a pulse moved %d of %d samples -- that is a flash, not a gesture",
          lifted, SAMPLES);
    CHECK(fabsf(peak_at - 0.5f) < 0.03f, "the pulse peaked at u=%.3f, not 0.5",
          peak_at);
    // The peak reads slightly UNDER WL_PULSE_AMP, and must: u = 0.5 falls
    // between two samples on a 72-point grid, so the nearest sample sits
    // 1/142 of the span from the pulse's centre and picks the bump up a little
    // down its flank.  Asserting equality would be asserting that a pulse
    // happens to land on a sample, which is a property of the sample count
    // rather than of the code.
    CHECK(peak <= WL_PULSE_AMP[WL_FILAMENT] + 1e-6f,
          "the pulse lifted the curve by %.5f, past its own limit %.5f",
          peak, WL_PULSE_AMP[WL_FILAMENT]);
    CHECK(peak > WL_PULSE_AMP[WL_FILAMENT] * 0.97f,
          "the pulse only lifted the curve by %.5f of a possible %.5f",
          peak, WL_PULSE_AMP[WL_FILAMENT]);
}

// --- colour ---------------------------------------------------------------

static void test_colour(void)
{
    wm_params q;
    wl_rgb    accent     = { 0xAA, 0x5C, 0xC3 };   /* theme XMB wave */
    wl_rgb    accent_alt = { 0x00, 0xA4, 0xDC };
    wl_rgb    bg_crest   = { 0x0D, 0x10, 0x22 };
    wl_rgb    bg_foot    = { 0x05, 0x06, 0x0C };
    wl_rgb    top, bot, top2, bot2;
    int       i;

    memset(&q, 0, sizeof q);
    q.bright = 0.5f;
    q.hue    = WM_HUE_LO;

    // Violet end vs cyan end: blue must not fall, and red must.
    wl_shade(&q, WL_BODY, accent, accent_alt, bg_crest, bg_foot, 28, 1.0f,
             &top, &bot);
    q.hue = WM_HUE_LO + WM_HUE_SPAN;
    wl_shade(&q, WL_BODY, accent, accent_alt, bg_crest, bg_foot, 28, 1.0f,
             &top2, &bot2);
    printf("  hue %.2f -> crest #%02X%02X%02X,  hue %.2f -> crest #%02X%02X%02X\n",
           WM_HUE_LO, top.r, top.g, top.b,
           WM_HUE_LO + WM_HUE_SPAN, top2.r, top2.g, top2.b);
    CHECK(top2.r <= top.r, "the cyan end is redder than the violet end");
    CHECK(top2.g >= top.g, "the cyan end is less green than the violet end");

    // Brightness must raise the crest, monotonically.
    {
        int prev = -1;
        for (i = 0; i <= 10; i++) {
            q.bright = (float)i * 0.1f;
            wl_shade(&q, WL_BODY, accent, accent_alt, bg_crest, bg_foot, 28, 1.0f,
                     &top, &bot);
            CHECK((int)top.g >= prev, "crest brightness is not monotone at %.1f",
                  q.bright);
            prev = (int)top.g;
        }
    }

    // THE CHROMATIC FRINGE.  Crest cool, foot warm -- this is the whole of the
    // CRT atmosphere and it is three multiplies, so it had better be there.
    q.bright = 0.8f;
    q.hue    = 0.5f;
    wl_shade(&q, WL_BODY, accent, accent_alt, bg_crest, bg_foot, 200, 1.0f,
             &top, &bot);
    printf("  fringe: crest #%02X%02X%02X over bg #%02X%02X%02X, "
           "foot #%02X%02X%02X over bg #%02X%02X%02X\n",
           top.r, top.g, top.b, bg_crest.r, bg_crest.g, bg_crest.b,
           bot.r, bot.g, bot.b, bg_foot.r, bg_foot.g, bg_foot.b);
    {
        // The crest must have moved further from the background in blue than
        // in red, relative to how far the tint itself is in each channel.
        int dr = (int)top.r - (int)bg_crest.r;
        int db = (int)top.b - (int)bg_crest.b;
        CHECK(db > dr, "the crest does not fringe cool: dr %d, db %d", dr, db);
        // The foot is faint, and warm relative to the crest.
        CHECK(bot.r >= bg_foot.r && bot.b >= bg_foot.b, "the foot went darker than the bg");
        CHECK((int)bot.r - (int)bg_foot.r >= (int)bot.b - (int)bg_foot.b,
              "the foot does not fringe warm");
    }

    // Suppression, per handoff section 1.5: a third whenever a hero backdrop
    // is on screen.  It must pull the crest toward the background, not away.
    wl_shade(&q, WL_BODY, accent, accent_alt, bg_crest, bg_foot, 200, 0.333f,
             &top2, &bot2);
    CHECK(abs((int)top2.g - (int)bg_crest.g) < abs((int)top.g - (int)bg_crest.g),
          "suppress did not dim the wave");
    wl_shade(&q, WL_BODY, accent, accent_alt, bg_crest, bg_foot, 200, 0.0f,
             &top2, &bot2);
    CHECK(top2.r == bg_crest.r && top2.g == bg_crest.g && top2.b == bg_crest.b,
          "suppress 0 should leave the background untouched");

    // Only the near layers glint.
    q.glow = 1.0f;
    {
        wl_rgb a0, b0, a1, b1;
        q.glow = 0.0f;
        wl_shade(&q, WL_SWELL, accent, accent_alt, bg_crest, bg_foot, 200, 1.0f, &a0, &b0);
        q.glow = 1.0f;
        wl_shade(&q, WL_SWELL, accent, accent_alt, bg_crest, bg_foot, 200, 1.0f, &a1, &b1);
        CHECK(a0.g == a1.g, "glow reached the furthest layer");
        q.glow = 0.0f;
        wl_shade(&q, WL_SHEEN, accent, accent_alt, bg_crest, bg_foot, 200, 1.0f, &a0, &b0);
        q.glow = 1.0f;
        wl_shade(&q, WL_SHEEN, accent, accent_alt, bg_crest, bg_foot, 200, 1.0f, &a1, &b1);
        CHECK(a1.g > a0.g, "glow did not reach the nearest layer");
    }

    // Defensive: a bad layer or a null param must yield the background rather
    // than reading off the end of a constant table.
    wl_shade(&q, -1, accent, accent_alt, bg_crest, bg_foot, 28, 1.0f, &top, &bot);
    CHECK(top.r == bg_crest.r && bot.r == bg_foot.r, "a bad layer index leaked");
    wl_shade(NULL, WL_BODY, accent, accent_alt, bg_crest, bg_foot, 28, 1.0f, &top, &bot);
    CHECK(top.r == bg_crest.r, "a null param leaked");
    wl_shade(&q, WL_BODY, accent, accent_alt, bg_crest, bg_foot, 28, 1.0f, NULL, NULL);
}

// --- the ribbon stage accepts it -----------------------------------------

static void test_ribbon_integration(void)
{
    int entry = failures;
    static float buf[BLK];
    static pipe  p;
    long f, t = 0;
    int  i, k, n;

    pipe_init(&p);
    for (f = 0; f < 60 * 60; f++) {
        gen_loud(buf, BLK, t);
        t += BLK;
        pipe_frame(&p, buf, BLK);

        for (i = 0; i < WL_LAYERS; i++) {
            n = wr_build(p.ox, p.ly[i], SAMPLES, wl_half(i),
                         0x40, 0x50, 0xA0, 0x10, 0x14, 0x30,
                         p.vb, 2 * SAMPLES);
            CHECK(n == 2 * SAMPLES, "wr_build returned %d, expected %d",
                  n, 2 * SAMPLES);
            if (failures > entry) return;
            for (k = 0; k < n; k++) {
                const wr_vert *v = &p.vb[k];
                CHECK(v->x >= -1.0f && v->x <= 1.0f, "vertex x %f out of clip space", v->x);
                CHECK(v->y >= -1.0f && v->y <= 1.0f, "vertex y %f out of clip space", v->y);
                CHECK(v->z == 0.0f && v->w == 1.0f, "vertex z/w wrong");
                CHECK((v->rgba & 0xFFu) == 0xFFu, "vertex alpha is not opaque");
                if (failures > entry) return;
            }
        }
    }
    printf("  %d layers x %d vertices = %d vertices per frame "
           "(the shipping wave draws 586)\n",
           WL_LAYERS, 2 * SAMPLES, WL_LAYERS * 2 * SAMPLES);
}

// --- defensiveness, determinism, and a rough cost ------------------------

static void test_defensive(void)
{
    int entry = failures;
    wm_params q;
    float in[SAMPLES], out[SAMPLES];
    int k;

    memset(&q, 0, sizeof q);
    q.hue = 0.5f;
    for (k = 0; k < SAMPLES; k++) { in[k] = 0.0f; out[k] = 12345.0f; }

    CHECK(wl_curve(NULL, SAMPLES, &q, 0, out) == 0, "null base_y should fail");
    CHECK(wl_curve(in, SAMPLES, &q, 0, NULL) == 0, "null out should fail");
    CHECK(wl_curve(in, SAMPLES, NULL, 0, out) == 0, "null params should fail");
    CHECK(wl_curve(in, 1, &q, 0, out) == 0, "m of 1 should fail");
    CHECK(wl_curve(in, 0, &q, 0, out) == 0, "m of 0 should fail");
    CHECK(wl_curve(in, SAMPLES, &q, -1, out) == 0, "a negative layer should fail");
    CHECK(wl_curve(in, SAMPLES, &q, WL_LAYERS, out) == 0, "layer == WL_LAYERS should fail");
    CHECK(out[0] == 12345.0f, "a rejected call wrote to out");

    // A params struct full of NaN must still produce finite geometry: the
    // clamps in wl_curve are the last line before the vertex buffer, and a NaN
    // vertex is a wedged GPU rather than a glitchy frame.
    {
        int i;
        float *fp = (float *)&q;
        for (i = 0; i < (int)(sizeof q / sizeof(float)); i++) fp[i] = (float)NAN;
        for (k = 0; k < SAMPLES; k++) in[k] = (float)NAN;
        CHECK(wl_curve(in, SAMPLES, &q, WL_BODY, out) == SAMPLES,
              "wl_curve rejected a NaN-filled params struct instead of "
              "handling it");
        for (k = 0; k < SAMPLES; k++) {
            CHECK(out[k] == out[k], "NaN survived to the geometry at sample %d", k);
            CHECK(out[k] >= WL_BAND_BOT && out[k] <= WL_BAND_TOP,
                  "a NaN-driven sample left the band: %f", out[k]);
            if (failures > entry) return;
        }
    }
}

static void test_determinism(void)
{
    static float buf[BLK];
    static pipe  a, b;
    long f, t;

    pipe_init(&a);
    for (f = 0, t = 0; f < 1200; f++, t += BLK) { gen_loud(buf, BLK, t); pipe_frame(&a, buf, BLK); }
    pipe_init(&b);
    for (f = 0, t = 0; f < 1200; f++, t += BLK) { gen_loud(buf, BLK, t); pipe_frame(&b, buf, BLK); }

    CHECK(memcmp(a.ly, b.ly, sizeof a.ly) == 0,
          "identical audio produced different geometry");
    CHECK(memcmp(&a.m, &b.m, sizeof a.m) == 0,
          "identical audio produced different motion state");
}

// Not an assertion -- host timing is not PS3 timing, and .clinerules rule 9
// asks for invariants rather than golden values.  It is printed because the
// spec makes a cost claim (~50 us a frame all in on the PPU) and a reader
// deserves to see the shape of the number the claim was made from.
static void report_cost(void)
{
    static float buf[BLK];
    static pipe  p;
    clock_t t0, t1;
    long f, t = 0;
    const long N = 6000;

    pipe_init(&p);
    gen_loud(buf, BLK, 0);
    pipe_frame(&p, buf, BLK);                     /* warm */

    t0 = clock();
    for (f = 0; f < N; f++) {
        gen_loud(buf, BLK, t);
        t += BLK;
        pipe_frame(&p, buf, BLK);
    }
    t1 = clock();
    printf("  full pipeline, %ld frames including signal generation: "
           "%.1f us/frame on this host\n",
           N, 1e6 * (double)(t1 - t0) / (double)CLOCKS_PER_SEC / (double)N);
    printf("  (host timing, not PS3 timing -- the PPU is ~3.2 GHz in-order "
           "with no speculation, so expect several times this)\n");
}


// --- the renderer calibration seam ---------------------------------------

static void test_render_mapping(void)
{
    wrm_out  o;
    wm_state m;
    float    prev;
    int      i;

    // wrm_map(NULL) is the path taken when the gate is off, when wa_init
    // failed, and on the very first frame before the analyser has started.  It
    // has to be the values ui_wave.cpp passed BEFORE any of this existed, or
    // turning the feature off would not actually restore the old behaviour.
    wrm_map(NULL, &o);
    printf("  idle mapping: dt_scale %.4f, perturb %.4f, drive %.4f\n",
           o.dt_scale, o.perturb, o.drive);
    CHECK(o.dt_scale == 1.0f,
          "idle dt_scale is %.5f, not exactly 1 -- the drift rate with no "
          "music would differ from today's", o.dt_scale);
    CHECK(o.perturb == 0.02f,
          "idle perturb is %.5f, not the 0.02 literal ui_wave.cpp used",
          o.perturb);
    CHECK(o.drive == WRM_DRIVE_IDLE, "idle drive is %.4f", o.drive);

    // A freshly initialised motion stage must map to exactly the same thing,
    // so a client that never feeds audio and one whose gate is off look
    // identical rather than merely similar.
    wm_init(&m);
    {
        wrm_out n;
        wrm_map(&m.p, &n);
        CHECK(n.dt_scale == o.dt_scale && n.perturb == o.perturb &&
              n.drive == o.drive,
              "a fresh wm_state does not map to the idle set: "
              "%.4f/%.4f/%.4f vs %.4f/%.4f/%.4f",
              n.dt_scale, n.perturb, n.drive, o.dt_scale, o.perturb, o.drive);
    }

    // Monotone and bounded across the whole of stage B's range.
    prev = -1.0f;
    for (i = 0; i <= 100; i++) {
        wm_params p;
        memset(&p, 0, sizeof p);
        p.drive     = WM_IDLE_DRIVE + (WM_MAX_DRIVE - WM_IDLE_DRIVE) * (float)i * 0.01f;
        p.timescale = WM_MIN_TIME   + (WM_MAX_TIME  - WM_MIN_TIME)   * (float)i * 0.01f;
        p.perturb   = WM_MAX_PERTURB * (float)i * 0.01f;
        wrm_map(&p, &o);
        CHECK(o.drive >= WRM_DRIVE_IDLE && o.drive <= WRM_DRIVE_MAX,
              "mapped drive %.4f out of range at i=%d", o.drive, i);
        CHECK(o.dt_scale >= WRM_TS_MIN && o.dt_scale <= WRM_TS_MAX,
              "mapped dt_scale %.4f out of range at i=%d", o.dt_scale, i);
        CHECK(o.drive >= prev, "mapped drive is not monotone at i=%d", i);
        if (failures) return;
        prev = o.drive;
    }
    CHECK(fabsf(o.drive - WRM_DRIVE_MAX) < 1e-5f,
          "stage B's maximum drive maps to %.4f, not WRM_DRIVE_MAX %.4f",
          o.drive, WRM_DRIVE_MAX);

    // NaN must not reach the solver: wf_step hands drive to wk_step, which
    // rejects a bad dt but multiplies drive straight into the target field.
    {
        wm_params p;
        memset(&p, 0, sizeof p);
        p.drive = (float)NAN; p.timescale = (float)NAN; p.perturb = (float)NAN;
        wrm_map(&p, &o);
        CHECK(o.drive == o.drive && o.dt_scale == o.dt_scale &&
              o.perturb == o.perturb, "NaN survived the mapping");
        CHECK(o.drive >= WRM_DRIVE_IDLE && o.dt_scale >= WRM_TS_MIN,
              "a NaN mapped below the idle floor");
    }
}

// THE assertion this seam exists for: the mapped maximum must keep the chain
// clear of its own soft clip.
//
// WM_MAX_DRIVE was pinned at 1.10 because 1.30 reaches 0.840 against a
// WK_KNEE of 0.80, and a solver sitting in its own clip stops responding to
// level -- the clip is monotone but compressive, so the top of the dynamic
// range flattens out.  That reasoning was done against a bare wk_chain.  What
// the renderer actually drives is wf_step, which scales dt by WF_RATE (up to
// 1.34) and drive by WF_DRIVE per layer, so the property has to be re-checked
// through THAT api or it is not checked at all.
//
// dt is swept rather than taking ui_wave.cpp's WAVE_FIELD_DT, which is a
// constant in a .cpp this test cannot see.  Sweeping is the stronger check
// anyway: it holds for any frame-dt constant the renderer might be retuned to
// inside the swept range, instead of pinning a duplicate of one number.
static void test_mapped_drive_clears_the_knee(void)
{
    static wf_field f;
    const float dts[4] = { 0.75f, 1.25f, 2.00f, 3.00f };
    int   d, l, k;
    long  step;

    for (d = 0; d < 4; d++) {
        float dt   = dts[d] * WRM_TS_MAX;
        float peak = 0.0f;

        wf_init(&f, 0);
        for (step = 0; step < 60 * 180; step++) {      /* 3 min at 60 fps */
            wf_step(&f, dt, WM_MAX_PERTURB, WRM_DRIVE_MAX);
            for (l = 0; l < WF_LAYERS; l++) {
                for (k = 0; k <= 64; k++) {
                    float v = wf_disp(&f, l, (float)k * (1.0f / 64.0f));
                    float a = (v < 0.0f) ? -v : v;
                    if (a > peak) peak = a;
                }
            }
        }
        printf("  wf_step at dt %.2f x %.2f, drive %.2f: peak %.4f "
               "(WK_KNEE %.2f)\n", dts[d], WRM_TS_MAX, WRM_DRIVE_MAX,
               peak, WK_KNEE);
        CHECK(peak < WK_KNEE,
              "at dt %.2f the mapped maximum drive reaches %.4f, inside the "
              "solver's own soft clip at %.2f -- WRM_DRIVE_MAX is too high",
              dts[d], peak, WK_KNEE);
        CHECK(peak > 0.20f,
              "at dt %.2f the mapped maximum only reaches %.4f -- the wave "
              "would barely move at full volume", dts[d], peak);
    }
}

int main(void)
{
    printf("wave_layers: %d layers, %d nodes, %d samples, band [%.2f, %.2f]\n",
           WL_LAYERS, NODES, SAMPLES, WL_BAND_BOT, WL_BAND_TOP);

    printf("\n-- the pulse bump --\n");            test_bump();
    printf("\n-- WL_BASE_MAX, re-measured --\n");  test_base_max();
    printf("\n-- THE AMPLITUDE BUDGET --\n");      test_amplitude_budget();
    printf("\n-- depth ordering --\n");            test_depth_ordering();
    printf("\n-- the layers must cross --\n");     test_crossings();
    printf("\n-- idle is alive but calm --\n");    test_idle_is_alive();
    printf("\n-- pulses deform locally --\n");     test_pulses_move_geometry();
    printf("\n-- colour --\n");                    test_colour();
    printf("\n-- the ribbon stage accepts it --\n"); test_ribbon_integration();
    printf("\n-- defensive API --\n");             test_defensive();
    printf("\n-- determinism --\n");               test_determinism();
    printf("\n-- the renderer calibration seam --\n"); test_render_mapping();
    printf("\n-- the mapped maximum clears the knee --\n");
                                                   test_mapped_drive_clears_the_knee();
    printf("\n-- rough cost --\n");                report_cost();

    if (failures) {
        printf("\nFAILED: %d check(s)\n", failures);
        return 1;
    }
    printf("\nOK\n");
    return 0;
}
