// sv_spectrum.h -- the PS3 music visualizer's spectrum analyser, as a spec.
//
// WHERE THIS COMES FROM
//
// The stock XMB music player's "Canyon" visualizer is a scrolling spectrogram
// (see canyon.h).  Its analysis is NOT in the renderer: it lives in
// dev_flash/vsh/module/soundvisualizer_plugin.sprx (fw 4.93), which holds a
// ring of timestamped PCM packets pushed by the audio player and hands the
// renderer a table of callbacks.  Callback 2 (code 0x14b4) returns a spectrum
// computed by sub_1c9c.  Every constant below was read out of that function
// and its helpers; nothing here is Sony code, only the maths it performs:
//
//   window    sub_1aec(buf, 256): 513 taps, x = (i-256) * 2.355 / 256,
//             w = 2.5 * 2.7^(-x^2 / 0.56) / sqrt(6.36159), then the edge value
//             w[0] is subtracted from every tap so the window ends at zero.
//             (A Gaussian, sigma ~58 samples: deliberately smeary -- it is a
//             landscape generator, not a tuner.)
//   FFT       sub_19ac: complex radix-2, order 9 (512 points, `li r0,9` at
//             0x238c), scaled by 1/N afterwards.  L and R separately.
//   magnitude sqrt(re^2 + im^2) for bins 0..255.
//   smooth    sub_1aec(k, 8): the same Gaussian shape over 17 taps, applied
//             across neighbouring BINS, normalised by the kernel's sum.
//   remap     output band i (0..255) reads bin position
//                p = 255 * (2^((i + 1/64) / 64) - 1) / (2^3.98462 - 1)
//             with linear interpolation -- 2^3.98462 is exactly
//             2^((255 + 1/64) / 64), so band 255 lands on bin 255.
//   level     v = 3 * ln(1 + 512 * m) / ln(513)  -> 0..3.
//
// It assumes 48 kHz (the plugin converts sample counts with n * 1e6 / 48000)
// and samples in [-1, 1].  A full-scale sine reads about 2.
//
// House rules: header-only, pure C, deterministic, caller-owned state, no PS3
// headers.  libm is used (expf/powf/logf/sqrtf) -- this runs once per frame on
// 512 samples, not per vertex.

#ifndef SV_SPECTRUM_H
#define SV_SPECTRUM_H

#include <math.h>
#include <stdint.h>
#include <string.h>

#define SV_N      512          // FFT points (order 9)
#define SV_BINS   256          // magnitude bins kept == output bands
#define SV_KHALF  8            // smoothing kernel half-width (17 taps)

typedef struct {
    float win[SV_N + 1];            // 513 taps; the FFT uses the first 512
    float kern[2 * SV_KHALF + 1];
    float ksum;
    float tw_re[SV_N / 2], tw_im[SV_N / 2];
    uint16_t brev[SV_N];
    int   remap_i[SV_BINS];         // floor(p)
    float remap_f[SV_BINS];         // p - floor(p)
    int   ready;
} sv_spec_t;

// Gaussian taps exactly as sub_1aec builds them.  Returns the sum after the
// edge subtraction (the plugin keeps it; the kernel is divided by it).
static inline float sv__gauss(float *out, int n) {
    for (int i = 0; i <= 2 * n; i++) {
        float x = (float)(i - n) * 2.355f / (float)n;
        out[i] = 2.5f * powf(2.7f, -(x * x) / 0.56f) / sqrtf(6.36159f);
    }
    float edge = out[0], sum = 0.0f;
    for (int i = 0; i <= 2 * n; i++) { out[i] -= edge; sum += out[i]; }
    return sum;
}

static inline void sv_spec_init(sv_spec_t *s) {
    memset(s, 0, sizeof(*s));
    sv__gauss(s->win, SV_N / 2);
    s->ksum = sv__gauss(s->kern, SV_KHALF);
    for (int k = 0; k < SV_N / 2; k++) {
        float a = -2.0f * 3.14159265358979f * (float)k / (float)SV_N;
        s->tw_re[k] = cosf(a);
        s->tw_im[k] = sinf(a);
    }
    for (int i = 0; i < SV_N; i++) {
        unsigned r = 0;
        for (int b = 0; b < 9; b++) if (i & (1 << b)) r |= 1u << (8 - b);
        s->brev[i] = (uint16_t)r;
    }
    const float top = powf(2.0f, 3.98462f) - 1.0f;
    for (int i = 0; i < SV_BINS; i++) {
        float e = ((float)i + 0.015625f) * 0.015625f;
        float p = 255.0f * (powf(2.0f, e) - 1.0f) / top;
        if (p < 0.0f) p = 0.0f;
        if (p > 255.0f) p = 255.0f;
        int ip = (int)p;
        s->remap_i[i] = ip;
        s->remap_f[i] = p - (float)ip;
    }
    s->ready = 1;
}

static inline void sv__fft(const sv_spec_t *s, float *re, float *im) {
    for (int len = 2; len <= SV_N; len <<= 1) {
        int half = len >> 1, step = SV_N / len;
        for (int base = 0; base < SV_N; base += len)
            for (int j = 0; j < half; j++) {
                float wr = s->tw_re[j * step], wi = s->tw_im[j * step];
                int a = base + j, b = a + half;
                float tr = re[b] * wr - im[b] * wi;
                float ti = re[b] * wi + im[b] * wr;
                re[b] = re[a] - tr;  im[b] = im[a] - ti;
                re[a] += tr;         im[a] += ti;
            }
    }
}

// One channel: samples[i * stride] for i in 0..511 -> out[256] in 0..3.
static inline void sv__channel(const sv_spec_t *s, const float *samples, int stride,
                               float *out) {
    float re[SV_N], im[SV_N], mag[SV_BINS], sm[SV_BINS];
    for (int i = 0; i < SV_N; i++) {
        re[s->brev[i]] = samples[i * stride] * s->win[i];
        im[s->brev[i]] = 0.0f;
    }
    sv__fft(s, re, im);
    const float inv_n = 1.0f / (float)SV_N;
    for (int k = 0; k < SV_BINS; k++) {
        float r = re[k] * inv_n, m = im[k] * inv_n;
        mag[k] = sqrtf(r * r + m * m);
    }
    // Scatter form, as the plugin writes it: each bin spreads itself over its
    // 17 neighbours; taps that fall off either end are dropped.
    memset(sm, 0, sizeof(sm));
    for (int k = 0; k < SV_BINS; k++) {
        float v = mag[k] / s->ksum;
        for (int t = 0; t <= 2 * SV_KHALF; t++) {
            int d = k - SV_KHALF + t;
            if (d >= 0 && d < SV_BINS) sm[d] += v * s->kern[t];
        }
    }
    const float l513 = logf(513.0f);
    for (int i = 0; i < SV_BINS; i++) {
        int   ip = s->remap_i[i];
        float f  = s->remap_f[i];
        float v  = sm[ip];
        if (f > 0.0f && ip + 1 < SV_BINS) v += (sm[ip + 1] - v) * f;
        out[i] = 3.0f * logf(1.0f + 512.0f * v) / l513;
    }
}

// lr = 512 interleaved stereo pairs.  outL/outR = 256 bands each, 0..3.
static inline void sv_spec_run(const sv_spec_t *s, const float *lr,
                               float *outL, float *outR) {
    sv__channel(s, lr,     2, outL);
    sv__channel(s, lr + 1, 2, outR);
}

#endif
