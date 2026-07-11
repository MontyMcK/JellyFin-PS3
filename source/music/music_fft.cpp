// Spectrum bands for the music visualizer — C port of MediaWave's
// mediawave-fft.py analysis loop: Hann window -> FFT -> log-spaced band
// means -> normalize to the frame max -> smooth.  numpy's rfft becomes a
// small iterative radix-2 FFT with precomputed twiddle/bit-reversal tables;
// the 250 ms HTTP poll becomes a per-frame call, so the single smoothing
// constant splits into fast-attack / slow-decay to keep the same feel at
// 60 Hz.

#include <math.h>
#include <string.h>
#include <sys/mutex.h>

#include "music_fft.h"

#define FFT_N     2048            // 43 ms window; 23 Hz bins keep the bass
#define FFT_BITS  11              //   bands from collapsing onto one bin
#define FFT_HALF  (FFT_N / 2)
#define TAP_CAP   4096            // mono tap ring (power of two, >= FFT_N)
#define VIZ_RATE  48000.0f
#define BAND_LO   40.0f           // MediaWave's 40 Hz – 16 kHz span
#define BAND_HI   16000.0f

// ---- decoder tap (guarded by s_mtx) ----
static float        s_tap[TAP_CAP];
static int          s_tap_wr    = 0;
static u64          s_tap_count = 0;   // total samples ever pushed
static sys_mutex_t  s_mtx;
static bool         s_mtx_ok    = false;

// ---- analysis tables (built once) ----
static float s_hann[FFT_N];
static float s_tw_re[FFT_HALF], s_tw_im[FFT_HALF];
static u16   s_brev[FFT_N];
static int   s_band_lo[MUSIC_VIZ_BANDS], s_band_hi[MUSIC_VIZ_BANDS];
static float s_band_ctr[MUSIC_VIZ_BANDS];   // fractional center bin
static bool  s_tables_ok = false;

// ---- smoothing state (UI thread only) ----
static float s_smooth[MUSIC_VIZ_BANDS];
static u64   s_last_count = 0;

static void viz_build_tables(void) {
    if (s_tables_ok) return;
    for (int i = 0; i < FFT_N; i++)
        s_hann[i] = 0.5f - 0.5f * cosf(2.0f * 3.14159265f * (float)i / (FFT_N - 1));
    for (int k = 0; k < FFT_HALF; k++) {
        float ang = -2.0f * 3.14159265f * (float)k / FFT_N;
        s_tw_re[k] = cosf(ang);
        s_tw_im[k] = sinf(ang);
    }
    for (int i = 0; i < FFT_N; i++) {
        u32 r = 0;
        for (int b = 0; b < FFT_BITS; b++)
            if (i & (1u << b)) r |= 1u << (FFT_BITS - 1 - b);
        s_brev[i] = (u16)r;
    }
    // Log-spaced band edges -> FFT bin ranges (bin 0 = DC, skipped).  The
    // geometric-center bin drives narrow (single-bin) bands via fractional
    // interpolation so neighbouring bass bars don't move in lockstep.
    const float binw = VIZ_RATE / FFT_N;
    for (int i = 0; i < MUSIC_VIZ_BANDS; i++) {
        float fl = BAND_LO * powf(BAND_HI / BAND_LO, (float)i       / MUSIC_VIZ_BANDS);
        float fh = BAND_LO * powf(BAND_HI / BAND_LO, (float)(i + 1) / MUSIC_VIZ_BANDS);
        int lo = (int)(fl / binw); if (lo < 1) lo = 1;
        int hi = (int)(fh / binw); if (hi <= lo) hi = lo + 1;
        if (hi > FFT_HALF) hi = FFT_HALF;
        if (lo >= hi) lo = hi - 1;
        s_band_lo[i]  = lo;
        s_band_hi[i]  = hi;
        float ctr = sqrtf(fl * fh) / binw;
        if (ctr < 1.0f)            ctr = 1.0f;
        if (ctr > FFT_HALF - 2.0f) ctr = FFT_HALF - 2.0f;
        s_band_ctr[i] = ctr;
    }
    s_tables_ok = true;
}

void music_viz_reset(void) {
    viz_build_tables();
    if (!s_mtx_ok) {
        sys_mutex_attr_t mattr;
        sysMutexAttrInitialize(mattr);
        sysMutexCreate(&s_mtx, &mattr);
        s_mtx_ok = true;
    }
    sysMutexLock(s_mtx, 0);
    memset(s_tap, 0, sizeof(s_tap));
    s_tap_wr    = 0;
    s_tap_count = 0;
    sysMutexUnlock(s_mtx);
    memset(s_smooth, 0, sizeof(s_smooth));
    s_last_count = 0;
}

void music_viz_push(const float *lr, int n_pairs) {
    if (!s_mtx_ok || n_pairs <= 0) return;
    sysMutexLock(s_mtx, 0);
    for (int i = 0; i < n_pairs; i++) {
        s_tap[s_tap_wr] = 0.5f * (lr[i * 2] + lr[i * 2 + 1]);
        s_tap_wr = (s_tap_wr + 1) & (TAP_CAP - 1);
    }
    s_tap_count += (u64)n_pairs;
    sysMutexUnlock(s_mtx);
}

// In-place iterative radix-2 FFT over re/im[FFT_N].
static void viz_fft(float *re, float *im) {
    for (int len = 2; len <= FFT_N; len <<= 1) {
        int half = len >> 1;
        int step = FFT_N / len;                // twiddle stride
        for (int base = 0; base < FFT_N; base += len) {
            for (int j = 0; j < half; j++) {
                int   k  = j * step;
                float wr = s_tw_re[k], wi = s_tw_im[k];
                int   a  = base + j, b = base + j + half;
                float tr = re[b] * wr - im[b] * wi;
                float ti = re[b] * wi + im[b] * wr;
                re[b] = re[a] - tr;  im[b] = im[a] - ti;
                re[a] += tr;         im[a] += ti;
            }
        }
    }
}

void music_viz_bands(float *out) {
    static float re[FFT_N], im[FFT_N];

    // No new audio since the last frame (paused / stalled / stopped):
    // let every bar fall smoothly instead of freezing mid-dance.
    bool have_audio = false;
    if (s_mtx_ok && s_tables_ok) {
        sysMutexLock(s_mtx, 0);
        if (s_tap_count != s_last_count && s_tap_count >= FFT_N) {
            s_last_count = s_tap_count;
            int start = (s_tap_wr - FFT_N) & (TAP_CAP - 1);
            for (int i = 0; i < FFT_N; i++) {
                float v = s_tap[(start + i) & (TAP_CAP - 1)] * s_hann[i];
                re[s_brev[i]] = v;
                im[s_brev[i]] = 0.0f;
            }
            have_audio = true;
        }
        sysMutexUnlock(s_mtx);
    }

    float raw[MUSIC_VIZ_BANDS];
    if (have_audio) {
        viz_fft(re, im);
        float mx = 0.0f;
        for (int i = 0; i < MUSIC_VIZ_BANDS; i++) {
            if (s_band_hi[i] - s_band_lo[i] == 1) {
                // Narrow band: interpolate at its fractional center bin.
                int   k0 = (int)s_band_ctr[i];
                float fr = s_band_ctr[i] - (float)k0;
                float m0 = sqrtf(re[k0]     * re[k0]     + im[k0]     * im[k0]);
                float m1 = sqrtf(re[k0 + 1] * re[k0 + 1] + im[k0 + 1] * im[k0 + 1]);
                raw[i] = m0 + (m1 - m0) * fr;
            } else {
                float acc = 0.0f;
                for (int k = s_band_lo[i]; k < s_band_hi[i]; k++)
                    acc += sqrtf(re[k] * re[k] + im[k] * im[k]);
                raw[i] = acc / (float)(s_band_hi[i] - s_band_lo[i]);
            }
            if (raw[i] > mx) mx = raw[i];
        }
        // Normalize to the frame max (MediaWave's full-scale dance), with an
        // absolute floor so digital silence doesn't amplify noise into bars.
        if (mx < 0.25f) {
            for (int i = 0; i < MUSIC_VIZ_BANDS; i++) raw[i] = 0.0f;
        } else {
            for (int i = 0; i < MUSIC_VIZ_BANDS; i++) {
                raw[i] /= mx;
                if (raw[i] > 1.0f) raw[i] = 1.0f;
            }
        }
    } else {
        memset(raw, 0, sizeof(raw));
    }

    for (int i = 0; i < MUSIC_VIZ_BANDS; i++) {
        if (raw[i] > s_smooth[i])
            s_smooth[i] += (raw[i] - s_smooth[i]) * 0.55f;   // fast attack
        else
            s_smooth[i] += (raw[i] - s_smooth[i]) * 0.13f;   // slow fall
        out[i] = s_smooth[i];
    }
}
