// JellyWave 2.0: the scope -- stereo response and a waveform snapshot.
//
// wave_audio.h already gives the per-band levels, loudness, onsets and the
// beat.  What it cannot give, because it folds the channels to mono before
// anything else, is WHERE the sound is and what the waveform itself looks
// like.  This file adds exactly those two signals, off the same tap:
//
//   balance   -1 (left) .. +1 (right): the energy balance between channels,
//             attack/release smoothed so a panned instrument leans the wave
//             instead of twitching it.
//   width     0 (mono) .. 1 (wide): side energy over total.
//   wave[]    WSC_POINTS points of the recent waveform (a box-filtered,
//             decimated oscilloscope trace), DC-removed, level-normalised by
//             its own running peak (so a loud master and a quiet one draw
//             the same size), tapered to zero at both ends (so the band's
//             ends never jump) and smoothed per point in time -- a shimmer,
//             not static.
//
// The technique is the ordinary one from the visualiser projects referenced
// for this work (oscilloscope-style waveform, L/R energy for stereo, every
// value self-normalised against its own history, attack/release on
// everything): no copied code or constants.  No FFT here -- the band levels
// come from wave_audio.h's filter bank, which costs a few multiply-adds per
// sample instead of a transform per frame.
//
// COST.  wsc_push: ~10 flops per stereo frame, no branches in the hot path
// besides the decimator.  wsc_frame: WSC_POINTS * 8 adds.  Memory: ~1.3 KB.
//
// House rules: header-only, pure C, no libm, no PS3 headers, caller-owned
// state.  tests/test_wave_scope.c.

#ifndef WAVE_SCOPE_H
#define WAVE_SCOPE_H

#define WSC_RING     512        // decimated mono samples kept (~43 ms at 12 kHz)
#define WSC_SPAN     256        // aperiodic view: the newest ~21 ms

// PITCH-SYNCHRONOUS VIEW (2026-09-25: "the waveform should hold still").
// Like an oscilloscope's trigger: the period is estimated by a normalised
// autocorrelation over the newest WSC_AC_N samples (lags for ~67 Hz ..
// 1 kHz); when the signal is clearly periodic the view is exactly two periods
// starting at a rising zero crossing, so a held note draws the SAME shape
// every frame.  Otherwise (noise, dense chords) it falls back to the newest
// WSC_SPAN samples as before.  ~48k flops a frame.
#define WSC_LAG_MIN  12
#define WSC_LAG_MAX  180
#define WSC_AC_N     96
#define WSC_PERIODIC 0.55f

// KEY (2026-09-26, "snap on key changes").  Each periodic frame's pitch is
// folded to a pitch class and accumulated in a slowly decaying histogram
// (~6 s); the strongest class is the tonal centre.  A different class that
// has led for WSC_KEY_HOLD and by WSC_KEY_MARGIN is a key change: one event.
// Approximate on purpose -- it only has to notice that the harmony MOVED.
#define WSC_KEY_TAU    6.0f
#define WSC_KEY_HOLD   2.5f
#define WSC_KEY_MARGIN 1.25f

// BASS GLIDE ("808 glide bend").  A low-passed copy of the 12 kHz trace,
// decimated to 1.5 kHz, autocorrelated over 37..125 Hz: when the bass is
// clearly pitched its pitch is tracked, and the rate it slides at (octaves a
// second, smoothed) is handed on.  An octave jump is ignored, not a glide.
#define WSC_BASS_RING  256
#define WSC_BASS_DEC   8
#define WSC_BLAG_MIN   12
#define WSC_BLAG_MAX   40
#define WSC_BASS_N     96
#define WSC_BASS_PER   0.60f
#define WSC_DECIM    4          // 48 kHz -> 12 kHz
#define WSC_POINTS   32         // waveform points handed to the renderer

#define WSC_TAU_BAL_A   0.12f   // balance: lean in
#define WSC_TAU_BAL_R   0.45f   //          and back out, slower
#define WSC_TAU_WIDTH   0.40f
#define WSC_TAU_PEAK_UP 0.05f   // waveform normaliser
#define WSC_TAU_PEAK_DN 1.50f
#define WSC_PEAK_FLOOR  0.004f
#define WSC_TAU_POINT   0.045f  // per-point temporal smoothing

typedef struct {
    // written by wsc_push (audio thread, under the caller's lock)
    float l2, r2, m2, s2;       // energy sums since the last frame
    int   frames;
    float dacc;                 // decimator accumulator
    int   dn;
    float ring[WSC_RING];
    int   wr;
    float blp;                  // bass low-pass on the 12 kHz trace
    int   bdn;
    float bring[WSC_BASS_RING];
    int   bwr;
    // written by wsc_frame (UI thread, same lock)
    float bal, width, peak;
    float wave[WSC_POINTS];
    float lin[WSC_RING];        // scratch: the ring, oldest first
    int   period;               // last periodic lag, 0 = aperiodic
    float pch[12];              // pitch-class histogram
    int   key, cand;            // current key (-1 none), candidate
    float cand_t;
    float bpitch, bglide;       // bass pitch (log2 Hz), glide (oct/s)
    int   bvalid;
} wsc_state;

typedef struct {
    float balance;              // -1..1
    float width;                // 0..1
    float wave[WSC_POINTS];     // -1..1, tapered
    int   key;                  // 0..11 (C..B) tonal centre, -1 unknown
    int   key_changed;          // 1 on the frame the key changes
    float bass_glide;           // octaves / s the bass is sliding (+ up)
} wsc_out;

// log2 without libm (see wave_audio.h's wa_log2)
static inline float wsc_log2(float x)
{
    union { float f; unsigned int u; } v;
    int e;
    float m;
    if (!(x > 1e-12f)) return -40.0f;
    v.f = x;
    e = (int)((v.u >> 23) & 0xFF) - 128;
    v.u = (v.u & 0x007FFFFFu) | 0x3F800000u;
    m = v.f;
    return (float)e + (-0.34484843f * m + 2.02466578f) * m - 0.67487759f;
}

static inline float wsc_clamp(float v, float lo, float hi)
{
    if (v != v) return lo;
    return v < lo ? lo : (v > hi ? hi : v);
}

static inline float wsc_k(float dt, float tau) { return dt / (tau + dt); }

static inline void wsc_init(wsc_state *s)
{
    int i;
    if (!s) return;
    s->l2 = s->r2 = s->m2 = s->s2 = 0.0f;
    s->frames = 0; s->dacc = 0.0f; s->dn = 0; s->wr = 0;
    s->bal = 0.0f; s->width = 0.0f; s->peak = WSC_PEAK_FLOOR;
    s->blp = 0.0f; s->bdn = 0; s->bwr = 0;
    for (i = 0; i < WSC_BASS_RING; i++) s->bring[i] = 0.0f;
    for (i = 0; i < 12; i++) s->pch[i] = 0.0f;
    s->key = -1; s->cand = -1; s->cand_t = 0.0f;
    s->bpitch = 0.0f; s->bglide = 0.0f; s->bvalid = 0;
    for (i = 0; i < WSC_RING; i++) s->ring[i] = 0.0f;
    for (i = 0; i < WSC_POINTS; i++) s->wave[i] = 0.0f;
}

// lr: interleaved stereo float pairs.
static inline void wsc_push(wsc_state *s, const float *lr, int n)
{
    int i;
    if (!s || !lr || n <= 0) return;
    for (i = 0; i < n; i++) {
        float l = lr[2 * i], r = lr[2 * i + 1];
        if (l != l) l = 0.0f;
        if (r != r) r = 0.0f;
        l = wsc_clamp(l, -4.0f, 4.0f);
        r = wsc_clamp(r, -4.0f, 4.0f);
        {
            const float m = 0.5f * (l + r), sd = 0.5f * (l - r);
            s->l2 += l * l; s->r2 += r * r;
            s->m2 += m * m; s->s2 += sd * sd;
            s->dacc += m;
        }
        if (++s->dn >= WSC_DECIM) {
            s->ring[s->wr] = s->dacc * (1.0f / (float)WSC_DECIM);
            // the bass trace: ~200 Hz low-pass, then 1.5 kHz
            s->blp += (s->ring[s->wr] - s->blp) * 0.10f;
            if (++s->bdn >= WSC_BASS_DEC) {
                s->bdn = 0;
                s->bring[s->bwr] = s->blp;
                s->bwr = (s->bwr + 1) & (WSC_BASS_RING - 1);
            }
            s->wr = (s->wr + 1) & (WSC_RING - 1);
            s->dacc = 0.0f; s->dn = 0;
        }
    }
    s->frames += n;
}

// Once per UI frame.  present: 0 at rest .. 1 with audio (wave_audio.h's
// silence, inverted); at 0 everything eases back to neutral.
static inline void wsc_frame(wsc_state *s, float dt, float present, wsc_out *o)
{
    int i, j;
    float bal_raw = 0.0f, wid_raw = 0.0f, mean = 0.0f, pk = 0.0f;
    float pts[WSC_POINTS];
    if (!s || !o) return;
    if (!(dt > 0.0f)) dt = 0.0f;
    present = wsc_clamp(present, 0.0f, 1.0f);

    if (s->frames > 0) {
        const float lr = s->l2 + s->r2, ms = s->m2 + s->s2;
        bal_raw = lr > 1e-9f ? (s->r2 - s->l2) / lr : 0.0f;
        wid_raw = ms > 1e-9f ? s->s2 / ms : 0.0f;
    }
    s->l2 = s->r2 = s->m2 = s->s2 = 0.0f;
    s->frames = 0;
    bal_raw *= present; wid_raw *= present;
    {
        const float tau = (bal_raw * bal_raw > s->bal * s->bal) ? WSC_TAU_BAL_A : WSC_TAU_BAL_R;
        s->bal   += (bal_raw - s->bal) * wsc_k(dt, tau);
        s->width += (wid_raw - s->width) * wsc_k(dt, WSC_TAU_WIDTH);
    }

    {
        int k, rd = s->wr, lag = 0, start = 0, span = WSC_SPAN;
        float *x = s->lin;
        for (k = 0; k < WSC_RING; k++) { x[k] = s->ring[rd]; rd = (rd + 1) & (WSC_RING - 1); }

        // the period: the smallest lag within 90% of the best correlation
        {
            const int end = WSC_RING - 1;
            float best = 0.0f, r[WSC_LAG_MAX + 1];
            int L;
            for (L = WSC_LAG_MIN; L <= WSC_LAG_MAX; L++) {
                float num = 0.0f, e0 = 0.0f, e1 = 0.0f;
                int n;
                for (n = end - WSC_AC_N + 1; n <= end; n++) {
                    const float a = x[n], b = x[n - L];
                    num += a * b; e0 += a * a; e1 += b * b;
                }
                r[L] = (e0 + e1) > 1e-12f ? 2.0f * num / (e0 + e1) : 0.0f;   // <= 1
                if (r[L] > best) best = r[L];
            }
            if (best >= WSC_PERIODIC) {
                for (L = WSC_LAG_MIN; L <= WSC_LAG_MAX; L++)
                    if (r[L] >= 0.9f * best) {
                        // walk to the local peak of this lobe
                        while (L < WSC_LAG_MAX && r[L + 1] > r[L]) L++;
                        lag = L;
                        break;
                    }
            }
        }
        if (lag) {
            // two periods ending as late as possible, starting on a rising
            // zero crossing (DC removed over the window first)
            float m = 0.0f;
            int t, t0;
            span = 2 * lag;
            t0 = WSC_RING - span;                  // latest possible start
            for (k = t0 - lag; k < WSC_RING; k++) m += x[k];
            m *= 1.0f / (float)(WSC_RING - (t0 - lag));
            start = t0;
            for (t = t0; t > t0 - lag && t > 0; t--)
                if (x[t - 1] - m < 0.0f && x[t] - m >= 0.0f) { start = t; break; }
        } else {
            start = WSC_RING - WSC_SPAN;
        }
        s->period = lag;
        // --- the key: fold this frame's pitch into the histogram ------------
        {
            const float kd = 1.0f / (1.0f + dt / WSC_KEY_TAU);
            int best = 0;
            for (k = 0; k < 12; k++) s->pch[k] *= kd;
            if (lag && present > 0.0f) {
                const float semis = 12.0f * wsc_log2((12000.0f / (float)lag) / 261.63f);
                int pc = (int)(semis + (semis >= 0.0f ? 0.5f : -0.5f)) % 12;
                if (pc < 0) pc += 12;
                s->pch[pc] += dt;
            }
            for (k = 1; k < 12; k++) if (s->pch[k] > s->pch[best]) best = k;
            o->key_changed = 0;
            if (s->pch[best] > 0.5f && best != s->key) {
                if (best == s->cand) s->cand_t += dt; else { s->cand = best; s->cand_t = 0.0f; }
                if (s->cand_t >= WSC_KEY_HOLD &&
                    (s->key < 0 || s->pch[best] > WSC_KEY_MARGIN * s->pch[s->key])) {
                    if (s->key >= 0) o->key_changed = 1;
                    s->key = best; s->cand_t = 0.0f;
                }
            } else {
                s->cand = -1; s->cand_t = 0.0f;
            }
            o->key = s->key;
        }
        // --- the bass glide --------------------------------------------------
        {
            float bx[WSC_BASS_RING], br[WSC_BLAG_MAX + 2], bbest = 0.0f, e0 = 0.0f;
            int bl = 0, L, n, rdb = s->bwr;
            for (k = 0; k < WSC_BASS_RING; k++) { bx[k] = s->bring[rdb]; rdb = (rdb + 1) & (WSC_BASS_RING - 1); }
            for (n = WSC_BASS_RING - WSC_BASS_N; n < WSC_BASS_RING; n++) e0 += bx[n] * bx[n];
            for (L = WSC_BLAG_MIN - 1; L <= WSC_BLAG_MAX + 1; L++) {
                float num = 0.0f, e1 = 0.0f;
                for (n = WSC_BASS_RING - WSC_BASS_N; n < WSC_BASS_RING; n++) {
                    num += bx[n] * bx[n - L]; e1 += bx[n - L] * bx[n - L];
                }
                br[L] = (e0 + e1) > 1e-10f ? 2.0f * num / (e0 + e1) : 0.0f;
                if (L >= WSC_BLAG_MIN && L <= WSC_BLAG_MAX && br[L] > bbest) { bbest = br[L]; bl = L; }
            }
            if (bl && bbest >= WSC_BASS_PER && present > 0.0f && e0 > 1e-6f) {
                const float y0 = br[bl - 1], y1 = br[bl], y2 = br[bl + 1];
                const float den = y0 - 2.0f * y1 + y2;
                float off = den < -1e-6f ? 0.5f * (y0 - y2) / den : 0.0f;
                if (off > 0.5f) off = 0.5f;
                if (off < -0.5f) off = -0.5f;
                {
                    const float lp = wsc_log2(1500.0f / ((float)bl + off));
                    if (s->bvalid && dt > 0.0f) {
                        float d = lp - s->bpitch;
                        if (d > 0.5f || d < -0.5f) d = 0.0f;          // an octave jump, not a slide
                        s->bglide += (d / dt - s->bglide) * wsc_k(dt, 0.08f);
                    }
                    s->bpitch = lp; s->bvalid = 1;
                }
            } else {
                s->bvalid = 0;
                s->bglide *= 1.0f / (1.0f + dt / 0.20f);
            }
            o->bass_glide = wsc_clamp(s->bglide, -4.0f, 4.0f);
        }
        // resample [start, start + span) into the points, box-averaged
        for (i = 0; i < WSC_POINTS; i++) {
            const float f0 = (float)start + (float)span * (float)i / (float)WSC_POINTS;
            const float f1 = (float)start + (float)span * (float)(i + 1) / (float)WSC_POINTS;
            int a0 = (int)f0, a1 = (int)f1;
            float acc = 0.0f;
            if (a1 <= a0) a1 = a0 + 1;
            if (a1 > WSC_RING) a1 = WSC_RING;
            for (j = a0; j < a1; j++) acc += x[j];
            pts[i] = acc / (float)(a1 - a0);
            mean += pts[i];
        }
        mean *= 1.0f / (float)WSC_POINTS;
        for (i = 0; i < WSC_POINTS; i++) {
            const float a = pts[i] - mean;
            pts[i] = a;
            if (a > pk) pk = a; else if (-a > pk) pk = -a;
        }
    }
    s->peak += (pk - s->peak) * wsc_k(dt, pk > s->peak ? WSC_TAU_PEAK_UP : WSC_TAU_PEAK_DN);
    if (!(s->peak > WSC_PEAK_FLOOR)) s->peak = WSC_PEAK_FLOOR;
    {
        const float inv = 1.0f / s->peak, kp = wsc_k(dt, WSC_TAU_POINT);
        for (i = 0; i < WSC_POINTS; i++) {
            const float u = (float)i * (1.0f / (float)(WSC_POINTS - 1));
            const float taper = 4.0f * u * (1.0f - u);
            const float v = wsc_clamp(pts[i] * inv, -1.0f, 1.0f) * taper * present;
            s->wave[i] += (v - s->wave[i]) * kp;
            o->wave[i] = s->wave[i];
        }
    }
    o->balance = wsc_clamp(s->bal, -1.0f, 1.0f);
    o->width   = wsc_clamp(s->width, 0.0f, 1.0f);
}

#endif // WAVE_SCOPE_H
