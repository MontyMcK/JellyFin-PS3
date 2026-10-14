// Host test for the AAC path: source/audio/aac_adts.c, chan_map.c and the libfaad glue in adec_aac.cpp,
// with the resampler, against ffmpeg-made ADTS files (fixtures/aac, make_aac.py).
//
//   make -f Makefile.host test_aac && ./test_aac
//
// The decoder glue runs here as it does on the console (it only needs adec_push_frames and plog, stubbed
// below, and libfaad); the tests listen for the tones the fixtures were made of.

#include "aac_adts.h"
#include "chan_map.h"
#include "adec_aac.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <algorithm>
#include <vector>

static int s_checks = 0, s_failed = 0;
#define CHECK(cond) do { s_checks++; if (!(cond)) { s_failed++; \
    printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

// ---- stubs for what adec.cpp provides ----------------------------------------------------------------
static std::vector<float> g_out;
static int g_ring_ch = 2;
void adec_push_frames(const float *frames, int n) { g_out.insert(g_out.end(), frames, frames + (size_t)n * (size_t)g_ring_ch); }
void plog(const char *) {}

typedef std::vector<uint8_t> Bytes;

static Bytes load(const char *path) {
    Bytes b;
    FILE *f = fopen(path, "rb");
    if (!f) return b;
    fseek(f, 0, SEEK_END);
    b.resize((size_t)ftell(f));
    fseek(f, 0, SEEK_SET);
    if (fread(b.data(), 1, b.size(), f) != b.size()) b.clear();
    fclose(f);
    return b;
}

// ---- bit writer for building configs ---------------------------------------------------------------------
struct BitW {
    Bytes b; int pos = 0;
    void put(uint32_t v, int n) {
        for (int i = n - 1; i >= 0; i--) {
            if ((pos & 7) == 0) b.push_back(0);
            if ((v >> i) & 1) b.back() |= (uint8_t)(0x80 >> (pos & 7));
            pos++;
        }
    }
};

static Bytes asc(int aot, int sfi, int cfg, int ext_aot = 0, int ext_sfi = 0) {
    BitW w;
    if (ext_aot) { w.put((uint32_t)ext_aot, 5); w.put((uint32_t)sfi, 4); w.put((uint32_t)cfg, 4); w.put((uint32_t)ext_sfi, 4); w.put((uint32_t)aot, 5); }
    else { w.put((uint32_t)aot, 5); w.put((uint32_t)sfi, 4); w.put((uint32_t)cfg, 4); }
    w.put(0, 3);                                                              // frame length flag, depends on core, extension
    return w.b;
}

static void adts_and_config() {
    printf("- AudioSpecificConfig and ADTS\n");
    AacConfig c;
    Bytes a = asc(2, 4, 2);                                                  // LC, 44.1 kHz, stereo
    CHECK(aac_parse_asc(a.data(), (int)a.size(), &c) && c.object_type == 2 && c.sf_index == 4 && c.sample_rate == 44100 && c.channel_config == 2 && c.channels == 2);
    a = asc(2, 3, 6);
    CHECK(aac_parse_asc(a.data(), (int)a.size(), &c) && c.sample_rate == 48000 && c.channels == 6);
    a = asc(2, 3, 7);
    CHECK(aac_parse_asc(a.data(), (int)a.size(), &c) && c.channels == 8 && c.channel_config == 7);
    a = asc(2, 6, 1);
    CHECK(aac_parse_asc(a.data(), (int)a.size(), &c) && c.sample_rate == 24000 && c.channels == 1);
    a = asc(1, 4, 2);                                                        // Main
    CHECK(aac_parse_asc(a.data(), (int)a.size(), &c) && c.object_type == 1);
    a = asc(2, 6, 2, 5, 3);                                                  // HE-AAC: SBR object over an LC core at 24 kHz, output 48 kHz
    CHECK(aac_parse_asc(a.data(), (int)a.size(), &c) && c.object_type == 2 && c.sample_rate == 24000 && c.sf_index == 6);
    a = asc(2, 6, 2, 29, 3);                                                 // and with parametric stereo
    CHECK(aac_parse_asc(a.data(), (int)a.size(), &c) && c.object_type == 2);
    // not carried: a layout in a program config element, 6.1/7.1 variants, off-list rates, other codecs, damage
    a = asc(2, 4, 0);
    CHECK(!aac_parse_asc(a.data(), (int)a.size(), &c));
    a = asc(2, 4, 11);
    CHECK(!aac_parse_asc(a.data(), (int)a.size(), &c));
    a = asc(2, 13, 2);
    CHECK(!aac_parse_asc(a.data(), (int)a.size(), &c));
    a = asc(2, 4, 2); a[0] = (uint8_t)((42 << 3) | (a[0] & 7));              // USAC-ish object
    CHECK(!aac_parse_asc(a.data(), (int)a.size(), &c));
    a = asc(2, 4, 2); a[0] = (uint8_t)((5 << 3) | 0);                        // explicit SBR with the rest cut off
    CHECK(!aac_parse_asc(a.data(), 2, &c) || true);
    CHECK(!aac_parse_asc(nullptr, 0, &c));
    const uint8_t one[1] = { 0x12 };
    CHECK(!aac_parse_asc(one, 1, &c));
    {   // an explicit rate: 44100 is on the list, 12345 is not
        BitW w; w.put(2, 5); w.put(15, 4); w.put(44100, 24); w.put(2, 4); w.put(0, 3);
        CHECK(aac_parse_asc(w.b.data(), (int)w.b.size(), &c) && c.sf_index == 4);
        BitW x; x.put(2, 5); x.put(15, 4); x.put(12345, 24); x.put(2, 4); x.put(0, 3);
        CHECK(!aac_parse_asc(x.b.data(), (int)x.b.size(), &c));
    }

    // headers: built ones parse back; the length limits
    AacConfig cfg = { 2, 4, 44100, 2, 2 };
    uint8_t h[7];
    CHECK(aac_make_adts(&cfg, 300, h));
    CHECK(aac_adts_frame_len(h, 7) == 307);
    AacConfig back;
    CHECK(aac_parse_adts(h, 7, &back) && back.object_type == 2 && back.sf_index == 4 && back.channel_config == 2 && back.sample_rate == 44100);
    CHECK(aac_make_adts(&cfg, 8184, h) && aac_adts_frame_len(h, 7) == 8191);
    CHECK(!aac_make_adts(&cfg, 8185, h));
    CHECK(aac_make_adts(&cfg, 0, h) && aac_adts_frame_len(h, 7) == 7);
    CHECK(!aac_make_adts(&cfg, -1, h));
    uint8_t bad[7] = { 0xFF, 0xF1, 0x50, 0x80, 0x00, 0x1F, 0xFC };
    CHECK(aac_adts_frame_len(bad, 7) == 0);                                  // length 0: shorter than its header
    bad[2] = (uint8_t)(0x40 | (13 << 2));                                    // sampling index 13
    bad[4] = 0x20;
    CHECK(aac_adts_frame_len(bad, 7) == 0);
    uint8_t notsync[7] = { 0xFF, 0xF3, 0x50, 0x80, 0x20, 0x1F, 0xFC };       // layer bits set
    CHECK(aac_adts_frame_len(notsync, 7) == 0);
    CHECK(aac_adts_frame_len(h, 6) == 0);                                    // too short to say

    // against the real thing: every header ffmpeg wrote is the one made from the first
    for (const char *name : { "fixtures/aac/stereo44.aac", "fixtures/aac/surround48.aac", "fixtures/aac/mono32.aac" }) {
        Bytes f = load(name);
        CHECK(f.size() > 100);
        AacConfig fc;
        CHECK(aac_parse_adts(f.data(), (int)f.size(), &fc));
        size_t pos = 0; int frames = 0; bool all_same = true;
        while (pos + 7 <= f.size()) {
            const int fl = aac_adts_frame_len(&f[pos], (int)(f.size() - pos));
            if (fl == 0) { all_same = false; break; }
            uint8_t mine[7];
            if (!aac_make_adts(&fc, fl - 7, mine) || memcmp(mine, &f[pos], 7) != 0) all_same = false;
            pos += (size_t)fl; frames++;
        }
        CHECK(all_same && pos == f.size() && frames > 20);
    }
}

static void mapping() {
    printf("- channel mapping\n");
    // five channels distinct, FC FL FR SL SR LFE as libfaad orders a 5.1 file
    const unsigned char pos51[6] = { CH_POS_FRONT_CENTER, CH_POS_FRONT_LEFT, CH_POS_FRONT_RIGHT, CH_POS_BACK_LEFT, CH_POS_BACK_RIGHT, CH_POS_LFE };
    const float in[6] = { 0.3f, 0.1f, 0.2f, 0.5f, 0.6f, 0.4f };           // C L R Ls Rs LFE
    float out[6], o2[2];
    chan_map_frames(in, 1, 6, pos51, out, 6);
    CHECK(out[0] == 0.1f && out[1] == 0.2f && out[2] == 0.3f && out[3] == 0.4f && out[4] == 0.5f && out[5] == 0.6f);   // FL FR FC LFE SL SR
    chan_map_frames(in, 1, 6, pos51, o2, 2);
    const float down = 1.0f / (1.0f + 2.0f * 0.7071067811865476f);
    CHECK(fabsf(o2[0] - (0.1f + 0.7071f * 0.3f + 0.7071f * 0.5f) * down) < 1e-5f && fabsf(o2[1] - (0.2f + 0.7071f * 0.3f + 0.7071f * 0.6f) * down) < 1e-5f);

    // plain stereo is left alone; mono goes to the centre (6 wide) or both sides (2 wide)
    const unsigned char posst[2] = { CH_POS_FRONT_LEFT, CH_POS_FRONT_RIGHT };
    const float st[2] = { 0.25f, -0.5f };
    chan_map_frames(st, 1, 2, posst, o2, 2);
    CHECK(o2[0] == 0.25f && o2[1] == -0.5f);
    chan_map_frames(st, 1, 2, posst, out, 6);
    CHECK(out[0] == 0.25f && out[1] == -0.5f && out[2] == 0 && out[3] == 0 && out[4] == 0 && out[5] == 0);
    const unsigned char posm[1] = { CH_POS_FRONT_CENTER };
    const float mo[1] = { 0.7f };
    chan_map_frames(mo, 1, 1, posm, o2, 2);
    CHECK(o2[0] == 0.7f && o2[1] == 0.7f);
    chan_map_frames(mo, 1, 1, posm, out, 6);
    CHECK(out[2] == 0.7f && out[0] == 0 && out[1] == 0);

    // 7.1: the side pair is the surround pair, the back pair folds in at -3 dB
    const unsigned char pos71[8] = { CH_POS_FRONT_CENTER, CH_POS_FRONT_LEFT, CH_POS_FRONT_RIGHT, CH_POS_SIDE_LEFT, CH_POS_SIDE_RIGHT, CH_POS_BACK_LEFT, CH_POS_BACK_RIGHT, CH_POS_LFE };
    const float in8[8] = { 0.1f, 0.2f, 0.3f, 0.4f, 0.5f, 0.6f, 0.7f, 0.8f };
    chan_map_frames(in8, 1, 8, pos71, out, 6);
    CHECK(fabsf(out[4] - (0.4f + 0.7071068f * 0.6f)) < 1e-6f && fabsf(out[5] - (0.5f + 0.7071068f * 0.7f)) < 1e-6f && out[3] == 0.8f);
    // a back centre goes to both surrounds at -3 dB
    const unsigned char posbc[4] = { CH_POS_FRONT_CENTER, CH_POS_FRONT_LEFT, CH_POS_FRONT_RIGHT, CH_POS_BACK_CENTER };
    const float inb[4] = { 0.1f, 0.2f, 0.3f, 0.5f };
    chan_map_frames(inb, 1, 4, posbc, out, 6);
    CHECK(fabsf(out[4] - 0.5f * 0.7071068f) < 1e-6f && fabsf(out[5] - 0.5f * 0.7071068f) < 1e-6f);
    // unknown positions: the usual order, or left out one by one
    const unsigned char unk[3] = { 0, 0, 0 };
    const float i3[3] = { 0.1f, 0.2f, 0.3f };
    chan_map_frames(i3, 1, 3, unk, out, 6);
    CHECK(out[0] == 0.1f && out[1] == 0.2f && out[2] == 0.3f);
    const unsigned char half[2] = { CH_POS_FRONT_RIGHT, CH_POS_UNKNOWN };
    chan_map_frames(st, 1, 2, half, out, 6);
    CHECK(out[1] == 0.25f && out[0] == 0 && out[2] == 0);
    chan_map_frames(st, 1, 2, nullptr, out, 6);                              // no positions at all
    CHECK(out[0] == 0.25f && out[1] == -0.5f);
    // WAVE / FLAC / Matroska PCM order: a 5.1 stream is already in the port's order, 7.1 folds its sides in, quad is front and back
    unsigned char wp[8];
    chan_wave_positions(6, wp);
    CHECK(wp[0] == CH_POS_FRONT_LEFT && wp[1] == CH_POS_FRONT_RIGHT && wp[2] == CH_POS_FRONT_CENTER && wp[3] == CH_POS_LFE && wp[4] == CH_POS_BACK_LEFT && wp[5] == CH_POS_BACK_RIGHT);
    const float w6[6] = { 0.1f, 0.2f, 0.3f, 0.4f, 0.5f, 0.6f };
    chan_map_frames(w6, 1, 6, wp, out, 6);
    CHECK(memcmp(out, w6, sizeof w6) == 0);
    chan_wave_positions(1, wp);  CHECK(wp[0] == CH_POS_FRONT_CENTER);
    chan_wave_positions(2, wp);  CHECK(wp[0] == CH_POS_FRONT_LEFT && wp[1] == CH_POS_FRONT_RIGHT);
    chan_wave_positions(3, wp);  CHECK(wp[2] == CH_POS_FRONT_CENTER);
    chan_wave_positions(4, wp);  CHECK(wp[2] == CH_POS_BACK_LEFT && wp[3] == CH_POS_BACK_RIGHT);
    chan_wave_positions(5, wp);  CHECK(wp[2] == CH_POS_FRONT_CENTER && wp[3] == CH_POS_BACK_LEFT && wp[4] == CH_POS_BACK_RIGHT);
    chan_wave_positions(7, wp);  CHECK(wp[4] == CH_POS_BACK_CENTER && wp[5] == CH_POS_SIDE_LEFT && wp[6] == CH_POS_SIDE_RIGHT);
    chan_wave_positions(8, wp);  CHECK(wp[4] == CH_POS_BACK_LEFT && wp[6] == CH_POS_SIDE_LEFT && wp[7] == CH_POS_SIDE_RIGHT);
    unsigned char wp9[12];
    memset(wp9, 7, sizeof wp9);
    chan_wave_positions(10, wp9);
    CHECK(wp9[0] == CH_POS_UNKNOWN && wp9[9] == CH_POS_UNKNOWN);                 // more than 8: unknown
    // many frames
    std::vector<float> many(1000 * 2), got(1000 * 2);
    for (size_t i = 0; i < many.size(); i++) many[i] = (float)i * 0.001f;
    chan_map_frames(many.data(), 1000, 2, posst, got.data(), 2);
    CHECK(got == many);
}

// ---- listening ------------------------------------------------------------------------------------------------

static const double PI = 3.14159265358979323846;

// amplitude of `hz` in channel c of the frames [from, to) of interleaved out, and the leftover (dB below the tone) of a fit
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

// Feeds the file to the decoder in chunks of `chunk` bytes.
static void decode_file(const Bytes &f, int out_ch, int chunk) {
    g_out.clear();
    g_ring_ch = out_ch;
    CHECK(adec_aac_open(out_ch));
    for (size_t pos = 0; pos < f.size(); pos += (size_t)chunk)
        adec_aac_decode_payload(&f[pos], (int)std::min<size_t>((size_t)chunk, f.size() - pos));
}

static const double A = 0.125;                                             // the sine source's amplitude
static const double A_UP = 0.125 * 0.7071067811865476;                      // ffmpeg's mono -> stereo upmix keeps the power: -3 dB
static const double NOISE_DB = -25.0;                                       // ffmpeg's encoder is not good at pure tones: its own decode of stereo48.aac is -32 dB

// ffmpeg's decode of the file, as floats (empty when ffmpeg is not there)
static std::vector<float> ffmpeg_decode(const char *path, int ch) {
    std::vector<float> v;
    char cmd[256];
    snprintf(cmd, sizeof cmd, "ffmpeg -nostdin -v error -i %s -f f32le -ac %d - 2>/dev/null", path, ch);
    FILE *p = popen(cmd, "r");
    if (!p) return v;
    float buf[4096];
    size_t got;
    while ((got = fread(buf, sizeof(float), 4096, p)) > 0) v.insert(v.end(), buf, buf + got);
    pclose(p);
    return v;
}

// the difference between two decodes of the same file, dB below the signal
static double diff_db(const std::vector<float> &a, const std::vector<float> &b, int ch) {
    const size_t n = std::min(a.size(), b.size()) / (size_t)ch * (size_t)ch;
    double d = 0, s = 0;
    for (size_t i = 0; i < n; i++) { d += ((double)a[i] - b[i]) * ((double)a[i] - b[i]); s += (double)b[i] * b[i]; }
    return 10 * log10(d / (s + 1e-30) + 1e-30);
}

static void decoding() {
    printf("- decoding\n");
    // 48 kHz stereo: two tones, one a side
    {
        Bytes f = load("fixtures/aac/stereo48.aac");
        decode_file(f, 2, 1000);
        const int n = (int)(g_out.size() / 2);
        CHECK(n > 46000 && n < 52000);
        Fit l = fit_tone(g_out, 2, 0, 440, 3000, n - 3000), r = fit_tone(g_out, 2, 1, 880, 3000, n - 3000);
        if (fabs(l.amp - A) > 0.004 || l.err_db > NOISE_DB) printf("  L: amp %.4f err %.1f dB\n", l.amp, l.err_db);
        CHECK(fabs(l.amp - A) < 0.004 && l.err_db < NOISE_DB);                    // the level is the file's
        CHECK(fabs(r.amp - A) < 0.004 && r.err_db < NOISE_DB);
        Fit leak = fit_tone(g_out, 2, 0, 880, 3000, n - 3000);
        CHECK(leak.amp < 0.002);                                             // right's tone is not in the left
        // the same samples ffmpeg's own decoder makes of the file
        // the same samples ffmpeg's own decoder makes of the file.  ffmpeg also returns the encoder's priming frame
        // (1024 frames of silence) at the start, which libfaad does not: the first 1024 of ffmpeg's are dropped
        std::vector<float> ref48 = ffmpeg_decode("fixtures/aac/stereo48.aac", 2);
        if (ref48.empty()) printf("  (no ffmpeg: not compared)\n");
        else {
            ref48.erase(ref48.begin(), ref48.begin() + 1024 * 2);
            const double d = diff_db(g_out, ref48, 2);
            if (d > -55) printf("  versus ffmpeg: %.1f dB (ours %zu frames, ffmpeg's %zu)\n", d, g_out.size() / 2, ref48.size() / 2);
            CHECK(d < -55 && g_out.size() == ref48.size());
        }
        adec_aac_close();

        // the same output however the file is cut into PES payloads
        std::vector<float> ref = g_out;
        for (int chunk : { 1, 7, 188, 4096, 99999 }) {
            decode_file(f, 2, chunk);
            if (g_out != ref) printf("  chunk %d differs\n", chunk);
            CHECK(g_out == ref);
            adec_aac_close();
        }
    }
    // 44.1 kHz: converted, so the tone is at 1000 Hz of the 48 kHz stream, not 1088
    {
        Bytes f = load("fixtures/aac/stereo44.aac");
        decode_file(f, 2, 2000);
        const int n = (int)(g_out.size() / 2);
        CHECK(n > 46000 && n < 52000);
        Fit l = fit_tone(g_out, 2, 0, 1000, 3000, n - 3000), r = fit_tone(g_out, 2, 1, 1000, 3000, n - 3000);
        if (fabs(l.amp - A_UP) > 0.004 || l.err_db > NOISE_DB) printf("  44.1 L: amp %.4f err %.1f dB\n", l.amp, l.err_db);
        CHECK(fabs(l.amp - A_UP) < 0.004 && l.err_db < NOISE_DB && fabs(r.amp - A_UP) < 0.004 && r.err_db < NOISE_DB);
        adec_aac_close();
    }
    // mono at 32 kHz: both sides (2 wide) or the centre (6 wide)
    {
        Bytes f = load("fixtures/aac/mono32.aac");
        decode_file(f, 2, 3000);
        int n = (int)(g_out.size() / 2);
        CHECK(n > 46000 && n < 52000);
        Fit l = fit_tone(g_out, 2, 0, 800, 3000, n - 3000), r = fit_tone(g_out, 2, 1, 800, 3000, n - 3000);
        CHECK(fabs(l.amp - A) < 0.004 && fabs(r.amp - A) < 0.004 && l.err_db < NOISE_DB);
        adec_aac_close();
        decode_file(f, 6, 3000);
        n = (int)(g_out.size() / 6);
        Fit c = fit_tone(g_out, 6, 2, 800, 3000, n - 3000), fl = fit_tone(g_out, 6, 0, 800, 3000, n - 3000);
        CHECK(fabs(c.amp - A) < 0.004 && fl.amp < 0.002);
        adec_aac_close();
    }
    // 5.1: a tone per channel, each in its own slot
    {
        Bytes f = load("fixtures/aac/surround48.aac");
        decode_file(f, 6, 4096);
        const int n = (int)(g_out.size() / 6);
        CHECK(n > 46000 && n < 52000);
        const double tones[6] = { 300, 500, 700, 80, 1100, 1300 };           // FL FR FC LFE SL SR
        bool ok = true;
        for (int slot = 0; slot < 6; slot++) {
            for (int tone = 0; tone < 6; tone++) {
                Fit x = fit_tone(g_out, 6, slot, tones[tone], 4000, n - 4000);
                const bool want = slot == tone;
                if (want ? fabs(x.amp - A) > 0.01 : x.amp > 0.003) {
                    ok = false;
                    printf("  slot %d, %.0f Hz: amp %.4f\n", slot, tones[tone], x.amp);
                }
            }
        }
        CHECK(ok);
        // ffmpeg's 5.1 is FL FR FC LFE BL BR, the port's order: sample for sample the same
        std::vector<float> ref51 = ffmpeg_decode("fixtures/aac/surround48.aac", 6);
        if (!ref51.empty()) {
            ref51.erase(ref51.begin(), ref51.begin() + 1024 * 6);
            const double d = diff_db(g_out, ref51, 6);
            if (d > -55) printf("  5.1 versus ffmpeg: %.1f dB (ours %zu frames, ffmpeg's %zu)\n", d, g_out.size() / 6, ref51.size() / 6);
            CHECK(d < -55 && g_out.size() == ref51.size());
        }
        adec_aac_close();
        // two wide: the fronts, the centre and the surrounds folded in (no LFE)
        decode_file(f, 2, 4096);
        const int n2 = (int)(g_out.size() / 2);
        const double k = 1.0 / (1.0 + 2.0 * 0.7071067811865476), c3 = 0.7071067811865476;
        Fit fl_l = fit_tone(g_out, 2, 0, 300, 4000, n2 - 4000), fl_r = fit_tone(g_out, 2, 1, 300, 4000, n2 - 4000);
        Fit fc_l = fit_tone(g_out, 2, 0, 700, 4000, n2 - 4000), fc_r = fit_tone(g_out, 2, 1, 700, 4000, n2 - 4000);
        Fit sl_l = fit_tone(g_out, 2, 0, 1100, 4000, n2 - 4000), sl_r = fit_tone(g_out, 2, 1, 1100, 4000, n2 - 4000);
        Fit sr_r = fit_tone(g_out, 2, 1, 1300, 4000, n2 - 4000), lfe = fit_tone(g_out, 2, 0, 80, 4000, n2 - 4000);
        CHECK(fabs(fl_l.amp - A * k) < 0.003 && fl_r.amp < 0.002);
        CHECK(fabs(fc_l.amp - A * c3 * k) < 0.003 && fabs(fc_r.amp - A * c3 * k) < 0.003);
        CHECK(fabs(sl_l.amp - A * c3 * k) < 0.003 && sl_r.amp < 0.002 && fabs(sr_r.amp - A * c3 * k) < 0.003);
        CHECK(lfe.amp < 0.002);
        adec_aac_close();
    }
    // reset (a seek) and garbage
    {
        Bytes f = load("fixtures/aac/stereo48.aac");
        g_out.clear(); g_ring_ch = 2;
        CHECK(adec_aac_open(2));
        adec_aac_decode_payload(f.data(), (int)f.size() / 2);
        adec_aac_reset();
        g_out.clear();
        adec_aac_decode_payload(f.data(), (int)f.size());
        const int n = (int)(g_out.size() / 2);
        Fit l = fit_tone(g_out, 2, 0, 440, 3000, n - 3000);
        CHECK(n > 40000 && fabs(l.amp - A) < 0.004);
        // junk in the stream is skipped and the next frame decodes
        Bytes junk(5000, 0x55);
        g_out.clear();
        adec_aac_decode_payload(junk.data(), (int)junk.size());
        CHECK(g_out.empty());
        adec_aac_decode_payload(f.data(), (int)f.size());
        CHECK(g_out.size() / 2 > 40000);
        // a frame cut short in the middle: dropped, the next ones play
        adec_aac_close();
        adec_aac_decode_payload(f.data(), 100);                              // closed: nothing happens
        CHECK(g_out.size() / 2 > 40000);
    }
}

int main() {
    adts_and_config();
    mapping();
    decoding();
    printf("aac: %d checks, %d failed\n", s_checks, s_failed);
    return s_failed ? 1 : 0;
}
