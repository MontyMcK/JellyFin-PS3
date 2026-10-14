// Host test for source/audio/resample.c: conversion to 48 kHz.
//
//   make -f Makefile.host test_resample && ./test_resample

#include "resample.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>

static int s_checks = 0, s_failed = 0;
#define CHECK(cond) do { s_checks++; if (!(cond)) { s_failed++; \
    printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

static const double PI = 3.14159265358979323846;

typedef std::vector<float> Vec;

// frames of a sine at `hz`, amplitude amp, at `rate`, one per channel with channel c at amp*(c+1)/ch and a different phase
static Vec sine(int rate, double hz, double amp, int frames, int ch = 1) {
    Vec v((size_t)frames * (size_t)ch);
    for (int n = 0; n < frames; n++)
        for (int c = 0; c < ch; c++)
            v[(size_t)n * (size_t)ch + (size_t)c] = (float)(amp * (double)(c + 1) / (double)ch * sin(2 * PI * hz * (double)n / (double)rate + 0.3 * c));
    return v;
}

static Vec convert(Resampler *r, const Vec &in, int ch, int chunk) {
    Vec out;
    std::vector<float> tmp((size_t)resample_max_out(r, chunk) * (size_t)ch);
    for (size_t pos = 0; pos < in.size() / (size_t)ch; pos += (size_t)chunk) {
        const int n = (int)std::min<size_t>((size_t)chunk, in.size() / (size_t)ch - pos);
        const int m = resample_process(r, &in[pos * (size_t)ch], n, tmp.data(), (int)(tmp.size() / (size_t)ch));
        out.insert(out.end(), tmp.begin(), tmp.begin() + (size_t)m * (size_t)ch);
    }
    return out;
}

// What a tone of `hz` at 48 kHz looks like in channel c of out: the amplitude and the error (dB below the tone)
// of a least-squares fit of a sine of that frequency over frames [from, to).
struct Fit { double amp, err_db; };
static Fit fit_tone(const Vec &out, int ch, int c, double hz, int from, int to) {
    double ss = 0, cc = 0, sc = 0, ys = 0, yc = 0, yy = 0;
    const int n = to - from;
    for (int i = from; i < to; i++) {
        const double w = 2 * PI * hz * (double)i / 48000.0;
        const double s = sin(w), co = cos(w), y = out[(size_t)i * (size_t)ch + (size_t)c];
        ss += s * s; cc += co * co; sc += s * co; ys += y * s; yc += y * co; yy += y * y;
    }
    const double det = ss * cc - sc * sc;
    const double a = (ys * cc - yc * sc) / det, b = (yc * ss - ys * sc) / det;
    const double amp = sqrt(a * a + b * b);
    const double resid = yy - (a * ys + b * yc);                        // residual energy of the least-squares fit
    const double rms_err = sqrt(fabs(resid) / n), rms_sig = amp / sqrt(2.0);
    return { amp, 20 * log10(rms_err / rms_sig + 1e-30) };
}

static void basics() {
    printf("- the basics\n");
    CHECK(resample_open(44000, 2) == nullptr);                        // not a rate it knows
    CHECK(resample_open(44100, 0) == nullptr && resample_open(44100, 9) == nullptr);
    CHECK(!resample_rate_supported(0) && !resample_rate_supported(-1) && resample_rate_supported(44100) && resample_rate_supported(48000));
    for (int rate : { 8000, 11025, 12000, 16000, 22050, 24000, 32000, 44100, 48000, 64000, 88200, 96000, 176400, 192000 }) {
        Resampler *r = resample_open(rate, 2);
        CHECK(r != nullptr);
        resample_close(r);
    }
    resample_close(nullptr);                                           // harmless

    // 48 kHz: a copy, bit for bit, whatever the chunking
    Resampler *r = resample_open(48000, 2);
    Vec in = sine(48000, 1000, 0.7, 5000, 2);
    Vec out = convert(r, in, 2, 333);
    CHECK(out.size() == in.size() && memcmp(out.data(), in.data(), in.size() * sizeof(float)) == 0);
    CHECK(resample_max_out(r, 100) == 100);
    resample_close(r);
}

static void tone_quality() {
    printf("- a tone converted\n");
    struct { int rate; double hz; } t[] = {
        { 44100, 1000 }, { 44100, 100 }, { 44100, 12345 }, { 22050, 1000 }, { 32000, 3000 }, { 8000, 440 }, { 11025, 1000 },
        { 12000, 1000 }, { 16000, 5000 }, { 24000, 1000 }, { 64000, 1000 }, { 88200, 10000 }, { 96000, 1000 }, { 96000, 20000 }, { 176400, 1000 }, { 192000, 15000 },
    };
    for (auto &x : t) {
        Resampler *r = resample_open(x.rate, 1);
        const int n_in = x.rate;                                       // one second
        Vec in = sine(x.rate, x.hz, 0.5, n_in);
        Vec out = convert(r, in, 1, 1024);
        resample_close(r);
        const int n_out = (int)out.size();
        // the output is the same signal sampled at 48 kHz: a tone at the same frequency, phase 0, the same amplitude
        Fit f = fit_tone(out, 1, 0, x.hz, 300, n_out - 300);
        if (f.err_db > -90.0 || fabs(f.amp - 0.5) > 0.0005 || n_out < 47300 || n_out > 48002)
            printf("  %d Hz tone at %d Hz: amp %.5f, error %.1f dB, %d frames\n", (int)x.hz, x.rate, f.amp, f.err_db, n_out);
        CHECK(f.err_db < -90.0);                                       // THD+N, the aliasing and the images included
        CHECK(fabs(f.amp - 0.5) < 0.0005);
        CHECK(n_out > 47300 && n_out <= 48002);                        // a second in; the last few milliseconds wait for more input

        // and it is the same sine, not one shifted in time: compare against the ideal sample by sample
        double worst = 0;
        for (int m = 300; m < n_out - 300; m++)
            worst = std::max(worst, fabs((double)out[(size_t)m] - 0.5 * sin(2 * PI * x.hz * (double)m / 48000.0)));
        CHECK(worst < 0.0005);
    }
}

// The count of frames made follows the rate exactly: more input makes exactly the proportional more output, so
// a long file does not drift out of step with its clock.
static void no_drift() {
    printf("- no drift\n");
    for (int rate : { 44100, 22050, 32000, 88200, 96000 }) {
        Resampler *r = resample_open(rate, 1);
        Vec one = sine(rate, 440, 0.5, rate);
        Vec three = sine(rate, 440, 0.5, rate * 3);
        const size_t a = convert(r, one, 1, 1000).size();
        resample_reset(r);
        const size_t b = convert(r, three, 1, 1000).size();
        resample_close(r);
        const long diff = (long)b - (long)a;
        if (labs(diff - 2 * 48000) > 1) printf("  %d Hz: 1 s makes %zu, 3 s make %zu\n", rate, a, b);
        CHECK(labs(diff - 2 * 48000) <= 1);
    }
}

static void flatness() {
    printf("- passband flatness, 44.1 kHz\n");
    double worst_db = 0;
    for (double hz : { 50.0, 500.0, 2000.0, 5000.0, 8000.0, 12000.0, 16000.0, 18000.0, 19000.0, 20000.0 }) {
        Resampler *r = resample_open(44100, 1);
        Vec in = sine(44100, hz, 0.5, 44100);
        Vec out = convert(r, in, 1, 2048);
        resample_close(r);
        Fit f = fit_tone(out, 1, 0, hz, 300, (int)out.size() - 300);
        const double db = 20 * log10(f.amp / 0.5);
        worst_db = std::max(worst_db, fabs(db));
        if (fabs(db) > 0.05) printf("  %.0f Hz: %.3f dB\n", hz, db);
    }
    CHECK(worst_db < 0.05);                                            // up to 20 kHz
}

static void anti_aliasing() {
    printf("- what must not get through\n");
    struct { int rate; double hz, alias_hz; } t[] = {
        { 96000, 30000, 18000 },                                        // 30 kHz folds to 18 kHz at 48 kHz
        { 96000, 40000, 8000 },
        { 192000, 40000, 8000 },
        { 192000, 90000, 6000 },
        { 88200, 30000, 18000 },
        { 64000, 28000, 20000 },
    };
    for (auto &x : t) {
        Resampler *r = resample_open(x.rate, 1);
        Vec in = sine(x.rate, x.hz, 0.5, x.rate);
        Vec out = convert(r, in, 1, 1024);
        resample_close(r);
        Fit f = fit_tone(out, 1, 0, x.alias_hz, 400, (int)out.size() - 400);
        const double db = 20 * log10(f.amp / 0.5 + 1e-30);
        if (db > -80) printf("  %.0f Hz at %d Hz leaves %.1f dB at %.0f Hz\n", x.hz, x.rate, db, x.alias_hz);
        CHECK(db < -80);
    }
    // images: a 44.1 kHz stream with a tone near its Nyquist must not leave a mirror above the output's
    Resampler *r = resample_open(44100, 1);
    Vec in = sine(44100, 21000, 0.5, 44100);
    Vec out = convert(r, in, 1, 1024);
    resample_close(r);
    Fit f = fit_tone(out, 1, 0, 48000.0 - 21000.0, 400, (int)out.size() - 400);          // the image at 27 kHz folds to 21 kHz: the tone itself, in the output
    CHECK(f.amp < 0.5);
}

static void chunking() {
    printf("- chunking, channels, reset\n");
    Vec in = sine(44100, 1234, 0.6, 30000, 2);
    Resampler *r = resample_open(44100, 2);
    Vec whole = convert(r, in, 2, 30000);
    resample_close(r);
    bool same = true;
    for (int chunk : { 1, 2, 7, 100, 1000, 4095, 4096, 4097, 9999 }) {
        r = resample_open(44100, 2);
        Vec got = convert(r, in, 2, chunk);
        resample_close(r);
        if (got.size() != whole.size() || memcmp(got.data(), whole.data(), whole.size() * sizeof(float)) != 0) { same = false; printf("  chunk %d differs\n", chunk); }
    }
    CHECK(same);                                                        // the same output whatever the input is cut into

    // channels do not leak into each other: a tone in channel 0 only
    Vec mono = sine(44100, 1000, 0.5, 20000);
    Vec four((size_t)20000 * 4, 0.0f);
    for (int n = 0; n < 20000; n++) four[(size_t)n * 4 + 2] = mono[(size_t)n];
    r = resample_open(44100, 4);
    Vec o4 = convert(r, four, 4, 1000);
    resample_close(r);
    double other = 0;
    for (size_t i = 0; i < o4.size(); i++) if (i % 4 != 2) other = std::max(other, (double)fabsf(o4[i]));
    CHECK(other == 0.0);
    Fit f2 = fit_tone(o4, 4, 2, 1000, 300, (int)(o4.size() / 4) - 300);
    CHECK(f2.err_db < -90.0);

    // eight channels
    Vec e = sine(32000, 700, 0.8, 16000, 8);
    r = resample_open(32000, 8);
    Vec oe = convert(r, e, 8, 512);
    resample_close(r);
    CHECK(oe.size() / 8 > 23000 && oe.size() / 8 < 24100);

    // reset: starts over, exactly as a new converter
    r = resample_open(44100, 2);
    convert(r, in, 2, 777);
    resample_reset(r);
    Vec again = convert(r, in, 2, 777);
    resample_close(r);
    CHECK(again.size() == whole.size() && memcmp(again.data(), whole.data(), whole.size() * sizeof(float)) == 0);

    // an impulse comes out where the same instant is in the new rate
    r = resample_open(44100, 1);
    Vec imp(20000, 0.0f);
    imp[10000] = 1.0f;
    Vec oi = convert(r, imp, 1, 4096);
    resample_close(r);
    int peak = 0;
    for (size_t i = 0; i < oi.size(); i++) if (fabsf(oi[i]) > fabsf(oi[(size_t)peak])) peak = (int)i;
    CHECK(abs(peak - (int)lround(10000.0 * 48000.0 / 44100.0)) <= 1);

    // output capacity: never written past, and the bound holds
    r = resample_open(44100, 2);
    Vec big = sine(44100, 500, 0.5, 5000, 2);
    for (int cap : { 0, 1, 10, 100 }) {
        std::vector<float> tiny((size_t)(cap + 8) * 2, 777.0f);          // guard frames after the end
        const int m = resample_process(r, big.data(), 5000, tiny.data(), cap);
        bool guard_ok = true;
        for (size_t i = (size_t)cap * 2; i < tiny.size(); i++) if (tiny[i] != 777.0f) guard_ok = false;
        CHECK(m <= cap && guard_ok);
        resample_reset(r);
    }
    for (int n : { 1, 10, 4096, 5000, 20000 }) {
        std::vector<float> o((size_t)resample_max_out(r, n) * 2);
        Vec in2 = sine(44100, 500, 0.5, n, 2);
        const int m = resample_process(r, in2.data(), n, o.data(), (int)(o.size() / 2));
        CHECK(m <= resample_max_out(r, n));
        resample_reset(r);
    }
    resample_close(r);
}

int main() {
    basics();
    tone_quality();
    no_drift();
    flatness();
    anti_aliasing();
    chunking();
    printf("resample: %d checks, %d failed\n", s_checks, s_failed);
    return s_failed ? 1 : 0;
}
