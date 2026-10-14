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
#include "../source/ui/render/wave_gel.h"
#include "../source/ui/render/wave_deform.h"

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

// --- the look: per-layer height and colour --------------------------------
//
// These drive the REAL stage B (wm_update) with synthetic features and read
// what wrm_map hands the renderer, because the properties that matter are of
// the two together: which band moves which ribbon, and how fast anything can
// change between frames.

#define LOOK_DT (1.0f / 60.0f)

static void look_feat(wa_features *f, float bass, float mids, float highs,
                      float rms, float onset)
{
    memset(f, 0, sizeof *f);
    f->band[WA_SUB]    = f->band[WA_BASS] = bass;
    f->band[WA_LOWMID] = f->band[WA_MID]  = mids;
    f->band[WA_HIGH]   = f->band[WA_AIR]  = highs;
    memcpy(f->band_fast, f->band, sizeof f->band);
    f->rms            = rms;
    f->centroid       = 0.5f;
    f->onset          = onset > 0.0f ? 1.0f : 0.0f;
    f->onset_strength = onset;
    f->silence        = 0.0f;
}

// Hold one feature set for `secs` and return the mapped look it settles on.
static void look_settle(wm_state *m, const wa_features *f, float secs,
                        wrm_out *o)
{
    int i, n = (int)(secs / LOOK_DT);
    for (i = 0; i < n; i++) wm_update(m, f, LOOK_DT);
    wrm_map(&m->p, o);
}

static int look_in_range(const wrm_out *o)
{
    int i;
    for (i = 0; i < 3; i++)
        if (!(o->amp[i] >= WRM_AMP_MIN && o->amp[i] <= WRM_AMP_MAX)) return 0;
    return o->lum >= WRM_LUM_MIN && o->lum <= WRM_LUM_MAX;
}

static void test_look_mapping(void)
{
    wrm_out     o, q;
    wm_state    m;
    wa_features f;
    int         i;

    // Rest is EXACTLY 1.0, from both the gate-off path and a fresh stage B,
    // or the XMB with no music playing would look different from before.
    wrm_map(NULL, &o);
    CHECK(o.amp[0] == 1.0f && o.amp[1] == 1.0f && o.amp[2] == 1.0f &&
          o.lum == 1.0f,
          "idle look is %.4f/%.4f/%.4f lum %.4f, not exactly 1",
          o.amp[0], o.amp[1], o.amp[2], o.lum);
    wm_init(&m);
    wrm_map(&m.p, &q);
    CHECK(memcmp(o.amp, q.amp, sizeof o.amp) == 0 && o.lum == q.lum,
          "a fresh wm_state does not map to the idle look");

    // Each band lifts its own ribbon above rest and leads the other two.
    {
        static const char *NAME[3] = { "bass", "mids", "highs" };
        for (i = 0; i < 3; i++) {
            wm_init(&m);
            look_feat(&f, i == 0, i == 1, i == 2, 0.5f, 0.0f);
            look_settle(&m, &f, 4.0f, &o);
            printf("  %-5s -> amp %.3f / %.3f / %.3f   lum %.3f\n",
                   NAME[i], o.amp[0], o.amp[1], o.amp[2], o.lum);
            CHECK(o.amp[i] > 1.05f, "%s leaves its ribbon at %.3f", NAME[i],
                  o.amp[i]);
            CHECK(o.amp[i] > o.amp[(i + 1) % 3] && o.amp[i] > o.amp[(i + 2) % 3],
                  "%s does not lead its own ribbon", NAME[i]);
            CHECK(look_in_range(&o), "%s mapped out of range", NAME[i]);
        }
    }

    // Monotone in each band's level, and the whole mix at full lands on the
    // measured ceiling.
    for (i = 0; i < 3; i++) {
        float prev = 0.0f;
        int   k;
        for (k = 0; k <= 10; k++) {
            float lv = 0.1f * (float)k;
            wm_init(&m);
            look_feat(&f, i == 0 ? lv : 0.0f, i == 1 ? lv : 0.0f,
                      i == 2 ? lv : 0.0f, 0.5f, 0.0f);
            look_settle(&m, &f, 4.0f, &o);
            CHECK(o.amp[i] >= prev - 1e-6f,
                  "amp[%d] fell from %.4f to %.4f as its band rose to %.1f",
                  i, prev, o.amp[i], lv);
            prev = o.amp[i];
        }
    }
    wm_init(&m);
    look_feat(&f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f);
    look_settle(&m, &f, 4.0f, &o);
    printf("  full  -> amp %.3f / %.3f / %.3f   lum %.3f\n",
           o.amp[0], o.amp[1], o.amp[2], o.lum);
    CHECK(look_in_range(&o), "full-scale input mapped out of range");
    CHECK(fabsf(o.amp[0] - WRM_AMP_MAX) < 1e-3f &&
          fabsf(o.amp[2] - WRM_AMP_MAX) < 1e-3f,
          "full-scale input does not reach WRM_AMP_MAX");

    // Louder is brighter, bounded.
    {
        float prev = 0.0f;
        for (i = 0; i <= 10; i++) {
            wm_init(&m);
            look_feat(&f, 0.3f, 0.3f, 0.3f, 0.1f * (float)i, 0.0f);
            look_settle(&m, &f, 4.0f, &o);
            CHECK(o.lum >= prev - 1e-6f, "lum is not monotone in rms at %d", i);
            CHECK(look_in_range(&o), "lum out of range at rms %d", i);
            prev = o.lum;
        }
    }

    // An onset rises and then decays back to where it started.
    {
        float base, peak = 0.0f;
        wm_init(&m);
        look_feat(&f, 0.3f, 0.3f, 0.3f, 0.5f, 0.0f);
        look_settle(&m, &f, 4.0f, &o);
        base = o.lum;
        look_feat(&f, 0.3f, 0.3f, 0.3f, 0.5f, 1.0f);
        for (i = 0; i < 6; i++) {                     // a 100 ms onset
            look_settle(&m, &f, LOOK_DT, &o);
            if (o.lum > peak) peak = o.lum;
        }
        look_feat(&f, 0.3f, 0.3f, 0.3f, 0.5f, 0.0f);
        look_settle(&m, &f, 2.0f, &o);
        printf("  onset -> lum %.4f, peak %.4f, back to %.4f\n", base, peak, o.lum);
        CHECK(peak > base + 0.03f, "a 100 ms onset only lifts lum %.4f", peak - base);
        CHECK(fabsf(o.lum - base) < 1e-4f, "lum did not return after an onset");
    }

    // Beats every 250 ms for 30 s: bounded, and no creep between the early
    // and late stretches.
    {
        float hi_early = 0.0f, hi_late = 0.0f;
        int   fr;
        wm_init(&m);
        for (fr = 0; fr < 60 * 30; fr++) {
            look_feat(&f, 1.0f, 0.6f, 0.6f, 1.0f, (fr % 15) < 6 ? 1.0f : 0.0f);
            look_settle(&m, &f, LOOK_DT, &o);
            CHECK(look_in_range(&o), "repeated beats pushed the look out of range");
            if (failures) return;
            if (fr >= 60 * 5  && fr < 60 * 10 && o.lum > hi_early) hi_early = o.lum;
            if (fr >= 60 * 25 &&                 o.lum > hi_late)  hi_late  = o.lum;
        }
        CHECK(fabsf(hi_late - hi_early) < 1e-3f,
              "lum peaks creep under repeated beats: %.4f then %.4f",
              hi_early, hi_late);
    }

    // THE STROBE BOUND.  Hostile input -- every feature jumping to a fresh
    // random value every frame -- must still only move the look by a small
    // step per frame, because nothing downstream smooths it again.
    {
        uint32_t seed = 12345u;
        float    dl = 0.0f, da = 0.0f;
        wrm_out  prev;
        int      fr, k;
        wm_init(&m);
        wrm_map(&m.p, &prev);
        for (fr = 0; fr < 60 * 60; fr++) {
            float r[5];
            for (k = 0; k < 5; k++) {
                seed = seed * 1664525u + 1013904223u;
                r[k] = (float)(seed >> 8) * (1.0f / 16777216.0f);
            }
            look_feat(&f, r[0], r[1], r[2], r[3], r[4] > 0.5f ? r[4] : 0.0f);
            wm_update(&m, &f, LOOK_DT);
            wrm_map(&m.p, &o);
            if (fabsf(o.lum - prev.lum) > dl) dl = fabsf(o.lum - prev.lum);
            for (k = 0; k < 3; k++)
                if (fabsf(o.amp[k] - prev.amp[k]) > da)
                    da = fabsf(o.amp[k] - prev.amp[k]);
            prev = o;
        }
        printf("  hostile input: largest step per frame  lum %.4f  amp %.4f\n",
               dl, da);
        CHECK(dl <= 0.018f, "lum can jump %.4f in one frame", dl);
        CHECK(da <= 0.009f, "amp can jump %.4f in one frame", da);
    }

    // Long silence after music settles back onto exactly the rest look.
    wm_init(&m);
    look_feat(&f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f);
    look_settle(&m, &f, 5.0f, &o);
    wm_update(&m, NULL, LOOK_DT);
    for (i = 0; i < 60 * 30; i++) wm_update(&m, NULL, LOOK_DT);
    wrm_map(&m.p, &o);
    printf("  after 30 s of silence -> amp %.5f / %.5f / %.5f  lum %.5f\n",
           o.amp[0], o.amp[1], o.amp[2], o.lum);
    CHECK(fabsf(o.amp[0] - 1.0f) < 1e-4f && fabsf(o.amp[1] - 1.0f) < 1e-4f &&
          fabsf(o.amp[2] - 1.0f) < 1e-4f && fabsf(o.lum - 1.0f) < 1e-4f,
          "silence does not return to the rest look");

    // NaN and infinities land inside the ranges, never outside them.
    {
        static const float BAD[3] = { NAN, INFINITY, -INFINITY };
        for (i = 0; i < 3; i++) {
            wm_params p;
            int       k;
            wm_init(&m);
            p = m.p;
            for (k = 0; k < WM_LAYERS; k++) p.amp[k] = BAD[i];
            p.bright = BAD[i];
            p.glow   = BAD[i];
            wrm_map(&p, &o);
            CHECK(look_in_range(&o), "bad input %d escaped the ranges", i);
        }
    }

    // Deterministic: the same input twice gives bit-identical output.
    {
        wm_state a, b;
        wrm_out  oa, ob;
        wm_init(&a); wm_init(&b);
        for (i = 0; i < 600; i++) {
            look_feat(&f, (i % 7) * 0.14f, (i % 5) * 0.2f, (i % 3) * 0.4f,
                      0.5f, (i % 30) == 0 ? 0.8f : 0.0f);
            wm_update(&a, &f, LOOK_DT);
            wm_update(&b, &f, LOOK_DT);
        }
        wrm_map(&a.p, &oa);
        wrm_map(&b.p, &ob);
        CHECK(memcmp(&oa, &ob, sizeof oa) == 0, "the look is not deterministic");
    }
}

// The measurement WRM_AMP_MAX, WRM_THICK_MAX and WRM_ACC_H were chosen from,
// re-run against the constants: the loudest drive, the tallest multiplier,
// the fullest swell and an accent sitting on the crest along the WHOLE band
// (a pessimistic stand-in for a bump that happens to land on a peak), all at
// once, through the real solver and the real JellyWave loft, must keep every
// layer inside test_wave_gel.c's framing box with 0.05 to spare under the
// midline.  WANT_TOP is that test's table.
static void test_look_framing(void)
{
    static const float WANT_TOP[JW_LAYERS] = { -0.16f, -0.09f, -0.12f };
    static wf_field f;
    static jw_vert  v[JW_VERTS];
    static float    dsp[WF_SAMPLES];
    float hi[JW_LAYERS];
    int   l, i, fr, bad = 0;

    for (l = 0; l < JW_LAYERS; l++) hi[l] = -9.0f;
    wf_init(&f, 0);
    for (fr = 0; fr < 60 * 60; fr++) {
        wf_step(&f, 1.25f * WRM_TS_MAX, WM_MAX_PERTURB, WRM_DRIVE_MAX);
        if (fr % 3) continue;
        for (l = 0; l < JW_LAYERS; l++) {
            jw_layer L = JW_LAYER[l];
            int      k;
            L.disp_gain *= WRM_AMP_MAX;
            L.bright    *= WRM_LUM_MAX;
            L.scale     *= WRM_THICK_MAX;
            for (k = 0; k < WF_SAMPLES; k++)
                dsp[k] = f.sy[l][k] + WRM_ACC_H * WRM_ACC_LAYER[l];
            if (jw_build_layer(&L, dsp, WF_SAMPLES, 16.0f / 9.0f,
                               v, JW_VERTS) != JW_VERTS) { bad++; continue; }
            for (i = 0; i < JW_VERTS; i++) {
                if (!v[i].ok || v[i].x != v[i].x || v[i].y != v[i].y) {
                    bad++;
                    continue;
                }
                if (v[i].y > hi[l]) hi[l] = v[i].y;
            }
        }
    }
    for (l = 0; l < JW_LAYERS; l++) {
        printf("  layer %d at drive %.2f x amp %.2f, swell %.2f, accent on the"
               " crest: top edge %+.3f (box %+.3f, midline 0)\n",
               l, WRM_DRIVE_MAX, WRM_AMP_MAX, WRM_THICK_MAX, hi[l],
               WANT_TOP[l] + 0.16f);
        CHECK(hi[l] < WANT_TOP[l] + 0.16f && hi[l] < -0.05f,
              "layer %d's top edge reaches %+.3f at the loudest look -- "
              "WRM_AMP_MAX is too high", l, hi[l]);
    }
    CHECK(bad == 0, "%d vertices were non-finite or behind the camera", bad);
}

// The same measurement for the distinct-band mapping (wave_render_map.h):
// drive held at WRM_DB_DRIVE_MAX, tempo at rest, a fraction of the ripple,
// each layer at ITS OWN height cap, the fullest swell and the (reduced)
// accent on the crest along the whole band.  Same box, same 0.05 margin.
static void test_distinct_framing(void)
{
    static const float WANT_TOP[JW_LAYERS] = { -0.16f, -0.09f, -0.12f };
    static wf_field f;
    static jw_vert  v[JW_VERTS];
    static float    dsp[WF_SAMPLES];
    const float pert = WM_IDLE_PERTURB +
                       (WM_MAX_PERTURB - WM_IDLE_PERTURB) * WRM_DB_PERTURB_KEEP;
    float hi[JW_LAYERS];
    int   l, i, fr, bad = 0;

    for (l = 0; l < JW_LAYERS; l++) hi[l] = -9.0f;
    wf_init(&f, 0);
    for (fr = 0; fr < 60 * 60; fr++) {
        wf_step(&f, 1.25f * WRM_TS_IDLE, pert, WRM_DB_DRIVE_MAX);
        if (fr % 3) continue;
        for (l = 0; l < JW_LAYERS; l++) {
            jw_layer L = JW_LAYER[l];
            int      k;
            L.disp_gain *= WRM_DB_AMP_MAX[l];
            L.bright    *= WRM_LUM_MAX * WRM_DB_LUM_MAX[l];
            L.scale     *= WRM_THICK_MAX;
            if (fr & 1) L.y_off *= 1.50f;                        // full stereo spread
            for (k = 0; k < WF_SAMPLES; k++)
                dsp[k] = f.sy[l][k] + WRM_ACC_H * WRM_DB_ACC_KEEP * WRM_ACC_LAYER[l]
                       + WDF_POS_MAX * WDF_LAYER[l];   // + JellyWave 2.0's whole allowance
            if (jw_build_layer(&L, dsp, WF_SAMPLES, 16.0f / 9.0f,
                               v, JW_VERTS) != JW_VERTS) { bad++; continue; }
            for (i = 0; i < JW_VERTS; i++) {
                if (!v[i].ok || v[i].x != v[i].x || v[i].y != v[i].y) {
                    bad++;
                    continue;
                }
                if (v[i].y > hi[l]) hi[l] = v[i].y;
            }
        }
    }
    for (l = 0; l < JW_LAYERS; l++) {
        printf("  distinct: layer %d at drive %.2f x amp %.2f: top edge %+.3f"
               " (box %+.3f)\n", l, WRM_DB_DRIVE_MAX, WRM_DB_AMP_MAX[l], hi[l],
               WANT_TOP[l] + 0.16f);
        CHECK(hi[l] < WANT_TOP[l] + 0.16f && hi[l] < -0.05f,
              "distinct: layer %d's top edge reaches %+.3f -- WRM_DB_AMP_MAX is"
              " too high", l, hi[l]);
    }
    CHECK(bad == 0, "%d vertices were non-finite or behind the camera", bad);
}

// Distinct bands: rest is exactly rest, and a band moves only its own layer.
static void test_distinct_bands(void)
{
    wrm_db_state st;
    wrm_out o;
    float lum3[3], src[3] = { 0, 0, 0 };
    int i, n;

    memset(&st, 0, sizeof st);
    wrm_map(NULL, &o);
    wrm_distinct(&st, src, 0.0f, 0.0f, 1.0f, 1.0f / 60.0f, &o, lum3);
    CHECK(o.amp[0] == 1.0f && o.amp[1] == 1.0f && o.amp[2] == 1.0f &&
          lum3[0] == 1.0f && lum3[1] == 1.0f && lum3[2] == 1.0f &&
          o.dt_scale == WRM_TS_IDLE && o.drive == WRM_DRIVE_IDLE,
          "distinct bands move the wave at rest");

    // Bass alone, held: layer 0 climbs toward its cap, 1 and 2 sink below rest.
    memset(&st, 0, sizeof st);
    src[0] = 1.0f; src[1] = 0.0f; src[2] = 0.0f;
    for (n = 0; n < 120; n++) {
        wrm_map(NULL, &o);
        wrm_distinct(&st, src, 0.0f, 1.0f, 1.0f, 1.0f / 60.0f, &o, lum3);
    }
    printf("  distinct: bass only -> amp %.2f/%.2f/%.2f lum %.2f/%.2f/%.2f\n",
           o.amp[0], o.amp[1], o.amp[2], lum3[0], lum3[1], lum3[2]);
    CHECK(o.amp[0] > 1.28f && o.amp[1] < 1.0f && o.amp[2] < 1.0f,
          "bass does not stand out on its own layer");

    // A LOUD, fast kick (the v4 complaint): the bass band sits high all the
    // time (self-referenced loud track) and the fast level pulses at 170 BPM.
    // The bass layer must swing hard with every beat, not sit on a swell.
    {
        float lo = 9.0f, hi = 0.0f;
        memset(&st, 0, sizeof st);
        st.tempo_hz = 2.83f; st.tempo_conf = 1.0f;
        src[0] = 0.72f; src[1] = 0.5f; src[2] = 0.3f;
        for (n = 0; n < 60 * 6; n++) {
            const int ph = n % 21;                 // 21 frames = 170 BPM at 60 fps
            st.in_fast[0] = ph < 4 ? 0.95f : 0.62f;
            st.in_fast[1] = 0.5f; st.in_fast[2] = 0.3f;
            wrm_map(NULL, &o);
            wrm_distinct(&st, src, 0.0f, 1.0f, 1.0f, 1.0f / 60.0f, &o, lum3);
            if (n > 60 * 3) {
                if (o.amp[0] < lo) lo = o.amp[0];
                if (o.amp[0] > hi) hi = o.amp[0];
            }
            CHECK(o.amp[0] <= WRM_DB_AMP_MAX[0] + 1e-4f, "punch passes the cap");
        }
        printf("  distinct: loud 170 BPM kick -> bass layer swings %.2f..%.2f, ts %.2f\n",
               lo, hi, o.dt_scale);
        CHECK(hi - lo > 0.35f, "a loud fast kick barely moves the bass layer (%.2f)", hi - lo);
        CHECK(o.dt_scale > 1.2f, "a fast locked tempo does not speed the wave");
    }

    // A DOUBLE KICK (v7: "the second one doesn't look distinct enough"):
    // two hits 150 ms apart on a loud master, once a second.  The layer must
    // fall visibly between them and the second must rise nearly as far as
    // the first.
    {
        float p1 = 0, p2 = 0, tr = 9, b0 = 9;
        memset(&st, 0, sizeof st);
        st.energy = 1.0f; st.tempo_hz = 2.4f; st.tempo_conf = 0.8f;
        src[0] = 0.70f; src[1] = 0.5f; src[2] = 0.3f;
        for (n = 0; n < 60 * 8; n++) {
            const int ph = n % 60;
            const int hit = (ph < 4) || (ph >= 9 && ph < 13);
            st.in_fast[0] = hit ? 0.95f : 0.62f;
            st.in_fast[1] = 0.5f; st.in_fast[2] = 0.3f;
            wrm_map(NULL, &o);
            wrm_distinct(&st, src, 0.0f, 1.0f, 1.0f, 1.0f / 60.0f, &o, lum3);
            if (n >= 60 * 7) {                       /* the last cycle */
                if (ph >= 55 || ph == 0) { if (o.amp[0] < b0) b0 = o.amp[0]; }
                if (ph >= 0 && ph <= 8 && o.amp[0] > p1) p1 = o.amp[0];
                if (ph >= 6 && ph <= 11 && o.amp[0] < tr) tr = o.amp[0];
                if (ph >= 9 && ph <= 20 && o.amp[0] > p2) p2 = o.amp[0];
            }
        }
        printf("  distinct: double kick -> base %.2f, first %.2f, dip to %.2f, second %.2f "
               "(dip %.0f%% of the rise, second %.0f%% of the first)\n",
               b0, p1, tr, p2, 100.0f * (p1 - tr) / (p1 - b0 + 1e-6f), 100.0f * (p2 - tr) / (p1 - b0 + 1e-6f));
        CHECK((p1 - tr) > 0.35f * (p1 - b0), "the layer does not fall between the two kicks");
        CHECK((p2 - tr) > 0.30f * (p1 - b0), "the second kick does not rise on its own");
    }

    // Physical, not rigid (v5): the same kick never moves a layer by more
    // than a small step in one frame -- the spring carries it there.
    {
        float prev = 1.0f, maxstep = 0.0f;
        memset(&st, 0, sizeof st);
        src[0] = 0.72f; src[1] = 0.5f; src[2] = 0.3f;
        for (n = 0; n < 60 * 4; n++) {
            const int ph = n % 21;
            st.in_fast[0] = ph < 4 ? 0.95f : 0.62f;
            st.in_fast[1] = 0.5f; st.in_fast[2] = 0.3f;
            wrm_map(NULL, &o);
            wrm_distinct(&st, src, 0.0f, 1.0f, 1.0f, 1.0f / 60.0f, &o, lum3);
            if (n > 60) {
                const float d = prev - o.amp[0];      // falling steps only (v6)
                if (d > maxstep) maxstep = d;
            }
            prev = o.amp[0];
        }
        printf("  distinct: largest one-frame FALL of the bass layer %.3f\n", maxstep);
        CHECK(maxstep < 0.10f, "the bass layer still drops %.3f in one frame", maxstep);
    }

    // Highs alone: attack within a few frames, release within ~0.3 s.
    memset(&st, 0, sizeof st);
    src[0] = 0.0f; src[2] = 1.0f;
    for (n = 0; n < 6; n++) {
        wrm_map(NULL, &o);
        wrm_distinct(&st, src, 0.0f, 1.0f, 1.0f, 1.0f / 60.0f, &o, lum3);
    }
    CHECK(o.amp[2] > 1.6f && o.amp[0] < 1.0f, "highs are not quick on their own layer");
    src[2] = 0.0f;
    for (n = 0; n < 20; n++) {
        wrm_map(NULL, &o);
        wrm_distinct(&st, src, 0.0f, 1.0f, 1.0f, 1.0f / 60.0f, &o, lum3);
    }
    CHECK(o.amp[2] < 1.0f, "highs do not let go quickly");
    for (i = 0; i < 3; i++)
        CHECK(o.amp[i] >= WRM_DB_AMP_QUIET - 1e-4f && o.amp[i] <= WRM_DB_AMP_MAX[i] + 1e-4f,
              "layer %d outside its range", i);

    // Sub-bass hit: settle, then a step -- a kick fires, a spike rises and
    // splits into two crests running outward, the layers barely move, nothing
    // passes its cap, the accent total stays inside the bound throughout.
    {
        float before[3], asum;
        int j, fired = 0;
        memset(&st, 0, sizeof st);
        src[0] = 0.3f; src[1] = 0.3f; src[2] = 0.3f;
        for (n = 0; n < 240; n++) {
            wrm_map(NULL, &o);
            wrm_distinct(&st, src, 0.2f, 1.0f, 1.0f, 1.0f / 60.0f, &o, lum3);
        }
        for (i = 0; i < 3; i++) before[i] = o.amp[i];
        for (n = 0; n < 6; n++) {
            wrm_map(NULL, &o);
            wrm_distinct(&st, src, 0.9f, 1.0f, 1.0f, 1.0f / 60.0f, &o, lum3);
            fired |= st.kick > 0.0f;
        }
        {
            float sep0, sep1, amax = 0.0f;
            const int s0 = WM_PULSES;
            sep0 = o.acc.x[s0 + 1] - o.acc.x[s0];
            for (n = 0; n < 30; n++) {
                wrm_map(NULL, &o);
                wrm_distinct(&st, src, 0.9f, 1.0f, 1.0f, 1.0f / 60.0f, &o, lum3);
                asum = 0.0f;
                for (j = 0; j < WRM_ACC_SLOTS; j++) asum += o.acc.a[j];
                CHECK(asum <= WRM_ACC_TOTAL_MAX + 1e-4f, "accent total %.3f over the bound", asum);
                for (i = 0; i < 3; i++) {
                    CHECK(o.amp[i] <= WRM_DB_AMP_MAX[i] + 1e-4f, "the ripple passes layer %d's cap", i);
                    if (o.amp[i] - before[i] > amax) amax = o.amp[i] - before[i];
                }
            }
            sep1 = o.acc.x[s0 + 1] - o.acc.x[s0];
            printf("  distinct: sub hit -> crests %.2f apart -> %.2f after 0.5 s, "
                   "amp lift %.3f, crest %.3f\n", sep0, sep1, amax, o.acc.a[s0]);
            CHECK(fired, "a sub-bass step does not fire a kick");
            CHECK(sep0 < 0.35f && sep1 > sep0 + 0.4f, "the spike does not spread into a ripple");
            CHECK(o.acc.a[s0] > 0.0f, "the ripple died too soon");
            CHECK(amax < 0.12f, "the hit still jolts the whole wave (%.3f)", amax);
        }
    }
}

// --- the response gain (jellyfin_wavereact.txt level) ---------------------
//
// Steeper, never higher: at every gain the outputs stay inside the same caps
// the framing test measures, rest stays exactly rest, gain 1 IS wrm_map, and
// a mid-level passage reads more strongly as the gain rises.
static void test_gain(void)
{
    static const float G[] = { 1.0f, 1.8f, 2.6f };
    wrm_out  a, b;
    wm_state m;
    int      gi, i, k;
    uint32_t r = 99u;

    for (i = 0; i < 2000; i++) {
        wm_params p;
        memset(&p, 0, sizeof p);
        r = r * 1664525u + 1013904223u; p.drive = WM_IDLE_DRIVE + (WM_MAX_DRIVE - WM_IDLE_DRIVE) * (float)(r >> 8) / 16777216.0f;
        r = r * 1664525u + 1013904223u; p.timescale = WM_MIN_TIME + (WM_MAX_TIME - WM_MIN_TIME) * (float)(r >> 8) / 16777216.0f;
        r = r * 1664525u + 1013904223u; p.perturb = WM_MAX_PERTURB * (float)(r >> 8) / 16777216.0f;
        r = r * 1664525u + 1013904223u; p.bright = (float)(r >> 8) / 16777216.0f;
        r = r * 1664525u + 1013904223u; p.glow = (float)(r >> 8) / 16777216.0f;
        for (k = 0; k < WM_LAYERS; k++) {
            r = r * 1664525u + 1013904223u; p.amp[k] = (float)(r >> 8) / 16777216.0f;
        }
        for (k = 0; k < WM_PULSES; k++) {
            p.pulse[k].x = 0.3f * (float)k; p.pulse[k].width = 0.1f;
            p.pulse[k].amp = 0.25f * (float)k; p.pulse[k].live = k & 1;
        }
        wrm_map(&p, &a);
        wrm_map_gain(&p, 1.0f, &b);
        CHECK(memcmp(&a, &b, sizeof a) == 0, "gain 1 is not wrm_map");
        for (gi = 0; gi < 3; gi++) {
            wrm_map_gain(&p, G[gi] * 1.5f, &b);   /* past the levels on purpose */
            CHECK(b.drive >= WRM_DRIVE_IDLE && b.drive <= WRM_DRIVE_MAX &&
                  b.dt_scale >= WRM_TS_MIN && b.dt_scale <= WRM_TS_MAX &&
                  b.perturb >= 0.0f && b.perturb <= WM_MAX_PERTURB &&
                  b.amp[0] <= WRM_AMP_MAX && b.amp[2] >= WRM_AMP_MIN &&
                  b.lum <= WRM_LUM_MAX && b.lum >= WRM_LUM_MIN &&
                  b.thick <= WRM_THICK_MAX && b.thick >= WRM_THICK_MIN &&
                  b.acc.a[3] <= 1.0f,
                  "gain %.1f escaped a cap", G[gi] * 1.5f);
        }
        if (failures) return;
    }

    wm_init(&m);
    wrm_map(NULL, &a);
    for (gi = 0; gi < 3; gi++) {
        wrm_map_gain(&m.p, G[gi], &b);
        CHECK(a.drive == b.drive && a.dt_scale == b.dt_scale &&
              a.perturb == b.perturb && a.lum == b.lum && a.thick == b.thick &&
              memcmp(a.amp, b.amp, sizeof a.amp) == 0 &&
              b.acc.a[0] == 0.0f && b.acc.a[1] == 0.0f &&
              b.acc.a[2] == 0.0f && b.acc.a[3] == 0.0f,
              "rest is not exact at gain %.1f", G[gi]);
    }

    {
        wm_params p = m.p;
        float prev_amp = 0.0f, prev_lum = 0.0f, prev_drv = 0.0f;
        p.amp[0] = 0.55f; p.bright = 0.75f; p.drive = 0.60f;
        for (gi = 0; gi < 3; gi++) {
            wrm_map_gain(&p, G[gi], &b);
            printf("  mid-level passage at x%.1f: amp0 %.3f  lum %.3f  drive %.3f\n",
                   G[gi], b.amp[0], b.lum, b.drive);
            CHECK(b.amp[0] > prev_amp && b.lum > prev_lum && b.drive > prev_drv,
                  "gain %.1f does not strengthen a mid-level passage", G[gi]);
            prev_amp = b.amp[0]; prev_lum = b.lum; prev_drv = b.drive;
        }
    }
    {
        wm_params p = m.p;
        p.drive = NAN;
        wrm_map_gain(&p, NAN, &b);
        CHECK(b.drive == b.drive && b.drive >= WRM_DRIVE_IDLE, "a NaN gain leaked");
    }
}

// --- JellyWave 2.0: the body swell and the travelling accent -------------

static float accent_peak(const wrm_out *o, int layer, int *at)
{
    static float z[WF_SAMPLES], out[WF_SAMPLES];
    float best = -1.0f;
    int   k;
    memset(z, 0, sizeof z);
    wrm_accent(&o->acc, layer, z, out, WF_SAMPLES);
    *at = 0;
    for (k = 0; k < WF_SAMPLES; k++)
        if (out[k] > best) { best = out[k]; *at = k; }
    return best;
}

static void test_shape_mapping(void)
{
    static float in[WF_SAMPLES], out[WF_SAMPLES];
    wrm_out  o;
    wm_state m;
    int      i, k, at;

    // REST IS BIT-IDENTICAL: no swell, and an accent that leaves the solver
    // curve exactly as it was -- the idle XMB must not change by one ulp.
    for (k = 0; k < WF_SAMPLES; k++) in[k] = 0.37f * sinf(0.11f * (float)k) - 0.05f;
    wrm_map(NULL, &o);
    wrm_accent(&o.acc, 0, in, out, WF_SAMPLES);
    CHECK(o.thick == 1.0f, "idle swell is %.6f, not exactly 1", o.thick);
    CHECK(memcmp(in, out, sizeof in) == 0, "the idle accent changes the curve");
    wm_init(&m);
    wrm_map(&m.p, &o);
    wrm_accent(&o.acc, 2, in, out, WF_SAMPLES);
    CHECK(o.thick == 1.0f && memcmp(in, out, sizeof in) == 0,
          "a fresh stage B is not bit-identical to rest");

    // Swell: monotone in stage B's Body amplitude, inside its range, and at
    // its measured ceiling when Body is full.
    {
        float prev = 0.0f;
        for (i = 0; i <= 20; i++) {
            wm_params p = m.p;
            p.amp[1] = 0.05f * (float)i;
            wrm_map(&p, &o);
            CHECK(o.thick >= prev && o.thick >= WRM_THICK_MIN &&
                  o.thick <= WRM_THICK_MAX,
                  "swell %.4f not monotone/bounded at Body %.2f", o.thick, p.amp[1]);
            prev = o.thick;
        }
        CHECK(fabsf(prev - WRM_THICK_MAX) < 1e-5f, "full Body does not reach the swell cap");
    }

    // One real pulse from stage B: the accent follows it along the band,
    // never exceeds its height, and the ribbon is bit-identical again once
    // the pulse has retired.
    {
        int   last_at = -1, moved = 0, backwards = 0;
        float peak_max = 0.0f, worst_step = 0.0f, prev[WF_SAMPLES];
        int   fr, have_prev = 0;

        wm_init(&m);
        // The fastest, tightest pulse stage B can make: 200 BPM, hard onset.
        wm_spawn_pulse(&m, 1.0f, WA_BEAT_MIN, 1.0f);
        for (fr = 0; fr < 240; fr++) {
            float cur[WF_SAMPLES];
            wm_step_pulses(&m, LOOK_DT);
            wrm_map(&m.p, &o);
            memset(in, 0, sizeof in);
            wrm_accent(&o.acc, 0, in, cur, WF_SAMPLES);
            {
                float pk = accent_peak(&o, 0, &at);
                if (pk > peak_max) peak_max = pk;
                if (pk > 0.2f * WRM_ACC_H) {
                    if (last_at >= 0 && at > last_at) moved++;
                    if (last_at >= 0 && at < last_at) backwards++;
                    last_at = at;
                }
            }
            if (have_prev)
                for (k = 0; k < WF_SAMPLES; k++) {
                    float d = fabsf(cur[k] - prev[k]);
                    if (d > worst_step) worst_step = d;
                }
            memcpy(prev, cur, sizeof prev);
            have_prev = 1;
        }
        printf("  accent: peak %.4f (cap %.2f), advanced on %d frames, back on %d;"
               " largest change in one frame %.4f (%.0f%% of the cap)\n",
               peak_max, WRM_ACC_H, moved, backwards, worst_step,
               100.0f * worst_step / WRM_ACC_H);
        CHECK(peak_max > 0.5f * WRM_ACC_H && peak_max <= WRM_ACC_H + 1e-6f,
              "a full pulse's accent peaks at %.4f", peak_max);
        CHECK(moved > 10 && backwards == 0, "the accent does not travel one way");
        CHECK(worst_step <= 0.45f * WRM_ACC_H,
              "the fastest pulse moves the curve %.4f in one frame", worst_step);

        for (k = 0; k < WF_SAMPLES; k++) in[k] = 0.2f * sinf(0.05f * (float)k);
        wrm_accent(&o.acc, 0, in, out, WF_SAMPLES);
        CHECK(memcmp(in, out, sizeof in) == 0,
              "after the pulse retires the curve is not bit-identical");
    }

    // Four pulses stacked on one spot: clamped, never more than the cap.
    {
        wm_params p = m.p;
        for (i = 0; i < WM_PULSES; i++) {
            p.pulse[i].x = 0.5f; p.pulse[i].width = 0.1f;
            p.pulse[i].amp = 1.0f; p.pulse[i].live = 1;
        }
        wrm_map(&p, &o);
        CHECK(fabsf(accent_peak(&o, 0, &at) - WRM_ACC_H) < 1e-6f,
              "stacked pulses exceed or miss the cap");
        CHECK(accent_peak(&o, 2, &at) <= WRM_ACC_H * WRM_ACC_LAYER[2] + 1e-6f,
              "the far layer's accent is not scaled down");
    }

    // Hostile pulse fields stay finite and capped.
    {
        static const float BAD[3] = { NAN, INFINITY, -INFINITY };
        for (i = 0; i < 3; i++) {
            wm_params p = m.p;
            float     pk;
            int       j;
            for (j = 0; j < WM_PULSES; j++) {
                p.pulse[j].x = BAD[i]; p.pulse[j].width = BAD[i];
                p.pulse[j].amp = BAD[i]; p.pulse[j].live = 1;
            }
            p.amp[1] = BAD[i];
            wrm_map(&p, &o);
            pk = accent_peak(&o, 0, &at);
            CHECK(pk == pk && pk >= 0.0f && pk <= WRM_ACC_H + 1e-6f &&
                  o.thick >= WRM_THICK_MIN && o.thick <= WRM_THICK_MAX,
                  "bad pulse input %d escaped: peak %f swell %f", i, pk, o.thick);
        }
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
    printf("\n-- the look: per-layer height and colour --\n");
                                                   test_look_mapping();
    printf("\n-- response gain (wavereact level) --\n");
                                                   test_gain();
    printf("\n-- JellyWave 2.0: swell and accent --\n");
                                                   test_shape_mapping();
    printf("\n-- the loudest look stays framed --\n");
                                                   test_look_framing();
    test_distinct_framing();
    test_distinct_bands();
    printf("\n-- rough cost --\n");                report_cost();

    if (failures) {
        printf("\nFAILED: %d check(s)\n", failures);
        return 1;
    }
    printf("\nOK\n");
    return 0;
}
