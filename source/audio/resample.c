// Sample-rate conversion to 48 kHz: see resample.h.
//
// An output frame at input time t = i0 + p/L (i0 whole, p in 0..L-1) is a weighted sum of the T input
// frames around it, i0-(T/2-1) .. i0+T/2, with the weights taken from phase p of a table: the
// windowed sinc evaluated at the distance of each of those frames from t.  The next output is M/L
// input frames later.  Each phase's weights are scaled to sum to 1, so a constant stays a constant.

#include "resample.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define OUT_RATE   48000
#define HALF_LEN   60            // the filter reaches HALF_LEN/cut input frames each side of the sample being made
#define KAISER_BETA 9.6          // ~96 dB stopband
#define CUTOFF     0.95          // of the lower Nyquist: the middle of a transition band a tenth of it wide
#define CHUNK      4096          // input frames taken at a time

struct Resampler {
    int  in_rate, ch, L, M, T;
    bool identity;
    float *tab;                  // L phases of T weights
    float *buf;                  // (T + CHUNK) frames of input, interleaved
    int   have;                  // valid frames in buf
    long  base;                  // the input index of buf[0]
    long  i0;                    // the input index the next output is made at
    int   ph;                    // and its phase
};

static const int RATES[] = { 8000, 11025, 12000, 16000, 22050, 24000, 32000, 44100, 48000, 64000, 88200, 96000, 176400, 192000 };

bool resample_rate_supported(int rate) {
    for (size_t i = 0; i < sizeof RATES / sizeof RATES[0]; i++) if (RATES[i] == rate) return true;
    return false;
}

static int gcd(int a, int b) { while (b) { int t = a % b; a = b; b = t; } return a; }

static double bessel_i0(double x) {
    double sum = 1.0, term = 1.0;
    const double q = x * x / 4.0;
    for (int k = 1; k < 60; k++) {
        term *= q / ((double)k * (double)k);
        sum += term;
        if (term < sum * 1e-17) break;
    }
    return sum;
}

static void build_table(Resampler *r) {
    const double cut = CUTOFF * (r->L < r->M ? (double)r->L / (double)r->M : 1.0);
    const double half = r->T / 2.0;
    const double i0b = bessel_i0(KAISER_BETA);
    const double pi = 3.14159265358979323846;
    for (int p = 0; p < r->L; p++) {
        const double frac = (double)p / (double)r->L;
        double sum = 0.0;
        double w[512];                                        // T <= 512 (checked in resample_open)
        for (int j = 0; j < r->T; j++) {
            const double d = (double)(j - (r->T / 2 - 1)) - frac;       // from the output time to this input frame
            const double x = pi * cut * d;
            const double s = fabs(x) < 1e-12 ? 1.0 : sin(x) / x;
            const double u = d / half;
            const double win = fabs(u) >= 1.0 ? 0.0 : bessel_i0(KAISER_BETA * sqrt(1.0 - u * u)) / i0b;
            w[j] = cut * s * win;
            sum += w[j];
        }
        for (int j = 0; j < r->T; j++) r->tab[(size_t)p * (size_t)r->T + (size_t)j] = (float)(w[j] / sum);
    }
}

Resampler *resample_open(int in_rate, int channels) {
    if (channels < 1 || channels > 8 || !resample_rate_supported(in_rate)) return NULL;
    Resampler *r = (Resampler *)calloc(1, sizeof *r);
    if (!r) return NULL;
    r->in_rate = in_rate;
    r->ch = channels;
    if (in_rate == OUT_RATE) { r->identity = true; r->L = r->M = r->T = 1; return r; }
    const int g = gcd(in_rate, OUT_RATE);
    r->L = OUT_RATE / g;
    r->M = in_rate / g;
    const double cut = CUTOFF * (r->L < r->M ? (double)r->L / (double)r->M : 1.0);
    int t = 2 * (int)ceil((double)HALF_LEN / cut);
    if (t > 512) t = 512;
    r->T = t;
    r->tab = (float *)malloc(sizeof(float) * (size_t)r->L * (size_t)r->T);
    r->buf = (float *)malloc(sizeof(float) * (size_t)(r->T + CHUNK + 2) * (size_t)channels);
    if (!r->tab || !r->buf) { resample_close(r); return NULL; }
    build_table(r);
    resample_reset(r);
    return r;
}

void resample_close(Resampler *r) {
    if (!r) return;
    free(r->tab);
    free(r->buf);
    free(r);
}

void resample_reset(Resampler *r) {
    if (r->identity) return;
    // before the first frame there is silence: T/2-1 frames of it, so the first output sits on frame 0
    const int lead = r->T / 2 - 1;
    memset(r->buf, 0, sizeof(float) * (size_t)lead * (size_t)r->ch);
    r->have = lead;
    r->base = -(long)lead;
    r->i0 = 0;
    r->ph = 0;
}

int resample_max_out(const Resampler *r, int n_in) {
    if (r->identity) return n_in;
    // n_in * L / M, plus the frames the history may release, plus rounding
    return (int)(((long)n_in * r->L) / r->M) + r->T + 2;
}

int resample_process(Resampler *r, const float *in, int n_in, float *out, int out_cap) {
    const int ch = r->ch;
    if (r->identity) {
        const int n = n_in < out_cap ? n_in : out_cap;
        memcpy(out, in, sizeof(float) * (size_t)n * (size_t)ch);
        return n;
    }
    int made = 0;
    while (n_in > 0) {
        const int take = n_in < CHUNK ? n_in : CHUNK;
        memcpy(r->buf + (size_t)r->have * (size_t)ch, in, sizeof(float) * (size_t)take * (size_t)ch);
        r->have += take;
        in += (size_t)take * (size_t)ch;
        n_in -= take;

        const int T = r->T;
        // the frames an output needs end at i0 + T/2
        while (made < out_cap && r->i0 + T / 2 - r->base < r->have) {
            const float *g = r->tab + (size_t)r->ph * (size_t)T;
            const float *src = r->buf + (size_t)(r->i0 - (T / 2 - 1) - r->base) * (size_t)ch;
            for (int c = 0; c < ch; c++) {
                float acc = 0.0f;
                for (int j = 0; j < T; j++) acc += g[j] * src[(size_t)j * (size_t)ch + (size_t)c];
                out[(size_t)made * (size_t)ch + (size_t)c] = acc;
            }
            made++;
            r->ph += r->M;
            r->i0 += r->ph / r->L;
            r->ph %= r->L;
        }
        if (made >= out_cap && r->i0 + T / 2 - r->base < r->have) break;   // no room: the rest of the input is dropped
        // keep what the next output still needs
        const long keep_from = r->i0 - (T / 2 - 1) - r->base;
        if (keep_from > 0) {
            const int drop = keep_from < r->have ? (int)keep_from : r->have;
            memmove(r->buf, r->buf + (size_t)drop * (size_t)ch, sizeof(float) * (size_t)(r->have - drop) * (size_t)ch);
            r->have -= drop;
            r->base += drop;
        }
    }
    return made;
}
