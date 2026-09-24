// Host test for stage A of the audio-reactive wave,
// source/ui/render/wave_audio.h.
//
// Invariants, not golden values (.clinerules rule 9): finite, bounded,
// deterministic, selective, self-calibrating, and silent when the input is.
//
// Two of these are regression guards rather than invariants, and both come
// from the design session that produced this header:
//
//   test_selectivity() asserts that every band's MAXIMUM is its own band.  A
//   three-pole cascade was measured during design and looked strictly better
//   on paper -- 18 dB/octave instead of 12 -- but the extra pole drags each
//   cascade's effective corner further below its nominal one, which moves
//   every band's peak up into the band ABOVE it.  A boundedness test and a
//   finiteness test both pass against that.  Nothing but this assertion
//   catches it.
//
//   test_self_calibration() asserts that a quiet tone and a tone 26 dB louder
//   converge to the SAME level.  Without the reference tracker they would
//   differ by 26 dB, and the wave would either do nothing on quiet music or
//   pin on loud music.  That is the single behaviour that separates this front
//   end from music_fft.cpp's, which normalises to the frame maximum and so
//   cannot tell a flute from a wall of distortion.
//
// libm is used freely HERE.  The kernel may not use it; the test checking it
// may, and generating real sines with sinf() is a far better check than a
// table of hand-written expected values.

#include <stdio.h>
#include <stdint.h>
#include <math.h>
#include <string.h>

#include "../source/ui/render/wave_audio.h"

#define RATE    48000.0f
#define FPS     60.0f
#define DT      (1.0f / FPS)
#define BLK     800                 /* 48000 / 60, one frame of audio */

static int failures = 0;

#define CHECK(cond, ...) do {                                   \
    if (!(cond)) {                                              \
        printf("  FAIL %s:%d: ", __FILE__, __LINE__);           \
        printf(__VA_ARGS__);                                    \
        printf("\n");                                           \
        failures++;                                             \
    }                                                           \
} while (0)

static float band_centre(int i)
{
    return sqrtf(WA_EDGE_HZ[i] * WA_EDGE_HZ[i + 1]);
}

// --- a tiny signal generator ---------------------------------------------
// Everything below drives the analyser at a real 60 fps cadence with a real
// 800-sample block per frame, because the block boundary is part of the code
// under test: wa_frame closes the accumulators, and a test that pushed one
// enormous buffer would never exercise that.

typedef struct {
    long  t;            // sample counter, for continuous phase across blocks
    float hz, amp;
} tone;

static void tone_block(tone *g, float *buf, int n)
{
    int i;
    for (i = 0; i < n; i++) {
        buf[i] = g->amp * sinf(2.0f * (float)M_PI * g->hz * (float)g->t / RATE);
        g->t++;
    }
}

// Run `secs` of a steady tone and return the features of the last frame.
static void run_tone(wa_state *s, float hz, float amp, float secs,
                     wa_features *out)
{
    static float buf[BLK];
    tone g;
    long f, frames = (long)(secs * FPS);
    g.t = 0; g.hz = hz; g.amp = amp;
    for (f = 0; f < frames; f++) {
        tone_block(&g, buf, BLK);
        wa_push(s, buf, BLK, 1);
        wa_frame(s, DT, out);
    }
}

// --- the arithmetic helpers ----------------------------------------------

static void test_helpers(void)
{
    float x;

    // wa_k must land in [0,1) for every positive dt and tau, which is what
    // makes the one-pole unconditionally stable and non-overshooting.
    for (x = 0.0001f; x < 10.0f; x *= 1.3f) {
        float t;
        for (t = 0.001f; t < 30.0f; t *= 1.7f) {
            float k = wa_k(x, t);
            CHECK(k >= 0.0f && k < 1.0f, "wa_k(%g,%g) = %g out of [0,1)", x, t, k);
            if (failures) return;
        }
    }
    CHECK(wa_k(0.0f, 1.0f) == 0.0f, "wa_k with dt 0 should not move");
    CHECK(wa_k(-1.0f, 1.0f) == 0.0f, "wa_k with negative dt should not move");

    // wa_coef must be in (0,1) and MONOTONE in frequency -- monotone is the
    // property that stops two band edges crossing and producing a
    // negative-width band.
    {
        float prev = -1.0f;
        for (x = 1.0f; x < 100000.0f; x *= 1.15f) {
            float a = wa_coef(x, RATE);
            CHECK(a > 0.0f && a < 1.0f, "wa_coef(%g) = %g out of (0,1)", x, a);
            CHECK(a > prev, "wa_coef not monotone at %g: %g <= %g", x, a, prev);
            if (failures) return;
            prev = a;
        }
    }

    // The compressor's fixed point is the whole reason it was chosen: a band
    // sitting at its own typical level must read exactly mid-scale.
    CHECK(fabsf(wa_compress(0.3f, 0.3f) - 0.5f) < 1e-6f,
          "wa_compress at x == ref should be 0.5, got %f",
          wa_compress(0.3f, 0.3f));
    // Bounded and monotone.
    {
        float prev2 = -1.0f;
        for (x = 0.0f; x < 100.0f; x = x * 1.4f + 0.001f) {
            float v = wa_compress(x, 0.5f);
            CHECK(v >= 0.0f && v < 1.0f, "wa_compress(%g) = %g out of [0,1)", x, v);
            CHECK(v >= prev2, "wa_compress not monotone at %g", x);
            if (failures) return;
            prev2 = v;
        }
    }
    CHECK(wa_compress((float)NAN, 0.5f) == 0.0f, "wa_compress of NaN should be 0");

    // The sqrt, against libm.
    {
        float worst = 0.0f;
        for (x = 1e-8f; x < 1e6f; x *= 1.21f) {
            float got = wa_sqrtf(x), ref = sqrtf(x);
            float rel = fabsf(got - ref) / ref;
            if (rel > worst) worst = rel;
        }
        printf("  sqrt: worst relative error vs libm = %.2e\n", worst);
        CHECK(worst < 1e-6f, "wa_sqrtf relative error %.3e exceeds 1e-6", worst);
    }
    CHECK(wa_sqrtf(-1.0f) == 0.0f, "sqrt of a negative should be 0");
    CHECK(wa_sqrtf((float)NAN) == 0.0f, "sqrt of NaN should be 0");
}

// --- the filterbank -------------------------------------------------------

// Raw (pre-envelope, pre-AGC, PRE-GAIN) band RMS for a tone, read straight out
// of the accumulators so the measurement is of the FILTER and nothing else.
//
// WA_BAND_GAIN is deliberately NOT applied here, and that distinction turned
// out to matter.  The gains normalise each band's on-centre response to the
// same value, which is what an unbiased centroid needs -- but they span 2.06
// to 4.67, and applying them flips any band pair whose raw responses are
// within 3.6 dB of each other.  Bands 3 and 4 are 0.3 dB apart at band 3's
// centre, so the gained response peaks in band 4 there.
//
// That is not a filter defect: selectivity is a property of the filter, and
// the gain is a weighting applied afterwards for one consumer.  Asserting the
// diagonal on the gained response would be asserting the wrong thing about
// the wrong stage.  It is asserted on the raw response below, and the gains
// get their own separate assertion.
static void raw_bands(float hz, float *out)
{
    static float buf[BLK];
    wa_state s;
    tone  g;
    double acc[WA_BANDS];
    long   f, n = 0;
    int    i;

    wa_init(&s, RATE);
    g.t = 0; g.hz = hz; g.amp = 1.0f;
    for (i = 0; i < WA_BANDS; i++) acc[i] = 0.0;

    for (f = 0; f < 240; f++) {                  /* 4 s */
        tone_block(&g, buf, BLK);
        wa_push(&s, buf, BLK, 1);
        if (f >= 120) {                          /* let the filters settle */
            for (i = 0; i < WA_BANDS; i++) acc[i] += s.acc[i];
            n += s.acc_n;
        }
        for (i = 0; i < WA_BANDS; i++) s.acc[i] = 0.0f;
        s.acc_rms = 0.0f;
        s.acc_n   = 0;
    }
    for (i = 0; i < WA_BANDS; i++)
        out[i] = (float)sqrt(acc[i] / (double)n);
}

static void test_selectivity(void)
{
    float r[WA_BANDS];
    float on_centre[WA_BANDS];
    int   i, j;

    printf("  raw selectivity, dB (row = tone at band r's centre):\n       ");
    for (j = 0; j < WA_BANDS; j++) printf("%8d", j);
    printf("\n");

    for (i = 0; i < WA_BANDS; i++) {
        int   best = 0;
        float bestv = -1.0f;
        raw_bands(band_centre(i), r);
        printf("    %d: ", i);
        for (j = 0; j < WA_BANDS; j++) {
            printf("%8.1f", 20.0 * log10((double)(r[j] > 1e-9f ? r[j] : 1e-9f)));
            if (r[j] > bestv) { bestv = r[j]; best = j; }
        }
        printf("\n");

        // THE assertion: a tone at a band's centre must read loudest in THAT
        // band.  See the header comment for what this catches.
        CHECK(best == i, "tone at band %d's centre (%.0f Hz) peaks in band %d",
              i, band_centre(i), best);
        on_centre[i] = r[i] * WA_BAND_GAIN[i];

        // A band two or more away must be clearly down.  7 dB is the measured
        // worst case (upward, from SUB into LOWMID) with a little margin; the
        // downward direction is 19 dB or better.  These are loose on purpose:
        // the bands are MEANT to overlap, and a tight bound here would be
        // asserting a filter design that was deliberately not chosen.
        for (j = 0; j < WA_BANDS; j++) {
            if (j >= i - 1 && j <= i + 1) continue;
            CHECK(r[j] < r[i] * 0.45f,
                  "band %d leaks into band %d at %.1f dB (want < -7 dB)",
                  i, j, 20.0 * log10((double)(r[j] / r[i])));
        }
    }

    // WA_BAND_GAIN's one job: make the ON-CENTRE responses comparable across
    // the bands, so the spectral centroid -- a weighted mean over them -- is
    // not biased toward the wide ones.  If this fires, the gain table is stale
    // and needs re-measuring; see its comment in wave_audio.h.
    {
        float lo = on_centre[0], hi = on_centre[0];
        printf("  on-centre response after WA_BAND_GAIN:");
        for (i = 0; i < WA_BANDS; i++) {
            printf(" %.3f", on_centre[i]);
            if (on_centre[i] < lo) lo = on_centre[i];
            if (on_centre[i] > hi) hi = on_centre[i];
        }
        printf("  (spread %.1f%%)\n", 100.0 * (hi / lo - 1.0));
        CHECK(hi < lo * 1.25f,
              "WA_BAND_GAIN is stale: on-centre responses span %.0f%%",
              100.0 * (hi / lo - 1.0));
    }
}

static void test_dc_rejection(void)
{
    static float buf[BLK];
    wa_state s;
    wa_features f;
    int i, k;

    // A constant input.  Every lowpass converges to the same value, so every
    // difference is exactly zero -- in float, not just in theory.  This is why
    // there is no DC blocker in the push path.
    for (i = 0; i < BLK; i++) buf[i] = 0.7f;
    wa_init(&s, RATE);
    for (k = 0; k < 600; k++) {                  /* 10 s */
        wa_push(&s, buf, BLK, 1);
        wa_frame(&s, DT, &f);
    }
    for (i = 0; i < WA_BANDS; i++)
        CHECK(f.band[i] < 0.02f, "DC leaked into band %d: %.6f", i, f.band[i]);
    CHECK(f.onset == 0.0f, "DC should never fire an onset");

    // And the accumulators themselves, before any smoothing rounds it away.
    printf("  DC: largest band accumulator after 10 s = %.3e\n",
           (double)s.acc[0]);
}

static void test_silence(void)
{
    static float buf[BLK];
    wa_state s;
    wa_features f;
    int i, k, onsets = 0;

    memset(buf, 0, sizeof buf);
    wa_init(&s, RATE);
    for (k = 0; k < 600; k++) {
        wa_push(&s, buf, BLK, 1);
        wa_frame(&s, DT, &f);
        if (f.onset > 0.5f) onsets++;
    }
    CHECK(f.silence > 0.95f, "10 s of digital silence gives silence = %.3f",
          f.silence);
    CHECK(onsets == 0, "digital silence fired %d onsets", onsets);
    for (i = 0; i < WA_BANDS; i++)
        CHECK(f.band[i] < 0.01f, "silent band %d reads %.4f", i, f.band[i]);

    // THE REF FLOOR.  Feed a signal 80 dB down -- the kind of thing a decoder
    // emits between tracks -- and the compressor must NOT amplify it to full
    // scale.  Without WA_REF_FLOOR the reference decays toward zero and this
    // reads as a loud track.
    run_tone(&s, 440.0f, 1.0e-4f, 20.0f, &f);
    printf("  ref floor: a -80 dBFS tone reads rms = %.4f, silence = %.3f\n",
           f.rms, f.silence);
    CHECK(f.rms < 0.20f, "a -80 dBFS tone reads rms %.3f -- ref floor is broken",
          f.rms);
    CHECK(f.silence > 0.5f, "a -80 dBFS tone should still count as silence");
}

static void test_self_calibration(void)
{
    wa_state s;
    wa_features quiet, loud;
    int i;

    // Same tone, 26 dB apart.  After the reference has settled both must read
    // about mid-scale: that is the fixed point of wa_compress, and it is the
    // property that makes the wave respond to a solo piano and not pin on a
    // loudness-war master.
    wa_init(&s, RATE);
    run_tone(&s, band_centre(WA_LOWMID), 0.02f, 30.0f, &quiet);
    wa_init(&s, RATE);
    run_tone(&s, band_centre(WA_LOWMID), 0.40f, 30.0f, &loud);

    printf("  self-calibration: -34 dBFS band = %.3f, -8 dBFS band = %.3f\n",
           quiet.band[WA_LOWMID], loud.band[WA_LOWMID]);

    CHECK(fabsf(quiet.band[WA_LOWMID] - 0.5f) < 0.12f,
          "quiet tone settles at %.3f, want ~0.5", quiet.band[WA_LOWMID]);
    CHECK(fabsf(loud.band[WA_LOWMID] - 0.5f) < 0.12f,
          "loud tone settles at %.3f, want ~0.5", loud.band[WA_LOWMID]);
    CHECK(fabsf(quiet.band[WA_LOWMID] - loud.band[WA_LOWMID]) < 0.10f,
          "26 dB of input difference survives as %.3f of output difference",
          fabsf(quiet.band[WA_LOWMID] - loud.band[WA_LOWMID]));

    // A steady tone must not fire onsets once it has settled.  A visualiser
    // that flickers on a held organ chord is reacting to its own envelope
    // noise, not to the music.
    {
        static float buf[BLK];
        wa_features f;
        tone g;
        int onsets = 0, k;
        g.t = 0; g.hz = band_centre(WA_MID); g.amp = 0.3f;
        wa_init(&s, RATE);
        for (k = 0; k < 1800; k++) {             /* 30 s */
            tone_block(&g, buf, BLK);
            wa_push(&s, buf, BLK, 1);
            wa_frame(&s, DT, &f);
            if (k > 300 && f.onset > 0.5f) onsets++;
        }
        printf("  steady tone: %d onsets after the first 5 s\n", onsets);
        CHECK(onsets <= 2, "a steady tone fired %d onsets", onsets);
    }

    // The centroid must separate a low tone from a high one, and must sit
    // somewhere sane for both.
    {
        wa_features lo, hi;
        wa_init(&s, RATE); run_tone(&s, band_centre(WA_BASS), 0.3f, 12.0f, &lo);
        wa_init(&s, RATE); run_tone(&s, band_centre(WA_AIR),  0.3f, 12.0f, &hi);
        printf("  centroid: %.0f Hz -> %.3f, %.0f Hz -> %.3f\n",
               band_centre(WA_BASS), lo.centroid, band_centre(WA_AIR), hi.centroid);
        CHECK(hi.centroid > lo.centroid + 0.35f,
              "centroid barely moves: %.3f -> %.3f", lo.centroid, hi.centroid);
        CHECK(lo.centroid < 0.45f, "bass tone centroid %.3f is too high", lo.centroid);
        CHECK(hi.centroid > 0.55f, "air tone centroid %.3f is too low", hi.centroid);
    }
    (void)i;
}

// --- onsets and the beat estimate ----------------------------------------

// A kick-and-tick pattern at a given BPM over a quiet sustained pad, which is
// what a real track looks like to an envelope follower: a steady bed with
// transients on top, not impulses in a vacuum.
static void run_beats(wa_state *s, float bpm, float secs,
                      int *onsets, wa_features *out)
{
    static float buf[BLK];
    long   f, frames = (long)(secs * FPS);
    long   t = 0;
    double period = 60.0 / (double)bpm;
    int    i;

    *onsets = 0;
    for (f = 0; f < frames; f++) {
        for (i = 0; i < BLK; i++, t++) {
            double tt   = (double)t / (double)RATE;
            double beat = tt - floor(tt / period) * period;   /* time into beat */
            float  v;
            /* sustained pad, so the silence detector never engages */
            v  = 0.05f * sinf(2.0f * (float)M_PI * 220.0f * (float)tt);
            v += 0.04f * sinf(2.0f * (float)M_PI * 330.0f * (float)tt);
            /* kick: low sine with a fast decay */
            v += 0.85f * expf(-(float)beat * 38.0f)
                       * sinf(2.0f * (float)M_PI * 72.0f * (float)beat);
            /* tick: a high burst, so the flux is broadband like a real hit */
            v += 0.30f * expf(-(float)beat * 120.0f)
                       * sinf(2.0f * (float)M_PI * 5200.0f * (float)beat);
            buf[i] = v;
        }
        wa_push(s, buf, BLK, 1);
        wa_frame(s, DT, out);
        if (out->onset > 0.5f) (*onsets)++;
    }
}

static void test_onsets_and_beat(void)
{
    const float bpms[3] = { 90.0f, 120.0f, 160.0f };
    int k;

    for (k = 0; k < 3; k++) {
        wa_state s;
        wa_features f;
        int   onsets;
        float secs     = 30.0f;
        float expected = bpms[k] * secs / 60.0f;
        float period   = 60.0f / bpms[k];

        wa_init(&s, RATE);
        run_beats(&s, bpms[k], secs, &onsets, &f);

        printf("  %3.0f BPM over %.0f s: %d onsets (expected %.0f), "
               "beat_hz %.3f (want %.3f), conf %.2f\n",
               bpms[k], secs, onsets, expected, f.beat_hz, 1.0f / period,
               f.beat_conf);

        // Onset count.  Generous: the first couple of seconds go into
        // settling the flux average, and a detector that fires on every beat
        // forever is not the goal -- a detector that fires on MOST beats and
        // on nothing else is.
        CHECK(onsets > expected * 0.70f && onsets < expected * 1.35f,
              "%.0f BPM gave %d onsets, expected about %.0f",
              bpms[k], onsets, expected);

        // The beat estimate only has to be right about fast versus slow --
        // 10% is well inside what WM_BEAT_REF then does with it.
        CHECK(f.beat_conf > 0.5f, "%.0f BPM: confidence only %.2f",
              bpms[k], f.beat_conf);
        if (f.beat_conf > 0.5f) {
            float got = (f.beat_hz > 0.0f) ? 1.0f / f.beat_hz : 0.0f;
            CHECK(fabsf(got - period) < period * 0.10f,
                  "%.0f BPM: period estimate %.3f s, want %.3f s",
                  bpms[k], got, period);
        }

        // Confidence must decay once the beats stop, so the wave's drift rate
        // returns to idle instead of holding a finished track's tempo.
        {
            static float zero[BLK];
            int j;
            memset(zero, 0, sizeof zero);
            for (j = 0; j < 600; j++) {          /* 10 s of nothing */
                wa_push(&s, zero, BLK, 1);
                wa_frame(&s, DT, &f);
            }
            CHECK(f.beat_conf < 0.3f,
                  "confidence stayed at %.2f 10 s after the music stopped",
                  f.beat_conf);
        }
    }
}

// --- robustness -----------------------------------------------------------

static int features_sane(const wa_features *f, const char *what)
{
    int i, ok = 1;
    for (i = 0; i < WA_BANDS; i++) {
        if (!(f->band[i] >= 0.0f && f->band[i] <= 1.0f)) {
            printf("  FAIL %s: band[%d] = %f\n", what, i, f->band[i]); ok = 0;
        }
        if (!(f->band_fast[i] >= 0.0f && f->band_fast[i] <= 1.0f)) {
            printf("  FAIL %s: band_fast[%d] = %f\n", what, i, f->band_fast[i]);
            ok = 0;
        }
    }
    if (!(f->rms      >= 0.0f && f->rms      <= 1.0f)) { printf("  FAIL %s: rms %f\n", what, f->rms); ok = 0; }
    if (!(f->flux     >= 0.0f && f->flux     <= 1.0f)) { printf("  FAIL %s: flux %f\n", what, f->flux); ok = 0; }
    if (!(f->centroid >= 0.0f && f->centroid <= 1.0f)) { printf("  FAIL %s: centroid %f\n", what, f->centroid); ok = 0; }
    if (!(f->silence  >= 0.0f && f->silence  <= 1.0f)) { printf("  FAIL %s: silence %f\n", what, f->silence); ok = 0; }
    if (!(f->beat_conf>= 0.0f && f->beat_conf<= 1.0f)) { printf("  FAIL %s: beat_conf %f\n", what, f->beat_conf); ok = 0; }
    if (!(f->beat_hz  >= 0.0f && f->beat_hz  < 100.0f)){ printf("  FAIL %s: beat_hz %f\n", what, f->beat_hz); ok = 0; }
    if (f->onset != 0.0f && f->onset != 1.0f)          { printf("  FAIL %s: onset %f\n", what, f->onset); ok = 0; }
    if (!ok) failures++;
    return ok;
}

static void test_hostile(void)
{
    static float buf[BLK * 2];
    wa_state s;
    wa_features f;
    int k, i;

    // Full-scale square wave -- the loudest thing a decoder can hand us.
    wa_init(&s, RATE);
    for (k = 0; k < 900; k++) {
        for (i = 0; i < BLK; i++) buf[i] = ((k * BLK + i) % 96 < 48) ? 1.0f : -1.0f;
        wa_push(&s, buf, BLK, 1);
        wa_frame(&s, DT, &f);
        if (!features_sane(&f, "square wave")) return;
    }

    // NaN, +inf and -inf in the SAME frame.  Clamping the summed downmix is
    // not enough for this case: +inf + -inf is NaN, which passes both of the
    // magnitude clamps, and one NaN through a recursive filter poisons every
    // later output for the rest of the session.  wa_push clamps per channel
    // before summing, which is what this checks.
    wa_init(&s, RATE);
    for (k = 0; k < 300; k++) {
        for (i = 0; i < BLK; i++) {
            float v;
            switch (i & 3) {
            case 0:  v = (float)NAN;       break;
            case 1:  v =  (float)INFINITY; break;
            case 2:  v = -(float)INFINITY; break;
            default: v = 1.0e30f;          break;
            }
            buf[i * 2] = v;
            buf[i * 2 + 1] = -v;
        }
        wa_push(&s, buf, BLK, 2);
        wa_frame(&s, DT, &f);
        if (!features_sane(&f, "NaN/inf storm")) return;
    }
    // ...and it must RECOVER: a poisoned filter would keep reading NaN here.
    run_tone(&s, 440.0f, 0.3f, 10.0f, &f);
    CHECK(f.rms > 0.05f,
          "the analyser did not recover after a NaN storm: rms %f", f.rms);
    features_sane(&f, "recovery after NaN");

    // Wild dt, including a stall far past WA_DT_MAX.
    wa_init(&s, RATE);
    for (k = 0; k < 400; k++) {
        for (i = 0; i < BLK; i++) buf[i] = 0.3f * sinf(0.01f * (float)(k * BLK + i));
        wa_push(&s, buf, BLK, 1);
        wa_frame(&s, (k % 5 == 0) ? 12.0f : DT, &f);
        if (!features_sane(&f, "wild dt")) return;
    }
    wa_frame(&s, 0.0f, &f);          features_sane(&f, "dt 0");
    wa_frame(&s, -1.0f, &f);         features_sane(&f, "dt negative");
    wa_frame(&s, (float)NAN, &f);    features_sane(&f, "dt NaN");
    wa_frame(&s, (float)INFINITY, &f); features_sane(&f, "dt inf");
}

static void test_defensive(void)
{
    wa_state s;
    wa_features f;
    float buf[8] = { 0 };

    CHECK(wa_init(NULL, RATE) == 0, "wa_init(NULL) should fail");
    CHECK(wa_init(&s, 0.0f) == 0, "wa_init at rate 0 should fail");
    CHECK(wa_init(&s, 1.0e9f) == 0, "wa_init at an absurd rate should fail");
    CHECK(wa_init(&s, (float)NAN) == 0, "wa_init at NaN rate should fail");
    // A rejected init must leave a state that is safe to keep calling.
    wa_frame(&s, DT, &f);
    CHECK(f.silence == 1.0f, "a rejected init should report silence");
    features_sane(&f, "after rejected init");
    wa_push(&s, buf, 4, 1);          /* must be a no-op, not a crash */

    CHECK(wa_init(&s, RATE) == 1, "wa_init at 48 kHz should succeed");
    wa_push(&s, NULL, 4, 1);
    wa_push(&s, buf, 0, 1);
    wa_push(&s, buf, -1, 1);
    wa_push(&s, buf, 4, 0);
    wa_push(&s, buf, 4, WA_MAX_CH + 1);
    wa_push(NULL, buf, 4, 1);
    wa_frame(&s, DT, &f);
    features_sane(&f, "after rejected pushes");
    wa_frame(&s, DT, NULL);          /* must not crash */

    // Every sample rate a PS3 decoder can produce must init and behave.
    {
        const float rates[5] = { 32000.0f, 44100.0f, 48000.0f, 96000.0f, 192000.0f };
        int r;
        for (r = 0; r < 5; r++) {
            CHECK(wa_init(&s, rates[r]) == 1, "wa_init at %.0f Hz failed", rates[r]);
            run_tone(&s, 1000.0f, 0.3f, 6.0f, &f);
            features_sane(&f, "alternate sample rate");
        }
    }
}

static void test_determinism(void)
{
    wa_state a, b;
    wa_features fa, fb;
    int onsets_a, onsets_b;

    wa_init(&a, RATE); run_beats(&a, 128.0f, 20.0f, &onsets_a, &fa);
    wa_init(&b, RATE); run_beats(&b, 128.0f, 20.0f, &onsets_b, &fb);

    CHECK(onsets_a == onsets_b, "onset counts differ between runs: %d vs %d",
          onsets_a, onsets_b);
    CHECK(memcmp(&fa, &fb, sizeof fa) == 0,
          "identical input produced different features");
    CHECK(memcmp(&a, &b, sizeof a) == 0,
          "identical input produced different state");
}

int main(void)
{
    printf("wave_audio: %d bands, %d edges, %.0f Hz test rate\n",
           WA_BANDS, WA_EDGES, RATE);

    printf("\n-- arithmetic helpers --\n");      test_helpers();
    printf("\n-- filterbank selectivity --\n");  test_selectivity();
    printf("\n-- DC rejection --\n");            test_dc_rejection();
    printf("\n-- silence and the ref floor --\n"); test_silence();
    printf("\n-- self-calibration --\n");        test_self_calibration();
    printf("\n-- onsets and the beat estimate --\n"); test_onsets_and_beat();
    printf("\n-- hostile input --\n");           test_hostile();
    printf("\n-- defensive API --\n");           test_defensive();
    printf("\n-- determinism --\n");             test_determinism();

    if (failures) {
        printf("\nFAILED: %d check(s)\n", failures);
        return 1;
    }
    printf("\nOK\n");
    return 0;
}
