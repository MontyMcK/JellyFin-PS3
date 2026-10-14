// Host test for the parts of local playback that sit between a file and the decoders:
//   - the demuxer's audio-track choice (source/video/ts_demux.cpp, TSState.want_audio_pid), run over
//     the ffmpeg-made transport streams of fixtures/ts, the Blu-ray one stripped to 188-byte packets;
//   - the stream clock of a file entered part way (stream_local_clock).
//
//   make -f Makefile.host test_local_play && ./test_local_play

#include "ts_demux.h"
#include "local_probe.h"
#include "stream_local.h"

#include <codec/vdec.h>   // VDEC_TS_INVALID (hoststub)
#include <algorithm>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>

void plog(const char *) {}               // the demuxer's log line, not wanted here

static int s_checks = 0, s_failed = 0;
#define CHECK(cond) do { s_checks++; if (!(cond)) { s_failed++; \
    printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

typedef std::vector<uint8_t> Vec;

static Vec load(const char *path) {
    Vec v;
    FILE *f = fopen(path, "rb");
    if (!f) return v;
    fseek(f, 0, SEEK_END);
    v.resize((size_t)ftell(f));
    fseek(f, 0, SEEK_SET);
    if (fread(v.data(), 1, v.size(), f) != v.size()) v.clear();
    fclose(f);
    return v;
}

// The 188-byte transport stream inside a Blu-ray .m2ts, as the player's stream buffer strips it.
static Vec stripped(Vec raw) {
    raw.resize((size_t)local_m2ts_compact(raw.data(), (int)raw.size()));
    return raw;
}

struct Demuxed {
    int video_pid = 0, audio_pid = 0, audio_codec = 0;
    int audio_pes = 0;
    uint8_t first_audio_byte0 = 0, first_audio_byte1 = 0;     // the first two bytes of the first audio frame
    uint64_t first_video_pts = 0; bool have_video_pts = false;
};

static Demuxed demux(const Vec &ts, int want_pid) {
    Demuxed d;
    TSState *st = (TSState *)calloc(1, sizeof(TSState));
    static uint8_t vpes[TS_VPES_BUF_SIZE], apes[TS_APES_BUF_SIZE];
    st->want_audio_pid = (u16)want_pid;
    for (size_t i = 0; i + 188 <= ts.size(); i += 188) {
        int vlen = 0, alen = 0;
        const int r = ts_process(st, &ts[i], vpes, &vlen, apes, &alen);
        if ((r & 1) && !d.have_video_pts) {
            const uint8_t *es; int es_len; u64 pts;
            if (pes_payload(vpes, vlen, &es, &es_len, &pts) && pts != (u64)VDEC_TS_INVALID) { d.first_video_pts = pts; d.have_video_pts = true; }
        }
        if (r & 2) {
            const uint8_t *es; int es_len; u64 pts;
            if (pes_payload(apes, alen, &es, &es_len, &pts) && es_len >= 2) {
                if (d.audio_pes++ == 0) { d.first_audio_byte0 = es[0]; d.first_audio_byte1 = es[1]; }
            }
        }
    }
    d.video_pid = st->video_pid; d.audio_pid = st->audio_pid; d.audio_codec = st->audio_codec;
    free(st);
    return d;
}

static void audio_choice() {
    printf("- which audio stream the demuxer follows\n");
    const Vec av = load("fixtures/ts/av.ts");
    CHECK(!av.empty());
    // 0 = the first the PMT lists; else exactly that PID
    Demuxed d = demux(av, 0);
    CHECK(d.video_pid == 0x100 && d.audio_pid == 0x101 && d.audio_codec == TS_AUDIO_AC3);
    CHECK(d.audio_pes > 0 && d.first_audio_byte0 == 0x0B && d.first_audio_byte1 == 0x77);      // AC-3 syncword
    d = demux(av, 0x102);
    CHECK(d.audio_pid == 0x102 && d.audio_codec == TS_AUDIO_MP3 && d.audio_pes > 0);
    CHECK(d.first_audio_byte0 == 0xFF && (d.first_audio_byte1 & 0xE0) == 0xE0);               // an MPEG audio frame header
    d = demux(av, 0x101);
    CHECK(d.audio_pid == 0x101 && d.audio_codec == TS_AUDIO_AC3);
    d = demux(av, 0x555);                                                                      // not in the file: no audio, never another track
    CHECK(d.audio_pid == 0 && d.audio_pes == 0 && d.video_pid == 0x100);

    // Blu-ray layout, stripped: PIDs 0x1011 / 0x1100 / 0x1101, both Dolby Digital
    const Vec bd = stripped(load("fixtures/ts/av.m2ts"));
    CHECK(!bd.empty() && bd.size() % 188 == 0);
    d = demux(bd, 0);
    CHECK(d.video_pid == 0x1011 && d.audio_pid == 0x1100 && d.audio_codec == TS_AUDIO_AC3);
    d = demux(bd, 0x1101);
    CHECK(d.audio_pid == 0x1101 && d.audio_codec == TS_AUDIO_AC3 && d.audio_pes > 0);

    // AAC in a transport stream (type 0x0F) is picked, and chosen by PID like the rest
    d = demux(load("fixtures/ts/aac.ts"), 0);
    CHECK(d.audio_pid == 0x101 && d.audio_codec == TS_AUDIO_AAC && d.audio_pes > 0);
    CHECK(d.first_audio_byte0 == 0xFF && (d.first_audio_byte1 & 0xF6) == 0xF0);                  // an ADTS sync word

    // two lossless / surround tracks: the choice is by PID, whatever the codec
    const Vec hd = load("fixtures/ts/hd.ts");
    CHECK(!hd.empty());
    const Demuxed first = demux(hd, 0);
    CHECK(first.audio_pid != 0);
    int other = 0;
    for (int pid = 0x101; pid <= 0x103; pid++) if (pid != first.audio_pid) { const Demuxed t = demux(hd, pid); if (t.audio_pid == pid) other = pid; }
    CHECK(other != 0);
    CHECK(demux(hd, other).audio_codec != first.audio_codec);                                 // TrueHD vs DTS
    CHECK(demux(hd, other).audio_pes > 0);
}

static void clock() {
    printf("- the clock of a file entered part way\n");
    StreamLocalIndex idx;
    memset(&idx, 0, sizeof idx);
    const uint64_t LEAD = 40000;
    uint64_t origin, base;

    idx.first_pts = 126000;                                    // 1.4 s: where ffmpeg's muxer starts a stream
    stream_local_clock(&idx, 0, LEAD, &origin, &base);
    CHECK(origin == 1360000 && base == 0);                     // the key frame at stream time 40 ms, position 0
    stream_local_clock(&idx, 600000000ULL, LEAD, &origin, &base);
    CHECK(origin == 601360000ULL && base == 599960000ULL);
    idx.first_pts = 0;
    stream_local_clock(&idx, 0, LEAD, &origin, &base);
    CHECK(origin == 0 && base == 0);                           // no room for a lead before time 0
    idx.first_pts = 1800;                                      // 20 ms
    stream_local_clock(&idx, 0, LEAD, &origin, &base);
    CHECK(origin == 0 && base == 0);

    // for every entry: the key frame (stream time = its PTS - origin) lands at position = the entry
    bool ok = true;
    for (uint64_t first : { 0ULL, 1800ULL, 3600ULL, 126000ULL, 900000ULL, 54000000ULL }) {
        idx.first_pts = first;
        for (uint64_t entry_us : { 0ULL, 1ULL, 39999ULL, 40000ULL, 40001ULL, 2500000ULL, 3600000000ULL, 7200123456ULL }) {
            stream_local_clock(&idx, entry_us, LEAD, &origin, &base);
            const uint64_t abs_us = first * 100 / 9 + entry_us;
            const uint64_t t_us = abs_us - origin;                        // the key frame's time in the stream
            // it reads as the entry, except within the lead of the start: a position cannot go below 0, so
            // there the HUD shows the lead (40 ms) instead
            if (base + t_us != (entry_us > t_us ? entry_us : t_us)) ok = false;
            if (t_us > LEAD) ok = false;
            if (abs_us >= LEAD && t_us != LEAD) ok = false;
        }
    }
    CHECK(ok);

    // the real file: the index's first PTS is the first picture's, so the entry reads as stream time 40 ms
    const Vec av = load("fixtures/ts/av.ts");
    static uint8_t scratch[256 * 1024];
    struct M { const Vec *v; } m = { &av };
    auto rd = [](void *c, uint64_t off, uint8_t *buf, int len) -> int {
        const Vec &v = *((M *)c)->v;
        if (off >= v.size()) return 0;
        const size_t n = std::min<size_t>((size_t)len, v.size() - (size_t)off);
        memcpy(buf, v.data() + off, n);
        return (int)n;
    };
    StreamLocalIndex real;
    CHECK(stream_local_index(rd, &m, av.size(), scratch, (int)sizeof scratch, &real));
    const Demuxed d = demux(av, 0);
    CHECK(d.have_video_pts && d.first_video_pts == real.first_pts);
    uint64_t off, at_us;
    CHECK(stream_local_seek(rd, &m, &real, 0, scratch, (int)sizeof scratch, &off, &at_us) && at_us == 0);
    stream_local_clock(&real, at_us, LEAD, &origin, &base);
    const uint64_t origin_ticks = origin * 9 / 100;
    const uint64_t t_ticks = d.first_video_pts - origin_ticks;
    CHECK(t_ticks >= 3599 && t_ticks <= 3601);                                              // 40 ms, to a tick
}

int main() {
    audio_choice();
    clock();
    printf("local play: %d checks, %d failed\n", s_checks, s_failed);
    return s_failed ? 1 : 0;
}
