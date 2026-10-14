// Stage A of the audio-reactive wave: PCM in, musical features out.
//
//   wave_audio.h   -> features   (this file)
//   wave_motion.h  -> smoothed, slew-limited wave parameters
//   wave_layers.h  -> per-layer geometry and colour over the existing spline
//
// and from there into the geometry pipeline, which is NOT modified:
//
//   wave_kernel.h  -> node positions      (takes drive + perturb from stage B)
//   wave_spline.h  -> dense smooth curve
//   wave_ribbon.h  -> triangle-strip vertices
//
// Written to docs/wave-audio-spec.md sections 2, 3.4 and 3.5.  Nothing here
// is derived from any reference implementation's code: the reference material
// (audio_reactive_analysis.md) contains a property list and four sentences of
// visual description, and the two mappings it actually proposes -- one line
// per band, and a dark-blue-to-bright-red intensity ramp -- are rejected in
// the spec.  What is taken from it is one principle: silence is a state with
// its own animation and needs an explicit threshold (.clinerules rule 10).
//
// House rules (.clinerules rule 7): header-only, pure C, no libm, no PS3
// headers, struct-of-arrays where it matters, deterministic, and NO GLOBALS --
// all state is caller-owned, so the host test runs this exact code rather than
// a stand-in for it.
//
// THREADING.  wa_push() writes the filter state; wa_frame() reads and resets
// it.  They are not synchronised here and must not run concurrently.  On the
// console that is the same contract music_fft.cpp already has between the
// decoder thread and the UI thread, and it is the caller's mutex, not ours --
// this header has no PS3 headers and no locks by design.

#ifndef WAVE_AUDIO_H
#define WAVE_AUDIO_H

#include <stdint.h>
#include <string.h>     /* memcpy, for the rsqrt seed; not libm */

#define WA_TWO_PI       6.28318530718f

// --- band layout ---------------------------------------------------------
// Six bands from seven edges; band i is the difference of the cascades at
// edge i and edge i+1.  Spans are NOMINAL -- see the warping note on wa_coef.
#define WA_BANDS        6
#define WA_EDGES        (WA_BANDS + 1)

#define WA_SUB          0
#define WA_BASS         1
#define WA_LOWMID       2
#define WA_MID          3
#define WA_HIGH         4
#define WA_AIR          5

// --- filter ---------------------------------------------------------------
// Two cascaded one-pole lowpasses per edge, and each band is the difference
// of two adjacent cascades.  Chosen over an FFT for the reasons in spec
// section 7.2; the properties that matter to the code below are:
//
//   UNCONDITIONALLY STABLE.  a is in (0,1) for every positive frequency and
//   every sample rate, and lp += a*(x-lp) is then a convex combination, so
//   |lp| <= max(|lp|, |x|).  No stability condition to check, no resonance to
//   blow up, nothing to clamp.  A Chamberlin state-variable filter would be
//   sharper but its stability limit (f < 2 - q) is reached by the top band at
//   48 kHz, which is exactly the kind of thing that works on the bench and
//   fails on one particular track.
//
//   EXACTLY ZERO AT DC.  Every lowpass has unity gain at DC, so the
//   difference of any two of them is exactly zero there -- in float, not just
//   in theory, because both cascades converge to the same value bit for bit
//   on a constant input.  A DC offset in the PCM (which decoders do produce)
//   therefore cannot leak into SUB, and no DC blocker is needed.
//   test_wave_audio.c asserts this.
//
//   12 dB/OCTAVE SKIRTS, DELIBERATELY BROAD.  Measured at 48 kHz with a
//   full-scale sine at each band's geometric centre: every band's maximum IS
//   its own band (test_wave_audio.c asserts exactly that), but an ADJACENT
//   band is only 0.3 - 4.6 dB down, a band two away is 7 - 10 dB down going
//   up and 19 - 26 dB going down, and anything further is 25 - 60 dB down.
//
//   So the separation is weak between neighbours and strong between the pairs
//   that actually drive different things: SUB/BASS against HIGH/AIR, which
//   drive the far geometry and the fine detail respectively, are 27 - 60 dB
//   apart.  Neighbours blurring into each other is not a defect here, it is
//   the point -- adjacent bands drive adjacent layers, and adjacent layers are
//   meant to move as one family with a phase gradient across them rather than
//   independently, which is what a physical medium does.
//
//   Three cascaded poles were measured and are WORSE, not better: the extra
//   pole drags each cascade's effective corner further below its nominal one,
//   which moves every band's peak up into the band above it.  Two is right.
static const float WA_EDGE_HZ[WA_EDGES] = {
    25.0f, 80.0f, 250.0f, 700.0f, 2000.0f, 6000.0f, 14000.0f
};

// Per-band output gain, so a tone at one band's centre reads about the same as
// a tone at another's.  The bands differ in width (1.68 octaves at SUB, 1.22
// at AIR) and a difference-of-cascades peaks lower the narrower it is, so
// without this the spectral centroid -- which is a weighted mean over the
// bands -- would be biased toward the wide ones.
//
// CALIBRATED, NOT DERIVED, in the same sense as WK_NOISE_SCALE: the values are
// the reciprocal of each band's measured response to a full-scale sine at its
// own geometric centre at 48 kHz, and test_wave_audio.c re-measures them and
// fails if any band drifts more than 25% from the others.  They are not used
// for the per-band levels that drive geometry -- those are self-referencing
// (wa_state::ref), which absorbs any gain error -- only for the centroid and
// for the band-comparison assertions in the test.
static const float WA_BAND_GAIN[WA_BANDS] = {
    2.062f, 2.108f, 2.318f, 2.426f, 2.750f, 4.673f
};

// Onset weighting across the bands.  Kick and snare dominate; the top and
// bottom contribute but do not decide.  Sums to 1 so the flux figure stays
// comparable to a band level.
static const float WA_FLUX_W[WA_BANDS] = {
    0.11f, 0.24f, 0.17f, 0.15f, 0.20f, 0.13f
};

// --- envelope timescales (seconds) ---------------------------------------
// Attack is faster than release everywhere, because that is how hearing
// works: onsets are sharp and decays are gradual, and a symmetric envelope
// makes music look like it is breathing backwards.
#define WA_TAU_FAST_A   0.025f     // (12 ms was tried in v8: the bass hit looked worse -- reverted)
#define WA_TAU_FAST_R   0.070f
#define WA_TAU_MED_A    0.110f
#define WA_TAU_MED_R    0.280f
#define WA_TAU_SLOW_A   0.320f
#define WA_TAU_SLOW_R   0.900f

// --- self-calibration -----------------------------------------------------
// Each band tracks its own typical level and reports energy/(energy+ref),
// whose fixed point is 0.5: a band sitting at its own normal level reads
// mid-scale, within a few seconds of a track starting, whatever the mastering.
#define WA_TAU_REF_UP   2.0f
#define WA_TAU_REF_DN   12.0f
#define WA_REF_CATCHUP  2.5f       // a band this far above its reference ...
#define WA_TAU_REF_FAST 0.35f      // ... re-references this fast

// THE FLOOR IS NOT OPTIONAL.  Without it the reference decays toward zero
// during silence, and then the first denormal of noise reads as full scale --
// which is how a silent screen ends up with a wave thrashing at maximum.
// 0.002 RMS is about -54 dBFS.
#define WA_REF_FLOOR    0.002f

// --- onsets ---------------------------------------------------------------
// Flux is the part of the fast envelope that the slow one has not caught up
// with, summed over the bands.  The threshold is relative to a slowly-tracked
// mean of the flux itself, so a dense track does not fire on every frame and
// a sparse one still fires at all.
#define WA_TAU_FLUX     1.5f
#define WA_FLUX_RATIO   1.9f       // times the running mean
#define WA_FLUX_FLOOR   0.012f     // absolute floor, so silence never fires
#define WA_FLUX_SCALE   0.10f      // excess over threshold that reads as 1.0
#define WA_REFRACTORY   0.115f     // seconds; caps at ~520 onsets/minute

// DENSE, LOUD MIXES (2026-09-25).  Measured on a -3.1 LUFS trap master (LRA
// 1.2): 33 onsets a minute against ~105 for dynamic material, because a
// threshold at 1.9x the running mean is out of reach when the mean itself is
// always high.  So the broadband threshold is ALSO allowed to sit at the mean
// plus a multiple of the flux's own spread, whichever is lower -- sparse music
// keeps the ratio rule, dense music gets a bar it can clear -- and a second
// detector watches the low bands alone (the 808 / kick), with its own
// mean-plus-spread threshold, so a kick counts even under constant hats.
#define WA_FLUX_DEV_K   2.2f
#define WA_KICK_DEV_K   2.0f
#define WA_KICK_FLOOR   0.020f
#define WA_KICK_SCALE   0.08f

// Absolute loudness: rms_ref (the level the bands are referenced to) on a
// dB scale.  Everything else here is self-referenced on purpose; this is the
// one signal that says how LOUD the master is, so the mapping can give a
// loudness-war mix the energy it has.  In the analyser's own units, measured
// on real masters: a -12 LUFS master reads about -7, a -3 LUFS one about +2.
// TEMPO BY AUTOCORRELATION (2026-09-25: "make the wave move faster with the
// BPM").  The interval tracker below matches single onset gaps, which on real
// mixes (hats, triplets, half-time 808s) settled everything near 120 BPM at
// low confidence.  So, as dedicated beat trackers do: the last WA_ODF_N
// frames of the continuous onset signal (flux + kick, unthresholded, blurred
// +-2 frames) are autocorrelated every WA_ODF_EVERY frames over 60..200 BPM;
// each lag scores r(L) times a gentle preference around WA_ODF_PREF BPM,
// which settles half- vs double-time (a bonus for r(2L), r(3L) was tried and
// systematically picked half-time: the slower tempo's multiples are beat lags
// too); parabolic interpolation refines the peak.  When
// that is confident it provides beat_hz / beat_conf; otherwise the interval
// tracker's answer stands.
#define WA_ODF_N       384
#define WA_ODF_EVERY   20
#define WA_ODF_PREF    120.0f
#define WA_ODF_OCT     1.3f      // preference width, octaves
#define WA_ODF_MIN_CONF 0.18f

#define WA_LEVEL_DB_LO  (-22.02f)    // was -16 against a log2 that read +1 high
#define WA_LEVEL_DB_HI  (-4.02f)

// --- beat estimate --------------------------------------------------------
// Not a beat tracker.  It only has to be right about fast versus slow, and it
// only moves the wave's drift rate between 0.7x and 1.6x (spec section 2.4).
#define WA_BEAT_MIN     0.30f      // 200 BPM
#define WA_BEAT_MAX     0.86f      // ~70 BPM
#define WA_BEAT_TOL     0.12f      // relative agreement that counts as a hit
#define WA_TAU_BEAT     3.0f       // period estimate, once locked
#define WA_BEAT_CONF_A  0.25f      // confidence step toward 1 on a hit
#define WA_BEAT_CONF_R  0.18f      // confidence step toward 0 on a miss
#define WA_BEAT_RELOCK  0.25f      // below this confidence, jump instead of drift
#define WA_BEAT_TIMEOUT 3.0f       // no onsets for this long -> confidence falls

// --- silence --------------------------------------------------------------
// Hysteresis, because level hovers around any single threshold during fades
// and gaps between tracks, and a wave that flickers between idle and live is
// worse than one that does neither.
#define WA_SILENCE_IN   0.0015f    // enter below this broadband RMS (~-56 dBFS)
#define WA_SILENCE_OUT  0.0040f    // leave above this (~-48 dBFS)
#define WA_SILENCE_HOLD 0.40f      // seconds below WA_SILENCE_IN before entering
#define WA_TAU_SIL_IN   1.2f       // calming down
#define WA_TAU_SIL_OUT  0.12f      // blooming back up: FAST (was 1.8 s -- measured, the
                                   // wave took 4.15 s to reach 90% at every song start)

// Defensive bounds on the API's inputs.
#define WA_DT_MAX       0.50f      // a frame longer than this is a stall
#define WA_RATE_MIN     8000.0f
#define WA_RATE_MAX     192000.0f
#define WA_MAX_CH       16

// What stage B consumes.  Every field is finite and in its stated range for
// every input wa_push accepts, including hostile ones.
typedef struct {
    float band[WA_BANDS];   // 0..1, self-referenced, medium envelope
    float band_fast[WA_BANDS]; // 0..1, fast envelope of the same levels
    float rms;              // 0..1, broadband loudness, self-referenced
    float flux;             // 0..1, instantaneous onset strength
    float onset;            // 1 on the frame an onset fires, else 0
    float onset_strength;   // 0..1, how far past threshold that onset was
    float centroid;         // 0..1, spectral tilt: 0 = dark, 1 = bright
    float beat_hz;          // estimated beat rate, 0 when not locked
    float beat_conf;        // 0..1
    float silence;          // 0 = audio present, 1 = fully silent
    float level;            // 0..1, ABSOLUTE loudness (see WA_LEVEL_DB_*)
    float bass_rel;         // 0..1, the bass's absolute level vs its own peak (~30 s memory)
    float mid_rel;          // 0..1, the same for the mids
    float density;          // 0..1, onsets per second, smoothed (0 .. ~4/s)
} wa_features;

typedef struct {
    // filter (written by wa_push)
    float a[WA_EDGES];              // one-pole coefficients
    float lp1[WA_EDGES];            // first pole of each cascade
    float lp2[WA_EDGES];            // second pole
    float acc[WA_BANDS];            // sum of band^2 over the block
    float acc_rms;                  // sum of mono^2 over the block
    int   acc_n;                    // samples in the block

    // envelopes and calibration (written by wa_frame)
    float ref[WA_BANDS];
    float fast[WA_BANDS], med[WA_BANDS], slow[WA_BANDS];
    float rms_ref, rms_lvl;

    // onsets and beat
    float flux_avg, flux_dev;
    float kick_avg, kick_dev;       // the low-band detector
    float density;                  // onsets per second, smoothed
    float odf[WA_ODF_N];            // the onset signal, one sample per frame
    int   odf_w, odf_n, odf_tick;
    float fps;                      // frame rate, smoothed
    float ac_hz, ac_conf;           // the autocorrelation's tempo
    float bass_pk, mid_pk;          // slowly decaying peaks of the absolute band levels
    float bass_sm, mid_sm;          // those levels, smoothed ~0.5 s
    float since_onset;              // seconds since the last onset fired
    float beat_period;              // seconds, 0 until first locked
    float beat_conf;

    // silence
    float quiet_for;                // seconds below WA_SILENCE_IN
    float silence;
    int   quiet;                    // hysteresis latch

    int   ready;                    // 0 until wa_init succeeds
} wa_state;

// sqrt(x) without libm: exponent-halving seed plus three Newton steps on the
// reciprocal square root, which reaches float precision.  memcpy keeps the
// type pun well defined in C and C++ and preserves the IEEE754 pattern on
// either endianness, so host and PPU agree.
//
// This is a third copy of a routine that also appears as wk_sqrtf and
// wr_rsqrt.  That is the cost of .clinerules rule 7's header-only,
// self-contained kernels, and it is the existing precedent: wave_spline.h
// does not include wave_kernel.h either.  Seven calls per frame.
static inline float wa_sqrtf(float x)
{
    uint32_t i;
    float    h, y;
    if (!(x > 0.0f)) return 0.0f;           // also catches NaN
    memcpy(&i, &x, sizeof i);
    i = 0x5f3759dfu - (i >> 1);
    memcpy(&y, &i, sizeof y);
    h = 0.5f * x;
    y = y * (1.5f - h * y * y);
    y = y * (1.5f - h * y * y);
    y = y * (1.5f - h * y * y);
    return x * y;
}

// The one-pole coefficient for a cutoff of f Hz at a sample rate of fs.
//
//   a = w / (1 + w),  w = 2*pi*f/fs
//
// This is the backward-Euler pole, not the usual 1 - exp(-w).  Three reasons,
// all of which matter here: it needs no libm; it lands in (0,1) for EVERY
// positive w, so there is no stability condition and no clamp; and it is
// monotone in f, so the edges can never cross and produce a negative-width
// band.
//
// It does warp: as w approaches 1 the effective cutoff sits below the nominal
// one, so AIR's 14 kHz upper edge is not its true -3 dB point at 44.1 kHz.
// That is accepted rather than corrected -- these are aesthetic band edges,
// and the host test asserts relative selectivity rather than corner
// frequencies.  Prewarping would need a tangent.
static inline float wa_coef(float f, float fs)
{
    float w;
    if (!(f > 0.0f) || !(fs > 0.0f)) return 0.0f;
    w = WA_TWO_PI * f / fs;
    if (w > 64.0f) w = 64.0f;               // f above Nyquist: effectively pass-through
    return w / (1.0f + w);
}

// One-pole smoothing coefficient for a time constant of tau seconds over dt.
// Backward Euler again, and for the same reasons: no libm, always in [0,1),
// cannot overshoot, and correctly frame-rate independent -- a dropped frame
// with a doubled dt lands in the same place, which matters on a console that
// drops frames when a poster decodes.
static inline float wa_k(float dt, float tau)
{
    if (!(dt > 0.0f)) return 0.0f;
    if (!(tau > 0.0f)) return 1.0f;
    return dt / (tau + dt);
}

// Asymmetric one-pole: attack when rising, release when falling.
static inline float wa_env(float y, float x, float dt, float tau_a, float tau_r)
{
    float k = wa_k(dt, (x > y) ? tau_a : tau_r);
    return y + (x - y) * k;
}

// Soft-knee compressor, x/(x+ref).  Bounded in [0,1) by construction for any
// non-negative x and positive ref, monotone, no libm, and it behaves like a
// logarithm across the two decades in the middle -- which is the range
// musical dynamics actually occupy.  Its fixed point is the useful part: at
// x == ref it returns exactly 0.5.
// log2 without libm: exponent from the bits, a quadratic on the mantissa
// (error < 0.01, plenty for a loudness meter).
static inline float wa_log2(float x)
{
    union { float f; uint32_t u; } v;
    float m;
    int   e;
    if (!(x > 1e-12f)) return -40.0f;
    v.f = x;
    e = (int)((v.u >> 23) & 0xFF) - 128;           // 128: the quadratic below is log2(m) + 1
    v.u = (v.u & 0x007FFFFFu) | 0x3F800000u;       // mantissa in [1,2)
    m = v.f;
    return (float)e + (-0.34484843f * m + 2.02466578f) * m - 0.67487759f;
}

static inline float wa_compress(float x, float ref)
{
    float d;
    if (!(x > 0.0f)) return 0.0f;
    if (!(ref > 0.0f)) ref = WA_REF_FLOOR;
    d = x + ref;
    return x / d;
}

static inline float wa_clamp01(float v)
{
    if (v != v)    return 0.0f;             // NaN in, defined out
    if (v < 0.0f)  return 0.0f;
    if (v > 1.0f)  return 1.0f;
    return v;
}

// Bind a state to a sample rate and reset every filter and envelope.
// Returns 1 on success, 0 if the rate is outside WA_RATE_MIN..WA_RATE_MAX, in
// which case the state is marked not-ready and wa_push is a no-op while
// wa_frame returns the silent feature set.
static inline int wa_init(wa_state *s, float sample_rate)
{
    int i;
    if (!s) return 0;
    memset(s, 0, sizeof *s);
    if (!(sample_rate >= WA_RATE_MIN) || !(sample_rate <= WA_RATE_MAX)) return 0;
    for (i = 0; i < WA_EDGES; i++)
        s->a[i] = wa_coef(WA_EDGE_HZ[i], sample_rate);
    for (i = 0; i < WA_BANDS; i++)
        s->ref[i] = WA_REF_FLOOR;
    s->rms_ref  = WA_REF_FLOOR;
    s->quiet    = 1;
    s->silence  = 1.0f;             // start calm, bloom up when audio arrives
    s->quiet_for = WA_SILENCE_HOLD;
    s->ready    = 1;
    return 1;
}

// Feed one block of interleaved PCM.  Samples are expected in roughly
// [-1, 1]; anything finite is accepted and the filters bound it regardless.
// Non-finite samples are replaced with zero rather than allowed to poison the
// filter state permanently -- one NaN through a recursive filter would make
// every later output NaN for the rest of the session.
//
// The downmix is the mean of ALL channels, which differs from
// music_fft.cpp's 0.5*(L+R) on purpose: on 5.1 and 7.1 content the centre
// channel carries dialogue and lead vocals, and an L/R-only downmix would
// leave the wave deaf to exactly the part of the mix a listener is following.
static inline void wa_push(wa_state *s, const float *pcm, int frames, int channels)
{
    int f, c, e;
    float inv_ch;

    if (!s || !s->ready || !pcm) return;
    if (frames <= 0 || channels <= 0 || channels > WA_MAX_CH) return;
    inv_ch = 1.0f / (float)channels;

    for (f = 0; f < frames; f++) {
        const float *fr = pcm + (size_t)f * (size_t)channels;
        float mono = 0.0f, prev, band;

        // Each channel is clamped BEFORE it is summed, not after.  Clamping
        // the sum is not enough: +inf in one channel and -inf in another sum
        // to NaN, which passes both "> 16" and "< -16", and one NaN through a
        // recursive filter makes every output NaN for the rest of the session.
        // Clamping first makes mono a sum of at most WA_MAX_CH finite values,
        // so it cannot be anything but finite.
        for (c = 0; c < channels; c++) {
            float v = fr[c];
            if (v != v) continue;           // NaN: drop it, keep the rest
            if (v >  16.0f) v =  16.0f;     // also catches +inf
            if (v < -16.0f) v = -16.0f;     // also catches -inf
            mono += v;
        }
        mono *= inv_ch;

        // Cascade every edge, then take the differences.  The loop runs the
        // edges in order and keeps the previous cascade output, so each band
        // costs one subtract on top of the four multiply-adds its upper edge
        // already needed.
        prev = 0.0f;
        for (e = 0; e < WA_EDGES; e++) {
            float a = s->a[e];
            s->lp1[e] += a * (mono      - s->lp1[e]);
            s->lp2[e] += a * (s->lp1[e] - s->lp2[e]);
            if (e > 0) {
                band = prev - s->lp2[e];
                s->acc[e - 1] += band * band;
            }
            prev = s->lp2[e];
        }
        s->acc_rms += mono * mono;
    }
    s->acc_n += frames;
}

// Close the block, advance every envelope by dt seconds, and fill out.
//
// Safe to call with no audio pushed since the last call: the band energies
// then read zero, every envelope releases toward zero, and the silence ramp
// takes over -- which is the correct behaviour for a paused or stalled
// decoder and needs no separate code path.
//
// Non-finite or non-positive dt leaves the state untouched and reports the
// previous frame's features.
// See WA_ODF_*.  Returns 1 and sets *hz / *conf when it has an answer.
static inline int wa_tempo_ac(const wa_state *s, float *hz, float *conf)
{
    static float x[WA_ODF_N];
    float r[WA_ODF_N / 2 + 1], mean = 0.0f, r0 = 0.0f, best = -1.0f;
    int   n = s->odf_n, i, L, lmin, lmax, bl = 0;
    if (n < WA_ODF_N / 2 || !(s->fps > 10.0f)) return 0;
    for (i = 0; i < n; i++) {
        x[i] = s->odf[(s->odf_w - n + i + WA_ODF_N) % WA_ODF_N];
        mean += x[i];
    }
    mean /= (float)n;
    // blur by +-2 frames: a beat that falls between two frames otherwise
    // correlates at only half strength at either integer lag
    {
        float y[WA_ODF_N];
        for (i = 0; i < n; i++) {
            float a = 0.0f, ws = 0.0f; int k;
            for (k = -2; k <= 2; k++) {
                const int j = i + k;
                const float wk = (float)(3 - (k < 0 ? -k : k));
                if (j < 0 || j >= n) continue;
                a += wk * x[j]; ws += wk;
            }
            y[i] = a / ws;
        }
        for (i = 0; i < n; i++) x[i] = y[i];
    }
    for (i = 0; i < n; i++) { x[i] -= mean; r0 += x[i] * x[i]; }
    if (r0 < 1e-9f) return 0;
    lmin = (int)(s->fps * 60.0f / 200.0f);
    lmax = (int)(s->fps * 60.0f / 60.0f + 0.5f);
    if (lmin < 2) lmin = 2;
    if (3 * lmax + 1 > n / 2) lmax = n / 6 - 1;
    for (L = 1; L <= 3 * lmax + 1 && L <= WA_ODF_N / 2; L++) {
        float a = 0.0f;
        for (i = L; i < n; i++) a += x[i] * x[i - L];
        r[L] = a / r0 * ((float)n / (float)(n - L));     // unbiased-ish
    }
    for (L = lmin; L <= lmax; L++) {
        const float bpm = s->fps * 60.0f / (float)L;
        const float oc  = (wa_log2(bpm) - wa_log2(WA_ODF_PREF)) / WA_ODF_OCT;
        const float w   = 1.0f / (1.0f + 0.5f * oc * oc);
        const float sc  = r[L] * w;
        if (sc > best) { best = sc; bl = L; }
    }
    if (bl <= lmin || bl >= lmax || r[bl] <= 0.0f) return 0;
    {
        // parabolic peak of r around bl
        const float y0 = r[bl - 1], y1 = r[bl], y2 = r[bl + 1];
        const float den = y0 - 2.0f * y1 + y2;
        float off = den < -1e-6f ? 0.5f * (y0 - y2) / den : 0.0f;
        if (off > 0.5f) off = 0.5f;
        if (off < -0.5f) off = -0.5f;
        *hz   = s->fps / ((float)bl + off);
        *conf = wa_clamp01(r[bl] * 1.6f);
    }
    return 1;
}

static inline void wa_frame(wa_state *s, float dt, wa_features *out)
{
    float e[WA_BANDS];
    float rms_amp, flux, thresh, num, den, cent;
    int   i, fired;

    if (!out) return;
    if (!s || !s->ready) { memset(out, 0, sizeof *out); out->silence = 1.0f; return; }
    if (!(dt > 0.0f) || !(dt < 1.0e30f)) dt = 0.0f;
    if (dt > WA_DT_MAX) dt = WA_DT_MAX;

    // --- close the block: mean square -> RMS amplitude per band ----------
    if (s->acc_n > 0) {
        float inv = 1.0f / (float)s->acc_n;
        for (i = 0; i < WA_BANDS; i++)
            e[i] = wa_sqrtf(s->acc[i] * inv) * WA_BAND_GAIN[i];
        rms_amp = wa_sqrtf(s->acc_rms * inv);
    } else {
        for (i = 0; i < WA_BANDS; i++) e[i] = 0.0f;
        rms_amp = 0.0f;
    }
    for (i = 0; i < WA_BANDS; i++) s->acc[i] = 0.0f;
    s->acc_rms = 0.0f;
    s->acc_n   = 0;

    if (dt <= 0.0f) {
        // Nothing to advance; report the last envelopes so a zero-dt call is
        // a read rather than a glitch.  The block above still cleared the
        // accumulators, so no energy is double-counted.
        for (i = 0; i < WA_BANDS; i++) {
            out->band[i]      = wa_clamp01(s->med[i]);
            out->band_fast[i] = wa_clamp01(s->fast[i]);
        }
        out->rms = wa_clamp01(s->rms_lvl);
        out->level = 0.0f;
        out->density = 0.0f;
        out->bass_rel = out->mid_rel = 0.0f;
        out->flux = 0.0f; out->onset = 0.0f; out->onset_strength = 0.0f;
        out->centroid = 0.5f;
        out->beat_hz = (s->beat_period > 0.0f) ? 1.0f / s->beat_period : 0.0f;
        out->beat_conf = wa_clamp01(s->beat_conf);
        out->silence = wa_clamp01(s->silence);
        return;
    }

    // --- self-calibration and the three envelopes ------------------------
    // The reference chases the level up quickly-ish and falls away slowly, so
    // a loud passage normalises within a couple of seconds while a quiet one
    // keeps its headroom for much longer.  Floored, per WA_REF_FLOOR.
    for (i = 0; i < WA_BANDS; i++) {
        float lvl;
        // A band far above its reference -- the bass coming back after a
        // breakdown -- catches up in ~0.35 s instead of 2 s.  Otherwise it
        // reads saturated near 1 for seconds and its beats vanish inside
        // it: the "drop" that took seconds to hit (v10).
        s->ref[i] = wa_env(s->ref[i], e[i], dt,
                           e[i] > WA_REF_CATCHUP * s->ref[i] ? WA_TAU_REF_FAST : WA_TAU_REF_UP,
                           WA_TAU_REF_DN);
        if (!(s->ref[i] > WA_REF_FLOOR)) s->ref[i] = WA_REF_FLOOR;
        lvl = wa_compress(e[i], s->ref[i]);
        s->fast[i] = wa_env(s->fast[i], lvl, dt, WA_TAU_FAST_A, WA_TAU_FAST_R);
        s->med[i]  = wa_env(s->med[i],  lvl, dt, WA_TAU_MED_A,  WA_TAU_MED_R);
        s->slow[i] = wa_env(s->slow[i], lvl, dt, WA_TAU_SLOW_A, WA_TAU_SLOW_R);
    }
    // the same catch-up for the broadband level (absolute loudness): after a
    // quiet intro the drop's loudness is known in ~0.35 s, not ~8 s (v10)
    s->rms_ref = wa_env(s->rms_ref, rms_amp, dt,
                        rms_amp > WA_REF_CATCHUP * s->rms_ref ? WA_TAU_REF_FAST : WA_TAU_REF_UP,
                        WA_TAU_REF_DN);
    if (!(s->rms_ref > WA_REF_FLOOR)) s->rms_ref = WA_REF_FLOOR;
    s->rms_lvl = wa_env(s->rms_lvl, wa_compress(rms_amp, s->rms_ref),
                        dt, WA_TAU_SLOW_A, WA_TAU_SLOW_R);

    // --- flux: what the fast envelope has and the slow one has not -------
    // Half-wave rectified, so a decay contributes nothing.  This is spectral
    // flux computed from envelopes rather than from bin differences, which is
    // both cheaper and better behaved: there are no bin boundaries to chatter
    // across.
    flux = 0.0f;
    for (i = 0; i < WA_BANDS; i++) {
        float d = s->fast[i] - s->slow[i];
        if (d > 0.0f) flux += d * WA_FLUX_W[i];
    }
    if (flux > 1.0f) flux = 1.0f;

    s->since_onset += dt;
    {
        const float t_ratio = s->flux_avg * WA_FLUX_RATIO;
        const float t_dev   = s->flux_avg + WA_FLUX_DEV_K * s->flux_dev;
        thresh = (t_dev < t_ratio ? t_dev : t_ratio) + WA_FLUX_FLOOR;
    }
    // the low-band (kick) detector
    float kick = 0.0f, kthresh;
    {
        int b;
        for (b = 0; b < 2 && b < WA_BANDS; b++) {        // SUB, BASS
            const float d = s->fast[b] - s->slow[b];
            if (d > 0.0f) kick += d;
        }
        kthresh = s->kick_avg + WA_KICK_DEV_K * s->kick_dev + WA_KICK_FLOOR;
    }
    const int fired_b = (flux > thresh);
    const int fired_k = (kick > kthresh);
    fired  = (fired_b || fired_k) && (s->since_onset >= WA_REFRACTORY);

    // The running means are updated AFTER the comparison, so a single loud
    // onset cannot raise the bar it is being judged against in the same
    // frame.  Symmetric tau: this is a threshold reference, not a musical
    // envelope, and an asymmetric one would ratchet upward on dense material
    // until nothing could clear it.
    {
        // WINSORISED (v11): one frame moves the mean and the spread by at
        // most 3x the current spread.  A drop's own transient used to lift
        // both thresholds so far that the next ~2 s of kicks went unheard
        // (Rockstar Lifestyle: 3 onsets in 2.5 s after the drop, ~4/s after).
        const float k = wa_k(dt, WA_TAU_FLUX);
        const float fcap = s->flux_avg + 3.0f * s->flux_dev + WA_FLUX_FLOOR;
        const float kcap = s->kick_avg + 3.0f * s->kick_dev + WA_KICK_FLOOR;
        const float fl = flux < fcap ? flux : fcap;
        const float kl = kick < kcap ? kick : kcap;
        const float fd = fl > s->flux_avg ? fl - s->flux_avg : s->flux_avg - fl;
        const float kd = kl > s->kick_avg ? kl - s->kick_avg : s->kick_avg - kl;
        s->flux_avg += (fl - s->flux_avg) * k;
        s->flux_dev += (fd - s->flux_dev) * k;
        s->kick_avg += (kl - s->kick_avg) * k;
        s->kick_dev += (kd - s->kick_dev) * k;
    }

    out->onset = 0.0f;
    out->onset_strength = 0.0f;
    s->density += ((fired ? 1.0f / (dt > 1e-4f ? dt : 1e-4f) : 0.0f) - s->density) * wa_k(dt, 2.5f);
    if (fired) {
        float excess = fired_b ? (flux - thresh) * (1.0f / WA_FLUX_SCALE) : 0.0f;
        const float kx = fired_k ? (kick - kthresh) * (1.0f / WA_KICK_SCALE) : 0.0f;
        if (kx > excess) excess = kx;
        out->onset = 1.0f;
        out->onset_strength = wa_clamp01(excess);

        // --- beat estimate, octave folded ---------------------------------
        // The interval between onsets is folded into WA_BEAT_MIN..MAX so
        // eighth notes and half bars land on the same estimate as the beat
        // itself.  The fold is bounded to six halvings/doublings, which
        // covers a factor of 64 -- far more than any real interval -- and
        // makes the loop provably terminate whatever the input.
        if (s->since_onset < WA_BEAT_TIMEOUT) {
            float iv = s->since_onset;
            int   guard;
            for (guard = 0; guard < 6 && iv > WA_BEAT_MAX; guard++) iv *= 0.5f;
            for (guard = 0; guard < 6 && iv < WA_BEAT_MIN; guard++) iv *= 2.0f;
            if (iv >= WA_BEAT_MIN && iv <= WA_BEAT_MAX) {
                if (s->beat_period <= 0.0f || s->beat_conf < WA_BEAT_RELOCK) {
                    s->beat_period = iv;            // cold start, or re-lock
                    s->beat_conf  += (1.0f - s->beat_conf) * WA_BEAT_CONF_A;
                } else {
                    float rel = (iv - s->beat_period) / s->beat_period;
                    if (rel < 0.0f) rel = -rel;
                    if (rel <= WA_BEAT_TOL) {
                        // Agreement: drift the estimate, raise confidence.
                        s->beat_period += (iv - s->beat_period)
                                        * wa_k(s->since_onset, WA_TAU_BEAT);
                        s->beat_conf   += (1.0f - s->beat_conf) * WA_BEAT_CONF_A;
                    } else {
                        s->beat_conf   -= s->beat_conf * WA_BEAT_CONF_R;
                    }
                }
            }
        }
        s->since_onset = 0.0f;
    } else if (s->since_onset > WA_BEAT_TIMEOUT) {
        // Nothing rhythmic for a while: let confidence fall so the drift rate
        // returns to idle rather than holding the last track's tempo.
        s->beat_conf -= s->beat_conf * wa_k(dt, WA_TAU_BEAT);
    }
    s->beat_conf = wa_clamp01(s->beat_conf);

    // --- tempo by autocorrelation (see WA_ODF_*) --------------------------
    if (dt > 0.0f) {
        s->fps += ((1.0f / dt) - s->fps) * (s->fps > 1.0f ? 0.02f : 1.0f);
        s->odf[s->odf_w] = flux + kick;
        s->odf_w = (s->odf_w + 1) % WA_ODF_N;
        if (s->odf_n < WA_ODF_N) s->odf_n++;
        if (++s->odf_tick >= WA_ODF_EVERY) {
            float hz, cf;
            s->odf_tick = 0;
            if (wa_tempo_ac(s, &hz, &cf)) {
                // a small change is smoothed in; a jump needs confidence
                if (s->ac_hz > 0.0f) {
                    float rel = (hz - s->ac_hz) / s->ac_hz;
                    if (rel < 0.0f) rel = -rel;
                    if (rel < 0.08f) s->ac_hz += (hz - s->ac_hz) * 0.35f;
                    else if (cf > s->ac_conf * 0.9f) s->ac_hz = hz;
                } else {
                    s->ac_hz = hz;
                }
                s->ac_conf += (cf - s->ac_conf) * 0.4f;
            } else {
                s->ac_conf *= 0.8f;
            }
        }
    }

    // --- spectral centroid ------------------------------------------------
    // Weighted mean of the band INDEX, which is a proxy for log frequency
    // because the edges are roughly geometric.  Scale-invariant (numerator
    // and denominator both scale with level), so it needs no reference of its
    // own -- only comparable per-band gains, which is what WA_BAND_GAIN is
    // for.  Falls back to the middle when there is nothing to weigh, so a
    // fade to silence holds the hue instead of swinging it.
    num = 0.0f; den = 0.0f;
    for (i = 0; i < WA_BANDS; i++) { num += (float)i * e[i]; den += e[i]; }
    cent = (den > WA_REF_FLOOR) ? (num / den) * (1.0f / (float)(WA_BANDS - 1))
                                : 0.5f;

    // --- silence, with hysteresis ----------------------------------------
    if (s->quiet) {
        if (rms_amp > WA_SILENCE_OUT) { s->quiet = 0; s->quiet_for = 0.0f; }
    } else {
        if (rms_amp < WA_SILENCE_IN) {
            s->quiet_for += dt;
            if (s->quiet_for >= WA_SILENCE_HOLD) s->quiet = 1;
        } else {
            s->quiet_for = 0.0f;
        }
    }
    s->silence += ((s->quiet ? 1.0f : 0.0f) - s->silence)
                * wa_k(dt, s->quiet ? WA_TAU_SIL_IN : WA_TAU_SIL_OUT);

    // --- publish ----------------------------------------------------------
    for (i = 0; i < WA_BANDS; i++) {
        out->band[i]      = wa_clamp01(s->med[i]);
        out->band_fast[i] = wa_clamp01(s->fast[i]);
    }
    out->rms       = wa_clamp01(s->rms_lvl);
    out->level     = wa_clamp01((6.0206f * wa_log2(s->rms_ref) - WA_LEVEL_DB_LO)
                                / (WA_LEVEL_DB_HI - WA_LEVEL_DB_LO));
    out->density   = wa_clamp01(s->density * 0.25f);
    // Absolute band levels against their own peaks: the one place a bass
    // DROPOUT shows (the self-referenced bands renormalise it away).
    // e[] is band energy; the amplitude is compared, on a dB-like scale:
    // rel = 1 at the peak, 0 at 24 dB below it.
    {
        const float eb = e[0] + e[1], em = e[2] + e[3];
        const float kk = wa_k(dt, 0.5f);
        float pb, pm;
        s->bass_sm += (eb - s->bass_sm) * kk;
        s->mid_sm  += (em - s->mid_sm)  * kk;
        s->bass_pk *= 1.0f / (1.0f + dt / 30.0f);
        s->mid_pk  *= 1.0f / (1.0f + dt / 30.0f);
        if (s->bass_sm > s->bass_pk) s->bass_pk = s->bass_sm;
        if (s->mid_sm  > s->mid_pk)  s->mid_pk  = s->mid_sm;
        pb = s->bass_pk > 1e-12f ? s->bass_sm / s->bass_pk : 0.0f;
        pm = s->mid_pk  > 1e-12f ? s->mid_sm  / s->mid_pk  : 0.0f;
        // amplitude ratio -> dB (6.0206 x log2) -> 0..1 over 24 dB
        out->bass_rel = wa_clamp01(1.0f + 6.0206f * wa_log2(pb > 1e-9f ? pb : 1e-9f) / 24.0f);
        out->mid_rel  = wa_clamp01(1.0f + 6.0206f * wa_log2(pm > 1e-9f ? pm : 1e-9f) / 24.0f);
    }
    out->flux      = wa_clamp01(flux);
    out->centroid  = wa_clamp01(cent);
    out->beat_hz   = (s->beat_period > 0.0f) ? 1.0f / s->beat_period : 0.0f;
    out->beat_conf = wa_clamp01(s->beat_conf);
    if (s->ac_hz > 0.0f && s->ac_conf >= WA_ODF_MIN_CONF && s->ac_conf * 1.2f >= out->beat_conf * 0.5f) {
        float hz = s->ac_hz;
        // Octave check: at frame rate a beat between two frames alternates
        // early/late, which half-time matches exactly.  When the interval
        // tracker is sure, it decides the octave.
        if (s->beat_period > 0.0f && s->beat_conf >= 0.6f) {
            const float ih = 1.0f / s->beat_period, q = hz / ih;
            if (q > 0.44f && q < 0.56f) hz *= 2.0f;
            else if (q > 1.8f && q < 2.2f) hz *= 0.5f;
        }
        out->beat_hz   = hz;
        out->beat_conf = wa_clamp01(s->ac_conf * 1.4f > out->beat_conf ? s->ac_conf * 1.4f : out->beat_conf);
    }
    out->silence   = wa_clamp01(s->silence);
}

#endif // WAVE_AUDIO_H
