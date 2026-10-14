// Host test for source/video/mkv_ts.cpp: Matroska presented as the MPEG-TS the player reads.
//
// The synthesised stream is read back by the player's own demuxer (source/video/ts_demux.cpp, built
// here against hoststub/) and every access unit and audio frame compared with what the Matroska demuxer
// delivered: byte-exact payloads, the PTS arithmetic, the packet structure (sync, continuity counters,
// PSI), the sizes around the packet boundaries.  Where ffmpeg is installed it also decodes the streams.
//
//   make -f Makefile.host test_mkv_ts && ./test_mkv_ts

#include "mkv_ts.h"
#include "mkv_builder.h"
#include "ts_demux.h"
#include "aac_adts.h"
#include "pcm_fmt.h"

#include <codec/vdec.h>   // VDEC_TS_INVALID (hoststub)
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

void plog(const char *) {}

static int s_checks = 0, s_failed = 0;
#define CHECK(cond) do { s_checks++; if (!(cond)) { s_failed++; \
    printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

typedef std::vector<uint8_t> Vec;

static int file_read(void *ctx, uint64_t off, uint8_t *buf, int len) {
    FILE *f = (FILE *)ctx;
    if (fseeko(f, (off_t)off, SEEK_SET) != 0) return -1;
    return (int)fread(buf, 1, (size_t)len, f);
}

// ---- reading a TS back through the player's demuxer -------------------------------------------------

struct Pes { Vec payload; uint64_t pts; bool has_pts; };
struct Parsed {
    std::vector<Pes> video, audio;
    bool bad_sync = false, bad_cc = false, bad_size = false;
    int psi_before_key = 0;
    uint16_t video_pid = 0, audio_pid = 0, pmt_pid = 0;
    int audio_codec = 0;
    int packets = 0;
};

static void take(std::vector<Pes> &v, const uint8_t *pes, int len) {
    const uint8_t *es; int es_len; u64 pts;
    Pes p;
    if (!pes_payload(pes, len, &es, &es_len, &pts)) { p.has_pts = false; p.pts = 0; v.push_back(p); return; }
    p.payload.assign(es, es + es_len);
    p.has_pts = pts != (u64)VDEC_TS_INVALID;
    p.pts = p.has_pts ? pts : 0;
    v.push_back(p);
}

static Parsed parse_ts(const Vec &ts) {
    Parsed out;
    TSState *st = (TSState *)calloc(1, sizeof(TSState));
    static uint8_t vpes[TS_VPES_BUF_SIZE], apes[TS_APES_BUF_SIZE];
    ts_pes_stats_reset();
    uint8_t cc[8192]; memset(cc, 0xFF, sizeof cc);
    size_t n = ts.size() / 188;
    if (ts.size() % 188) out.bad_size = true;
    for (size_t i = 0; i < n; i++) {
        const uint8_t *p = &ts[i * 188];
        if (p[0] != 0x47) { out.bad_sync = true; continue; }
        const uint16_t pid = (uint16_t)(((p[1] & 0x1F) << 8) | p[2]);
        const bool has_payload = (p[3] >> 4) & 1;
        const uint8_t c = p[3] & 0x0F;
        if (has_payload) {
            if (cc[pid] != 0xFF && c != ((cc[pid] + 1) & 0x0F)) out.bad_cc = true;
            cc[pid] = c;
        }
        out.packets++;
        int vlen = 0, alen = 0;
        const int r = ts_process(st, p, vpes, &vlen, apes, &alen);
        if (r & 1) take(out.video, vpes, vlen);
        if (r & 2) take(out.audio, apes, alen);
    }
    // flush the PES still being assembled: a packet that starts a new PES on each PID
    for (int k = 0; k < 2; k++) {
        const uint16_t pid = k == 0 ? st->video_pid : st->audio_pid;
        if (!pid) continue;
        uint8_t p[188]; memset(p, 0xFF, sizeof p);
        p[0] = 0x47; p[1] = (uint8_t)(0x40 | (pid >> 8)); p[2] = (uint8_t)pid; p[3] = 0x10;
        p[4] = 0; p[5] = 0; p[6] = 1;                                          // start code, so it parses
        int vlen = 0, alen = 0;
        const int r = ts_process(st, p, vpes, &vlen, apes, &alen);
        if (r & 1) take(out.video, vpes, vlen);
        if (r & 2) take(out.audio, apes, alen);
    }
    out.video_pid = st->video_pid; out.audio_pid = st->audio_pid; out.pmt_pid = st->pmt_pid;
    out.audio_codec = st->audio_codec;
    free(st);
    return out;
}

static Vec drain(MkvTs *t, int chunk = 188 * 40) {
    Vec all, buf((size_t)chunk);
    for (;;) {
        const int n = mkv_ts_read(t, buf.data(), chunk);
        if (n <= 0) break;
        all.insert(all.end(), buf.begin(), buf.begin() + n);
    }
    return all;
}

// ---- what the stream should contain ----------------------------------------------------------------

static Vec annexb_of(const MkvFrame &fr, const MkvAvcConfig &avc, bool key, int nal_len) {
    Vec out = { 0, 0, 0, 1, 0x09, 0xF0 };                      // the access unit delimiter
    auto sc = [&]() { out.push_back(0); out.push_back(0); out.push_back(0); out.push_back(1); };
    if (key) {
        for (int i = 0; i < avc.n_sps; i++) { sc(); out.insert(out.end(), avc.sps[i], avc.sps[i] + avc.sps_len[i]); }
        for (int i = 0; i < avc.n_pps; i++) { sc(); out.insert(out.end(), avc.pps[i], avc.pps[i] + avc.pps_len[i]); }
    }
    uint32_t pos = 0;
    while (pos + (uint32_t)nal_len <= fr.size) {
        uint32_t len = 0;
        for (int i = 0; i < nal_len; i++) len = (len << 8) | fr.data[pos + (uint32_t)i];
        pos += (uint32_t)nal_len;
        if (len == 0) continue;
        if (len > fr.size - pos) break;
        sc(); out.insert(out.end(), fr.data + pos, fr.data + pos + len);
        pos += len;
    }
    return out;
}

struct Want { std::vector<Vec> video; std::vector<uint64_t> video_pts; std::vector<Vec> audio; std::vector<uint64_t> audio_pts; };

static uint64_t pts90(int64_t ns, int64_t origin) {
    const int64_t rel = ns - origin;
    return (rel > 0 ? (uint64_t)rel * 9ULL / 100000ULL : 0) + MKV_TS_PTS_BASE_90K;
}

// What mkv_ts should produce from `f`, computed independently from the Matroska demuxer's frames.
static Want expect(MkvFile *f, int vt, int at, uint64_t from_cluster_time_hint, bool seek, uint64_t seek_ns) {
    (void)from_cluster_time_hint;
    Want w;
    const MkvTrack *vtrack = mkv_track(f, vt);
    MkvAvcConfig avc;
    mkv_parse_avcc(vtrack->codec_private, vtrack->cp_len, &avc);
    static std::vector<uint8_t> buf;
    buf.assign(1536 * 1024 + 4096, 0);
    MkvReader r;
    mkv_reader_init(&r, f, buf.data(), (uint32_t)buf.size());
    if (seek) { uint64_t ct; mkv_seek(f, &r, seek_ns, &ct); }
    const MkvTrack *atrack = at ? mkv_track(f, at) : nullptr;
    AacConfig acfg;
    const bool aac = atrack && mkv_audio_codec(atrack) == MKV_AC_AAC && aac_parse_asc(atrack->codec_private, atrack->cp_len, &acfg);
    const MkvAudioCodec amc = atrack ? mkv_audio_codec(atrack) : MKV_AC_OTHER;
    bool flac_header_due = amc == MKV_AC_FLAC;                       // "fLaC" and STREAMINFO go before the first frame only
    Vec pcm_hdr;
    if (amc == MKV_AC_PCM) {
        const int rate = (int)(atrack->sample_rate + 0.5);
        pcm_hdr = { 'P', 'C', (uint8_t)atrack->channels, (uint8_t)atrack->bit_depth,
                    (uint8_t)((!strcmp(atrack->codec_id, "A_PCM/FLOAT/IEEE") ? PCM_FLAG_FLOAT : 0) | (!strcmp(atrack->codec_id, "A_PCM/INT/BIG") ? PCM_FLAG_BIG : 0)),
                    (uint8_t)(rate >> 16), (uint8_t)(rate >> 8), (uint8_t)rate };
    }
    MkvFrame fr;
    bool started = false;
    int64_t origin = 0;
    while (mkv_next_frame(&r, &fr) == 1) {
        if (fr.track == vt) {
            if (!started) { if (!fr.key) continue; started = true; origin = fr.pts_ns; }
            w.video.push_back(annexb_of(fr, avc, fr.key, avc.nal_length_size));
            w.video_pts.push_back(pts90(fr.pts_ns, origin));
        } else if (at && fr.track == at) {
            if (!started || fr.pts_ns < origin) continue;
            Vec d;
            if (aac) { uint8_t h[7]; aac_make_adts(&acfg, (int)fr.size, h); d.assign(h, h + 7); }
            else if (flac_header_due) {
                static const uint8_t magic[8] = { 'f', 'L', 'a', 'C', 0x80, 0, 0, 34 };
                d.assign(magic, magic + 8);
                d.insert(d.end(), atrack->codec_private + 8, atrack->codec_private + 42);
                flac_header_due = false;
            }
            else if (amc == MKV_AC_PCM) d = pcm_hdr;
            else if (fr.prefix_len) d.assign(fr.prefix, fr.prefix + fr.prefix_len);
            d.insert(d.end(), fr.data, fr.data + fr.size);
            w.audio.push_back(d);
            w.audio_pts.push_back(pts90(fr.pts_ns, origin));
        }
    }
    return w;
}

static bool same(const Parsed &p, const Want &w, const char *what) {
    bool ok = true;
    if (p.video.size() != w.video.size()) { printf("  %s: %zu video PES, want %zu\n", what, p.video.size(), w.video.size()); ok = false; }
    if (p.audio.size() != w.audio.size()) { printf("  %s: %zu audio PES, want %zu\n", what, p.audio.size(), w.audio.size()); ok = false; }
    for (size_t i = 0; ok && i < w.video.size(); i++) {
        if (p.video[i].payload != w.video[i]) { printf("  %s: video AU %zu differs (%zu vs %zu bytes)\n", what, i, p.video[i].payload.size(), w.video[i].size()); ok = false; }
        else if (!p.video[i].has_pts || p.video[i].pts != w.video_pts[i]) { printf("  %s: video AU %zu pts %llu, want %llu\n", what, i, (unsigned long long)p.video[i].pts, (unsigned long long)w.video_pts[i]); ok = false; }
    }
    for (size_t i = 0; ok && i < w.audio.size(); i++) {
        if (p.audio[i].payload != w.audio[i]) { printf("  %s: audio frame %zu differs (%zu vs %zu bytes)\n", what, i, p.audio[i].payload.size(), w.audio[i].size()); ok = false; }
        else if (!p.audio[i].has_pts || p.audio[i].pts != w.audio_pts[i]) { printf("  %s: audio frame %zu pts %llu, want %llu\n", what, i, (unsigned long long)p.audio[i].pts, (unsigned long long)w.audio_pts[i]); ok = false; }
    }
    return ok;
}

static bool ffmpeg_available() { return system("ffmpeg -version >/dev/null 2>&1") == 0; }

// ffmpeg decodes the stream without a complaint, and finds `frames` pictures
static void ffmpeg_decodes(const Vec &ts, int frames, const char *tag) {
    if (!ffmpeg_available()) { printf("  (ffmpeg not installed: %s not decoded)\n", tag); return; }
    char path[64];
    snprintf(path, sizeof path, "/tmp/jf_mkvts_%s.ts", tag);
    FILE *f = fopen(path, "wb");
    fwrite(ts.data(), 1, ts.size(), f);
    fclose(f);
    char cmd[300];
    snprintf(cmd, sizeof cmd, "ffmpeg -nostdin -v error -i %s -f null - 2>/tmp/jf_mkvts_err.txt", path);
    const int rc = system(cmd);
    FILE *e = fopen("/tmp/jf_mkvts_err.txt", "r");
    char line[256];
    int errors = 0;
    while (e && fgets(line, sizeof line, e)) { errors++; if (errors < 4) printf("  ffmpeg: %s", line); }
    if (e) fclose(e);
    CHECK(rc == 0 && errors == 0);
    snprintf(cmd, sizeof cmd, "ffprobe -v error -count_frames -select_streams v:0 -show_entries stream=nb_read_frames -of csv=p=0 %s > /tmp/jf_mkvts_frames.txt", path);
    if (system(cmd) == 0) {
        FILE *c = fopen("/tmp/jf_mkvts_frames.txt", "r");
        int n = -1;
        if (c && fscanf(c, "%d", &n) == 1 && n != frames) printf("  ffprobe counts %d frames, want %d\n", n, frames);
        if (c) { CHECK(n == frames); fclose(c); }
    }
    unlink(path);
}

// ---- tests -------------------------------------------------------------------------------------------------

static void real_file() {
    printf("- av.mkv (H.264 + AC-3 + MP3 + SRT)\n");
    FILE *fp = fopen("fixtures/mkv/av.mkv", "rb");
    CHECK(fp != nullptr);
    if (!fp) return;
    fseeko(fp, 0, SEEK_END);
    const uint64_t size = (uint64_t)ftello(fp);
    MkvFile f;
    CHECK(mkv_open(&f, file_read, fp, size) == 0);

    char reason[64];
    CHECK(mkv_ts_video_supported(&f, 1, reason, sizeof reason) && !reason[0]);
    CHECK(!mkv_ts_video_supported(&f, 2, reason, sizeof reason));           // an audio track
    CHECK(mkv_ts_audio_supported(MKV_AC_AC3) && mkv_ts_audio_supported(MKV_AC_DTS) &&
          mkv_ts_audio_supported(MKV_AC_TRUEHD) && mkv_ts_audio_supported(MKV_AC_MP3) &&
          mkv_ts_audio_supported(MKV_AC_AAC) && mkv_ts_audio_supported(MKV_AC_MP2) &&
          mkv_ts_audio_supported(MKV_AC_FLAC) && mkv_ts_audio_supported(MKV_AC_PCM) &&
          !mkv_ts_audio_supported(MKV_AC_EAC3) && !mkv_ts_audio_supported(MKV_AC_VORBIS) && !mkv_ts_audio_supported(MKV_AC_OPUS) &&
          !mkv_ts_audio_supported(MKV_AC_OTHER));

    for (int audio : { 2, 3, 0 }) {
        char err[64] = "";
        uint64_t start = 99;
        MkvTs *t = mkv_ts_open(&f, 1, audio, 0, &start, err, sizeof err);
        CHECK(t != nullptr && start == 0);
        if (!t) { printf("  %s\n", err); continue; }
        Vec ts = drain(t);
        MkvTsStats st; mkv_ts_stats(t, &st);
        mkv_ts_close(t);
        CHECK(!ts.empty() && ts.size() % 188 == 0);
        Parsed p = parse_ts(ts);
        CHECK(!p.bad_sync && !p.bad_cc && !p.bad_size);
        CHECK(p.video_pid == 0x100 && p.pmt_pid == 0x1000);
        CHECK(audio == 0 ? p.audio_pid == 0 : p.audio_pid == 0x101);
        if (audio == 2) CHECK(p.audio_codec == TS_AUDIO_AC3);
        if (audio == 3) CHECK(p.audio_codec == TS_AUDIO_MP3);
        Want w = expect(&f, 1, audio, 0, false, 0);
        CHECK(same(p, w, audio == 2 ? "ac3" : audio == 3 ? "mp3" : "video only"));
        CHECK(p.video.size() == 48 && st.video_frames == 48);
        CHECK(audio == 0 || st.audio_frames == p.audio.size());
        ffmpeg_decodes(ts, 48, audio == 2 ? "ac3" : audio == 3 ? "mp3" : "v");
    }

    // PSI comes before every key frame: PAT, PMT, then the AUD + SPS of the picture
    {
        MkvTs *t = mkv_ts_open(&f, 1, 2, 0, nullptr, nullptr, 0);
        Vec ts = drain(t);
        mkv_ts_close(t);
        int keys = 0, ok = 0;
        for (size_t i = 0; i + 2 < ts.size() / 188; i++) {
            const uint8_t *p = &ts[i * 188];
            if (((p[1] & 0x1F) << 8 | p[2]) == 0 && (p[1] & 0x40)) {                 // a PAT
                const uint8_t *m = &ts[(i + 1) * 188], *v = &ts[(i + 2) * 188];
                if (((m[1] & 0x1F) << 8 | m[2]) == 0x1000 && ((v[1] & 0x1F) << 8 | v[2]) == 0x100 && (v[1] & 0x40)) ok++;
                keys++;
            }
        }
        CHECK(keys >= 4 && ok == keys);                                            // 2 s at a key frame every 12
    }

    // starting inside the file: from the key frame at or before the time asked for
    for (uint64_t at_ms : { 500ull, 1000ull, 1400ull, 1999ull }) {
        uint64_t start = 0;
        char err[64] = "";
        MkvTs *t = mkv_ts_open(&f, 1, 2, at_ms * 1000000ull, &start, err, sizeof err);
        CHECK(t != nullptr);
        if (!t) continue;
        Vec ts = drain(t);
        mkv_ts_close(t);
        CHECK(start <= at_ms * 1000000ull && start + 600000000ull > at_ms * 1000000ull);   // keyframes are 0.5 s apart
        Parsed p = parse_ts(ts);
        CHECK(!p.bad_sync && !p.bad_cc);
        Want w = expect(&f, 1, 2, 0, true, at_ms * 1000000ull);
        char tag[32]; snprintf(tag, sizeof tag, "seek %llu", (unsigned long long)at_ms);
        CHECK(same(p, w, tag));
        CHECK(!p.video.empty() && p.video[0].has_pts && p.video[0].pts == MKV_TS_PTS_BASE_90K);   // stream time starts at the key frame
        CHECK(!p.video.empty() && p.video[0].payload.size() > 10 && (p.video[0].payload[4] & 0x1F) == 9 && (p.video[0].payload[10] & 0x1F) == 7);
        ffmpeg_decodes(ts, (int)w.video.size(), "seek");
    }
    mkv_ts_release();
    mkv_close(&f);
    fclose(fp);
}

static void other_real_files() {
    printf("- dts.mkv, live.mkv\n");
    {
        FILE *fp = fopen("fixtures/mkv/dts.mkv", "rb");
        CHECK(fp != nullptr);
        if (fp) {
            fseeko(fp, 0, SEEK_END);
            MkvFile f;
            CHECK(mkv_open(&f, file_read, fp, (uint64_t)ftello(fp)) == 0);
            char err[64] = "";
            MkvTs *t = mkv_ts_open(&f, 1, 2, 0, nullptr, err, sizeof err);            // DTS
            CHECK(t != nullptr);
            if (t) {
                Vec ts = drain(t);
                mkv_ts_close(t);
                Parsed p = parse_ts(ts);
                CHECK(p.audio_codec == TS_AUDIO_DTS && p.audio_pid == 0x101);
                CHECK(same(p, expect(&f, 1, 2, 0, false, 0), "dts"));
                ffmpeg_decodes(ts, 24, "dts");
            }
            {   // its second track is FLAC: carried, with the stream header on the first frame only
                MkvTs *t3 = mkv_ts_open(&f, 1, 3, 0, nullptr, err, sizeof err);
                CHECK(t3 != nullptr);
                if (t3) {
                    Vec ts = drain(t3);
                    mkv_ts_close(t3);
                    Parsed p = parse_ts(ts);
                    CHECK(p.audio_codec == TS_AUDIO_FLAC && p.audio_pid == 0x101 && p.audio.size() >= 2);
                    CHECK(p.audio.size() >= 2 && p.audio[0].payload.size() > 42 && !memcmp(p.audio[0].payload.data(), "fLaC", 4) &&
                          p.audio[1].payload.size() > 2 && p.audio[1].payload[0] == 0xFF && (p.audio[1].payload[1] & 0xFE) == 0xF8);
                    CHECK(same(p, expect(&f, 1, 3, 0, false, 0), "dts.mkv flac"));
                }
            }
            CHECK(mkv_ts_open(&f, 1, 9, 0, nullptr, err, sizeof err) == nullptr);                              // no such track
            CHECK(mkv_ts_open(&f, 2, 0, 0, nullptr, err, sizeof err) == nullptr && err[0]);                   // not a video track
            mkv_close(&f);
            fclose(fp);
        }
    }
    {
        FILE *fp = fopen("fixtures/mkv/live.mkv", "rb");
        CHECK(fp != nullptr);
        if (fp) {
            fseeko(fp, 0, SEEK_END);
            MkvFile f;
            CHECK(mkv_open(&f, file_read, fp, (uint64_t)ftello(fp)) == 0);
            for (uint64_t ms : { 0ull, 1500ull }) {
                MkvTs *t = mkv_ts_open(&f, 1, 2, ms * 1000000ull, nullptr, nullptr, 0);
                CHECK(t != nullptr);
                if (!t) continue;
                Vec ts = drain(t);
                mkv_ts_close(t);
                Parsed p = parse_ts(ts);
                CHECK(!p.bad_cc && same(p, expect(&f, 1, 2, 0, ms > 0, ms * 1000000ull), "live"));
            }
            mkv_close(&f);
            fclose(fp);
        }
    }
    mkv_ts_release();
}

// AAC: a stereo 44.1 kHz and a 5.1 track of ffmpeg's encoder.  Matroska keeps raw frames and the config in
// CodecPrivate; the stream carries ADTS frames (aac_adts.h), which ffmpeg must read without complaint.
static void aac_file() {
    printf("- aac.mkv (H.264 + AAC stereo 44.1 kHz + AAC 5.1)\n");
    FILE *fp = fopen("fixtures/aac/aac.mkv", "rb");
    CHECK(fp != nullptr);
    if (!fp) return;
    fseeko(fp, 0, SEEK_END);
    MkvFile f;
    CHECK(mkv_open(&f, file_read, fp, (uint64_t)ftello(fp)) == 0);
    for (int audio : { 2, 3 }) {
        char err[64] = "";
        MkvTs *t = mkv_ts_open(&f, 1, audio, 0, nullptr, err, sizeof err);
        CHECK(t != nullptr);
        if (!t) { printf("  %s\n", err); continue; }
        Vec ts = drain(t);
        mkv_ts_close(t);
        Parsed p = parse_ts(ts);
        CHECK(!p.bad_sync && !p.bad_cc && !p.bad_size);
        CHECK(p.audio_pid == 0x101 && p.audio_codec == TS_AUDIO_AAC);
        char tag[32]; snprintf(tag, sizeof tag, "aac track %d", audio);
        CHECK(same(p, expect(&f, 1, audio, 0, false, 0), tag));
        // every audio PES is one ADTS frame whose length is its own
        bool whole = !p.audio.empty();
        const MkvTrack *tr = mkv_track(&f, audio);
        AacConfig want;
        CHECK(aac_parse_asc(tr->codec_private, tr->cp_len, &want));
        for (const Pes &a : p.audio) {
            AacConfig got;
            if (a.payload.size() < 8 || aac_adts_frame_len(a.payload.data(), (int)a.payload.size()) != (int)a.payload.size() ||
                !aac_parse_adts(a.payload.data(), (int)a.payload.size(), &got) || got.sample_rate != want.sample_rate ||
                got.channels != want.channels || got.object_type != want.object_type) whole = false;
        }
        CHECK(whole);
        CHECK(want.channels == (audio == 2 ? 2 : 6) && want.sample_rate == (audio == 2 ? 44100 : 48000));
        ffmpeg_decodes(ts, 48, audio == 2 ? "aac2" : "aac3");
    }
    mkv_ts_release();
    mkv_close(&f);
    fclose(fp);
}

// FLAC and the PCM layouts (fixtures/flac/flac_pcm.mkv): every frame carried with what the decoder needs.  ffmpeg cannot read
// these private stream types, so the stream is checked against the Matroska frames, and the decoders are run on it in test_lossless.
static void lossless_file() {
    printf("- flac_pcm.mkv (FLAC and PCM s16le, s24le, f32le, s16be)\n");
    FILE *fp = fopen("fixtures/flac/flac_pcm.mkv", "rb");
    CHECK(fp != nullptr);
    if (!fp) return;
    fseeko(fp, 0, SEEK_END);
    MkvFile f;
    CHECK(mkv_open(&f, file_read, fp, (uint64_t)ftello(fp)) == 0);
    struct { int track; int codec; int ch, bits, rate; } want[] = {
        { 2, TS_AUDIO_FLAC, 2, 16, 44100 }, { 3, TS_AUDIO_PCM, 2, 16, 44100 }, { 4, TS_AUDIO_PCM, 6, 24, 16000 },
        { 5, TS_AUDIO_PCM, 1, 32, 32000 }, { 6, TS_AUDIO_PCM, 2, 16, 22050 },
    };
    for (auto &w : want) {
        char err[64] = "", why[64] = "";
        const MkvTrack *tr = mkv_track(&f, w.track);
        CHECK(tr != nullptr && mkv_ts_audio_track_supported(tr, why, sizeof why) && !why[0]);
        MkvTs *t = mkv_ts_open(&f, 1, w.track, 0, nullptr, err, sizeof err);
        CHECK(t != nullptr);
        if (!t) { printf("  track %d: %s\n", w.track, err); continue; }
        Vec ts = drain(t);
        mkv_ts_close(t);
        Parsed p = parse_ts(ts);
        CHECK(!p.bad_sync && !p.bad_cc && !p.bad_size && p.audio_codec == w.codec && p.audio_pid == 0x101 && !p.audio.empty());
        char tag[40]; snprintf(tag, sizeof tag, "lossless track %d", w.track);
        CHECK(same(p, expect(&f, 1, w.track, 0, false, 0), tag));
        if (w.codec == TS_AUDIO_PCM) {
            const Vec &a = p.audio[0].payload;
            CHECK(a.size() > 8 && a[0] == 'P' && a[1] == 'C' && a[2] == w.ch && a[3] == w.bits && (a[5] << 16 | a[6] << 8 | a[7]) == w.rate);
            // whole sample frames after the header, in every PES
            bool whole = true;
            for (const Pes &q : p.audio) if ((q.payload.size() - 8) % (size_t)(w.ch * w.bits / 8) != 0) whole = false;
            CHECK(whole);
        }
    }
    CHECK(mkv_ts_audio_track_supported(mkv_track(&f, 5), nullptr, 0));
    mkv_ts_release();
    mkv_close(&f);
    fclose(fp);
}

// A hand-built file: NAL length sizes 1 to 4, several NALs per unit, a zero-length NAL, a damaged length,
// and audio frames of every size around the packet boundaries.
static void hand_built() {
    printf("- hand-built: NAL lengths, packet boundaries, header stripping, big frames\n");
    for (int nal_len = 1; nal_len <= 4; nal_len++) {
        Bytes tracks = track_video(1, avcc(nal_len)) + track_audio(2, "A_AC3", "eng", 2);
        auto nal = [&](size_t n, int seed) {
            Bytes b;
            for (int i = nal_len - 1; i >= 0; i--) b.push_back((char)((n >> (8 * i)) & 0xFF));
            Bytes body = frame_of(seed, n);
            if (!body.empty()) body[0] = (char)0x65;
            return b + body;
        };
        std::vector<Bytes> c(3);
        const size_t max_nal = nal_len == 1 ? 200 : 3000;
        for (int k = 0; k < 3; k++) {
            Bytes au = nal(std::min<size_t>(max_nal, 170 + (size_t)k * 7), 1 + k) + nal(5, 9) + nal(std::min<size_t>(max_nal, 183 + (size_t)k), 3);
            au += Bytes((size_t)nal_len, '\0');                                   // a zero-length NAL
            au += nal(std::min<size_t>(max_nal, 184), 4);
            c[k] += simple_block(1, 0, true, { au });
            for (int j = 1; j < 4; j++) c[k] += simple_block(1, j * 40, false, { nal(std::min<size_t>(max_nal, 60 + (size_t)j * 61), 20 + j) });
            for (size_t sz = 1; sz <= 130; sz++) c[k] += simple_block(2, (int)sz, true, { frame_of((int)sz, sz + (size_t)k * 130) });
        }
        Opts o;
        Built b = build(tracks, c, { 0, 2000, 4000 }, o);
        Mem m = { &b.file, -1, 0 };
        MkvFile f;
        CHECK(mkv_open(&f, mem_read, &m, b.file.size()) == 0);
        MkvTs *t = mkv_ts_open(&f, 1, 2, 0, nullptr, nullptr, 0);
        CHECK(t != nullptr);
        if (!t) continue;
        Vec ts = drain(t, 188 * 7);                                              // small reads: units straddle calls
        mkv_ts_close(t);
        Parsed p = parse_ts(ts);
        CHECK(!p.bad_sync && !p.bad_cc && !p.bad_size);
        char tag[32]; snprintf(tag, sizeof tag, "nal length %d", nal_len);
        CHECK(same(p, expect(&f, 1, 2, 0, false, 0), tag));
        mkv_close(&f);
    }

    // AAC with no AudioSpecificConfig cannot be given ADTS headers: refused, not played wrongly
    {
        Bytes tracks = track_video(1, avcc()) + track_audio(2, "A_AAC", "eng", 2);
        std::vector<Bytes> c(1);
        c[0] += simple_block(1, 0, true, { Bytes("\0\0\0\5\x65" "abcd", 9) });
        c[0] += simple_block(2, 0, true, { frame_of(1, 100) });
        Built b = build(tracks, c, { 0 }, Opts());
        Mem m = { &b.file, -1, 0 };
        MkvFile f;
        CHECK(mkv_open(&f, mem_read, &m, b.file.size()) == 0);
        char err[64] = "";
        CHECK(mkv_ts_open(&f, 1, 2, 0, nullptr, err, sizeof err) == nullptr && strstr(err, "AAC"));
        mkv_close(&f);
    }

    // sizes straddling the packet boundaries, both PES kinds, every residue: 1..2200 bytes in steps
    {
        Bytes tracks = track_video(1, avcc()) + track_audio(2, "A_DTS", "eng", 6);
        std::vector<Bytes> c(1);
        auto nal4 = [&](size_t n, int seed) {
            Bytes b; for (int i = 3; i >= 0; i--) b.push_back((char)((n >> (8 * i)) & 0xFF));
            return b + frame_of(seed, n);
        };
        int t_ms = 0;
        for (size_t n = 1; n <= 700; n++) {
            c[0] += simple_block(1, t_ms, n % 3 == 1, { nal4(n, (int)n) });
            c[0] += simple_block(2, t_ms, true, { frame_of((int)n, n * 3) });
            t_ms = (t_ms + 1) % 30000;
        }
        Built b = build(tracks, c, { 0 }, Opts());
        Mem m = { &b.file, -1, 0 };
        MkvFile f;
        CHECK(mkv_open(&f, mem_read, &m, b.file.size()) == 0);
        MkvTs *t = mkv_ts_open(&f, 1, 2, 0, nullptr, nullptr, 0);
        CHECK(t != nullptr);
        if (t) {
            Vec ts = drain(t, 188 * 3);
            mkv_ts_close(t);
            Parsed p = parse_ts(ts);
            CHECK(!p.bad_sync && !p.bad_cc && !p.bad_size && p.audio_codec == TS_AUDIO_DTS);
            CHECK(same(p, expect(&f, 1, 2, 0, false, 0), "boundaries"));
        }
        mkv_close(&f);
    }

    // header stripping: the audio track's removed bytes are put back; the damaged NAL length is cut short
    {
        Bytes comp = elem(ID_CONTENTENCODINGS, elem(ID_CONTENTENCODING, uint_el(ID_ENCTYPE, 0) +
                     elem(ID_ENCCOMP, uint_el(ID_COMPALGO, 3) + elem(ID_COMPSETTINGS, Bytes("\x0B\x77", 2)))));
        Bytes tracks = track_video(1, avcc()) +
            elem(ID_TRACKENTRY, uint_el(ID_TRACKNUMBER, 2) + uint_el(ID_TRACKTYPE, 2) + str_el(ID_CODECID, "A_AC3") + comp);
        std::vector<Bytes> c(1);
        Bytes bad = Bytes("\x00\x00\x00\x10", 4) + frame_of(1, 8);                // says 16 bytes, has 8: shortened
        Bytes good = Bytes("\x00\x00\x00\x06", 4) + frame_of(2, 6) + bad;
        c[0] += simple_block(1, 0, true, { good });
        c[0] += simple_block(2, 0, true, { Bytes("\x80\x01\x02", 3) });
        c[0] += simple_block(1, 40, false, { bad });
        c[0] += simple_block(2, 32, true, { frame_of(5, 50) });
        Built b = build(tracks, c, { 0 }, Opts());
        Mem m = { &b.file, -1, 0 };
        MkvFile f;
        CHECK(mkv_open(&f, mem_read, &m, b.file.size()) == 0);
        MkvTs *t = mkv_ts_open(&f, 1, 2, 0, nullptr, nullptr, 0);
        CHECK(t != nullptr);
        if (t) {
            Vec ts = drain(t);
            mkv_ts_close(t);
            Parsed p = parse_ts(ts);
            CHECK(!p.bad_cc && p.audio.size() == 2 && p.video.size() == 2);
            if (p.audio.size() == 2) CHECK(p.audio[0].payload == Vec({ 0x0B, 0x77, 0x80, 0x01, 0x02 }));
            if (p.video.size() == 2) {
                // AUD + SPS + PPS + the one whole NAL; the damaged second NAL is dropped; the second unit is AUD only
                const size_t pre = 6 + (4 + 26) + (4 + 6);
                CHECK(p.video[0].payload.size() == pre + 4 + 6);
                CHECK(p.video[1].payload.size() == 6);
            }
        }
        mkv_close(&f);
    }

    // a frame bigger than the buffer is skipped and counted; the stream goes on
    {
        Bytes tracks = track_video(1, avcc()) + track_audio(2, "A_AC3", "eng", 2);
        std::vector<Bytes> c(1);
        Bytes huge = Bytes("\x00\x19\x00\x00", 4) + Bytes(0x190000, 'x');         // 1.56 MiB: over the 1.5 MiB buffer
        c[0] += simple_block(1, 0, true, { Bytes("\x00\x00\x00\x05", 4) + frame_of(1, 5) });
        c[0] += simple_block(1, 40, false, { huge });
        c[0] += simple_block(2, 40, true, { frame_of(2, 100) });
        c[0] += simple_block(1, 80, false, { Bytes("\x00\x00\x00\x05", 4) + frame_of(3, 5) });
        Built b = build(tracks, c, { 0 }, Opts());
        Mem m = { &b.file, -1, 0 };
        MkvFile f;
        CHECK(mkv_open(&f, mem_read, &m, b.file.size()) == 0);
        MkvTs *t = mkv_ts_open(&f, 1, 2, 0, nullptr, nullptr, 0);
        CHECK(t != nullptr);
        if (t) {
            Vec ts = drain(t);
            MkvTsStats st; mkv_ts_stats(t, &st);
            mkv_ts_close(t);
            Parsed p = parse_ts(ts);
            CHECK(p.video.size() == 2 && p.audio.size() == 1 && !p.bad_cc);
            CHECK(st.dropped_oversize == 1);
        }
        mkv_close(&f);
    }

    // refusals: an HEVC track, a file with no key frame to start from, an unreadable avcC
    {
        Bytes hevc = elem(ID_TRACKENTRY, uint_el(ID_TRACKNUMBER, 1) + uint_el(ID_TRACKTYPE, 1) + str_el(ID_CODECID, "V_MPEGH/ISO/HEVC"));
        std::vector<Bytes> c(1);
        c[0] += simple_block(1, 0, true, { frame_of(1, 100) });
        Built b = build(hevc, c, { 0 }, Opts());
        Mem m = { &b.file, -1, 0 };
        MkvFile f;
        CHECK(mkv_open(&f, mem_read, &m, b.file.size()) == 0);
        char err[64] = "";
        CHECK(mkv_ts_open(&f, 1, 0, 0, nullptr, err, sizeof err) == nullptr && !strcmp(err, "HEVC"));
        mkv_close(&f);

        Bytes tracks = track_video(1, avcc()) + track_audio(2, "A_AC3", "eng", 2);
        std::vector<Bytes> d(1);
        d[0] += simple_block(1, 0, false, { Bytes("\x00\x00\x00\x05", 4) + frame_of(1, 5) });      // no key frame at all
        Built nb = build(tracks, d, { 0 }, Opts());
        Mem m2 = { &nb.file, -1, 0 };
        CHECK(mkv_open(&f, mem_read, &m2, nb.file.size()) == 0);
        CHECK(mkv_ts_open(&f, 1, 2, 0, nullptr, err, sizeof err) == nullptr && err[0]);
        mkv_close(&f);

        Built bad = build(track_video(1, Bytes("\x01\x64", 2)), d, { 0 }, Opts());
        Mem m3 = { &bad.file, -1, 0 };
        CHECK(mkv_open(&f, mem_read, &m3, bad.file.size()) == 0);
        CHECK(mkv_ts_open(&f, 1, 0, 0, nullptr, err, sizeof err) == nullptr);
        mkv_close(&f);
    }
    mkv_ts_release();
}

int main() {
    real_file();
    other_real_files();
    aac_file();
    lossless_file();
    hand_built();
    printf("mkv ts: %d checks, %d failed\n", s_checks, s_failed);
    return s_failed ? 1 : 0;
}
