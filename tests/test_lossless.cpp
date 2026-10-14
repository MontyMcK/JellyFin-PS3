// Host test for FLAC and PCM in a Matroska file, end to end as the player runs them: the file through
// video/mkv_ts, the transport stream through the player's own demuxer (ts_demux.cpp), each audio PES cut
// into queue slots the way adec.cpp cuts it, and the slots into adec_flac.cpp / adec_pcm.cpp (with
// adec_out.cpp, chan_map.c, resample.c and flac_dec.c).  What comes out is listened to: the tones the
// fixture (fixtures/flac/flac_pcm.mkv, make_flac.py) was made of, at the port's rate, in the port's slots.
//
//   make -f Makefile.host test_lossless && ./test_lossless

#include "mkv_ts.h"
#include "ts_demux.h"
#include "adec_flac.h"
#include "adec_pcm.h"

#include <codec/vdec.h>   // (hoststub)
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <algorithm>
#include <vector>

static int s_checks = 0, s_failed = 0;
#define CHECK(cond) do { s_checks++; if (!(cond)) { s_failed++; \
    printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

// ---- stubs for what adec.cpp and the player provide ---------------------------------------------------------
static std::vector<float> g_out;
static int g_ring_ch = 2;
void adec_push_frames(const float *frames, int n) { g_out.insert(g_out.end(), frames, frames + (size_t)n * (size_t)g_ring_ch); }
void plog(const char *) {}

typedef std::vector<uint8_t> Bytes;

static int file_read(void *ctx, uint64_t off, uint8_t *buf, int len) {
    FILE *f = (FILE *)ctx;
    if (fseeko(f, (off_t)off, SEEK_SET) != 0) return -1;
    return (int)fread(buf, 1, (size_t)len, f);
}

// The audio PES of the stream mkv_ts makes of `track` (from `start_ns`), complete, as the demuxer hands them over.
static std::vector<Bytes> audio_pes(MkvFile *f, int track, uint64_t start_ns, int *codec) {
    std::vector<Bytes> out;
    MkvTs *t = mkv_ts_open(f, 1, track, start_ns, nullptr, nullptr, 0);
    if (!t) return out;
    Bytes ts, buf(188 * 64);
    for (;;) {
        const int n = mkv_ts_read(t, buf.data(), (int)buf.size());
        if (n <= 0) break;
        ts.insert(ts.end(), buf.begin(), buf.begin() + n);
    }
    mkv_ts_close(t);
    TSState *st = (TSState *)calloc(1, sizeof(TSState));
    static uint8_t vpes[TS_VPES_BUF_SIZE], apes[TS_APES_BUF_SIZE];
    auto take = [&](int alen) { out.emplace_back(apes, apes + alen); };
    for (size_t i = 0; i + 188 <= ts.size(); i += 188) {
        int vlen = 0, alen = 0;
        if (ts_process(st, &ts[i], vpes, &vlen, apes, &alen) & 2) take(alen);
    }
    // the PES still being assembled: flushed by a packet that starts another
    if (st->audio_pid) {
        uint8_t p[188]; memset(p, 0xFF, sizeof p);
        p[0] = 0x47; p[1] = (uint8_t)(0x40 | (st->audio_pid >> 8)); p[2] = (uint8_t)st->audio_pid; p[3] = 0x10;
        p[4] = 0; p[5] = 0; p[6] = 1;
        int vlen = 0, alen = 0;
        if (ts_process(st, p, vpes, &vlen, apes, &alen) & 2) take(alen);
    }
    *codec = st->audio_codec;
    free(st);
    return out;
}

// Cuts a PES into slots of `slot` bytes and hands them to the codec as adec.cpp does: the first chunk loses its PES header.
template <typename F> static void feed(const Bytes &pes, size_t slot, F &&sink) {
    size_t pos = 0;
    bool cont = false;
    while (pos < pes.size()) {
        const size_t n = std::min(slot, pes.size() - pos);
        if (!cont) { const size_t hdr = 9 + pes[8]; sink(pes.data() + pos + hdr, (int)(n - hdr), true); }
        else sink(pes.data() + pos, (int)n, false);
        pos += n;
        cont = true;
    }
}

static const double PI = 3.14159265358979323846;
struct Fit { double amp, err_db; };
static Fit fit_tone(const std::vector<float> &out, int ch, int c, double hz, int from, int to) {
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
    const double resid = std::max(0.0, yy - (a * ys + b * yc));
    return { amp, 20 * log10(sqrt(resid / n) / (amp / sqrt(2.0) + 1e-30) + 1e-30) };
}

// Plays the track's PES through the matching decoder; leaves the output in g_out.
static void play(const std::vector<Bytes> &pes, int codec, int out_ch, size_t slot) {
    g_out.clear();
    g_ring_ch = out_ch;
    if (codec == TS_AUDIO_FLAC) {
        CHECK(adec_flac_open(out_ch));
        for (const Bytes &p : pes) feed(p, slot, [](const uint8_t *es, int n, bool) { adec_flac_decode_payload(es, n); });
        adec_flac_close();
    } else {
        CHECK(adec_pcm_open(out_ch));
        for (const Bytes &p : pes) feed(p, slot, [](const uint8_t *es, int n, bool head) { adec_pcm_decode_payload(es, n, head); });
        adec_pcm_close();
    }
}

static const double A = 0.125;                                    // the sine source's amplitude
static const double A_ST = 0.125 * 0.7071067811865476;           // ffmpeg's mono -> stereo upmix keeps the power
static const double QUANT_DB = -70.0;                              // a tone of amplitude 0.1 rounded to 16 bits is ~-77 dB under itself

int main() {
    FILE *fp = fopen("fixtures/flac/flac_pcm.mkv", "rb");
    CHECK(fp != nullptr);
    if (!fp) return 1;
    fseeko(fp, 0, SEEK_END);
    MkvFile f;
    CHECK(mkv_open(&f, file_read, fp, (uint64_t)ftello(fp)) == 0);

    printf("- FLAC stereo 44.1 kHz and PCM s16le: the same samples, the same sound\n");
    int codec_flac = 0, codec_pcm = 0;
    std::vector<Bytes> flac = audio_pes(&f, 2, 0, &codec_flac), pcm16 = audio_pes(&f, 3, 0, &codec_pcm);
    CHECK(codec_flac == TS_AUDIO_FLAC && codec_pcm == TS_AUDIO_PCM && flac.size() >= 2 && pcm16.size() >= 2);
    play(flac, codec_flac, 2, 3072);
    const std::vector<float> flac_out = g_out;
    const int n = (int)(flac_out.size() / 2);
    CHECK(n > 11000 && n <= 12010);                                // a quarter second at 48 kHz, the last few milliseconds waiting in the filter
    Fit l = fit_tone(flac_out, 2, 0, 1000, 400, n - 400), r = fit_tone(flac_out, 2, 1, 1000, 400, n - 400);
    if (fabs(l.amp - A_ST) > 0.0005 || l.err_db > QUANT_DB) printf("  FLAC L: amp %.5f err %.1f dB\n", l.amp, l.err_db);
    CHECK(fabs(l.amp - A_ST) < 0.0005 && l.err_db < QUANT_DB && fabs(r.amp - A_ST) < 0.0005 && r.err_db < QUANT_DB);   // lossless: what is left is the source's 16-bit rounding
    play(pcm16, codec_pcm, 2, 3072);
    CHECK(g_out == flac_out);                                      // PCM and FLAC of the same samples: bit for bit
    // however the PES are cut into slots (a frame cut in the middle of its samples or its bits)
    for (size_t slot : { (size_t)32, (size_t)100, (size_t)777, (size_t)3072, (size_t)65536 }) {
        play(flac, codec_flac, 2, slot);
        const bool a = g_out == flac_out;
        play(pcm16, codec_pcm, 2, slot);
        const bool b = g_out == flac_out;
        if (!a || !b) printf("  slot %zu: flac %s, pcm %s\n", slot, a ? "ok" : "differs", b ? "ok" : "differs");
        CHECK(a && b);
    }
    // folded to a 6-wide ring, stereo stays in the fronts
    play(flac, codec_flac, 6, 3072);
    CHECK(g_out.size() / 6 == flac_out.size() / 2);
    {
        bool fronts = true;
        for (size_t i = 0; i < g_out.size() / 6 && fronts; i++) {
            if (g_out[i * 6] != flac_out[i * 2] || g_out[i * 6 + 1] != flac_out[i * 2 + 1]) fronts = false;
            for (int c = 2; c < 6; c++) if (g_out[i * 6 + (size_t)c] != 0.0f) fronts = false;
        }
        CHECK(fronts);
    }

    printf("- PCM s24le 5.1 at 16 kHz: a tone in each slot\n");
    {
        int codec = 0;
        std::vector<Bytes> pes = audio_pes(&f, 4, 0, &codec);
        play(pes, codec, 6, 3072);
        const int n6 = (int)(g_out.size() / 6);
        CHECK(n6 > 11000 && n6 <= 12010);
        const double tones[6] = { 300, 500, 700, 80, 1100, 1300 };       // FL FR FC LFE BL BR: the port's order
        bool ok = true;
        for (int slot = 0; slot < 6; slot++)
            for (int tone = 0; tone < 6; tone++) {
                const Fit x = fit_tone(g_out, 6, slot, tones[tone], 400, n6 - 400);
                if (slot == tone ? fabs(x.amp - A) > 0.002 : x.amp > 0.002) { ok = false; printf("  slot %d, %.0f Hz: amp %.5f\n", slot, tones[tone], x.amp); }
            }
        CHECK(ok);
        // the same, however it is cut
        const std::vector<float> ref = g_out;
        for (size_t slot : { (size_t)32, (size_t)100, (size_t)1001 }) { play(pes, codec, 6, slot); CHECK(g_out == ref); }
        // folded to two: the fronts, the centre and the surrounds at -3 dB under the AC-3 downmix's scaling, no LFE
        play(pes, codec, 2, 3072);
        const int n2 = (int)(g_out.size() / 2);
        const double k = 1.0 / (1.0 + 2.0 * 0.7071067811865476), c3 = 0.7071067811865476;
        CHECK(fabs(fit_tone(g_out, 2, 0, 300, 400, n2 - 400).amp - A * k) < 0.002 && fit_tone(g_out, 2, 1, 300, 400, n2 - 400).amp < 0.001);
        CHECK(fabs(fit_tone(g_out, 2, 0, 700, 400, n2 - 400).amp - A * c3 * k) < 0.002 && fabs(fit_tone(g_out, 2, 1, 700, 400, n2 - 400).amp - A * c3 * k) < 0.002);
        CHECK(fit_tone(g_out, 2, 0, 80, 400, n2 - 400).amp < 0.001);
    }

    printf("- PCM float mono at 32 kHz, and 16-bit big-endian stereo at 22.05 kHz\n");
    {
        int codec = 0;
        std::vector<Bytes> pes = audio_pes(&f, 5, 0, &codec);
        CHECK(codec == TS_AUDIO_PCM);
        play(pes, codec, 6, 3072);                                    // mono: the centre
        int n6 = (int)(g_out.size() / 6);
        CHECK(n6 > 11000 && fabs(fit_tone(g_out, 6, 2, 800, 400, n6 - 400).amp - A) < 0.001 && fit_tone(g_out, 6, 0, 800, 400, n6 - 400).amp < 0.0005);
        play(pes, codec, 2, 3072);                                    // or both sides
        n6 = (int)(g_out.size() / 2);
        CHECK(fabs(fit_tone(g_out, 2, 0, 800, 400, n6 - 400).amp - A) < 0.001 && fabs(fit_tone(g_out, 2, 1, 800, 400, n6 - 400).amp - A) < 0.001);
        pes = audio_pes(&f, 6, 0, &codec);
        play(pes, codec, 2, 3072);
        n6 = (int)(g_out.size() / 2);
        CHECK(n6 > 11000);
        const Fit l2 = fit_tone(g_out, 2, 0, 440, 400, n6 - 400), r2 = fit_tone(g_out, 2, 1, 880, 400, n6 - 400);
        CHECK(fabs(l2.amp - A) < 0.001 && l2.err_db < QUANT_DB && fabs(r2.amp - A) < 0.001 && r2.err_db < QUANT_DB);
        CHECK(fit_tone(g_out, 2, 0, 880, 400, n6 - 400).amp < 0.0005);
    }

    printf("- seeking: a stream that begins again\n");
    {
        int codec = 0;
        std::vector<Bytes> pes = audio_pes(&f, 2, 0, &codec);
        // half the stream, a flush, then the whole stream from its start (a new stream begins with the header)
        g_out.clear(); g_ring_ch = 2;
        CHECK(adec_flac_open(2));
        for (size_t i = 0; i < pes.size() / 2; i++) feed(pes[i], 3072, [](const uint8_t *es, int k, bool) { adec_flac_decode_payload(es, k); });
        adec_flac_reset();
        g_out.clear();
        for (const Bytes &p : pes) feed(p, 3072, [](const uint8_t *es, int k, bool) { adec_flac_decode_payload(es, k); });
        adec_flac_close();
        const std::vector<float> after = g_out;
        CHECK(after.size() > 20000 && fabs(fit_tone(after, 2, 0, 1000, 400, (int)(after.size() / 2) - 400).amp - A_ST) < 0.0005);
        // junk before the header, and a stream with no header at all, produce nothing and do no harm
        g_out.clear();
        CHECK(adec_flac_open(2));
        Bytes junk(5000, 0x55);
        adec_flac_decode_payload(junk.data(), (int)junk.size());
        CHECK(g_out.empty());
        feed(pes[1], 3072, [](const uint8_t *es, int k, bool) { adec_flac_decode_payload(es, k); });     // a frame without its header
        CHECK(g_out.empty());
        feed(pes[0], 3072, [](const uint8_t *es, int k, bool) { adec_flac_decode_payload(es, k); });     // the header and the first frame
        CHECK(!g_out.empty());
        adec_flac_close();
        // closed or not opened: ignored
        adec_flac_decode_payload(junk.data(), (int)junk.size());
        adec_pcm_decode_payload(junk.data(), (int)junk.size(), true);
        // PCM: a continuation with no header before it is ignored; a header of something else too
        CHECK(adec_pcm_open(2));
        g_out.clear();
        adec_pcm_decode_payload(junk.data(), 100, false);
        adec_pcm_decode_payload(junk.data(), 100, true);
        CHECK(g_out.empty());
        adec_pcm_close();
    }

    mkv_ts_release();
    mkv_close(&f);
    fclose(fp);
    printf("lossless: %d checks, %d failed\n", s_checks, s_failed);
    return s_failed ? 1 : 0;
}
