// Host test for source/ui/render/wave_motes.h -- the JellyWave motes.
//
// Invariants, not golden values (.clinerules rule 9).  The ones that are
// load-bearing rather than hygienic:
//
//   THE SPREAD MATCHES THE FORMULA.  wave_motes.h picks its forcing from the
//   closed-form steady spread of a damped spring, sd = SIGMA / sqrt(2 GAMMA K).
//   If the integrator or the noise scaling is wrong the cloud comes out the
//   wrong size and nothing else notices, so the spread is measured.
//
//   NO MOTE ABOVE THE CARD GRID.  Enforced in the shading, asserted here for
//   every visible sprite, at 16:9 and 4:3, with the cloud lifted as high as
//   the audio can lift it.
//
//   THE FIELD DOES NOT STROBE.  The bound is on the TOTAL light from one frame
//   to the next, idle and under a beat-heavy audio script driven through the
//   real stage B.  Single flakes twinkling is the design; the whole field
//   brightening together is the failure.
//
//   A BRIGHT GLINT LASTS.  At the fastest spin the audio can produce, every
//   glint that reaches the core of the lobe is held for several frames.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include "wave_motes.h"
#include "wave_gel.h"

static int fails = 0;
static void ck(int ok, const char *what)
{
    printf("   %s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) fails++;
}

#define DT (1.0f / 60.0f)

static wmo_state  S, S2;
static wmo_sprite SP[WMO_MAX], SP2[WMO_MAX];

static int finite_f(float v) { return v == v && v < 1.0e30f && v > -1.0e30f; }

// Sum of alpha-weighted luminance: what an additive blend puts on screen.
static double total_light(const wmo_sprite *sp, int n)
{
    double t = 0.0;
    int    i;
    for (i = 0; i < n; i++)
        t += (sp[i].a / 255.0) * ((sp[i].r + sp[i].g + sp[i].b) / (3.0 * 255.0));
    return t;
}

// --- 1. idle is neutral ------------------------------------------------------
static void test_idle_controls(void)
{
    wmo_ctl  a, b;
    wm_state m;
    int      i, pa0 = 1;

    printf("1. idle controls\n");
    wmo_controls(NULL, &a);
    wm_init(&m);
    wmo_controls(&m.p, &b);
    for (i = 0; i < WM_PULSES; i++) pa0 &= (b.pa[i] == 0.0f);
    ck(a.ts == b.ts && a.noise == b.noise && a.spin == b.spin &&
       a.lift == b.lift && a.emit == b.emit && a.glint == b.glint &&
       a.tint == b.tint,
       "a fresh stage B maps to exactly the NULL (analyser off) controls");
    ck(b.ts == 1.0f && b.noise == 1.0f && b.spin == 1.0f && b.emit == 1.0f &&
       b.glint == 1.0f && b.lift == 0.0f && b.tint == 0.5f,
       "and those are exactly neutral");
    ck(pa0, "no pulse is live at rest");
}

// --- 2. the cloud follows the band -----------------------------------------
static void test_centreline(void)
{
    const jw_layer *mid = &JW_LAYER[1];
    float yk = JW_TILT / JW_LEN;
    float y0 = JW_TILT * 0.5f + JW_YBASE + mid->y_off;
    float zk = JW_ZRAMP / JW_LEN;
    float z0 = JW_Z0 + JW_ZRAMP * 0.5f + mid->z_off;
    jw_proj pc = jw_project(jw_v3(0.0f, WMO_YC0, WMO_ZC0), 16.0f / 9.0f);

    printf("2. centreline re-derived from wave_gel.h's mid layer\n");
    printf("   y = %+.4f %+.5f x   z = %+.4f %+.5f x   focus depth %.3f\n",
           y0, yk, z0, zk, pc.z);
    ck(fabsf(y0 - WMO_YC0) < 1e-4f && fabsf(yk - WMO_YC_K) < 1e-4f,
       "WMO_YC0 / WMO_YC_K match the mid layer's spine");
    ck(fabsf(z0 - WMO_ZC0) < 1e-4f && fabsf(zk - WMO_ZC_K) < 1e-4f,
       "WMO_ZC0 / WMO_ZC_K match the mid layer's spine");
    ck(pc.ok && fabsf(pc.z - WMO_FOCUS_Z) < 0.05f,
       "WMO_FOCUS_Z is the view depth of the cloud's centre");

    // Both ends of the cloud are off screen at 16:9 (the wider view), by the
    // fade width, for the centreline and for its far side two sd back.
    {
        const float ASP = 16.0f / 9.0f;
        float xlo = WMO_X_LO + WMO_X_FADE, xhi = WMO_X_HI - WMO_X_FADE;
        jw_proj l0 = jw_project(jw_v3(xlo, wmo_yc(xlo), wmo_zc(xlo)), ASP);
        jw_proj h0 = jw_project(jw_v3(xhi, wmo_yc(xhi), wmo_zc(xhi)), ASP);
        jw_proj l1 = jw_project(jw_v3(xlo, wmo_yc(xlo),
                                      wmo_zc(xlo) - 2.0f * WMO_SD_Z), ASP);
        jw_proj h1 = jw_project(jw_v3(xhi, wmo_yc(xhi),
                                      wmo_zc(xhi) - 2.0f * WMO_SD_Z), ASP);
        jw_proj p0 = jw_project(jw_v3(WMO_PULSE_X0, wmo_yc(WMO_PULSE_X0),
                                      wmo_zc(WMO_PULSE_X0)), ASP);
        jw_proj p1 = jw_project(jw_v3(WMO_PULSE_X1, wmo_yc(WMO_PULSE_X1),
                                      wmo_zc(WMO_PULSE_X1)), ASP);
        printf("   inside the fades, the ends project to x %+.2f / %+.2f,"
               " far side %+.2f / %+.2f\n", l0.x, h0.x, l1.x, h1.x);
        printf("   pulse span ends at x %+.2f / %+.2f\n", p0.x, p1.x);
        ck(l0.x < -1.0f && h0.x > 1.0f && l1.x < -1.0f && h1.x > 1.0f,
           "motes fade and respawn off screen at both ends");
        ck(fabsf(p0.x + 1.0f) < 0.05f && fabsf(p1.x - 1.0f) < 0.05f,
           "pulses run edge to edge of the visible centreline");
    }
}

// --- 3. finite and bounded under anything -----------------------------------
static float hostile(unsigned *r, float lo, float hi)
{
    *r = *r * 1103515245u + 12345u;
    switch ((*r >> 28) & 15) {
    case 0:  return NAN;
    case 1:  return INFINITY;
    case 2:  return -INFINITY;
    case 3:  return 1.0e20f;
    default: return lo + (hi - lo) * (float)((*r >> 8) & 0xffff) / 65535.0f;
    }
}

static int state_ok(const wmo_state *s)
{
    int i;
    for (i = 0; i < s->n; i++) {
        float dy = s->y[i] - wmo_yc(s->x[i]);
        float dz = s->z[i] - wmo_zc(s->x[i]);
        if (!finite_f(s->x[i]) || !finite_f(s->y[i]) || !finite_f(s->z[i]) ||
            !finite_f(s->vx[i]) || !finite_f(s->vy[i]) || !finite_f(s->vz[i]) ||
            !finite_f(s->age[i]) || !finite_f(s->ang[i]) || !finite_f(s->exc[i]))
            return 0;
        if (s->x[i] > WMO_X_HI || s->x[i] < WMO_X_LO) return 0;
        if (dy > WMO_BOX_Y + 0.5f || dy < -WMO_BOX_Y) return 0;
        if (dz > WMO_BOX_Z || dz < -WMO_BOX_Z) return 0;
        if (fabsf(s->vx[i]) > WMO_VMAX || fabsf(s->vy[i]) > WMO_VMAX ||
            fabsf(s->vz[i]) > WMO_VMAX) return 0;
        if (s->exc[i] < 0.0f || s->exc[i] > 1.0f) return 0;
    }
    return 1;
}

static int sprites_ok(const wmo_sprite *sp, int n)
{
    int i;
    for (i = 0; i < n; i++) {
        if (!sp[i].a) continue;
        if (!finite_f(sp[i].x) || !finite_f(sp[i].y) || !finite_f(sp[i].s))
            return 0;
        if (sp[i].s < WMO_MIN_S || sp[i].s > 0.5f) return 0;
        if (sp[i].y + sp[i].s > WMO_CLIP_TOP + 1e-5f) return 0;
    }
    return 1;
}

static void test_finite(void)
{
    unsigned r = 777u;
    int      fr, bad_state = 0, bad_sprite = 0;

    printf("3. finite and bounded: 10 min idle, then 10 min of hostile input\n");
    wmo_init(&S, WMO_MAX, 1u);
    for (fr = 0; fr < 36000; fr++) {
        wmo_step(&S, NULL, DT);
        if (fr % 60 == 0) {
            bad_state += !state_ok(&S);
            wmo_shade(&S, NULL, 16.0f / 9.0f, SP, WMO_MAX);
            bad_sprite += !sprites_ok(SP, S.n);
        }
    }
    for (fr = 0; fr < 36000; fr++) {
        wm_params p;
        wmo_ctl   c;
        int       k;
        memset(&p, 0, sizeof p);
        p.timescale = hostile(&r, 0.0f, 3.0f);
        p.lift      = hostile(&r, -1.0f, 2.0f);
        p.bright    = hostile(&r, -1.0f, 2.0f);
        p.hue       = hostile(&r, -1.0f, 2.0f);
        p.glow      = hostile(&r, -1.0f, 2.0f);
        for (k = 0; k < WM_LAYERS; k++) {
            p.amp[k]    = hostile(&r, -1.0f, 2.0f);
            p.detail[k] = hostile(&r, -1.0f, 2.0f);
        }
        for (k = 0; k < WM_PULSES; k++) {
            p.pulse[k].x     = hostile(&r, -1.0f, 2.0f);
            p.pulse[k].width = hostile(&r, -0.5f, 1.0f);
            p.pulse[k].amp   = hostile(&r, -1.0f, 3.0f);
            p.pulse[k].live  = (int)(r & 1);
        }
        wmo_controls(&p, &c);
        wmo_step(&S, &c, (fr % 97 == 0) ? hostile(&r, -1.0f, 5.0f) : DT);
        if (fr % 60 == 0) {
            bad_state += !state_ok(&S);
            wmo_shade(&S, &c, (fr % 2) ? 16.0f / 9.0f : 4.0f / 3.0f, SP, WMO_MAX);
            bad_sprite += !sprites_ok(SP, S.n);
        }
    }
    printf("   %d bad state samples, %d bad sprite samples\n", bad_state, bad_sprite);
    ck(bad_state == 0, "every mote stays finite, inside its box, under VMAX");
    ck(bad_sprite == 0, "every sprite is finite, sized, and below the card grid");
}

// --- 4. the spread is the one the constants promise --------------------------
static void measure_spread(const wmo_ctl *c, float *my, float *sy, float *sz)
{
    double m1 = 0, m2 = 0, z2 = 0;
    long   cnt = 0;
    int    fr, i;

    wmo_init(&S, WMO_MAX, 42u);
    for (fr = 0; fr < 60 * 60; fr++) wmo_step(&S, c, DT);          // settle
    for (fr = 0; fr < 60 * 120; fr++) {
        wmo_step(&S, c, DT);
        if (fr % 10) continue;
        for (i = 0; i < S.n; i++) {
            double dy = S.y[i] - wmo_yc(S.x[i]);
            double dz = S.z[i] - wmo_zc(S.x[i]);
            m1 += dy; m2 += dy * dy; z2 += dz * dz; cnt++;
        }
    }
    *my = (float)(m1 / cnt);
    *sy = (float)sqrt(m2 / cnt - (m1 / cnt) * (m1 / cnt));
    *sz = (float)sqrt(z2 / cnt);
}

static void test_spread(void)
{
    wmo_ctl c;
    float   my, sy, sz, my2, sy2, sz2, want_lift;

    printf("4. steady spread against sd = SIGMA / sqrt(2 GAMMA K)\n");
    wmo_controls(NULL, &c);
    measure_spread(&c, &my, &sy, &sz);
    printf("   idle:  sd_y %.3f (want %.2f)  sd_z %.3f (want %.2f)  mean_y %+.3f\n",
           sy, WMO_SD_Y, sz, WMO_SD_Z, my);
    ck(fabsf(sy / WMO_SD_Y - 1.0f) < 0.20f, "sd_y within 20% of the formula");
    ck(fabsf(sz / WMO_SD_Z - 1.0f) < 0.20f, "sd_z within 20% of the formula");
    ck(fabsf(my) < 0.1f * WMO_SD_Y, "the cloud is centred on the centreline");

    c.lift = WMO_LIFT_ACC;
    measure_spread(&c, &my2, &sy2, &sz2);
    want_lift = WMO_LIFT_ACC / WMO_K;
    printf("   lifted: mean_y %+.3f (want %+.3f)\n", my2, want_lift);
    ck(fabsf(my2 / want_lift - 1.0f) < 0.25f, "full lift raises the cloud by lift/K");

    wmo_controls(NULL, &c);
    c.noise = 1.0f + WMO_NOISE_GAIN * (1.0f - WM_IDLE_AMP);
    measure_spread(&c, &my2, &sy2, &sz2);
    printf("   full bass: sd_y %.3f, %.2fx idle (forcing gain %.2f)\n",
           sy2, sy2 / sy, c.noise);
    ck(fabsf((sy2 / sy) / c.noise - 1.0f) < 0.25f,
       "bass breathes the cloud in proportion to the forcing gain");
}

// --- 5. framing ---------------------------------------------------------------
static int cmp_f(const void *a, const void *b)
{
    float x = *(const float *)a, y = *(const float *)b;
    return (x > y) - (x < y);
}

static void test_framing(float aspect, const char *name)
{
    static float ys[WMO_MAX * 12];
    wmo_ctl c;
    float   xlo = 9.0f, xhi = -9.0f, med;
    long    ny = 0, vis = 0, tot = 0;
    int     fr, i, above = 0;

    printf("5. framing at %s, with the cloud lifted and breathing as far as it goes\n",
           name);
    wmo_controls(NULL, &c);
    c.lift  = WMO_LIFT_ACC;
    c.noise = 1.0f + WMO_NOISE_GAIN * (1.0f - WM_IDLE_AMP);
    wmo_init(&S, WMO_MAX, 5u);
    for (fr = 0; fr < 60 * 60; fr++) {
        wmo_step(&S, &c, DT);
        if (fr % 300) continue;
        wmo_shade(&S, &c, aspect, SP, WMO_MAX);
        for (i = 0; i < S.n; i++) {
            tot++;
            if (!SP[i].a) continue;
            vis++;
            if (SP[i].y + SP[i].s > WMO_CLIP_TOP) above++;
            if (SP[i].x < xlo) xlo = SP[i].x;
            if (SP[i].x > xhi) xhi = SP[i].x;
            if (ny < (long)(sizeof ys / sizeof ys[0])) ys[ny++] = SP[i].y;
        }
    }
    qsort(ys, (size_t)ny, sizeof ys[0], cmp_f);
    med = ny ? ys[ny / 2] : 0.0f;
    printf("   visible %.0f%%  x [%+.2f,%+.2f]  median y %+.2f  y 5..95%% [%+.2f,%+.2f]\n",
           100.0 * vis / (double)tot, xlo, xhi, med,
           ny ? ys[ny / 20] : 0.0f, ny ? ys[ny - 1 - ny / 20] : 0.0f);
    ck(above == 0, "no visible mote reaches above WMO_CLIP_TOP");
    ck(xlo < -0.9f && xhi > 0.9f, "the cloud spans the screen edge to edge");
    ck(med > -0.85f && med < -0.25f, "and sits in the lower half, around the band");
    ck(vis > tot / 3, "most of the cloud is on screen");
}

// --- 6. glints --------------------------------------------------------------
static void test_glints(void)
{
    static int run[WMO_MAX], armed[WMO_MAX];
    static float peak[WMO_MAX];
    wmo_ctl c;
    jw_vec3 hv = wmo_half_vector();
    long    samples = 0, glinting = 0, runs = 0;
    int     fr, i, shortest = 1 << 30;

    printf("6. glints: sparse, and held for several frames at the fastest spin\n");
    wmo_controls(NULL, &c);
    wmo_init(&S, WMO_COUNT_DEF, 9u);
    for (fr = 0; fr < 60 * 30; fr++) {
        wmo_step(&S, &c, DT);
        for (i = 0; i < S.n; i++) {
            float d;
            wmo_normal(&S, i, hv, &d);
            samples++;
            glinting += (d > 0.97f);
        }
    }
    printf("   idle: %.2f%% of flakes in the glint core at any moment\n",
           100.0 * glinting / (double)samples);
    ck(glinting > samples / 300 && glinting < samples / 8,
       "glints are present but sparse (0.3% .. 12.5%)");

    // Fastest spin the controls allow.  A run is the frames a flake spends
    // above the lobe's half-maximum (h^48 = 0.5 at h = 0.9857); only runs that
    // reach the bright core (peak >= 0.995) count, because a flake whose spin
    // plane only grazes the lobe never gets bright enough to flash.
    // A run only counts once the flake has been seen OUTSIDE the lobe, so a
    // glint already under way on the first frame is not mistaken for a short
    // one.
    c.spin = 1.0f + WMO_SPIN_GAIN;
    c.ts   = WMO_TS_MAX;
    memset(run, 0, sizeof run);
    memset(peak, 0, sizeof peak);
    memset(armed, 0, sizeof armed);
    wmo_init(&S, WMO_COUNT_DEF, 11u);
    for (fr = 0; fr < 60 * 60; fr++) {
        wmo_step(&S, &c, DT);
        for (i = 0; i < S.n; i++) {
            float d;
            wmo_normal(&S, i, hv, &d);
            if (d > 0.9857f) {
                run[i]++;
                if (d > peak[i]) peak[i] = d;
            } else {
                if (armed[i] && run[i] && peak[i] >= 0.995f) {
                    runs++;
                    if (run[i] < shortest) shortest = run[i];
                }
                run[i] = 0; peak[i] = 0.0f; armed[i] = 1;
            }
        }
    }
    printf("   fastest spin: %ld bright glints, shortest held %d frames\n",
           runs, shortest);
    ck(runs > 50, "bright glints happen at the fastest spin");
    ck(shortest >= 8, "no bright glint is shorter than 8 frames");
}

// --- 7. the field does not strobe --------------------------------------------
static void run_strobe(const char *name, int audio, double bound)
{
    wm_state    m;
    wa_features f;
    wmo_ctl     c;
    double      prev = -1.0, worst = 0.0, lo = 1e30, hi = 0.0;
    int         fr, i, births = 0, worst_births = 0, pops = 0;
    static float prev_age[WMO_MAX];
    static unsigned char prev_a[WMO_MAX];

    wm_init(&m);
    wmo_init(&S, WMO_COUNT_DEF, 21u);
    for (i = 0; i < S.n; i++) { prev_age[i] = S.age[i]; prev_a[i] = 0; }

    for (fr = 0; fr < 60 * 60; fr++) {
        if (audio) {
            // Loud four-on-the-floor at 120 BPM with a hard onset on every
            // beat, hats on the off-beats: every lever stage B has, pulled.
            int beat = (fr % 30) < 3;
            int hat  = (fr % 30) >= 15 && (fr % 30) < 17;
            memset(&f, 0, sizeof f);
            f.band[WA_SUB] = f.band[WA_BASS] = beat ? 1.0f : 0.5f;
            f.band[WA_MID] = 0.5f;
            f.band[WA_HIGH] = f.band[WA_AIR] = hat ? 1.0f : 0.3f;
            memcpy(f.band_fast, f.band, sizeof f.band);
            f.rms = 0.8f; f.centroid = 0.5f;
            f.onset = (float)beat; f.onset_strength = beat ? 1.0f : 0.0f;
            f.beat_hz = 2.0f; f.beat_conf = 1.0f;
            wm_update(&m, &f, DT);
            wmo_controls(&m.p, &c);
        } else {
            wmo_controls(NULL, &c);
        }
        wmo_step(&S, &c, DT);
        wmo_shade(&S, &c, 16.0f / 9.0f, SP, WMO_MAX);

        // A respawn is a death: the mote must have been invisible (alpha of
        // at most one code) on the frame before, or it popped out.
        births = 0;
        for (i = 0; i < S.n; i++) {
            int born = (S.age[i] < prev_age[i]);
            births += born;
            pops   += born && fr > 0 && prev_a[i] > 1;
            prev_age[i] = S.age[i];
            prev_a[i]   = SP[i].a;
        }
        if (births > worst_births) worst_births = births;

        {
            double L = total_light(SP, S.n);
            if (prev > 0.0 && fr > 60) {
                double step = fabs(L - prev) / prev;
                if (step > worst) worst = step;
            }
            if (fr > 60 && L < lo) lo = L;
            if (fr > 60 && L > hi) hi = L;
            prev = L;
        }
    }
    printf("   %-6s total light %.1f .. %.1f, worst frame-to-frame step %.2f%%,"
           " most births in a frame %d, pops %d\n",
           name, lo, hi, 100.0 * worst, worst_births, pops);
    ck(worst < bound, audio ? "under a beat-heavy script the field's total light"
                              " never jumps by the bound in one frame"
                            : "idle, the field's total light is steady frame to frame");
    // 1024 motes over a mean life of 10.5 s is 1.6 births a frame; a
    // synchronised wave would be hundreds.  12 is Poisson noise with room.
    ck(worst_births <= 12, "births are spread out, never a wave of them");
    ck(pops == 0, "no mote ever disappears while visible");
}

// The bounds: idle 2%, under the audio script 4%.  Both are several times
// what the field measures; they are there to catch a synchronised change --
// a glow that lights every flake at once, a lifetime wave -- which shows up
// as tens of percent, not as a drift in the second decimal.
static void test_strobe(void)
{
    printf("7. strobe: total light, frame to frame\n");
    run_strobe("idle", 0, 0.02);
    run_strobe("audio", 1, 0.04);
}

// --- 8. the audio does what it says ------------------------------------------
static double mean_rb(const wmo_sprite *sp, int n)
{
    double r = 0, b = 0;
    int    i;
    for (i = 0; i < n; i++) if (sp[i].a) { r += sp[i].r; b += sp[i].b; }
    return b > 0 ? r / b : 0;
}

static void test_audio(void)
{
    wmo_ctl  c;
    wm_state m;
    double   l0, l1, rb0, rb1;
    int      fr, i;

    printf("8. audio responses\n");

    // Loudness brightens, through the real controls.
    wmo_init(&S, WMO_COUNT_DEF, 3u);
    for (fr = 0; fr < 120; fr++) wmo_step(&S, NULL, DT);
    wmo_controls(NULL, &c);
    wmo_shade(&S, &c, 16.0f / 9.0f, SP, WMO_MAX);
    l0 = total_light(SP, S.n);
    wm_init(&m);
    m.p.bright = 1.0f;
    wmo_controls(&m.p, &c);
    wmo_shade(&S, &c, 16.0f / 9.0f, SP, WMO_MAX);
    l1 = total_light(SP, S.n);
    printf("   loudness: total light %.1f idle -> %.1f loud\n", l0, l1);
    ck(l1 > l0 * 1.15, "a loud passage brightens the cloud");

    // Hue moves the tint violet -> blue.
    wmo_controls(NULL, &c);
    c.tint = 0.0f; wmo_shade(&S, &c, 16.0f / 9.0f, SP, WMO_MAX); rb0 = mean_rb(SP, S.n);
    c.tint = 1.0f; wmo_shade(&S, &c, 16.0f / 9.0f, SP, WMO_MAX); rb1 = mean_rb(SP, S.n);
    printf("   hue: red/blue %.2f at tint 0, %.2f at tint 1\n", rb0, rb1);
    ck(rb0 > rb1 * 1.5, "tint 0 is violet, tint 1 is blue");

    // Onsets brighten glints.
    wmo_controls(NULL, &c);
    wmo_shade(&S, &c, 16.0f / 9.0f, SP, WMO_MAX); l0 = total_light(SP, S.n);
    c.glint = 1.0f + WMO_GLOW_GAIN;
    wmo_shade(&S, &c, 16.0f / 9.0f, SP, WMO_MAX); l1 = total_light(SP, S.n);
    printf("   onsets: total light %.1f -> %.1f at full glow\n", l0, l1);
    ck(l1 > l0 && l1 < l0 * 1.5, "full glow adds light, as a garnish");

    // A pulse travels, lifts and lights the motes under it.
    {
        double cx[2] = { 0, 0 }, vy = 0, ew = 0;
        int    k = 0;
        wm_init(&m);
        wm_spawn_pulse(&m, 1.0f, 0.5f, 1.0f);    // dir +1: enters at the left
        wmo_init(&S, WMO_COUNT_DEF, 17u);
        for (fr = 1; fr <= 60 * 3; fr++) {
            wm_step_pulses(&m, DT);
            wmo_controls(&m.p, &c);
            wmo_step(&S, &c, DT);
            if (fr == 20 || fr == 40) {
                double sw = 0, sx = 0;
                for (i = 0; i < S.n; i++) { sw += S.exc[i]; sx += S.exc[i] * S.x[i]; }
                cx[k++] = sw > 0 ? sx / sw : 0;
                if (fr == 40) {
                    for (i = 0; i < S.n; i++) { vy += S.exc[i] * S.vy[i]; }
                    ew = sw;
                }
            }
        }
        printf("   pulse: lit centroid x %+.2f at 0.33 s, %+.2f at 0.67 s;"
               " lit motes' mean vy %+.3f\n", cx[0], cx[1], ew > 0 ? vy / ew : 0.0);
        ck(cx[1] - cx[0] > 2.0 && cx[1] - cx[0] < 16.0,
           "the lit region travels with the pulse");
        ck(ew > 0 && vy / ew > 0.02, "the motes it passes under are lifted");
        {
            double left = 0;
            for (i = 0; i < S.n; i++) left += S.exc[i];
            printf("   3 s later: total excitation %.3f\n", left);
            ck(left < 0.02 * S.n, "and it all settles afterwards");
        }
    }
}

// --- 9. deterministic ---------------------------------------------------------
static void test_determinism(void)
{
    int fr, same;
    printf("9. determinism\n");
    wmo_init(&S, WMO_COUNT_DEF, 123u);
    wmo_init(&S2, WMO_COUNT_DEF, 123u);
    for (fr = 0; fr < 600; fr++) { wmo_step(&S, NULL, DT); wmo_step(&S2, NULL, DT); }
    wmo_shade(&S, NULL, 16.0f / 9.0f, SP, WMO_MAX);
    wmo_shade(&S2, NULL, 16.0f / 9.0f, SP2, WMO_MAX);
    ck(memcmp(SP, SP2, sizeof(wmo_sprite) * WMO_COUNT_DEF) == 0,
       "the same seed gives bit-identical sprites");
    wmo_init(&S2, WMO_COUNT_DEF, 124u);
    for (fr = 0; fr < 600; fr++) wmo_step(&S2, NULL, DT);
    wmo_shade(&S2, NULL, 16.0f / 9.0f, SP2, WMO_MAX);
    same = memcmp(SP, SP2, sizeof(wmo_sprite) * WMO_COUNT_DEF) == 0;
    ck(!same, "a different seed gives a different cloud");
}

// --- 10. defensive API --------------------------------------------------------
static void test_defensive(void)
{
    static wmo_state snap;
    int ok = 1;
    printf("10. defensive API\n");
    ck(wmo_init(NULL, 10, 1u) == 0, "wmo_init(NULL) refuses");
    wmo_init(&S, -5, 1u);      ok &= (S.n == 0);
    wmo_init(&S, 99999, 1u);   ok &= (S.n == WMO_MAX);
    ck(ok, "count is clamped to 0..WMO_MAX");
    wmo_step(NULL, NULL, DT);
    wmo_init(&S, 256, 1u);
    memcpy(&snap, &S, sizeof S);
    wmo_step(&S, NULL, NAN);
    wmo_step(&S, NULL, -1.0f);
    wmo_step(&S, NULL, 0.0f);
    ck(memcmp(&snap, &S, sizeof S) == 0, "a bad dt leaves the state untouched");
    // No input can put a NaN into a mote -- the controls and dt are both
    // sanitised -- so corrupt the state directly.  One step must respawn the
    // bad motes cleanly; a respawn written as keep*x + dead*x0 would carry the
    // NaN into the new mote, since 0 * NaN is NaN.
    S.x[5] = NAN; S.vy[6] = INFINITY; S.z[7] = NAN; S.vx[8] = -NAN;
    wmo_step(&S, NULL, DT);
    ck(state_ok(&S), "a corrupted mote is respawned whole in one step");
    ck(wmo_shade(&S, NULL, 16.0f / 9.0f, SP, 0) == 0, "cap 0 draws nothing");
    ck(wmo_shade(&S, NULL, 16.0f / 9.0f, NULL, 10) == 0, "NULL output draws nothing");
    ck(wmo_shade(&S, NULL, 0.0f, SP, WMO_MAX) == 0, "a zero aspect draws nothing");
    ck(wmo_shade(&S, NULL, 16.0f / 9.0f, SP, 10) <= 10, "cap bounds the output");
}

// --- 11. rough cost ---------------------------------------------------------
static void report_cost(void)
{
    clock_t t0, t1, t2;
    int     fr, reps = 2000;
    wmo_init(&S, WMO_COUNT_DEF, 1u);
    t0 = clock();
    for (fr = 0; fr < reps; fr++) wmo_step(&S, NULL, DT);
    t1 = clock();
    for (fr = 0; fr < reps; fr++) wmo_shade(&S, NULL, 16.0f / 9.0f, SP, WMO_MAX);
    t2 = clock();
    printf("11. rough cost at %d motes: step %.1f ns/mote, shade %.1f ns/mote\n",
           WMO_COUNT_DEF,
           1e9 * (double)(t1 - t0) / CLOCKS_PER_SEC / reps / WMO_COUNT_DEF,
           1e9 * (double)(t2 - t1) / CLOCKS_PER_SEC / reps / WMO_COUNT_DEF);
    printf("   (host timing -- the PPU measured 848 ns per gel vertex, so do not"
           " read these as console numbers)\n");
}

int main(void)
{
    printf("wave_motes: %d max, %d default, %d bytes of state\n",
           WMO_MAX, WMO_COUNT_DEF, (int)sizeof(wmo_state));
    test_idle_controls();
    test_centreline();
    test_finite();
    test_spread();
    test_framing(16.0f / 9.0f, "16:9");
    test_framing(4.0f / 3.0f, "4:3");
    test_glints();
    test_strobe();
    test_audio();
    test_determinism();
    test_defensive();
    report_cost();
    if (fails) { printf("\nFAILED: %d check(s)\n", fails); return 1; }
    printf("\nOK\n");
    return 0;
}
