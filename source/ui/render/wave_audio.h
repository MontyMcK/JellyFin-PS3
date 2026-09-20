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
#define WA_TAU_FAST_A   0.025f
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
#define WA_TAU_SIL_OUT  1.8f       // blooming back up; slower on purpose

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
    float flux_avg;
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
        s->ref[i] = wa_env(s->ref[i], e[i], dt, WA_TAU_REF_UP, WA_TAU_REF_DN);
        if (!(s->ref[i] > WA_REF_FLOOR)) s->ref[i] = WA_REF_FLOOR;
        lvl = wa_compress(e[i], s->ref[i]);
        s->fast[i] = wa_env(s->fast[i], lvl, dt, WA_TAU_FAST_A, WA_TAU_FAST_R);
        s->med[i]  = wa_env(s->med[i],  lvl, dt, WA_TAU_MED_A,  WA_TAU_MED_R);
        s->slow[i] = wa_env(s->slow[i], lvl, dt, WA_TAU_SLOW_A, WA_TAU_SLOW_R);
    }
    s->rms_ref = wa_env(s->rms_ref, rms_amp, dt, WA_TAU_REF_UP, WA_TAU_REF_DN);
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
    thresh = s->flux_avg * WA_FLUX_RATIO + WA_FLUX_FLOOR;
    fired  = (flux > thresh) && (s->since_onset >= WA_REFRACTORY);

    // The running mean is updated AFTER the comparison, so a single loud
    // onset cannot raise the bar it is being judged against in the same
    // frame.  Symmetric tau: this is a threshold reference, not a musical
    // envelope, and an asymmetric one would ratchet upward on dense material
    // until nothing could clear it.
    s->flux_avg += (flux - s->flux_avg) * wa_k(dt, WA_TAU_FLUX);

    out->onset = 0.0f;
    out->onset_strength = 0.0f;
    if (fired) {
        float excess = (flux - thresh) * (1.0f / WA_FLUX_SCALE);
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
    out->flux      = wa_clamp01(flux);
    out->centroid  = wa_clamp01(cent);
    out->beat_hz   = (s->beat_period > 0.0f) ? 1.0f / s->beat_period : 0.0f;
    out->beat_conf = wa_clamp01(s->beat_conf);
    out->silence   = wa_clamp01(s->silence);
}

#endif // WAVE_AUDIO_H
