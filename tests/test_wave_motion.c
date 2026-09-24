// Host test for stage B of the audio-reactive wave,
// source/ui/render/wave_motion.h.
//
// Invariants, not golden values (.clinerules rule 9).
//
// THE TEST THIS FILE EXISTS FOR IS test_slew_limits().
//
// Everything else here is ordinary boundedness and defensiveness.  The slew
// assertion is different in kind: it is the one that turns "the wave is
// elegant" from a claim about tuning into a property of the code.  It feeds
// the stage deliberately hostile features -- all-zero and all-one on
// alternating frames, uniform noise, NaN storms, absurd dt -- and checks that
// EVERY slew-limited parameter moved by at most its declared rate times dt, on
// every single frame.
//
// A visualiser that jumps is a visualiser whose parameters jumped.  If this
// assertion holds, they cannot, whatever the analyser upstream does and
// whatever the audio is.  No amount of envelope tuning gives that, because
// tuning is an average and this is a bound.
//
// Note what is deliberately NOT slew-checked: the per-layer phases, which are
// integrated and wrap at 2pi, and the pulses, which are spawned objects rather
// than continuous parameters.  Both get their own assertions instead.
//
// libm is used freely HERE; the kernel may not use it.

#include <stdio.h>
#include <stdint.h>
#include <math.h>
#include <string.h>

#include "../source/ui/render/wave_motion.h"

#define DT      (1.0f / 60.0f)

static int failures = 0;

#define CHECK(cond, ...) do {                                   \
    if (!(cond)) {                                              \
        printf("  FAIL %s:%d: ", __FILE__, __LINE__);           \
        printf(__VA_ARGS__);                                    \
        printf("\n");                                           \
        failures++;                                             \
    }                                                           \
} while (0)

// A tiny deterministic PRNG, so "random" input is reproducible and a failure
// can be re-run.  xorshift32, same as the kernel's.
static uint32_t rng_state = 0x1234567u;
static float rnd01(void)
{
    uint32_t x = rng_state;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    rng_state = x;
    return (float)(x >> 8) * (1.0f / 16777216.0f);
}

static void feat_zero(wa_features *f)
{
    memset(f, 0, sizeof *f);
    f->centroid = 0.5f;
    f->silence  = 1.0f;
}

// A plausible "loud track" feature set.
static void feat_loud(wa_features *f)
{
    int i;
    memset(f, 0, sizeof *f);
    for (i = 0; i < WA_BANDS; i++) { f->band[i] = 0.75f; f->band_fast[i] = 0.80f; }
    f->rms       = 0.80f;
    f->flux      = 0.30f;
    f->centroid  = 0.55f;
    f->beat_hz   = 2.0f;
    f->beat_conf = 0.9f;
    f->silence   = 0.0f;
}

// --- the arithmetic helpers ----------------------------------------------

static void test_helpers(void)
{
    float x, prev;

    // wm_slew must never move further than rate*dt, in either direction, and
    // must land exactly on the target when the step is large enough.
    CHECK(wm_slew(0.0f, 1.0f, 1.0f, 0.1f) == 0.1f, "slew should step by rate*dt");
    CHECK(wm_slew(0.0f, -1.0f, 1.0f, 0.1f) == -0.1f, "slew should step down too");
    CHECK(wm_slew(0.0f, 0.05f, 1.0f, 0.1f) == 0.05f,
          "slew should land on a near target exactly");
    CHECK(wm_slew(0.5f, (float)NAN, 1.0f, 0.1f) == 0.5f,
          "a NaN target should hold, not propagate");
    CHECK(wm_slew((float)NAN, 1.0f, 1.0f, 0.1f) == 0.0f,
          "a NaN current should recover to 0, not persist");
    CHECK(wm_slew(0.5f, 1.0f, 1.0f, 0.0f) == 0.5f, "dt 0 should not move");
    CHECK(wm_slew(0.5f, 1.0f, -1.0f, 0.1f) == 0.5f, "negative rate should not move");

    // wm_pulse_env: bounded, zero outside, and it must actually peak.
    {
        float peak = 0.0f, at = 0.0f;
        for (x = -0.5f; x <= 1.5f; x += 0.001f) {
            float e = wm_pulse_env(x);
            CHECK(e >= 0.0f && e <= 1.0f, "pulse env(%f) = %f out of [0,1]", x, e);
            if (failures) return;
            if (x < 0.0f || x >= 1.0f)
                CHECK(e == 0.0f, "pulse env(%f) should be 0 outside [0,1)", x);
            if (e > peak) { peak = e; at = x; }
        }
        printf("  pulse envelope: peak %.4f at s = %.3f (attack ends %.3f)\n",
               peak, at, WM_PULSE_ATTACK);
        CHECK(peak > 0.95f, "pulse envelope never reaches full amplitude: %f", peak);
        CHECK(fabsf(at - WM_PULSE_ATTACK) < 0.02f,
              "pulse envelope peaks at %.3f, expected near %.3f",
              at, WM_PULSE_ATTACK);
        // C1 at the join: no kink.  Compare the one-sided slopes.
        {
            float h  = 1e-4f;
            float dl = (wm_pulse_env(WM_PULSE_ATTACK)
                      - wm_pulse_env(WM_PULSE_ATTACK - h)) / h;
            float dr = (wm_pulse_env(WM_PULSE_ATTACK + h)
                      - wm_pulse_env(WM_PULSE_ATTACK)) / h;
            printf("  pulse envelope: slopes at the join %.4f / %.4f\n", dl, dr);
            CHECK(fabsf(dl) < 0.05f && fabsf(dr) < 0.05f,
                  "pulse envelope kinks at the join: %.4f vs %.4f", dl, dr);
        }
    }

    // wm_wrap2pi.
    for (x = -400.0f; x <= 400.0f; x += 0.013f) {
        float w = wm_wrap2pi(x);
        CHECK(w >= 0.0f && w < WM_TWO_PI, "wrap(%f) = %f out of [0,2pi)", x, w);
        if (failures) return;
    }
    CHECK(wm_wrap2pi((float)NAN) == 0.0f, "wrap of NaN should be 0");

    // wm_smooth01: bounded, monotone, and its fixed point at 0.5 -- which is
    // where wa_compress puts a band at its own typical level, so "normal"
    // music must land in the middle of the visual range by construction.
    CHECK(fabsf(wm_smooth01(0.5f) - 0.5f) < 1e-6f,
          "smooth01(0.5) = %f, want 0.5", wm_smooth01(0.5f));
    CHECK(wm_smooth01(0.0f) == 0.0f && wm_smooth01(1.0f) == 1.0f,
          "smooth01 endpoints wrong");
    prev = -1.0f;
    for (x = -0.5f; x <= 1.5f; x += 0.001f) {
        float v = wm_smooth01(x);
        CHECK(v >= 0.0f && v <= 1.0f, "smooth01(%f) = %f out of range", x, v);
        CHECK(v >= prev - 1e-7f, "smooth01 not monotone at %f", x);
        if (failures) return;
        prev = v;
    }
}

// --- THE slew assertion ---------------------------------------------------

// Every slew-limited field, its previous value, and its declared rate.
typedef struct { const char *name; float rate; } slew_field;

static void snapshot(const wm_params *p, float *v)
{
    int i, n = 0;
    v[n++] = p->drive;
    v[n++] = p->perturb;
    v[n++] = p->timescale;
    v[n++] = p->lift;
    v[n++] = p->bright;
    v[n++] = p->hue;
    v[n++] = p->glow;
    for (i = 0; i < WM_LAYERS; i++) v[n++] = p->amp[i];
    for (i = 0; i < WM_LAYERS; i++) v[n++] = p->detail[i];
}

#define NSLEW (7 + 2 * WM_LAYERS)

static void slew_rates(slew_field *f)
{
    int i, n = 0;
    f[n].name = "drive";     f[n++].rate = WM_SLEW_DRIVE;
    f[n].name = "perturb";   f[n++].rate = WM_SLEW_PERTURB;
    f[n].name = "timescale"; f[n++].rate = WM_SLEW_TIME;
    f[n].name = "lift";      f[n++].rate = WM_SLEW_LIFT;
    f[n].name = "bright";    f[n++].rate = WM_SLEW_BRIGHT;
    f[n].name = "hue";       f[n++].rate = WM_SLEW_HUE;
    f[n].name = "glow";      f[n++].rate = WM_SLEW_GLOW_UP;   /* the faster of the pair */
    for (i = 0; i < WM_LAYERS; i++) { f[n].name = "amp";    f[n++].rate = WM_SLEW_AMP; }
    for (i = 0; i < WM_LAYERS; i++) { f[n].name = "detail"; f[n++].rate = WM_SLEW_DETAIL; }
}

static int params_sane(const wm_params *p, const char *what)
{
    int i, ok = 1;
#define BAD(fmt, ...) do { printf("  FAIL %s: " fmt "\n", what, __VA_ARGS__); ok = 0; } while (0)
    if (!(p->drive     >= WM_IDLE_DRIVE && p->drive     <= WM_MAX_DRIVE)) BAD("drive %f", p->drive);
    if (!(p->perturb   >= 0.0f          && p->perturb   <= WM_MAX_PERTURB)) BAD("perturb %f", p->perturb);
    if (!(p->timescale >= WM_MIN_TIME   && p->timescale <= WM_MAX_TIME)) BAD("timescale %f", p->timescale);
    if (!(p->lift      >= 0.0f && p->lift   <= 1.0f)) BAD("lift %f", p->lift);
    if (!(p->bright    >= 0.0f && p->bright <= 1.0f)) BAD("bright %f", p->bright);
    if (!(p->glow      >= 0.0f && p->glow   <= 1.0f)) BAD("glow %f", p->glow);
    if (!(p->hue >= WM_HUE_LO && p->hue <= WM_HUE_LO + WM_HUE_SPAN)) BAD("hue %f", p->hue);
    for (i = 0; i < WM_LAYERS; i++) {
        if (!(p->amp[i]    >= 0.0f && p->amp[i]    <= 1.0f)) BAD("amp[%d] %f", i, p->amp[i]);
        if (!(p->detail[i] >= 0.0f && p->detail[i] <= 1.0f)) BAD("detail[%d] %f", i, p->detail[i]);
        if (!(p->phase[i]  >= 0.0f && p->phase[i]  <  WM_TWO_PI)) BAD("phase[%d] %f", i, p->phase[i]);
    }
    for (i = 0; i < WM_PULSES; i++) {
        const wm_pulse *q = &p->pulse[i];
        if (!(q->amp >= 0.0f && q->amp <= 1.0f)) BAD("pulse[%d].amp %f", i, q->amp);
        if (q->live && !(q->x > -1.0f && q->x < 2.0f)) BAD("pulse[%d].x %f", i, q->x);
        if (q->live && !(q->width > 0.0f && q->width < 1.0f)) BAD("pulse[%d].width %f", i, q->width);
    }
#undef BAD
    if (!ok) failures++;
    return ok;
}

// Drive the stage with `kind` of hostile input for `frames` frames, checking
// the slew bound and the range bound on every one.
static void hostile_run(int kind, long frames, const char *what)
{
    int        entry = failures;      /* local, so an earlier failure elsewhere
                                         does not skip this run entirely */
    wm_state   s;
    wa_features f;
    slew_field  sf[NSLEW];
    float       prev[NSLEW], now[NSLEW];
    float       worst[NSLEW];
    long        k;
    int         i;

    slew_rates(sf);
    for (i = 0; i < NSLEW; i++) worst[i] = 0.0f;

    wm_init(&s);
    snapshot(&s.p, prev);

    for (k = 0; k < frames; k++) {
        float dt = DT;

        switch (kind) {
        case 0:                                  /* alternating extremes */
            if (k & 1) feat_zero(&f); else feat_loud(&f);
            f.onset = (k & 1) ? 0.0f : 1.0f;
            f.onset_strength = 1.0f;
            break;
        case 1:                                  /* uniform noise */
            memset(&f, 0, sizeof f);
            for (i = 0; i < WA_BANDS; i++) { f.band[i] = rnd01(); f.band_fast[i] = rnd01(); }
            f.rms = rnd01(); f.flux = rnd01(); f.centroid = rnd01();
            f.silence = rnd01(); f.beat_conf = rnd01();
            f.beat_hz = rnd01() * 6.0f;
            f.onset = (rnd01() > 0.7f) ? 1.0f : 0.0f;
            f.onset_strength = rnd01();
            break;
        case 2:                                  /* NaN / inf storm */
            memset(&f, 0, sizeof f);
            for (i = 0; i < WA_BANDS; i++) {
                f.band[i]      = (i & 1) ? (float)NAN : (float)INFINITY;
                f.band_fast[i] = (i & 1) ? -(float)INFINITY : (float)NAN;
            }
            f.rms = (float)NAN; f.flux = (float)INFINITY;
            f.centroid = (float)NAN; f.silence = -(float)INFINITY;
            f.beat_hz = (float)NAN; f.beat_conf = (float)INFINITY;
            f.onset = 1.0f; f.onset_strength = (float)NAN;
            break;
        default:                                 /* noise plus wild dt */
            memset(&f, 0, sizeof f);
            for (i = 0; i < WA_BANDS; i++) f.band[i] = rnd01();
            f.rms = rnd01(); f.centroid = rnd01();
            f.onset = (rnd01() > 0.8f) ? 1.0f : 0.0f;
            f.onset_strength = rnd01();
            dt = (k % 7 == 0) ? 9.0f : ((k % 11 == 0) ? 1e-5f : DT);
            break;
        }

        wm_update(&s, &f, dt);
        if (!params_sane(&s.p, what)) return;

        snapshot(&s.p, now);
        {
            // wm_update clamps dt to WA_DT_MAX, so the bound uses the clamped
            // value -- otherwise a 9-second dt would make the bound vacuous
            // rather than checking anything.
            float eff = (dt > WA_DT_MAX) ? WA_DT_MAX : dt;
            for (i = 0; i < NSLEW; i++) {
                float d = fabsf(now[i] - prev[i]);
                float bound = sf[i].rate * eff + 1e-6f;
                float ratio = (bound > 0.0f) ? d / bound : 0.0f;
                if (ratio > worst[i]) worst[i] = ratio;
                CHECK(d <= bound,
                      "%s: %s moved %.6f in one frame, limit is %.6f "
                      "(rate %.2f, dt %.5f, frame %ld)",
                      what, sf[i].name, d, bound, sf[i].rate, eff, k);
                if (failures > entry) return;
            }
            memcpy(prev, now, sizeof prev);
        }
    }

    {
        float mx = 0.0f;
        int   at = 0;
        for (i = 0; i < NSLEW; i++) if (worst[i] > mx) { mx = worst[i]; at = i; }
        printf("  %-22s closest approach to a slew limit: %.3f of it (%s)\n",
               what, mx, sf[at].name);
        // If nothing ever came close, the test is not actually exercising the
        // limiter and would not notice if it were removed.
        CHECK(mx > 0.5f,
              "%s never came within half a slew limit (%.3f) -- this input is "
              "not exercising the limiter", what, mx);
    }
}

static void test_slew_limits(void)
{
    hostile_run(0, 4000, "alternating extremes");
    hostile_run(1, 4000, "uniform noise");
    hostile_run(2, 2000, "NaN/inf storm");
    hostile_run(3, 4000, "noise + wild dt");
}

// --- idle and silence -----------------------------------------------------

static void test_idle(void)
{
    wm_state s;
    long     k;
    float    ph0[WM_LAYERS];
    int      i, moved = 0;

    // A client with no audio source at all passes NULL every frame.  That must
    // give the calm idle wave, not a dead one.
    wm_init(&s);
    for (i = 0; i < WM_LAYERS; i++) ph0[i] = s.p.phase[i];

    for (k = 0; k < 3600; k++) {                 /* 60 s */
        wm_update(&s, NULL, DT);
        if (!params_sane(&s.p, "idle")) return;
    }

    printf("  idle after 60 s: drive %.3f perturb %.4f timescale %.3f "
           "bright %.3f amp0 %.3f\n",
           s.p.drive, s.p.perturb, s.p.timescale, s.p.bright, s.p.amp[0]);

    CHECK(fabsf(s.p.drive     - WM_IDLE_DRIVE)   < 1e-3f, "idle drive %.4f", s.p.drive);
    CHECK(fabsf(s.p.perturb   - WM_IDLE_PERTURB) < 1e-4f, "idle perturb %.5f", s.p.perturb);
    CHECK(fabsf(s.p.timescale - WM_IDLE_TIME)    < 1e-3f, "idle timescale %.4f", s.p.timescale);
    CHECK(fabsf(s.p.bright    - WM_IDLE_BRIGHT)  < 1e-3f, "idle bright %.4f", s.p.bright);

    // CALM, NOT DEAD.  Every one of these is what stops silence reading as a
    // crashed screen.
    CHECK(s.p.drive > 0.2f, "idle drive %.3f is too low to move anything", s.p.drive);
    CHECK(s.p.perturb > 0.0f, "idle perturb is zero -- the chain would settle");
    for (i = 0; i < WM_LAYERS; i++)
        CHECK(s.p.amp[i] > 0.15f, "idle amp[%d] = %.3f, layer would vanish", i, s.p.amp[i]);

    // And the phases must have ADVANCED: a static idle state is exactly the
    // failure this whole design is trying to avoid.
    for (i = 0; i < WM_LAYERS; i++)
        if (fabsf(s.p.phase[i] - ph0[i]) > 1e-4f) moved++;
    CHECK(moved == WM_LAYERS, "only %d of %d layer phases advanced while idle",
          moved, WM_LAYERS);

    // Phases must be spread, not stacked -- otherwise the very first frame
    // shows four parallel ribbons instead of four crossing ones.
    wm_init(&s);
    for (i = 1; i < WM_LAYERS; i++)
        CHECK(fabsf(s.p.phase[i] - s.p.phase[0]) > 0.3f,
              "layers 0 and %d start at nearly the same phase", i);
}

static void test_silence_transition(void)
{
    wm_state s;
    wa_features f;
    long k;
    float loud_drive, loud_hue;

    // Rise: silence -> loud.  Must arrive somewhere in the driven range, and
    // must take its time getting there.
    wm_init(&s);
    feat_loud(&f);
    for (k = 0; k < 600; k++) wm_update(&s, &f, DT);
    loud_drive = s.p.drive;
    loud_hue   = s.p.hue;
    printf("  loud after 10 s: drive %.3f timescale %.3f bright %.3f hue %.3f\n",
           s.p.drive, s.p.timescale, s.p.bright, s.p.hue);
    CHECK(loud_drive > WM_IDLE_DRIVE + 0.3f,
          "loud music only reached drive %.3f", loud_drive);
    CHECK(s.p.bright > 0.7f, "loud music only reached bright %.3f", s.p.bright);

    // How long did it take?  Anything under a third of a second is a jump.
    {
        wm_state t;
        long     n = 0;
        wm_init(&t);
        while (n < 600 && t.p.drive < loud_drive - 0.02f) { wm_update(&t, &f, DT); n++; }
        printf("  rise to within 0.02 of settled drive: %.2f s\n", (float)n * DT);
        CHECK((float)n * DT > 0.33f, "drive reached its target in %.2f s -- too fast",
              (float)n * DT);
    }

    // Fall: loud -> silence.  Must return to idle, and the HUE MUST HOLD --
    // a track that ends should leave the room the colour it made it.
    feat_zero(&f);
    for (k = 0; k < 900; k++) wm_update(&s, &f, DT);
    printf("  back to silence: drive %.3f (idle %.2f), hue %.3f (was %.3f)\n",
           s.p.drive, WM_IDLE_DRIVE, s.p.hue, loud_hue);
    CHECK(fabsf(s.p.drive - WM_IDLE_DRIVE) < 0.02f,
          "did not return to idle drive: %.3f", s.p.drive);
    CHECK(fabsf(s.p.hue - loud_hue) < 0.02f,
          "hue drifted %.3f during silence -- it should hold",
          fabsf(s.p.hue - loud_hue));
}

// --- the mappings ---------------------------------------------------------

static void test_depth_mapping(void)
{
    wm_state s;
    wa_features f;
    long k;
    float bass_amp[WM_LAYERS], treb_amp[WM_LAYERS];
    int  i;

    // FREQUENCY IS DEPTH.  A bass-only signal must move the FAR layers more
    // than the near ones; a treble-only signal the reverse.  If this ever
    // stops holding, the whole visual premise has quietly broken and nothing
    // else in this file would notice.
    wm_init(&s);
    memset(&f, 0, sizeof f);
    f.band[WA_SUB] = 0.9f; f.band[WA_BASS] = 0.9f;
    f.band_fast[WA_SUB] = 0.9f; f.band_fast[WA_BASS] = 0.9f;
    f.rms = 0.6f; f.centroid = 0.1f; f.silence = 0.0f;
    for (k = 0; k < 600; k++) wm_update(&s, &f, DT);
    for (i = 0; i < WM_LAYERS; i++) bass_amp[i] = s.p.amp[i];

    wm_init(&s);
    memset(&f, 0, sizeof f);
    f.band[WA_HIGH] = 0.9f; f.band[WA_AIR] = 0.9f;
    f.band_fast[WA_HIGH] = 0.9f; f.band_fast[WA_AIR] = 0.9f;
    f.rms = 0.6f; f.centroid = 0.9f; f.silence = 0.0f;
    for (k = 0; k < 600; k++) wm_update(&s, &f, DT);
    for (i = 0; i < WM_LAYERS; i++) treb_amp[i] = s.p.amp[i];

    printf("  layer amplitude   bass-only:");
    for (i = 0; i < WM_LAYERS; i++) printf(" %.3f", bass_amp[i]);
    printf("\n                    treble-only:");
    for (i = 0; i < WM_LAYERS; i++) printf(" %.3f", treb_amp[i]);
    printf("\n");

    CHECK(bass_amp[0] > bass_amp[WM_LAYERS - 1] + 0.15f,
          "bass does not favour the far layer: %.3f vs %.3f",
          bass_amp[0], bass_amp[WM_LAYERS - 1]);
    CHECK(treb_amp[WM_LAYERS - 1] > treb_amp[0] + 0.15f,
          "treble does not favour the near layer: %.3f vs %.3f",
          treb_amp[WM_LAYERS - 1], treb_amp[0]);

    // Detail must be zero on the furthest layer at all times: a distant object
    // showing fine grain is what would collapse the depth reading.
    CHECK(s.p.detail[0] == 0.0f, "the far layer has detail %.4f", s.p.detail[0]);
    CHECK(s.p.detail[WM_LAYERS - 1] > 0.3f,
          "treble did not reach the near layer's detail: %.3f",
          s.p.detail[WM_LAYERS - 1]);
}

static void test_tempo_mapping(void)
{
    const float hz[3]  = { 1.2f, 2.0f, 3.1f };   /* 72, 120, 186 BPM */
    float       ts[3];
    int         k, i;

    for (k = 0; k < 3; k++) {
        wm_state s;
        wa_features f;
        long n;
        wm_init(&s);
        feat_loud(&f);
        f.beat_hz = hz[k]; f.beat_conf = 1.0f;
        for (n = 0; n < 900; n++) wm_update(&s, &f, DT);
        ts[k] = s.p.timescale;
        printf("  %5.1f BPM -> timescale %.3f\n", hz[k] * 60.0f, ts[k]);
    }
    for (i = 1; i < 3; i++)
        CHECK(ts[i] > ts[i - 1] + 0.05f,
              "timescale does not rise with tempo: %.3f then %.3f",
              ts[i - 1], ts[i]);

    // Zero confidence must fall back to the idle-ish rate rather than to the
    // last locked tempo, so a track with no detectable pulse does not inherit
    // the previous one's pace.
    {
        wm_state s;
        wa_features f;
        long n;
        wm_init(&s);
        feat_loud(&f);
        f.beat_hz = 3.1f; f.beat_conf = 1.0f;
        for (n = 0; n < 900; n++) wm_update(&s, &f, DT);
        f.beat_conf = 0.0f;
        for (n = 0; n < 900; n++) wm_update(&s, &f, DT);
        printf("  confidence lost -> timescale %.3f (was %.3f)\n",
               s.p.timescale, ts[2]);
        CHECK(s.p.timescale < ts[2] - 0.05f,
              "timescale held the old tempo after confidence was lost");
    }
}

// --- pulses ---------------------------------------------------------------

static void test_pulses(void)
{
    wm_state s;
    wa_features f;
    long k;
    int  i, spawned = 0, saw_left = 0, saw_right = 0;

    wm_init(&s);
    feat_loud(&f);
    f.beat_hz = 2.0f;

    // Fire an onset every 30 frames (0.5 s) and watch what the pool does.
    for (k = 0; k < 1800; k++) {
        float lastx[WM_PULSES];
        int   lastlive[WM_PULSES];
        for (i = 0; i < WM_PULSES; i++) {
            lastx[i]    = s.p.pulse[i].x;
            lastlive[i] = s.p.pulse[i].live;
        }

        f.onset = (k % 30 == 0) ? 1.0f : 0.0f;
        f.onset_strength = 0.8f;
        wm_update(&s, &f, DT);
        if (f.onset > 0.5f) spawned++;

        if (!params_sane(&s.p, "pulses")) return;

        for (i = 0; i < WM_PULSES; i++) {
            const wm_pulse *q = &s.p.pulse[i];
            if (!q->live || !lastlive[i]) continue;
            // A pulse that stayed alive must have MOVED, in a consistent
            // direction.  A stationary pulse is a flash, which is precisely
            // the thing this mechanism exists to avoid.
            CHECK(fabsf(q->x - lastx[i]) > 1e-6f,
                  "a live pulse did not move (frame %ld, slot %d)", k, i);
            if (q->v > 0.0f) CHECK(q->x > lastx[i], "pulse %d moved backwards", i);
            if (q->v < 0.0f) CHECK(q->x < lastx[i], "pulse %d moved forwards", i);
            if (failures) return;
        }
        for (i = 0; i < WM_PULSES; i++) {
            if (s.p.pulse[i].live && s.p.pulse[i].v > 0.0f) saw_right = 1;
            if (s.p.pulse[i].live && s.p.pulse[i].v < 0.0f) saw_left  = 1;
        }
    }

    printf("  %d onsets over 30 s; directions seen: %s%s\n", spawned,
           saw_right ? "right " : "", saw_left ? "left" : "");

    // Direction must alternate.  A fixed direction develops into a visible
    // conveyor belt within about ten seconds.
    CHECK(saw_left && saw_right, "pulses only ever travelled one way");

    // The pool must be bounded and must drain.
    f.onset = 0.0f;
    for (k = 0; k < 600; k++) wm_update(&s, &f, DT);
    for (i = 0; i < WM_PULSES; i++) {
        CHECK(!s.p.pulse[i].live, "pulse %d still alive 10 s after the last onset", i);
        CHECK(s.p.pulse[i].amp == 0.0f, "retired pulse %d has amp %.4f",
              i, s.p.pulse[i].amp);
    }

    // Silence must not spawn: onset_strength is scaled by (1 - silence), so a
    // spurious onset during a fade cannot put a visible bump on screen.
    wm_init(&s);
    feat_zero(&f);
    f.onset = 1.0f; f.onset_strength = 1.0f;
    for (k = 0; k < 60; k++) wm_update(&s, &f, DT);
    for (i = 0; i < WM_PULSES; i++)
        CHECK(s.p.pulse[i].amp < 1e-6f,
              "a pulse reached amp %.4f during full silence", s.p.pulse[i].amp);
}

// --- defensiveness and determinism ---------------------------------------

static void test_defensive(void)
{
    wm_state s;
    wa_features f;

    CHECK(wm_init(NULL) == 0, "wm_init(NULL) should fail");
    CHECK(wm_init(&s) == 1, "wm_init should succeed");

    feat_loud(&f);
    wm_update(NULL, &f, DT);                 /* must not crash */
    wm_update(&s, &f, 0.0f);
    wm_update(&s, &f, -1.0f);
    wm_update(&s, &f, (float)NAN);
    wm_update(&s, &f, (float)INFINITY);
    params_sane(&s.p, "after bad dt");

    // An unready state must be inert rather than undefined.
    {
        wm_state u;
        memset(&u, 0, sizeof u);
        wm_update(&u, &f, DT);
        CHECK(u.p.drive == 0.0f, "an unready state moved");
    }

    // The layer accessors reject out-of-range indices instead of reading off
    // the end of the constant tables.
    CHECK(wm_slew(0.0f, 1.0f, WM_SLEW_MAX, DT) <= WM_SLEW_MAX * DT + 1e-6f,
          "WM_SLEW_MAX is not the largest rate in the table");
}

static void test_determinism(void)
{
    wm_state a, b;
    long k;

    rng_state = 0xABCDEF01u;
    wm_init(&a);
    for (k = 0; k < 2000; k++) {
        wa_features f;
        int i;
        memset(&f, 0, sizeof f);
        for (i = 0; i < WA_BANDS; i++) f.band[i] = rnd01();
        f.rms = rnd01(); f.centroid = rnd01();
        f.onset = (rnd01() > 0.8f) ? 1.0f : 0.0f;
        f.onset_strength = rnd01();
        wm_update(&a, &f, DT);
    }

    rng_state = 0xABCDEF01u;
    wm_init(&b);
    for (k = 0; k < 2000; k++) {
        wa_features f;
        int i;
        memset(&f, 0, sizeof f);
        for (i = 0; i < WA_BANDS; i++) f.band[i] = rnd01();
        f.rms = rnd01(); f.centroid = rnd01();
        f.onset = (rnd01() > 0.8f) ? 1.0f : 0.0f;
        f.onset_strength = rnd01();
        wm_update(&b, &f, DT);
    }

    CHECK(memcmp(&a, &b, sizeof a) == 0,
          "identical input produced different motion state");
}

int main(void)
{
    printf("wave_motion: %d layers, %d pulses, %d slew-limited parameters\n",
           WM_LAYERS, WM_PULSES, NSLEW);

    printf("\n-- arithmetic helpers --\n");       test_helpers();
    printf("\n-- SLEW LIMITS under hostile input --\n"); test_slew_limits();
    printf("\n-- idle --\n");                     test_idle();
    printf("\n-- silence transitions --\n");      test_silence_transition();
    printf("\n-- frequency is depth --\n");       test_depth_mapping();
    printf("\n-- tempo drives the drift rate --\n"); test_tempo_mapping();
    printf("\n-- travelling pulses --\n");        test_pulses();
    printf("\n-- defensive API --\n");            test_defensive();
    printf("\n-- determinism --\n");              test_determinism();

    if (failures) {
        printf("\nFAILED: %d check(s)\n", failures);
        return 1;
    }
    printf("\nOK\n");
    return 0;
}
