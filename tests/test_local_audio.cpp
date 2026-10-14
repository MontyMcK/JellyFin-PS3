// Host test for source/local/local_audio.c: a music file decoded to 48 kHz stereo floats.
//
// Every fixture (tests/fixtures/music, see make.sh) carries the same two chirps, so a decode is compared with
// the formula, not with another decoder:
//   left  = 0.5 sin(2 pi (300 t + 150 t^2))      right = 0.5 sin(2 pi (500 t + 100 t^2))
// The frequency rises with time, so a sample that comes from the wrong moment is a wrong sound.  A 48 kHz 16-bit
// file has to match to half a quantisation step; resampled and lossy ones to a signal-to-error ratio; and where
// the file does not say how to align itself (an MP3 without a LAME header) the test measures the offset rather
// than assume it.  WAVE files are built here, one for every sample format.  Damaged and failing files run under
// the address and undefined-behaviour sanitizers.
//
//   make -f Makefile.host test_local_audio && ./test_local_audio

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>
#include <vector>

#include "local_audio.h"

typedef std::vector<uint8_t> Bytes;
typedef std::vector<float> Floats;

static int s_checks = 0, s_failed = 0;
#define CHECK(cond) do { s_checks++; if (!(cond)) { s_failed++; \
    printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

static const double PI = 3.14159265358979323846;
static double chirp_l(double t) { return 0.5 * sin(2 * PI * (300 * t + 150 * t * t)); }
static double chirp_r(double t) { return 0.5 * sin(2 * PI * (500 * t + 100 * t * t)); }

// ---------------------------------------------------------------------------
//  A file in memory
// ---------------------------------------------------------------------------

struct Mem {
    const Bytes *b;
    int max_chunk;            // a read returns at most this many bytes (0 = as asked)
    int64_t fail_after;       // reads at or past this offset fail (-1 = never)
    bool overread;
    long reads;
};

static int mem_read(void *c, uint64_t off, uint8_t *buf, int len) {
    Mem *m = (Mem *)c;
    m->reads++;
    if (len < 0 || off > m->b->size() || (uint64_t)len > m->b->size() - off) m->overread = true;
    if (m->fail_after >= 0 && off >= (uint64_t)m->fail_after) return -2;
    if (off >= m->b->size()) return 0;
    uint64_t n = m->b->size() - off;
    if (n > (uint64_t)len) n = (uint64_t)len;
    if (m->max_chunk > 0 && n > (uint64_t)m->max_chunk) n = (uint64_t)m->max_chunk;
    if (m->fail_after >= 0 && off + n > (uint64_t)m->fail_after) n = (uint64_t)m->fail_after - off;
    memcpy(buf, m->b->data() + off, (size_t)n);
    return (int)n;
}

static bool load(const char *name, Bytes *out) {
    std::string p = std::string("fixtures/music/") + name;
    FILE *f = fopen(p.c_str(), "rb");
    if (!f) { printf("cannot open %s (run from tests/)\n", p.c_str()); return false; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    out->resize((size_t)n);
    const size_t got = fread(out->data(), 1, (size_t)n, f);
    fclose(f);
    return got == (size_t)n;
}

// An opened file: the decoder and what it reads from.  (Keeps the Mem alive as long as the decoder.)
struct Opened {
    Mem mem;
    LaMeta meta;
    LaDecoder *dec;
    Opened() : dec(NULL) { memset(&meta, 0, sizeof meta); }
    ~Opened() { la_close(dec); }
};

static bool open_file(Opened *o, const Bytes &b, LaKind kind, int chunk = 0, int64_t fail_after = -1) {
    o->mem = { &b, chunk, fail_after, false, 0 };
    if (!la_read_meta(mem_read, &o->mem, b.size(), kind, &o->meta)) return false;
    o->dec = la_open(mem_read, &o->mem, b.size(), &o->meta);
    return o->dec != NULL;
}

// Everything the decoder makes, asking for `chunk` pairs at a time (0 = whatever fits).  Returns the status of the
// last call: 0 at the end of the file, < 0 for a read error.
static int decode_all(LaDecoder *d, Floats *out, int chunk, size_t cap_pairs = 4000000) {
    out->clear();
    Floats buf((size_t)(chunk > 0 ? chunk : 4096) * 2);
    int n;
    while ((n = la_decode(d, buf.data(), chunk > 0 ? chunk : 4096)) > 0) {
        out->insert(out->end(), buf.begin(), buf.begin() + (long)n * 2);
        if (out->size() / 2 > cap_pairs) { CHECK(false); return 1; }
    }
    return n;
}

// ---------------------------------------------------------------------------
//  Measuring
// ---------------------------------------------------------------------------

// Signal-to-error in dB of the pairs from index `from` for `n` of them against the formula, the output's pair
// `from` being the formula at time t0 + from / 48000.
static double snr_db(const Floats &o, size_t from, size_t n, double t0, double *worst = NULL, bool mono_left_only = false) {
    double sig = 0, err = 0, w = 0;
    for (size_t k = from; k < from + n && k * 2 + 1 < o.size(); k++) {
        const double t = t0 + (double)(k - from) / 48000.0;
        const double l = chirp_l(t), r = mono_left_only ? chirp_l(t) : chirp_r(t);
        const double el = o[k * 2] - l, er = o[k * 2 + 1] - r;
        sig += l * l + r * r;
        err += el * el + er * er;
        if (fabs(el) > w) w = fabs(el);
        if (fabs(er) > w) w = fabs(er);
    }
    if (worst) *worst = w;
    return err <= 0 ? 300.0 : 10.0 * log10(sig / err);
}

// The lag (in output pairs, searched in [lo, hi]) at which the output best matches the formula, and its snr.
static int best_lag(const Floats &o, size_t from, size_t n, int lo, int hi, double *snr, bool mono_left_only = false) {
    int best = lo;
    double best_snr = -1000;
    for (int lag = lo; lag <= hi; lag++) {
        // output pair k holds the formula at (k - lag) / 48000
        const double s = snr_db(o, from, n, (double)((long)from - lag) / 48000.0, NULL, mono_left_only);
        if (s > best_snr) { best_snr = s; best = lag; }
    }
    if (snr) *snr = best_snr;
    return best;
}

// The time (s) at which a decode that was meant to start at `around` really starts: the offset of the formula
// that fits its first pairs best.
static double start_time(const Floats &o, double around, double span) {
    double best_t = around, best_snr = -1000;
    for (double t0 = around - span; t0 <= around + span; t0 += 0.0005) {
        const double s = snr_db(o, 300, 2048, t0 + 300.0 / 48000.0);
        if (s > best_snr) { best_snr = s; best_t = t0; }
    }
    return best_t;
}

// ---------------------------------------------------------------------------
//  WAVE files built here
// ---------------------------------------------------------------------------

static void le16(Bytes &b, uint32_t v) { b.push_back((uint8_t)v); b.push_back((uint8_t)(v >> 8)); }
static void le32(Bytes &b, uint32_t v) { le16(b, v & 0xFFFF); le16(b, v >> 16); }
static void put(Bytes &b, const char *s) { while (*s) b.push_back((uint8_t)*s++); }

// A WAVE of `frames` frames; value(frame, channel) in [-1, 1] is stored in the format asked for.
template <typename F>
static Bytes make_wav(int format, int ch, int rate, int bits, int frames, F value) {
    Bytes data;
    for (int i = 0; i < frames; i++)
        for (int c = 0; c < ch; c++) {
            const double v = value(i, c);
            if (format == 3 && bits == 32) { float f = (float)v; uint8_t t[4]; memcpy(t, &f, 4); data.insert(data.end(), t, t + 4); }
            else if (format == 3) { double f = v; uint8_t t[8]; memcpy(t, &f, 8); data.insert(data.end(), t, t + 8); }
            else if (bits == 8) data.push_back((uint8_t)lrint(v * 127.0 + 128.0));
            else if (bits == 16) le16(data, (uint32_t)(int16_t)lrint(v * 32767.0));
            else if (bits == 24) { const int32_t x = (int32_t)lrint(v * 8388607.0); data.push_back((uint8_t)x); data.push_back((uint8_t)(x >> 8)); data.push_back((uint8_t)(x >> 16)); }
            else le32(data, (uint32_t)(int32_t)lrint(v * 2147483647.0));
        }
    Bytes fmt;
    le16(fmt, (uint32_t)format); le16(fmt, (uint32_t)ch); le32(fmt, (uint32_t)rate);
    le32(fmt, (uint32_t)(rate * ch * (bits / 8))); le16(fmt, (uint32_t)(ch * (bits / 8))); le16(fmt, (uint32_t)bits);
    Bytes b;
    put(b, "RIFF"); le32(b, (uint32_t)(4 + 8 + fmt.size() + 8 + data.size())); put(b, "WAVE");
    put(b, "fmt "); le32(b, (uint32_t)fmt.size()); b.insert(b.end(), fmt.begin(), fmt.end());
    put(b, "data"); le32(b, (uint32_t)data.size()); b.insert(b.end(), data.begin(), data.end());
    return b;
}

// ---------------------------------------------------------------------------
//  Tests
// ---------------------------------------------------------------------------

static void test_flac_exact() {
    printf("- FLAC at 48 kHz is the file's samples\n");
    Bytes b;
    if (!load("chirp48.flac", &b)) { CHECK(false); return; }
    Opened o;
    CHECK(open_file(&o, b, LAF_FLAC));
    CHECK(la_can_decode(&o.meta) && la_gapless_trimmed(o.dec));
    Floats all;
    CHECK(decode_all(o.dec, &all, 4096) == 0);
    CHECK(all.size() == 96000 * 2);
    double worst;
    const double snr = snr_db(all, 0, 96000, 0.0, &worst);
    CHECK(worst < 1.6e-5);                                    // half a step of 1/32768, and the float conversion
    CHECK(snr > 80.0);
    // the same bits whatever the size of the asks, and with the file arriving in small pieces
    const int chunks[] = { 1, 7, 100, 512, 513, 100000 };
    for (int c : chunks) {
        Opened p;
        CHECK(open_file(&p, b, LAF_FLAC));
        Floats f;
        CHECK(decode_all(p.dec, &f, c) == 0 && f == all);
    }
    Opened q;
    CHECK(open_file(&q, b, LAF_FLAC, 61));
    Floats f61;
    CHECK(decode_all(q.dec, &f61, 4096) == 0 && f61 == all && !q.mem.overread);
    // a STREAMINFO that does not give its block sizes (0): every frame still decodes
    {
        Bytes nb = b;
        CHECK(nb.size() > 12 && !memcmp(nb.data(), "fLaC", 4));
        nb[8] = nb[9] = nb[10] = nb[11] = 0;                    // min and max block size of the first block, STREAMINFO
        Opened z;
        CHECK(open_file(&z, nb, LAF_FLAC) && z.meta.flac_max_block == 0);
        Floats fz;
        CHECK(decode_all(z.dec, &fz, 4096) == 0 && fz == all);
    }
    // after the end, the end
    float x[8];
    CHECK(la_decode(o.dec, x, 4) == 0 && la_decode(o.dec, x, 4) == 0);
    CHECK(la_decode(o.dec, x, 0) == 0 && la_decode(NULL, x, 4) == 0 && la_decode(o.dec, NULL, 4) == 0);
}

static void test_flac_resampled() {
    printf("- FLAC at other rates and sizes\n");
    Bytes b;
    Floats all;
    {
        if (!load("chirp44.flac", &b)) { CHECK(false); return; }
        Opened o;
        CHECK(open_file(&o, b, LAF_FLAC));
        CHECK(decode_all(o.dec, &all, 4096) == 0);
        const size_t pairs = all.size() / 2;
        CHECK(pairs <= 96000 && pairs >= 96000 - 80);           // the filter's tail is not played
        double worst;
        const double snr = snr_db(all, 100, pairs - 200, 100.0 / 48000.0, &worst);
        CHECK(snr > 70.0 && worst < 2e-3);
        double s2;
        CHECK(best_lag(all, 100, 20000, -6, 6, &s2) == 0);      // aligned: no delay from the converter
    }
    {
        if (!load("chirp96_24.flac", &b)) { CHECK(false); return; }
        Opened o;
        CHECK(open_file(&o, b, LAF_FLAC));
        CHECK(o.meta.bits == 24 && o.meta.sample_rate == 96000);
        Floats f;
        CHECK(decode_all(o.dec, &f, 1000) == 0);
        const size_t pairs = f.size() / 2;
        CHECK(pairs <= 24000 && pairs >= 24000 - 80);
        double worst;
        CHECK(snr_db(f, 100, pairs - 200, 100.0 / 48000.0, &worst) > 80.0 && worst < 5e-4);
    }
    {   // five channels and a LFE folded to two the way chan_map does
        if (!load("surround6.flac", &b)) { CHECK(false); return; }
        Opened o;
        CHECK(open_file(&o, b, LAF_FLAC));
        Floats f;
        CHECK(decode_all(o.dec, &f, 4096) == 0 && f.size() == 24000 * 2);
        const double k = 1.0 / (1.0 + 0.7071067811865476 * 2);
        double worst = 0;
        for (size_t i = 0; i < 24000; i++) {
            const double t = (double)i / 48000.0;
            auto s = [&](double hz) { return 0.2 * sin(2 * PI * hz * t); };
            const double el = (s(300) + 0.7071067811865476 * s(500) + 0.7071067811865476 * s(700)) * k;      // L + C + BL
            const double er = (s(400) + 0.7071067811865476 * s(500) + 0.7071067811865476 * s(800)) * k;      // R + C + BR
            worst = fmax(worst, fabs(f[i * 2] - el));
            worst = fmax(worst, fabs(f[i * 2 + 1] - er));
        }
        CHECK(worst < 6e-4);
    }
}

static void test_flac_seek() {
    printf("- FLAC seeking\n");
    Bytes b;
    if (!load("chirp48.flac", &b)) { CHECK(false); return; }
    Opened o;
    CHECK(open_file(&o, b, LAF_FLAC));
    Floats all, part;
    CHECK(decode_all(o.dec, &all, 4096) == 0);
    for (uint32_t secs : { 1u, 0u, 2u, 7u }) {
        CHECK(la_seek(o.dec, secs));
        CHECK(decode_all(o.dec, &part, 777) == 0);
        const size_t from = (size_t)(secs > 2 ? 2 : secs) * 48000;
        Floats want(all.begin() + (long)from * 2, all.end());
        CHECK(part == want);                                    // exactly the samples from there on
    }
    // many seeks in a row, to places that are not on a second
    CHECK(la_seek(o.dec, 1));
    float buf[512];
    CHECK(la_decode(o.dec, buf, 256) == 256);
    CHECK(la_seek(o.dec, 1) && la_decode(o.dec, buf, 256) == 256 && !memcmp(buf, &all[48000 * 2], sizeof(float) * 512));

    // a resampled one: the sound from the second on, aligned (the first pairs have no history behind them)
    Bytes c;
    if (!load("chirp44.flac", &c)) { CHECK(false); return; }
    Opened p;
    CHECK(open_file(&p, c, LAF_FLAC));
    CHECK(la_seek(p.dec, 1));
    CHECK(decode_all(p.dec, &part, 4096) == 0);
    CHECK(part.size() / 2 <= 48000 && part.size() / 2 >= 48000 - 80);
    double worst;
    CHECK(snr_db(part, 200, part.size() / 2 - 300, 1.0 + 200.0 / 48000.0, &worst) > 70.0 && worst < 2e-3);

    // the 24-bit file, with a seek table the encoder wrote or not: the same place
    Bytes d;
    if (!load("chirp96_24.flac", &d)) { CHECK(false); return; }
    Opened q;
    CHECK(open_file(&q, d, LAF_FLAC));
    CHECK(la_seek(q.dec, 0));
    CHECK(decode_all(q.dec, &part, 4096) == 0 && part.size() / 2 >= 24000 - 80);
}

static void test_mp3() {
    printf("- MP3\n");
    Bytes b;
    {   // the LAME header makes it gapless: the decode lines up with the formula without any offset
        if (!load("chirp44_v24.mp3", &b)) { CHECK(false); return; }
        Opened o;
        CHECK(open_file(&o, b, LAF_MP3));
        CHECK(la_gapless_trimmed(o.dec));
        Floats all;
        CHECK(decode_all(o.dec, &all, 4096) == 0);
        const size_t pairs = all.size() / 2;
        CHECK(pairs <= 96000 && pairs >= 96000 - 80);           // the length the tag states, less the converter's tail
        double snr;
        const int lag = best_lag(all, 2000, 20000, -8, 8, &snr);
        CHECK(lag == 0 && snr > 25.0);
        CHECK(snr_db(all, 0, pairs - 100, 0.0) > 25.0);        // from the first pair to nearly the last
        // chunking and short reads do not matter
        const int chunks[] = { 1, 333, 5000 };
        for (int c : chunks) {
            Opened p;
            CHECK(open_file(&p, b, LAF_MP3));
            Floats f;
            CHECK(decode_all(p.dec, &f, c) == 0 && f == all);
        }
        Opened q;
        CHECK(open_file(&q, b, LAF_MP3, 97));
        Floats fq;
        CHECK(decode_all(q.dec, &fq, 4096) == 0 && fq == all);
        // seeking: lands near the second (the table's resolution, and two frames dropped), and 0 is the start again
        CHECK(la_seek(o.dec, 1));
        Floats part;
        CHECK(decode_all(o.dec, &part, 4096) == 0);
        const double t = start_time(part, 1.0, 0.25);
        CHECK(fabs(t - 1.0) < 0.06);
        CHECK(part.size() / 2 > 40000 && part.size() / 2 < 52000);
        CHECK(la_seek(o.dec, 0));
        Floats again;
        CHECK(decode_all(o.dec, &again, 4096) == 0 && again == all);
        CHECK(la_seek(o.dec, 99));                               // past the end: clamped, plays what is left (nothing, or a little)
        CHECK(decode_all(o.dec, &part, 4096) == 0 && part.size() / 2 < 12000);
    }
    {   // ID3v2.3 and the same audio: the same samples
        Bytes c;
        if (!load("chirp44_v23.mp3", &c)) { CHECK(false); return; }
        Opened p;
        CHECK(open_file(&p, c, LAF_MP3));
        Floats f, g;
        CHECK(decode_all(p.dec, &f, 4096) == 0);
        Opened o;
        CHECK(open_file(&o, b, LAF_MP3));
        CHECK(decode_all(o.dec, &g, 4096) == 0 && f == g);
    }
    {   // without a LAME header nothing is trimmed: the sound starts late by the encoder's and decoder's delay (1105 samples at 44.1 kHz)
        Bytes c;
        if (!load("plain_v1.mp3", &c)) { CHECK(false); return; }
        Opened o;
        CHECK(open_file(&o, c, LAF_MP3));
        CHECK(!la_gapless_trimmed(o.dec));
        Floats all;
        CHECK(decode_all(o.dec, &all, 4096) == 0);
        double snr;
        const int lag = best_lag(all, 4000, 20000, 1100, 1300, &snr);
        CHECK(lag >= 1185 && lag <= 1225 && snr > 25.0);        // 1105 * 48000 / 44100 = 1203
        // a seek lands near the second (CBR: by the average bit rate)
        CHECK(la_seek(o.dec, 1));
        Floats part;
        CHECK(decode_all(o.dec, &part, 4096) == 0);
        CHECK(fabs(start_time(part, 1.0, 0.25) - 1.0) < 0.06);
    }
    {   // mono at 22.05 kHz: both sides the same, converted to 48 kHz
        Bytes c;
        if (!load("mono22.mp3", &c)) { CHECK(false); return; }
        Opened o;
        CHECK(open_file(&o, c, LAF_MP3));
        Floats all;
        CHECK(decode_all(o.dec, &all, 4096) == 0);
        const size_t pairs = all.size() / 2;
        CHECK(pairs <= 48000 && pairs >= 48000 - 200);
        bool same = true;
        for (size_t i = 0; i < pairs; i++) if (all[i * 2] != all[i * 2 + 1]) same = false;
        CHECK(same);
        double snr;
        const int lag = best_lag(all, 1000, 20000, -8, 8, &snr, true);
        CHECK(lag == 0 && snr > 20.0);
    }
}

static void test_wav() {
    printf("- WAVE\n");
    // 48 kHz: the stored sample, scaled; stereo with a different signal on each side
    auto sig = [](int i, int c) { return c == 0 ? chirp_l(i / 48000.0) : chirp_r(i / 48000.0); };
    const struct { int fmt, bits; double tol; } fm[] = {
        { 1, 8, 1.0 / 128 }, { 1, 16, 3.2e-5 }, { 1, 24, 2e-7 }, { 1, 32, 1e-7 }, { 3, 32, 1e-7 }, { 3, 64, 1e-7 },
    };
    for (const auto &f : fm) {
        const int frames = 20000;
        const Bytes b = make_wav(f.fmt, 2, 48000, f.bits, frames, sig);
        Opened o;
        CHECK(open_file(&o, b, LAF_WAV) && la_gapless_trimmed(o.dec));
        Floats all;
        CHECK(decode_all(o.dec, &all, 3000) == 0 && all.size() == (size_t)frames * 2);
        double worst = 0;
        for (int i = 0; i < frames; i++) {
            worst = fmax(worst, fabs(all[(size_t)i * 2] - sig(i, 0)));
            worst = fmax(worst, fabs(all[(size_t)i * 2 + 1] - sig(i, 1)));
        }
        CHECK(worst <= f.tol * 1.01 + 1e-9);
        // a seek is by the sample: to the second on a long enough file
        CHECK(la_seek(o.dec, 0));
        Floats a2;
        CHECK(decode_all(o.dec, &a2, 1) == 0 && a2 == all);
    }
    {   // exact values: 16-bit full scale and the 8-bit centre
        const Bytes b16 = make_wav(1, 1, 48000, 16, 4, [](int i, int) { return i == 0 ? 1.0 : i == 1 ? -1.0 : i == 2 ? 0.0 : 0.5; });
        Opened o;
        CHECK(open_file(&o, b16, LAF_WAV));
        Floats f;
        CHECK(decode_all(o.dec, &f, 8) == 0 && f.size() == 8);
        CHECK(f[0] == 32767.0f / 32768 && f[2] == -32767.0f / 32768 && f[4] == 0.0f && f[0] == f[1]);       // mono to both sides
        const Bytes b8 = make_wav(1, 2, 48000, 8, 2, [](int, int) { return 0.0; });
        Opened p;
        CHECK(open_file(&p, b8, LAF_WAV));
        CHECK(decode_all(p.dec, &f, 8) == 0 && f.size() == 4 && f[0] == 0.0f && f[3] == 0.0f);
    }
    {   // a float file with values that are not sound: no number, infinity, far beyond full scale
        const Bytes b = make_wav(3, 1, 48000, 32, 8, [](int i, int) { return i == 0 ? NAN : i == 1 ? INFINITY : i == 2 ? -INFINITY : i == 3 ? 1e30 : i == 4 ? 1.5 : 0.25; });
        Opened o;
        CHECK(open_file(&o, b, LAF_WAV));
        Floats f;
        CHECK(decode_all(o.dec, &f, 8) == 0 && f.size() == 16);
        CHECK(f[0] == 0.0f && f[2] == 2.0f && f[4] == -2.0f && f[6] == 2.0f && f[8] == 1.5f && f[10] == 0.25f);
    }
    {   // 44.1 kHz is converted, the sound stays where it was
        const Bytes b = make_wav(1, 2, 44100, 16, 44100, [](int i, int c) { return c == 0 ? chirp_l(i / 44100.0) : chirp_r(i / 44100.0); });
        Opened o;
        CHECK(open_file(&o, b, LAF_WAV));
        Floats all;
        CHECK(decode_all(o.dec, &all, 4096) == 0);
        CHECK(all.size() / 2 <= 48000 && all.size() / 2 >= 48000 - 80);
        double worst;
        CHECK(snr_db(all, 100, 47000, 100.0 / 48000.0, &worst) > 70.0);
        // seek to half a second: by the sample, so the second half is the formula from there
        CHECK(la_seek(o.dec, 1));
        Floats rest;
        CHECK(decode_all(o.dec, &rest, 4096) == 0 && rest.empty());
    }
    {   // six channels of a WAVE: folded as FLAC's are
        const Bytes b = make_wav(1, 6, 48000, 16, 4800, [](int i, int c) { return 0.2 * sin(2 * PI * (300 + 100 * c) * i / 48000.0); });
        Opened o;
        CHECK(open_file(&o, b, LAF_WAV));
        Floats f;
        CHECK(decode_all(o.dec, &f, 4096) == 0 && f.size() == 4800 * 2);
        const double k = 1.0 / (1.0 + 0.7071067811865476 * 2);
        double worst = 0;
        for (int i = 0; i < 4800; i++) {
            auto s = [&](int c) { return 0.2 * sin(2 * PI * (300 + 100 * c) * i / 48000.0); };
            worst = fmax(worst, fabs(f[(size_t)i * 2] - (s(0) + 0.7071067811865476 * s(2) + 0.7071067811865476 * s(4)) * k));
            worst = fmax(worst, fabs(f[(size_t)i * 2 + 1] - (s(1) + 0.7071067811865476 * s(2) + 0.7071067811865476 * s(5)) * k));
        }
        CHECK(worst < 6e-4);
    }
    {   // a data chunk cut short in the middle of a frame: whole frames only
        Bytes b = make_wav(1, 2, 48000, 16, 100, [](int, int) { return 0.25; });
        b.resize(b.size() - 3);
        Opened o;
        o.mem = { &b, 0, -1, false, 0 };
        CHECK(la_read_meta(mem_read, &o.mem, b.size(), LAF_WAV, &o.meta));
        o.dec = la_open(mem_read, &o.mem, b.size(), &o.meta);
        CHECK(o.dec != NULL);
        Floats f;
        CHECK(decode_all(o.dec, &f, 64) == 0 && f.size() == 99 * 2);
    }
}

static void test_can_decode() {
    printf("- what can be decoded\n");
    LaMeta m;
    memset(&m, 0, sizeof m);
    m.kind = LAF_FLAC; m.channels = 2; m.sample_rate = 44100; m.bits = 16;
    CHECK(la_can_decode(&m));
    m.bits = 24; CHECK(la_can_decode(&m));
    m.bits = 32; CHECK(!la_can_decode(&m));                    // beyond what the decoder does
    m.bits = 16; m.channels = 9; CHECK(!la_can_decode(&m));
    m.channels = 8; CHECK(la_can_decode(&m));
    m.sample_rate = 50000; CHECK(!la_can_decode(&m));
    m.sample_rate = 192000; CHECK(la_can_decode(&m));
    m.sample_rate = 384000; CHECK(!la_can_decode(&m));
    m.kind = LAF_MP3; m.sample_rate = 44100; m.channels = 2; CHECK(la_can_decode(&m));
    m.channels = 6; CHECK(!la_can_decode(&m));
    m.kind = LAF_WAV; m.channels = 2; m.bits = 16; m.block_align = 4; CHECK(la_can_decode(&m));
    m.block_align = 0; CHECK(!la_can_decode(&m));
    m.kind = LAF_NONE; CHECK(!la_can_decode(&m));
    CHECK(!la_can_decode(NULL) && la_open(mem_read, NULL, 100, NULL) == NULL);
    m.kind = LAF_FLAC; m.bits = 16; m.sample_rate = 44100;
    CHECK(la_open(NULL, NULL, 100, &m) == NULL);
    la_close(NULL);
}

// ---------------------------------------------------------------------------
//  Failing and damaged files
// ---------------------------------------------------------------------------

static void test_failures() {
    printf("- a drive that goes away\n");
    const char *names[] = { "chirp48.flac", "chirp44_v24.mp3", "plain_v1.mp3" };
    const LaKind kinds[] = { LAF_FLAC, LAF_MP3, LAF_MP3 };
    for (int i = 0; i < 3; i++) {
        Bytes b;
        if (!load(names[i], &b)) { CHECK(false); continue; }
        Opened full;
        CHECK(open_file(&full, b, kinds[i]));
        Floats all;
        CHECK(decode_all(full.dec, &all, 4096) == 0);
        // the reads stop working part way: what was decoded before comes out, then a failure, never a hang
        Mem keep;
        LaMeta meta;
        keep = { &b, 0, -1, false, 0 };
        CHECK(la_read_meta(mem_read, &keep, b.size(), kinds[i], &meta));
        Mem failing = { &b, 0, (int64_t)(b.size() * 6 / 10), false, 0 };
        LaDecoder *d = la_open(mem_read, &failing, b.size(), &meta);
        CHECK(d != NULL);
        Floats part;
        const int last = decode_all(d, &part, 4096);
        CHECK(last < 0);
        CHECK(part.size() / 2 > 20000 && part.size() < all.size());
        CHECK(!failing.overread);
        // what was decoded before the failure is what a good drive gives
        CHECK(memcmp(part.data(), all.data(), sizeof(float) * (part.size() > 2000 ? part.size() - 2000 : 0)) == 0);
        // the failure is reported once; a seek clears it (the drive is back)
        float x[4];
        CHECK(la_decode(d, x, 2) <= 0);
        failing.fail_after = -1;
        CHECK(la_seek(d, 0));
        CHECK(decode_all(d, &part, 4096) == 0 && part == all);
        la_close(d);
    }
    {   // failing from the very start
        Bytes b;
        if (!load("chirp48.flac", &b)) { CHECK(false); return; }
        Mem keep = { &b, 0, -1, false, 0 };
        LaMeta meta;
        CHECK(la_read_meta(mem_read, &keep, b.size(), LAF_FLAC, &meta));
        Mem failing = { &b, 0, 0, false, 0 };
        LaDecoder *d = la_open(mem_read, &failing, b.size(), &meta);
        if (d) {
            Floats f;
            CHECK(decode_all(d, &f, 4096) < 0 && f.empty());
            la_close(d);
        }
    }
}

static bool finite_and_bounded(const Floats &f) {
    for (float x : f) if (!(fabsf(x) <= 2.0f)) return false;
    return true;
}

static void test_damage() {
    printf("- damaged files\n");
    const struct { const char *name; LaKind kind; } files[] = {
        { "chirp44.flac", LAF_FLAC }, { "chirp96_24.flac", LAF_FLAC }, { "surround6.flac", LAF_FLAC },
        { "chirp44_v24.mp3", LAF_MP3 }, { "plain_v1.mp3", LAF_MP3 }, { "mono22.mp3", LAF_MP3 },
    };
    uint32_t rng = 777;
    auto next = [&]() { rng = rng * 1664525u + 1013904223u; return rng >> 8; };
    for (const auto &f : files) {
        Bytes whole;
        if (!load(f.name, &whole)) { CHECK(false); continue; }
        for (int i = 0; i < 40; i++) {
            Bytes dmg = whole;
            if (i < 10) dmg.resize(dmg.size() * (size_t)(i + 1) / 11);                   // cut short
            else {
                const int flips = 1 + (int)(next() % 40);
                for (int k = 0; k < flips; k++) dmg[next() % dmg.size()] = (uint8_t)next();
            }
            Opened o;
            if (!open_file(&o, dmg, f.kind, i % 3 == 0 ? 333 : 0)) continue;
            Floats out;
            decode_all(o.dec, &out, 2048, 600000);
            CHECK(finite_and_bounded(out) && !o.mem.overread);
            // seeking about in it is as safe
            for (uint32_t secs : { 1u, 0u, 3u, 1u }) { la_seek(o.dec, secs); decode_all(o.dec, &out, 4096, 600000); CHECK(finite_and_bounded(out)); }
            CHECK(!o.mem.overread);
        }
    }
    // a FLAC whose frames are all damaged: no sound, an end, no loop
    {
        Bytes b;
        if (!load("chirp48.flac", &b)) { CHECK(false); return; }
        Opened p;
        CHECK(open_file(&p, b, LAF_FLAC));
        const size_t first = (size_t)p.meta.data_off;
        for (size_t i = first + 20; i < b.size(); i += 97) b[i] ^= 0x5A;
        Opened o;
        if (open_file(&o, b, LAF_FLAC)) {
            Floats out;
            decode_all(o.dec, &out, 4096, 600000);
            CHECK(finite_and_bounded(out) && out.size() / 2 < 96000);
        }
    }
    // a file of one frame, and one of a few bytes
    {
        Bytes b;
        if (!load("chirp48.flac", &b)) { CHECK(false); return; }
        Opened p;
        CHECK(open_file(&p, b, LAF_FLAC));
        for (size_t extra : { (size_t)0, (size_t)1, (size_t)2, (size_t)50, (size_t)3000 }) {
            Bytes cut(b.begin(), b.begin() + (long)(p.meta.data_off + extra));
            Opened o;
            if (open_file(&o, cut, LAF_FLAC)) {
                Floats out;
                CHECK(decode_all(o.dec, &out, 4096) == 0);
            }
        }
    }
}

int main() {
    test_can_decode();
    test_flac_exact();
    test_flac_resampled();
    test_flac_seek();
    test_mp3();
    test_wav();
    test_failures();
    test_damage();
    printf("local audio: %d checks, %d failed\n", s_checks, s_failed);
    return s_failed ? 1 : 0;
}
