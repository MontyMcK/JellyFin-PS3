// Host test for source/local/local_probe.cpp (what a local video file is, which track to start with)
// and source/util/lang_names.c, against ffmpeg-made Matroska, MPEG-TS and Blu-ray .m2ts files
// (fixtures/mkv, fixtures/ts).
//
//   make -f Makefile.host test_local_probe && ./test_local_probe

#include "local_probe.h"
#include "lang_names.h"

#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>

static int s_checks = 0, s_failed = 0;
#define CHECK(cond) do { s_checks++; if (!(cond)) { s_failed++; \
    printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

static int file_read(void *ctx, uint64_t off, uint8_t *buf, int len) {
    FILE *f = (FILE *)ctx;
    if (fseeko(f, (off_t)off, SEEK_SET) != 0) return -1;
    return (int)fread(buf, 1, (size_t)len, f);
}

struct Mem { const std::vector<uint8_t> *b; };
static int mem_read(void *ctx, uint64_t off, uint8_t *buf, int len) {
    const std::vector<uint8_t> &b = *((Mem *)ctx)->b;
    if (off >= b.size()) return 0;
    const size_t n = std::min<size_t>((size_t)len, b.size() - (size_t)off);
    memcpy(buf, b.data() + off, n);
    return (int)n;
}

static bool probe_file(const char *path, LocalInfo *info, char *err = nullptr, int cap = 0) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    fseeko(f, 0, SEEK_END);
    const uint64_t size = (uint64_t)ftello(f);
    const char *name = strrchr(path, '/') ? strrchr(path, '/') + 1 : path;
    const bool ok = local_probe(file_read, f, size, name, info, err, cap);
    fclose(f);
    return ok;
}

static void mkv_files() {
    printf("- Matroska\n");
    LocalInfo i;
    CHECK(probe_file("fixtures/mkv/av.mkv", &i));
    CHECK(i.container == LM_MKV && i.video_ok && !i.video_reason[0]);
    CHECK(i.width == 64 && i.height == 64 && fabs(i.fps - 24.0) < 0.01);
    CHECK(i.duration_secs == 2);
    CHECK(!strcmp(i.video_desc, "H.264 High 64x64 24 fps"));
    CHECK(i.n_audio == 2 && i.n_subs == 1);
    CHECK(i.audio[0].codec == LA_AC3 && i.audio[0].decodable && !strcmp(i.audio[0].lang, "eng") && i.audio[0].channels == 6);
    CHECK(!strcmp(i.audio[0].label, "English - Dolby Digital - 5.1 - Default"));
    CHECK(i.audio[1].codec == LA_MP3 && !strcmp(i.audio[1].lang, "fra") && i.audio[1].channels == 2);
    CHECK(!strncmp(i.audio[1].label, "French - MP3 - Stereo", 21));
    CHECK(i.subs[0].kind == LS_SRT && i.subs[0].usable && !i.subs[0].forced && !strncmp(i.subs[0].label, "English", 7));
    CHECK(i.audio[0].id == 2 && i.audio[1].id == 3 && i.subs[0].id == 4 && i.video_id == 1);

    CHECK(probe_file("fixtures/mkv/live.mkv", &i));
    CHECK(i.container == LM_MKV && i.video_ok && i.duration_secs >= 1 && i.n_audio == 1 && i.n_subs == 1);

    CHECK(probe_file("fixtures/mkv/dts.mkv", &i));
    CHECK(i.n_audio == 2 && i.audio[0].codec == LA_DTS && i.audio[0].decodable);
    CHECK(i.audio[1].codec == LA_FLAC && i.audio[1].decodable);                // FLAC is carried now
    LocalAudioPrefs p = { true, false };
    CHECK(local_pick_audio(&i, &p) == 0);                                   // the one it can decode

    // FLAC and PCM in Matroska, whatever their rate
    CHECK(probe_file("fixtures/flac/flac_pcm.mkv", &i));
    CHECK(i.n_audio == 5 && i.audio[0].codec == LA_FLAC && i.audio[1].codec == LA_PCM && i.audio[2].codec == LA_PCM);
    for (int k = 0; k < 5; k++) CHECK(i.audio[k].decodable);
    CHECK(i.audio[2].channels == 6 && !strcmp(i.audio[2].label, "Unknown - PCM - 5.1") && !strcmp(i.audio[0].label, "Unknown - FLAC - Stereo - Default"));

    // AAC is decodable now (libfaad): a stereo 44.1 kHz track and a 5.1 one
    CHECK(probe_file("fixtures/aac/aac.mkv", &i));
    CHECK(i.container == LM_MKV && i.video_ok && i.n_audio == 2);
    CHECK(i.audio[0].codec == LA_AAC && i.audio[0].decodable && i.audio[0].channels == 2 && !strncmp(i.audio[0].label, "English - AAC - Stereo", 22));
    CHECK(i.audio[1].codec == LA_AAC && i.audio[1].decodable && i.audio[1].channels == 6 && !strncmp(i.audio[1].label, "English - AAC - 5.1", 19));
    CHECK(local_pick_audio(&i, &p) == 0);
    char why[160];
    CHECK(local_can_play(&i, &p, why, sizeof why));
}

static void ts_files() {
    printf("- MPEG-TS and m2ts\n");
    LocalInfo ts, m2;
    char err[64] = "";
    CHECK(probe_file("fixtures/ts/av.ts", &ts, err, sizeof err));
    CHECK(probe_file("fixtures/ts/av.m2ts", &m2, err, sizeof err));
    CHECK(ts.container == LM_TS && m2.container == LM_M2TS);
    for (LocalInfo *i : { &ts, &m2 }) {
        CHECK(i->video_ok && !i->video_reason[0] && i->width == 64 && i->height == 64 && fabs(i->fps - 24.0) < 0.01);
        CHECK(!strcmp(i->video_desc, "H.264 High 64x64 24 fps"));
        CHECK(i->n_audio == 2);
        CHECK(i->audio[0].codec == LA_AC3 && i->audio[0].decodable && !strcmp(i->audio[0].lang, "eng") && i->audio[0].is_default);
        CHECK(!strcmp(i->audio[1].lang, "fra") && !i->audio[1].is_default && i->audio[1].decodable);
        CHECK(!strcmp(i->audio[0].label, "English - Dolby Digital - Default"));
        CHECK(i->audio[0].id != i->audio[1].id && i->audio[0].id > 0);
        CHECK(i->duration_secs <= 2);
    }
    CHECK(ts.audio[1].codec == LA_MP3 && !strcmp(ts.audio[1].label, "French - MP3"));
    CHECK(m2.audio[1].codec == LA_AC3 && !strcmp(m2.audio[1].label, "French - Dolby Digital"));
    // each file's own PIDs: ffmpeg's plain TS (0x100...) and its Blu-ray layout (0x1011, 0x1100...)
    CHECK(ts.video_id == 0x100 && ts.audio[0].id == 0x101 && ts.audio[1].id == 0x102);
    CHECK(m2.video_id == 0x1011 && m2.audio[0].id == 0x1100 && m2.audio[1].id == 0x1101);

    // m2ts viewed as TS: every 188-byte packet of the view is the file's packet minus its 4-byte header
    FILE *f = fopen("fixtures/ts/av.m2ts", "rb");
    CHECK(f != nullptr);
    if (f) {
        fseeko(f, 0, SEEK_END);
        const uint64_t size = (uint64_t)ftello(f);
        std::vector<uint8_t> raw(size);
        fseeko(f, 0, SEEK_SET);
        CHECK(fread(raw.data(), 1, size, f) == size);
        fclose(f);
        Mem mem = { &raw };
        LocalM2tsView v = { mem_read, &mem };
        const uint64_t ts_size = local_m2ts_ts_size(size);
        CHECK(ts_size == size / 192 * 188);
        std::vector<uint8_t> want;
        for (size_t p = 0; p + 192 <= size; p += 192) want.insert(want.end(), raw.begin() + p + 4, raw.begin() + p + 192);
        std::vector<uint8_t> got(want.size());
        CHECK(local_m2ts_read_at(&v, 0, got.data(), (int)got.size()) == (int)got.size() && got == want);
        // arbitrary windows, across packet and read-buffer boundaries
        bool ok = true;
        for (uint64_t off : { 1ull, 187ull, 188ull, 189ull, 40000ull, 65423ull, 65424ull, 65425ull }) {
            for (int len : { 1, 187, 188, 189, 5000, 70000 }) {
                if (off + (uint64_t)len > want.size()) continue;
                std::vector<uint8_t> w(len);
                if (local_m2ts_read_at(&v, off, w.data(), len) != len || memcmp(w.data(), &want[off], (size_t)len) != 0) ok = false;
            }
        }
        CHECK(ok);
        // in-place compaction of whole reads, as the player's stream buffer does it (a short tail is dropped)
        for (int pkts : { 1, 2, 100, 351 }) {
            if ((size_t)pkts * 192 > raw.size()) continue;
            std::vector<uint8_t> blk(raw.begin(), raw.begin() + (size_t)pkts * 192 + 77);     // 77 stray bytes after the last packet
            const int n = local_m2ts_compact(blk.data(), (int)blk.size());
            CHECK(n == pkts * 188 && memcmp(blk.data(), want.data(), (size_t)n) == 0);
        }
        CHECK(local_m2ts_compact(raw.data(), 191) == 0);
        std::vector<uint8_t> past(100);
        CHECK(local_m2ts_read_at(&v, want.size(), past.data(), 100) == 0);                  // at the end
        CHECK(local_m2ts_read_at(&v, want.size() - 50, past.data(), 100) == 50);            // short at the end
    }

    // what the console cannot play, with the reason the file row shows
    LocalInfo h;
    CHECK(probe_file("fixtures/ts/aac.ts", &h) && h.video_ok && h.n_audio == 1 && h.audio[0].codec == LA_AAC && h.audio[0].decodable);
    CHECK(!strcmp(h.audio[0].label, "English - AAC - Default") && h.audio[0].id == 0x101);
    CHECK(probe_file("fixtures/ts/hevc.ts", &h) && !h.video_ok && !strcmp(h.video_reason, "HEVC") && !strncmp(h.video_desc, "HEVC", 4));
    CHECK(probe_file("fixtures/ts/high10.ts", &h) && !h.video_ok && !strcmp(h.video_reason, "10-bit"));

    // TrueHD and DTS
    CHECK(probe_file("fixtures/ts/hd.ts", &h) && h.video_ok && h.n_audio == 2);
    CHECK(h.audio[0].codec == LA_TRUEHD && h.audio[0].decodable && h.audio[1].codec == LA_DTS && h.audio[1].decodable);
    LocalAudioPrefs surround = { true, false };
    CHECK(local_pick_audio(&h, &surround) == 0);
}

static void refusals() {
    printf("- not a video\n");
    LocalInfo i;
    char err[64] = "";
    std::vector<uint8_t> junk(100000, 'x');
    Mem m = { &junk };
    CHECK(!local_probe(mem_read, &m, junk.size(), "notes.txt", &i, err, sizeof err) && err[0]);
    CHECK(!local_probe(mem_read, &m, junk.size(), "movie.mkv", &i, err, sizeof err));          // the name alone is not enough
    std::vector<uint8_t> none;
    Mem m2 = { &none };
    CHECK(!local_probe(mem_read, &m2, 0, "a.mkv", &i, err, sizeof err));
    std::vector<uint8_t> ts(188 * 10, 0);
    for (int p = 0; p < 10; p++) ts[(size_t)p * 188] = 0x47;                                    // sync bytes, nothing else
    Mem m3 = { &ts };
    CHECK(!local_probe(mem_read, &m3, ts.size(), "a.ts", &i, err, sizeof err));                 // no program
}

static LocalAudio aud(LocalAudioCodec c, bool dec, bool def, bool commentary, const char *lang) {
    LocalAudio a;
    memset(&a, 0, sizeof a);
    a.codec = c; a.decodable = dec; a.is_default = def; a.commentary = commentary;
    snprintf(a.lang, sizeof a.lang, "%s", lang);
    return a;
}

static void policy() {
    printf("- which track to start with\n");
    LocalInfo i;
    memset(&i, 0, sizeof i);
    LocalAudioPrefs surround = { true, false }, stereo = { false, false }, dd = { true, true };
    CHECK(local_pick_audio(&i, &surround) == -1);                           // no tracks

    i.audio[i.n_audio++] = aud(LA_AC3, true, true, false, "eng");
    i.audio[i.n_audio++] = aud(LA_DTS, true, false, false, "eng");
    i.audio[i.n_audio++] = aud(LA_TRUEHD, true, false, false, "eng");
    i.audio[i.n_audio++] = aud(LA_EAC3, false, false, false, "eng");
    CHECK(local_pick_audio(&i, &surround) == 2);                            // TrueHD beats the default Dolby Digital
    CHECK(local_pick_audio(&i, &stereo) == 2);
    CHECK(local_pick_audio(&i, &dd) == 0);                                  // Dolby Digital output: the AC-3 track goes through untouched
    CHECK(local_pick_audio(&i, nullptr) == 2);

    i.audio[2].decodable = false;
    CHECK(local_pick_audio(&i, &surround) == 1);                            // then DTS
    i.audio[1].codec = LA_DTS_HD;
    CHECK(local_pick_audio(&i, &surround) == 1);
    i.audio[1].decodable = false;
    CHECK(local_pick_audio(&i, &surround) == 0);

    // commentary only when nothing else decodes
    LocalInfo c;
    memset(&c, 0, sizeof c);
    c.audio[c.n_audio++] = aud(LA_TRUEHD, true, false, true, "eng");
    c.audio[c.n_audio++] = aud(LA_AC3, true, true, false, "eng");
    CHECK(local_pick_audio(&c, &surround) == 1);
    c.audio[1].decodable = false;
    CHECK(local_pick_audio(&c, &surround) == 0);

    // equal tracks: the default flag, then the first
    LocalInfo e;
    memset(&e, 0, sizeof e);
    e.audio[e.n_audio++] = aud(LA_AC3, true, false, false, "eng");
    e.audio[e.n_audio++] = aud(LA_AC3, true, true, false, "fra");
    e.audio[e.n_audio++] = aud(LA_AC3, true, true, false, "deu");
    CHECK(local_pick_audio(&e, &stereo) == 1);
    e.audio[1].is_default = e.audio[2].is_default = false;
    CHECK(local_pick_audio(&e, &stereo) == 0);

    // subtitles: only a forced, usable track in the audio track's language
    LocalInfo s;
    memset(&s, 0, sizeof s);
    s.audio[s.n_audio++] = aud(LA_AC3, true, true, false, "eng");
    s.audio[s.n_audio++] = aud(LA_AC3, true, false, false, "fra");
    LocalSub a; memset(&a, 0, sizeof a);
    snprintf(a.lang, sizeof a.lang, "eng"); a.kind = LS_SRT; a.usable = true;
    s.subs[s.n_subs++] = a;                                                  // full English: not forced
    a.forced = true; snprintf(a.lang, sizeof a.lang, "fra");
    s.subs[s.n_subs++] = a;                                                  // forced French
    CHECK(local_pick_sub(&s, 0) == -1);                                      // English audio: none
    CHECK(local_pick_sub(&s, 1) == 1);                                       // French audio: the forced French
    s.subs[1].usable = false;
    CHECK(local_pick_sub(&s, 1) == -1);
    CHECK(local_pick_sub(&s, -1) == -1 && local_pick_sub(&s, 9) == -1);
    s.subs[1].usable = true;
    snprintf(s.subs[1].lang, sizeof s.subs[1].lang, "fre");                  // another spelling of the same language
    CHECK(local_pick_sub(&s, 1) == 1);

    // whether the file can be played at all, and the sentence when not
    LocalInfo p;
    memset(&p, 0, sizeof p);
    p.video_ok = true;
    p.audio[p.n_audio++] = aud(LA_AC3, true, true, false, "eng");
    char why[160] = "x";
    CHECK(local_can_play(&p, &surround, why, sizeof why) && why[0] == '\0');
    CHECK(local_can_play(&p, &surround, nullptr, 0));
    p.video_ok = false;
    snprintf(p.video_reason, sizeof p.video_reason, "HEVC");
    CHECK(!local_can_play(&p, &surround, why, sizeof why) && strstr(why, "HEVC") && strstr(why, "picture"));
    p.video_reason[0] = '\0';
    CHECK(!local_can_play(&p, &surround, why, sizeof why) && strstr(why, "unsupported"));
    p.video_ok = true;
    p.audio[0].decodable = false;
    snprintf(p.audio[0].label, sizeof p.audio[0].label, "English - AAC - Stereo");
    CHECK(!local_can_play(&p, &surround, why, sizeof why) && strstr(why, "AAC") && strstr(why, "audio"));
    p.n_audio = 0;
    CHECK(!local_can_play(&p, &surround, why, sizeof why) && strstr(why, "no sound"));
    CHECK(!local_can_play(&p, &surround, why, 8) && strlen(why) == 7);       // cut, still terminated
}

static void languages() {
    printf("- language names\n");
    struct { const char *code, *name; } t[] = {
        { "eng", "English" }, { "en", "English" }, { "ENG", "English" }, { "fre", "French" }, { "fra", "French" }, { "fr", "French" },
        { "ger", "German" }, { "deu", "German" }, { "jpn", "Japanese" }, { "zho", "Chinese" }, { "chi", "Chinese" },
        { "pt-BR", "Portuguese" }, { "en_US", "English" }, { "und", "Unknown" }, { "", "Unknown" }, { "zxx", "Unknown" },
        { "tlh", "TLH" }, { "x", "X" },
    };
    for (auto &x : t) {
        char out[32];
        lang_name(x.code, out, sizeof out);
        if (strcmp(out, x.name)) printf("  %s -> %s (want %s)\n", x.code, out, x.name);
        CHECK(!strcmp(out, x.name));
    }
    char tiny[4];
    lang_name("eng", tiny, sizeof tiny);
    CHECK(!strcmp(tiny, "Eng"));                                             // cut, still terminated
    lang_name(nullptr, tiny, sizeof tiny);
    CHECK(!strcmp(tiny, "Unk"));
    lang_name("eng", tiny, 0);                                               // no room: nothing written, no crash
}

static void clipping() {
    printf("- clipping labels\n");
    const char *s = "ab\xC3\xA9" "cd";                                       // a b e-acute c d: the acute is 2 bytes
    CHECK(local_clip_utf8(s, 10) == 6);                                      // all of it
    CHECK(local_clip_utf8(s, 3) == 2);                                       // would cut the e-acute in half: stops before it
    CHECK(local_clip_utf8(s, 4) == 4);                                       // right after it
    CHECK(local_clip_utf8(s, 2) == 2 && local_clip_utf8(s, 0) == 0);
    const char *cjk = "\xE6\x97\xA5\xE6\x9C\xAC";                            // two 3-byte characters
    CHECK(local_clip_utf8(cjk, 5) == 3 && local_clip_utf8(cjk, 4) == 3 && local_clip_utf8(cjk, 6) == 6 && local_clip_utf8(cjk, 2) == 0);
    CHECK(local_clip_utf8("", 5) == 0);
}

// The Media browser probes on a worker thread, and loading_run's threads have a 128 KB stack: the
// probe must stay far below that (the big buffers live on the heap).
static void *probe_all_fixtures(void *ok) {
    static LocalInfo i;
    bool all = true;
    for (const char *p : { "fixtures/mkv/av.mkv", "fixtures/mkv/live.mkv", "fixtures/mkv/dts.mkv", "fixtures/ts/av.ts",
                           "fixtures/ts/av.m2ts", "fixtures/ts/hd.ts", "fixtures/ts/hevc.ts", "fixtures/ts/high10.ts" })
        if (!probe_file(p, &i)) all = false;
    *(bool *)ok = all;
    return nullptr;
}

static void small_stack() {
    printf("- on a small stack\n");
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, 48 * 1024);                      // well under a 128 KB worker's
    bool ok = false;
    pthread_t t;
    CHECK(pthread_create(&t, &a, probe_all_fixtures, &ok) == 0);
    pthread_join(t, nullptr);
    CHECK(ok);
}

int main() {
    small_stack();
    mkv_files();
    ts_files();
    refusals();
    policy();
    languages();
    clipping();
    printf("local probe: %d checks, %d failed\n", s_checks, s_failed);
    return s_failed ? 1 : 0;
}
