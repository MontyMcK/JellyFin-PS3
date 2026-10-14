// Host test for source/audio/flac_dec.c: the FLAC decoder.
//
//   make -f Makefile.host test_flac && ./test_flac
//
// Two kinds of evidence.  The ffmpeg-made files in fixtures/flac: every sample against ffmpeg's decoder
// and the MD5 the encoder wrote into STREAMINFO, however the stream is cut into pieces.  And frames made
// here by a small encoder of this file's own, to reach what ffmpeg's encoder never writes (every block size
// code, every rate code, 8 to 24 bits, all channel modes, Rice2, escaped partitions, wasted bits, long
// frame numbers) and to try damage: a frame cut at every length, bytes flipped at random.  Built with the
// address and undefined-behaviour sanitizers.

#include "flac_dec.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <algorithm>
#include <string>
#include <vector>

static int s_checks = 0, s_failed = 0;
#define CHECK(cond) do { s_checks++; if (!(cond)) { s_failed++; \
    printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

typedef std::vector<uint8_t> Bytes;
typedef std::vector<int32_t> Samples;

static Bytes load(const std::string &path) {
    Bytes b;
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) return b;
    fseek(f, 0, SEEK_END);
    b.resize((size_t)ftell(f));
    fseek(f, 0, SEEK_SET);
    if (fread(b.data(), 1, b.size(), f) != b.size()) b.clear();
    fclose(f);
    return b;
}

// ---- MD5 (RFC 1321), for STREAMINFO's checksum ---------------------------------------------------------------

struct Md5 {
    uint32_t a = 0x67452301, b = 0xefcdab89, c = 0x98badcfe, d = 0x10325476;
    uint64_t len = 0; uint8_t buf[64]; int n = 0;
    static uint32_t rol(uint32_t x, int s) { return (x << s) | (x >> (32 - s)); }
    void block(const uint8_t *p) {
        static const uint32_t K[64] = {
            0xd76aa478,0xe8c7b756,0x242070db,0xc1bdceee,0xf57c0faf,0x4787c62a,0xa8304613,0xfd469501,0x698098d8,0x8b44f7af,0xffff5bb1,0x895cd7be,0x6b901122,0xfd987193,0xa679438e,0x49b40821,
            0xf61e2562,0xc040b340,0x265e5a51,0xe9b6c7aa,0xd62f105d,0x02441453,0xd8a1e681,0xe7d3fbc8,0x21e1cde6,0xc33707d6,0xf4d50d87,0x455a14ed,0xa9e3e905,0xfcefa3f8,0x676f02d9,0x8d2a4c8a,
            0xfffa3942,0x8771f681,0x6d9d6122,0xfde5380c,0xa4beea44,0x4bdecfa9,0xf6bb4b60,0xbebfbc70,0x289b7ec6,0xeaa127fa,0xd4ef3085,0x04881d05,0xd9d4d039,0xe6db99e5,0x1fa27cf8,0xc4ac5665,
            0xf4292244,0x432aff97,0xab9423a7,0xfc93a039,0x655b59c3,0x8f0ccc92,0xffeff47d,0x85845dd1,0x6fa87e4f,0xfe2ce6e0,0xa3014314,0x4e0811a1,0xf7537e82,0xbd3af235,0x2ad7d2bb,0xeb86d391 };
        static const int S[64] = { 7,12,17,22,7,12,17,22,7,12,17,22,7,12,17,22, 5,9,14,20,5,9,14,20,5,9,14,20,5,9,14,20,
                                   4,11,16,23,4,11,16,23,4,11,16,23,4,11,16,23, 6,10,15,21,6,10,15,21,6,10,15,21,6,10,15,21 };
        uint32_t m[16];
        for (int i = 0; i < 16; i++) m[i] = (uint32_t)p[i * 4] | ((uint32_t)p[i * 4 + 1] << 8) | ((uint32_t)p[i * 4 + 2] << 16) | ((uint32_t)p[i * 4 + 3] << 24);
        uint32_t A = a, B = b, C = c, D = d;
        for (int i = 0; i < 64; i++) {
            uint32_t f; int g;
            if (i < 16) { f = (B & C) | (~B & D); g = i; }
            else if (i < 32) { f = (D & B) | (~D & C); g = (5 * i + 1) & 15; }
            else if (i < 48) { f = B ^ C ^ D; g = (3 * i + 5) & 15; }
            else { f = C ^ (B | ~D); g = (7 * i) & 15; }
            const uint32_t t = D; D = C; C = B;
            B = B + rol(A + f + K[i] + m[g], S[i]);
            A = t;
        }
        a += A; b += B; c += C; d += D;
    }
    void add(const uint8_t *p, size_t k) {
        len += k;
        while (k) { const size_t t = std::min<size_t>(k, 64 - (size_t)n); memcpy(buf + n, p, t); n += (int)t; p += t; k -= t; if (n == 64) { block(buf); n = 0; } }
    }
    void finish(uint8_t out[16]) {
        const uint64_t bits = len * 8;
        const uint8_t pad = 0x80; add(&pad, 1);
        const uint8_t z = 0; while (n != 56) add(&z, 1);
        uint8_t l[8]; for (int i = 0; i < 8; i++) l[i] = (uint8_t)(bits >> (8 * i));
        add(l, 8);
        uint32_t v[4] = { a, b, c, d };
        for (int i = 0; i < 16; i++) out[i] = (uint8_t)(v[i / 4] >> (8 * (i % 4)));
    }
};

// ---- a decoder driver like the player's: pieces arrive, frames come out ---------------------------------------------

struct Driver {
    FlacInfo si; bool have = false, bad = false;
    Bytes carry;
    Samples pcm;                          // interleaved
    int channels = 0, bps = 0, rate = 0;
    int frames = 0, skipped = 0;
    std::vector<Samples> planes;
    Driver() { planes.resize(FLAC_MAX_CHANNELS, Samples(FLAC_MAX_BLOCK)); }

    void feed(const uint8_t *p, size_t n) {
        carry.insert(carry.end(), p, p + n);
        if (!have) {
            const int r = flac_parse_header(carry.data(), (int)carry.size(), &si);
            if (r == 0) return;
            if (r < 0) { bad = true; carry.clear(); return; }
            carry.erase(carry.begin(), carry.begin() + r);
            have = true;
        }
        int32_t *out[FLAC_MAX_CHANNELS];
        for (int c = 0; c < FLAC_MAX_CHANNELS; c++) out[c] = planes[(size_t)c].data();
        for (;;) {
            size_t i = 0;
            while (i + 1 < carry.size() && !(carry[i] == 0xFF && (carry[i + 1] & 0xFE) == 0xF8)) i++;
            if (i + 1 >= carry.size()) { carry.erase(carry.begin(), carry.begin() + (carry.empty() ? 0 : (carry.back() == 0xFF ? (long)carry.size() - 1 : (long)carry.size()))); return; }
            if (i) carry.erase(carry.begin(), carry.begin() + (long)i);
            FlacFrame fr;
            const int r = flac_decode_frame(carry.data(), (int)carry.size(), &si, out, FLAC_MAX_BLOCK, &fr);
            if (r == 0) return;
            if (r < 0) { carry.erase(carry.begin(), carry.begin() + 1); skipped++; continue; }
            channels = fr.channels; bps = fr.bps; rate = fr.sample_rate; frames++;
            for (int s = 0; s < fr.blocksize; s++) for (int c = 0; c < fr.channels; c++) pcm.push_back(out[c][s]);
            carry.erase(carry.begin(), carry.begin() + r);
        }
    }
};

static Samples decode_file(const Bytes &f, size_t chunk, Driver *keep = nullptr) {
    Driver d;
    for (size_t pos = 0; pos < f.size(); pos += chunk) d.feed(&f[pos], std::min(chunk, f.size() - pos));
    if (keep) { Driver tmp; *keep = d; }
    return d.pcm;
}

static Samples ffmpeg_decode(const std::string &path) {
    Samples v;
    const std::string cmd = "ffmpeg -nostdin -v error -i " + path + " -f s32le - 2>/dev/null";
    FILE *p = popen(cmd.c_str(), "r");
    if (!p) return v;
    int32_t buf[4096];
    size_t got;
    while ((got = fread(buf, sizeof(int32_t), 4096, p)) > 0) v.insert(v.end(), buf, buf + got);
    pclose(p);
    return v;
}

static bool md5_matches(const Bytes &file, const Samples &pcm, int bps) {
    FlacInfo si;
    const int h = flac_parse_header(file.data(), (int)file.size(), &si);
    if (h <= 0) return false;
    uint8_t want[16];
    memcpy(want, file.data() + 8 + 18, 16);                       // STREAMINFO is the first block: 4 + 4, then 18 bytes before the MD5
    bool zero = true;
    for (int i = 0; i < 16; i++) if (want[i]) zero = false;
    if (zero) return true;                                        // no checksum written
    Md5 m;
    const int bytes = (bps + 7) / 8;
    for (int32_t v : pcm) { uint8_t b[4] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24) }; m.add(b, (size_t)bytes); }
    uint8_t got[16];
    m.finish(got);
    return memcmp(got, want, 16) == 0;
}

static void fixtures() {
    printf("- ffmpeg-made files\n");
    struct { const char *name; int ch, rate, bps; } f[] = {
        { "tone16", 2, 44100, 16 }, { "noise16", 2, 44100, 16 }, { "silence", 2, 44100, 16 }, { "tone24", 2, 96000, 24 },
        { "surround16", 6, 48000, 16 }, { "mono22", 1, 22050, 16 }, { "wasted12", 2, 44100, 16 }, { "ls", 2, 44100, 16 },
        { "rs", 2, 44100, 16 }, { "ms", 2, 44100, 16 }, { "indep", 2, 44100, 16 }, { "fixed", 2, 44100, 16 },
        { "fs100", 2, 44100, 16 }, { "fs1152", 2, 44100, 16 }, { "fs5000", 2, 44100, 16 },
    };
    for (auto &x : f) {
        const std::string path = std::string("fixtures/flac/") + x.name + ".flac";
        const Bytes file = load(path);
        CHECK(!file.empty());
        if (file.empty()) continue;
        Driver whole;
        whole.feed(file.data(), file.size());
        CHECK(!whole.bad && whole.have && whole.skipped == 0 && whole.frames > 0);
        CHECK(whole.channels == x.ch && whole.rate == x.rate && whole.bps == x.bps);
        CHECK(whole.si.channels == x.ch && whole.si.sample_rate == x.rate && whole.si.bps == x.bps);
        CHECK((uint64_t)whole.pcm.size() / (uint64_t)x.ch == whole.si.total_samples);
        if (!md5_matches(file, whole.pcm, x.bps)) printf("  %s: the MD5 of STREAMINFO differs\n", x.name);
        CHECK(md5_matches(file, whole.pcm, x.bps));
        // ffmpeg's decoder: s32 samples are the file's left-justified
        Samples ref = ffmpeg_decode(path);
        if (ref.empty()) printf("  (no ffmpeg: %s not compared)\n", x.name);
        else {
            Samples mine = whole.pcm;
            for (int32_t &v : mine) v = (int32_t)((uint32_t)v << (32 - x.bps));
            if (mine != ref) printf("  %s: differs from ffmpeg (%zu vs %zu samples)\n", x.name, mine.size(), ref.size());
            CHECK(mine == ref);
        }
        // the same however the stream arrives
        for (size_t chunk : { (size_t)1, (size_t)7, (size_t)188, (size_t)4096, (size_t)65536 }) {
            if (chunk == 1 && file.size() > 11000) continue;           // one byte at a time: the small files only (every byte retries the frame)
            Samples got = decode_file(file, chunk);
            if (got != whole.pcm) printf("  %s: chunk %zu differs\n", x.name, chunk);
            CHECK(got == whole.pcm);
        }
    }

    // the surround file's channels are in the port's order: FL FR FC LFE BL BR, tone per channel
    {
        Driver d;
        const Bytes file = load("fixtures/flac/surround16.flac");
        d.feed(file.data(), file.size());
        const double tones[6] = { 300, 500, 700, 80, 1100, 1300 };
        const size_t n = d.pcm.size() / 6;
        bool ok = n > 10000;
        for (int c = 0; c < 6 && ok; c++) {
            double best = -1; int best_t = -1;
            for (int t = 0; t < 6; t++) {
                double ss = 0, cc = 0;
                for (size_t i = 0; i < n; i++) { const double w = 6.283185307179586 * tones[t] * (double)i / 48000.0, v = d.pcm[i * 6 + (size_t)c]; ss += v * sin(w); cc += v * cos(w); }
                const double a = sqrt(ss * ss + cc * cc) * 2.0 / (double)n;
                if (a > best) { best = a; best_t = t; }
            }
            if (best_t != c || best < 2000) { ok = false; printf("  channel %d: strongest tone %d (%.0f)\n", c, best_t, best); }
        }
        CHECK(ok);
    }
}

// ---- a small encoder, to make what ffmpeg's does not -----------------------------------------------------------------

struct BW {
    Bytes b; int nbits = 0;
    void put(uint64_t v, int n) {
        for (int i = n - 1; i >= 0; i--) {
            if ((nbits & 7) == 0) b.push_back(0);
            if ((v >> i) & 1) b.back() |= (uint8_t)(0x80 >> (nbits & 7));
            nbits++;
        }
    }
    void put_signed(int64_t v, int n) { put((uint64_t)v & (n >= 64 ? ~0ull : ((1ull << n) - 1)), n); }
    void unary(int q) { for (int i = 0; i < q; i++) put(0, 1); put(1, 1); }
    void align() { while (nbits & 7) put(0, 1); }
};

static uint8_t enc_crc8(const uint8_t *p, size_t n) {
    uint8_t c = 0;
    for (size_t i = 0; i < n; i++) { c ^= p[i]; for (int k = 0; k < 8; k++) c = (uint8_t)((c & 0x80) ? (c << 1) ^ 0x07 : (c << 1)); }
    return c;
}
static uint16_t enc_crc16(const uint8_t *p, size_t n) {
    uint16_t c = 0;
    for (size_t i = 0; i < n; i++) { c ^= (uint16_t)(p[i] << 8); for (int k = 0; k < 8; k++) c = (uint16_t)((c & 0x8000) ? (c << 1) ^ 0x8005 : (c << 1)); }
    return c;
}

struct Sub {
    int kind = 1;                     // 0 constant, 1 verbatim, 2 fixed, 3 LPC
    int order = 0;                    // fixed 0..4, LPC 1..32
    std::vector<int32_t> coef;        // LPC
    int prec = 12, shift = 8;
    int po = 0, method = 0;           // partition order, 0 = Rice4 / 1 = Rice5
    int wasted = 0;
    bool escape = false;              // escape-code the partitions whose residual fits
};

static int zigzag_bits(const std::vector<int64_t> &r, size_t from, size_t to, int k) {
    int64_t bits = 0;
    for (size_t i = from; i < to; i++) { const uint64_t u = r[i] >= 0 ? (uint64_t)r[i] << 1 : (((uint64_t)(-r[i] - 1)) << 1) | 1; bits += (int64_t)(u >> k) + 1 + k; }
    return bits > 0x7FFFFFFF ? 0x7FFFFFFF : (int)bits;
}

static int signed_bits(int64_t v) { int n = 1; while (n < 40 && (v < -(1ll << (n - 1)) || v >= (1ll << (n - 1)))) n++; return n; }

// One subframe of `x` (blocksize samples) at `bps` bits.  The samples go in as the decoder will give them: wasted bits already zero.
static void write_sub(BW &w, const std::vector<int32_t> &xin, int bps, const Sub &s) {
    const int bs = (int)xin.size();
    std::vector<int32_t> x = xin;
    for (int32_t &v : x) v >>= s.wasted;                          // the zero low bits are not coded
    const int bits = bps - s.wasted;
    w.put(0, 1);
    if (s.kind == 0) w.put(0, 6);
    else if (s.kind == 1) w.put(1, 6);
    else if (s.kind == 2) w.put(8 + (uint64_t)s.order, 6);
    else w.put(32 + (uint64_t)(s.order - 1), 6);
    w.put(s.wasted ? 1 : 0, 1);
    if (s.wasted) w.unary(s.wasted - 1);
    if (s.kind == 0) { w.put_signed(x[0], bits); return; }
    if (s.kind == 1) { for (int i = 0; i < bs; i++) w.put_signed(x[(size_t)i], bits); return; }
    const int order = s.order;
    for (int i = 0; i < order; i++) w.put_signed(x[(size_t)i], bits);
    if (s.kind == 3) {
        w.put((uint64_t)(s.prec - 1), 4);
        w.put_signed(s.shift, 5);
        for (int j = 0; j < order; j++) w.put_signed(s.coef[(size_t)j], s.prec);
    }
    std::vector<int64_t> r((size_t)bs, 0);
    for (int i = order; i < bs; i++) {
        int64_t pred = 0;
        if (s.kind == 2) {
            switch (order) {
            case 1: pred = x[(size_t)i - 1]; break;
            case 2: pred = 2 * (int64_t)x[(size_t)i - 1] - x[(size_t)i - 2]; break;
            case 3: pred = 3 * (int64_t)x[(size_t)i - 1] - 3 * (int64_t)x[(size_t)i - 2] + x[(size_t)i - 3]; break;
            case 4: pred = 4 * (int64_t)x[(size_t)i - 1] - 6 * (int64_t)x[(size_t)i - 2] + 4 * (int64_t)x[(size_t)i - 3] - x[(size_t)i - 4]; break;
            default: break;
            }
        } else {
            int64_t sum = 0;
            for (int j = 0; j < order; j++) sum += (int64_t)s.coef[(size_t)j] * x[(size_t)(i - 1 - j)];
            pred = sum >> s.shift;
        }
        r[(size_t)i] = (int64_t)x[(size_t)i] - pred;
    }
    w.put((uint64_t)s.method, 2);
    w.put((uint64_t)s.po, 4);
    const int parts = 1 << s.po, psize = bs >> s.po, pbits = s.method ? 5 : 4, kmax = s.method ? 30 : 14;
    for (int p = 0; p < parts; p++) {
        const size_t from = p == 0 ? (size_t)order : (size_t)p * (size_t)psize, to = (size_t)(p + 1) * (size_t)psize;
        int best_k = 0, best = 0x7FFFFFFF;
        for (int k = 0; k <= kmax; k++) { const int b = zigzag_bits(r, from, to, k); if (b < best) { best = b; best_k = k; } }
        int esc_bits = 0;
        for (size_t i = from; i < to; i++) esc_bits = std::max(esc_bits, signed_bits(r[i]));
        bool all_zero = true;
        for (size_t i = from; i < to; i++) if (r[i]) all_zero = false;
        if (s.escape && esc_bits < 32 && (all_zero || esc_bits <= 31)) {
            w.put(s.method ? 31 : 15, pbits);
            const int n = all_zero ? 0 : esc_bits;
            w.put((uint64_t)n, 5);
            for (size_t i = from; i < to; i++) if (n) w.put_signed(r[i], n);
        } else {
            w.put((uint64_t)best_k, pbits);
            for (size_t i = from; i < to; i++) {
                const uint64_t u = r[i] >= 0 ? (uint64_t)r[i] << 1 : (((uint64_t)(-r[i] - 1)) << 1) | 1;
                w.unary((int)(u >> best_k));
                if (best_k) w.put(u & ((1ull << best_k) - 1), best_k);
            }
        }
    }
}

struct FrameSpec {
    int bs_code, sr_code, ss_code;      // as coded
    int ch_code;
    uint64_t number = 0;                // frame or sample number
    bool variable = false;
    int blocksize = 0;                  // for the explicit codes 6 and 7
    int explicit_rate = 0;              // for 12 (kHz), 13 (Hz), 14 (tens of Hz): the coded value
};

static void put_number(BW &w, uint64_t v) {
    if (v < 0x80) { w.put(v, 8); return; }
    int extra = 1;
    while (extra < 6 && v >= (1ull << (6 * extra + (6 - extra)))) extra++;
    // lead byte: `extra` ones, a zero, then the top bits
    const int lead_bits = 7 - extra;
    w.put(((0xFFull << (8 - extra - 1)) & 0xFF) | (v >> (6 * extra)), 8);
    (void)lead_bits;
    for (int i = extra - 1; i >= 0; i--) w.put(0x80 | ((v >> (6 * i)) & 0x3F), 8);
}

// A whole frame from the stored channels (after any decorrelation: side channels carry bps + 1 bits).
static Bytes make_frame(const FrameSpec &f, int bps, const std::vector<std::vector<int32_t>> &stored, const std::vector<Sub> &subs) {
    BW w;
    w.put(0xFFF8 | (f.variable ? 1 : 0), 16);
    w.put((uint64_t)f.bs_code, 4); w.put((uint64_t)f.sr_code, 4);
    w.put((uint64_t)f.ch_code, 4); w.put((uint64_t)f.ss_code, 3); w.put(0, 1);
    put_number(w, f.number);
    if (f.bs_code == 6) w.put((uint64_t)f.blocksize - 1, 8);
    if (f.bs_code == 7) w.put((uint64_t)f.blocksize - 1, 16);
    if (f.sr_code == 12) w.put((uint64_t)f.explicit_rate, 8);
    if (f.sr_code == 13 || f.sr_code == 14) w.put((uint64_t)f.explicit_rate, 16);
    w.put(enc_crc8(w.b.data(), w.b.size()), 8);
    for (size_t c = 0; c < stored.size(); c++) {
        const bool side = (f.ch_code == 8 && c == 1) || (f.ch_code == 9 && c == 0) || (f.ch_code == 10 && c == 1);
        write_sub(w, stored[c], bps + (side ? 1 : 0), subs[c]);
    }
    w.align();
    const uint16_t crc = enc_crc16(w.b.data(), w.b.size());
    w.put(crc, 16);
    return w.b;
}

struct Rng {
    uint64_t s;
    explicit Rng(uint64_t seed) : s(seed * 0x9E3779B97F4A7C15ull + 1) {}
    uint32_t next() { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return (uint32_t)(s >> 16); }
    int32_t range(int32_t lo, int32_t hi) { return lo + (int32_t)(next() % (uint32_t)(hi - lo + 1)); }
};

// A test signal that a predictor can work with: a slow wave plus noise, within bps bits (some at the extremes).
static std::vector<int32_t> signal(Rng &r, int n, int bps, int style) {
    std::vector<int32_t> x((size_t)n);
    const int32_t hi = (int32_t)((1ll << (bps - 1)) - 1), lo = -(int32_t)(1ll << (bps - 1));
    double ph = r.next() % 100, f = 0.01 + (r.next() % 100) * 0.001;
    for (int i = 0; i < n; i++) {
        double v;
        if (style == 0) v = 0.7 * hi * sin(ph + f * i) + (r.range(-100, 100) / 100.0) * (hi / 64.0);
        else if (style == 1) v = (i & 8) ? hi : lo;                              // square wave at the extremes
        else if (style == 2) v = r.range(lo, hi);                               // noise
        else v = (i % 7 == 0) ? hi : (i % 11 == 0 ? lo : 0);                    // sparse spikes
        long long q = (long long)v;
        x[(size_t)i] = (int32_t)std::max<long long>(lo, std::min<long long>(hi, q));
    }
    return x;
}

static bool decode_one(const Bytes &fr, const FlacInfo *si, std::vector<Samples> &planes, FlacFrame *out, int *ret) {
    int32_t *o[FLAC_MAX_CHANNELS];
    for (int c = 0; c < FLAC_MAX_CHANNELS; c++) o[c] = planes[(size_t)c].data();
    *ret = flac_decode_frame(fr.data(), (int)fr.size(), si, o, FLAC_MAX_BLOCK, out);
    return *ret == (int)fr.size();
}

static const FlacInfo kInfo = { 4096, 4096, 44100, 2, 16, 0 };

static void synthetic() {
    printf("- frames made here\n");
    std::vector<Samples> planes(FLAC_MAX_CHANNELS, Samples(FLAC_MAX_BLOCK));
    Rng rng(7);
    FlacFrame fr;
    int ret;

    // block size codes and the sample rate codes
    {
        struct { int code, size, ext; } bs[] = { { 1, 192, 0 }, { 2, 576, 0 }, { 3, 1152, 0 }, { 4, 2304, 0 }, { 5, 4608, 0 }, { 6, 100, 1 }, { 6, 256, 1 }, { 6, 1, 1 },
            { 7, 5000, 1 }, { 7, 65535, 1 }, { 7, 300, 1 }, { 8, 256, 0 }, { 9, 512, 0 }, { 10, 1024, 0 }, { 11, 2048, 0 }, { 12, 4096, 0 }, { 13, 8192, 0 }, { 14, 16384, 0 }, { 15, 32768, 0 } };
        for (auto &b : bs) {
            FrameSpec f; f.bs_code = b.code; f.sr_code = 9; f.ss_code = 4; f.ch_code = 1; f.blocksize = b.size; f.number = 3;
            std::vector<std::vector<int32_t>> st = { signal(rng, b.size, 16, 0), signal(rng, b.size, 16, 0) };
            std::vector<Sub> subs(2);
            subs[0].kind = 2; subs[0].order = 2; subs[0].po = (b.size >= 256 && b.size % 4 == 0) ? 2 : 0;       // partitions must divide the block
            if (b.size < 4) subs[0].kind = 1;                              // too short to predict
            Bytes bytes = make_frame(f, 16, st, subs);
            const bool decoded_ok = decode_one(bytes, &kInfo, planes, &fr, &ret) && fr.blocksize == b.size && fr.sample_rate == 44100;
            if (!decoded_ok) printf("  block size code %d, %d samples: ret %d of %zu bytes\n", b.code, b.size, ret, bytes.size());
            CHECK(decoded_ok);
            bool same = true;
            for (int c = 0; c < 2; c++) for (int i = 0; i < b.size; i++) if (planes[(size_t)c][(size_t)i] != st[(size_t)c][(size_t)i]) same = false;
            CHECK(same);
            // a frame whose block does not fit the caller's room is refused, not written
            int32_t *o[FLAC_MAX_CHANNELS];
            for (int c = 0; c < FLAC_MAX_CHANNELS; c++) o[c] = planes[(size_t)c].data();
            if (b.size > 1) CHECK(flac_decode_frame(bytes.data(), (int)bytes.size(), &kInfo, o, b.size - 1, &fr) == -1);
        }
        struct { int code, rate, ext; } sr[] = { { 0, 44100, 0 }, { 1, 88200, 0 }, { 2, 176400, 0 }, { 3, 192000, 0 }, { 4, 8000, 0 }, { 5, 16000, 0 }, { 6, 22050, 0 }, { 7, 24000, 0 },
            { 8, 32000, 0 }, { 9, 44100, 0 }, { 10, 48000, 0 }, { 11, 96000, 0 }, { 12, 44000, 44 }, { 12, 255000, 255 }, { 13, 44100, 44100 }, { 13, 65535, 65535 }, { 14, 44100, 4410 }, { 14, 655350, 65535 } };
        for (auto &s : sr) {
            FrameSpec f; f.bs_code = 8; f.sr_code = s.code; f.ss_code = 4; f.ch_code = 0; f.explicit_rate = s.ext; f.number = 0;
            std::vector<std::vector<int32_t>> st = { signal(rng, 256, 16, 0) };
            std::vector<Sub> subs(1); subs[0].kind = 1;
            Bytes bytes = make_frame(f, 16, st, subs);
            CHECK(decode_one(bytes, &kInfo, planes, &fr, &ret) && fr.sample_rate == s.rate);
        }
        // code 0 with no STREAMINFO to take it from: not decodable
        FrameSpec f; f.bs_code = 8; f.sr_code = 0; f.ss_code = 4; f.ch_code = 0;
        std::vector<std::vector<int32_t>> st = { signal(rng, 256, 16, 0) };
        std::vector<Sub> subs(1); subs[0].kind = 1;
        Bytes bytes = make_frame(f, 16, st, subs);
        int32_t *o[FLAC_MAX_CHANNELS];
        for (int c = 0; c < FLAC_MAX_CHANNELS; c++) o[c] = planes[(size_t)c].data();
        CHECK(flac_decode_frame(bytes.data(), (int)bytes.size(), nullptr, o, FLAC_MAX_BLOCK, &fr) == -1);
    }

    // sample sizes, with signals at the extremes, each subframe kind, every stereo mode
    for (int bps : { 8, 12, 16, 20, 24 }) {
        const int ss_code = bps == 8 ? 1 : bps == 12 ? 2 : bps == 16 ? 4 : bps == 20 ? 5 : 6;
        FlacInfo info = kInfo; info.bps = bps;
        for (int style = 0; style < 4; style++) {
            for (int mode = 0; mode <= 10; mode++) {
                const int channels = mode <= 7 ? mode + 1 : 2;
                if (mode > 2 && mode <= 7 && style != 0) continue;
                FrameSpec f; f.bs_code = 8; f.sr_code = 0; f.ss_code = (mode & 1) ? 0 : ss_code; f.ch_code = mode; f.number = (uint64_t)mode;
                const int bs = 256;
                std::vector<std::vector<int32_t>> real, stored;
                for (int c = 0; c < channels; c++) real.push_back(signal(rng, bs, bps, style));
                stored = real;
                if (mode == 8) for (int i = 0; i < bs; i++) stored[1][(size_t)i] = real[0][(size_t)i] - real[1][(size_t)i];                     // left, side
                if (mode == 9) for (int i = 0; i < bs; i++) { stored[0][(size_t)i] = real[0][(size_t)i] - real[1][(size_t)i]; }                 // side, right
                if (mode == 10) for (int i = 0; i < bs; i++) {
                    const int64_t l = real[0][(size_t)i], r = real[1][(size_t)i];
                    stored[0][(size_t)i] = (int32_t)((l + r) >> 1); stored[1][(size_t)i] = (int32_t)(l - r);                                // mid, side
                }
                std::vector<Sub> subs((size_t)channels);
                for (int c = 0; c < channels; c++) {
                    Sub &s = subs[(size_t)c];
                    const int pick = (c + style + mode) % 6;
                    if (pick == 0) { s.kind = 1; }
                    else if (pick == 1) { s.kind = 2; s.order = (c + mode) % 5; s.po = (mode + c) % 5; s.method = c & 1; }
                    else if (pick == 2) { s.kind = 3; s.order = 1 + (c * 5 + mode) % 12; s.coef.assign((size_t)s.order, 0); s.coef[0] = 3; if (s.order > 1) s.coef[1] = -1; s.prec = 4 + c; s.shift = 1; s.po = 1; s.escape = true; }
                    else if (pick == 3) { s.kind = 3; s.order = 32; s.coef.assign(32, 0); s.coef[0] = 5; s.coef[31] = 1; s.prec = 15; s.shift = 3; s.po = 3; s.method = 1; }
                    else if (pick == 4) { s.kind = 2; s.order = 3; s.po = 6; s.escape = true; }
                    else { s.kind = 3; s.order = 2; s.coef = { 2, -1 }; s.prec = 5; s.shift = 0; s.po = 0; }
                }
                // the same signal for a constant subframe
                if (style == 1 && mode == 0) { std::fill(stored[0].begin(), stored[0].end(), stored[0][0]); real = stored; subs[0].kind = 0; }
                Bytes bytes = make_frame(f, bps, stored, subs);
                const bool ok = decode_one(bytes, &info, planes, &fr, &ret);
                bool same = ok && fr.channels == channels && fr.bps == bps;
                for (int c = 0; same && c < channels; c++) for (int i = 0; i < bs; i++) if (planes[(size_t)c][(size_t)i] != real[(size_t)c][(size_t)i]) { same = false; break; }
                if (!same) printf("  bps %d style %d mode %d: ret %d (frame %zu bytes)\n", bps, style, mode, ret, bytes.size());
                CHECK(same);
            }
        }
    }

    // wasted bits: 1 to 8, on a subframe of each kind, and on one channel only
    for (int wasted : { 1, 2, 5, 8 }) {
        for (int kind : { 0, 1, 2, 3 }) {
            FrameSpec f; f.bs_code = 8; f.sr_code = 0; f.ss_code = 4; f.ch_code = 1; f.number = 1;
            std::vector<std::vector<int32_t>> real = { signal(rng, 256, 16, 0), signal(rng, 256, 16, 0) };
            for (int32_t &v : real[0]) v = (int32_t)((uint32_t)(v >> wasted) << wasted);
            if (kind == 0) std::fill(real[0].begin(), real[0].end(), real[0][3]);
            std::vector<Sub> subs(2);
            subs[0].kind = kind; subs[0].wasted = wasted; subs[0].order = kind == 3 ? 2 : 1; subs[0].coef = { 2, -1 }; subs[0].prec = 6; subs[0].shift = 0; subs[0].po = 2;
            Bytes bytes = make_frame(f, 16, real, subs);
            const bool ok = decode_one(bytes, &kInfo, planes, &fr, &ret);
            bool same = ok;
            for (int c = 0; same && c < 2; c++) for (int i = 0; i < 256; i++) if (planes[(size_t)c][(size_t)i] != real[(size_t)c][(size_t)i]) { same = false; break; }
            CHECK(same);
        }
    }

    // frame and sample numbers of every length
    for (uint64_t n : { 0ull, 1ull, 127ull, 128ull, 2047ull, 2048ull, 65535ull, 65536ull, 0x1FFFFFull, 0x200000ull, 0x3FFFFFFull, 0x4000000ull, 0x7FFFFFFFull, 0x80000000ull, 0xFFFFFFFFFull }) {
        FrameSpec f; f.bs_code = 8; f.sr_code = 9; f.ss_code = 4; f.ch_code = 0; f.number = n; f.variable = n > 0x7FFFFFFFull;
        std::vector<std::vector<int32_t>> st = { signal(rng, 256, 16, 0) };
        std::vector<Sub> subs(1); subs[0].kind = 1;
        Bytes bytes = make_frame(f, 16, st, subs);
        CHECK(decode_one(bytes, &kInfo, planes, &fr, &ret));
    }
}

static void damage() {
    printf("- damaged and cut frames\n");
    std::vector<Samples> planes(FLAC_MAX_CHANNELS, Samples(FLAC_MAX_BLOCK));
    Rng rng(99);
    FlacFrame fr;
    int ret;
    // a frame of each flavour, cut at every length: need-more or invalid, never a frame, never out of bounds
    for (int flavour = 0; flavour < 4; flavour++) {
        FrameSpec f; f.bs_code = 8; f.sr_code = 0; f.ss_code = 4; f.ch_code = flavour == 3 ? 10 : 1; f.number = 300 * (uint64_t)flavour;
        std::vector<std::vector<int32_t>> st = { signal(rng, 256, 16, flavour == 2 ? 2 : 0), signal(rng, 256, 16, 0) };
        std::vector<Sub> subs(2);
        for (int c = 0; c < 2; c++) {
            subs[(size_t)c].kind = flavour == 0 ? 1 : flavour == 1 ? 2 : 3;
            subs[(size_t)c].order = flavour == 1 ? 3 : 4; subs[(size_t)c].coef = { 3, -2, 1, 0 }; subs[(size_t)c].prec = 7; subs[(size_t)c].shift = 1; subs[(size_t)c].po = 3; subs[(size_t)c].escape = flavour == 2;
        }
        Bytes whole = make_frame(f, 16, st, subs);
        CHECK(decode_one(whole, &kInfo, planes, &fr, &ret));
        bool never_complete = true;
        for (size_t cut = 0; cut < whole.size(); cut++) {
            Bytes part(whole.begin(), whole.begin() + (long)cut);
            int32_t *o[FLAC_MAX_CHANNELS];
            for (int c = 0; c < FLAC_MAX_CHANNELS; c++) o[c] = planes[(size_t)c].data();
            const int r = flac_decode_frame(part.data(), (int)part.size(), &kInfo, o, FLAC_MAX_BLOCK, &fr);
            if (r != 0) never_complete = false;                      // always "give me more"
        }
        CHECK(never_complete);
        // trailing bytes after the frame are not part of it
        Bytes more = whole; more.insert(more.end(), { 0xFF, 0xF8, 1, 2 });
        int32_t *o[FLAC_MAX_CHANNELS];
        for (int c = 0; c < FLAC_MAX_CHANNELS; c++) o[c] = planes[(size_t)c].data();
        CHECK(flac_decode_frame(more.data(), (int)more.size(), &kInfo, o, FLAC_MAX_BLOCK, &fr) == (int)whole.size());
        // a flipped bit anywhere is caught by a CRC (or another check); the first bytes also may make it "need more", but not a frame
        int accepted = 0;
        for (size_t i = 0; i < whole.size(); i++) {
            for (int bit : { 0, 3, 7 }) {
                Bytes bad = whole; bad[i] ^= (uint8_t)(1 << bit);
                const int r = flac_decode_frame(bad.data(), (int)bad.size(), &kInfo, o, FLAC_MAX_BLOCK, &fr);
                if (r > 0) accepted++;
            }
        }
        CHECK(accepted == 0);
    }
    // random garbage and random corruption: no crash, no overrun (the sanitizers watch)
    {
        long decoded = 0;
        for (int round = 0; round < 1500; round++) {
            Bytes junk((size_t)rng.range(1, 700));
            for (uint8_t &b : junk) b = (uint8_t)rng.next();
            if (round & 1) { junk[0] = 0xFF; if (junk.size() > 1) junk[1] = 0xF8; }
            int32_t *o[FLAC_MAX_CHANNELS];
            for (int c = 0; c < FLAC_MAX_CHANNELS; c++) o[c] = planes[(size_t)c].data();
            if (flac_decode_frame(junk.data(), (int)junk.size(), &kInfo, o, FLAC_MAX_BLOCK, &fr) > 0) decoded++;
        }
        CHECK(decoded < 3);                                         // a CRC-16 passes 1 in 65536
        // frames with a fixed-up CRC but a nonsense body: the checks inside the subframes
        for (int round = 0; round < 800; round++) {
            BW w;
            w.put(0xFFF8, 16); w.put(8, 4); w.put(9, 4); w.put((uint64_t)(rng.next() % 11), 4); w.put(4, 3); w.put(0, 1); w.put(0, 8);
            w.put(enc_crc8(w.b.data(), w.b.size()), 8);
            for (int k = 0; k < 300; k++) w.put(rng.next() & 0xFF, 8);
            w.align();
            w.put(enc_crc16(w.b.data(), w.b.size()), 16);
            int32_t *o[FLAC_MAX_CHANNELS];
            for (int c = 0; c < FLAC_MAX_CHANNELS; c++) o[c] = planes[(size_t)c].data();
            flac_decode_frame(w.b.data(), (int)w.b.size(), &kInfo, o, FLAC_MAX_BLOCK, &fr);
        }
        CHECK(true);
    }
    // a real file with a byte flipped in the middle: that frame is skipped, the rest decode
    {
        Bytes file = load("fixtures/flac/tone16.flac");
        Driver ok;
        ok.feed(file.data(), file.size());
        file[file.size() / 2] ^= 0x10;
        Driver hurt;
        for (size_t pos = 0; pos < file.size(); pos += 1000) hurt.feed(&file[pos], std::min<size_t>(1000, file.size() - pos));
        // exactly one frame (4608 samples a channel, ffmpeg's block size at this level) is gone; every other sample is as it was
        const size_t lost = 4608 * 2;
        CHECK(hurt.frames == ok.frames - 1 && hurt.skipped >= 1 && hurt.pcm.size() + lost == ok.pcm.size());
        bool one_frame_missing = false;
        for (size_t at = 0; at + lost <= ok.pcm.size() && !one_frame_missing; at += lost) {
            Samples expect(ok.pcm.begin(), ok.pcm.begin() + (long)at);
            expect.insert(expect.end(), ok.pcm.begin() + (long)(at + lost), ok.pcm.end());
            if (expect == hurt.pcm) one_frame_missing = true;
        }
        CHECK(one_frame_missing);
    }
}

static void headers() {
    printf("- the stream header\n");
    FlacInfo si;
    const Bytes f = load("fixtures/flac/tone16.flac");
    const int h = flac_parse_header(f.data(), (int)f.size(), &si);
    CHECK(h > 42 && si.sample_rate == 44100 && si.channels == 2 && si.bps == 16 && si.total_samples == 11025 && si.min_block > 0 && si.max_block >= si.min_block);
    for (int cut : { 0, 1, 3, 4, 7, 8, 20, 41, h - 1 }) CHECK(flac_parse_header(f.data(), cut, &si) == 0);       // not all there yet
    CHECK(flac_parse_header((const uint8_t *)"RIFFxxxx", 8, &si) == -1);
    CHECK(flac_parse_header((const uint8_t *)"fLa", 3, &si) == 0);
    CHECK(flac_parse_header((const uint8_t *)"fLx", 3, &si) == -1);
    CHECK(flac_parse_header((const uint8_t *)"", 0, &si) == 0);
    // no STREAMINFO: a padding block only
    const uint8_t pad[] = { 'f', 'L', 'a', 'C', 0x81, 0, 0, 4, 0, 0, 0, 0 };
    CHECK(flac_parse_header(pad, (int)sizeof pad, &si) == -1);
    // a stream that is the first block plus others (comment, picture) and ends there
    Bytes more(f.begin(), f.begin() + 8 + 34);
    more[4] &= 0x7F;                                          // STREAMINFO is not the last block
    const uint8_t comment[] = { 0x04, 0, 0, 6, 1, 2, 3, 4, 5, 6 };
    const uint8_t last[] = { 0x86, 0, 0, 3, 9, 9, 9 };
    more.insert(more.end(), comment, comment + sizeof comment);
    more.insert(more.end(), last, last + sizeof last);
    CHECK(flac_parse_header(more.data(), (int)more.size(), &si) == (int)more.size() && si.sample_rate == 44100);
    CHECK(flac_parse_header(more.data(), (int)more.size() - 1, &si) == 0);
}

int main() {
    fixtures();
    synthetic();
    damage();
    headers();
    printf("flac: %d checks, %d failed\n", s_checks, s_failed);
    return s_failed ? 1 : 0;
}
