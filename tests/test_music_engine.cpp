// Host test for the music engine on files from a drive: music/music_player.cpp itself, compiled here with
// threads as pthreads, a "sound card" that records what the engine's pump reads, and lfs reading the fixtures of
// tests/fixtures/music.  The engine's queue logic (gapless handover, seeking, skipping, jumping, stopping and
// starting again) runs for real; only the console's services are stand-ins (tests/hoststub_music).
//
// What the pump hears is compared with what local_audio decodes from the same files: a lossless queue must come
// out as the files one after the other with nothing cut out of the joins.
//
//   make -f Makefile.host test_music_engine && ./test_music_engine

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

#include "music_player.h"
#include "audio.h"
#include "lfs.h"
#include "local_audio.h"
#include "stream.h"

static int s_checks = 0, s_failed = 0;
#define CHECK(cond) do { s_checks++; if (!(cond)) { s_failed++; \
    printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

// ---------------------------------------------------------------------------
//  The console's services, as the engine sees them
// ---------------------------------------------------------------------------

u32 running = 1;
volatile bool g_stream_cancel = false;
char g_server[256] = "";
char g_token[256] = "";
char g_userid[64] = "";
char responseBuffer[4096];

void crash_log(const char *) {}
static bool s_verbose = false;
void plog(const char *msg) { if (s_verbose) printf("    plog: %s\n", msg); }
u64 timing_get_us(void) { timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (u64)t.tv_sec * 1000000ULL + (u64)t.tv_nsec / 1000ULL; }
const char *jf_data_path(const char *) { return "/nonexistent/jf-test/none"; }

// the server: nothing is reached by a queue of files; these count anything that is
static std::atomic<int> s_server_calls(0);
const char *jf_device_id(void) { s_server_calls++; return "test"; }
bool jellyfin_get_play_session_id(const char *, char *, int, unsigned *) { s_server_calls++; return false; }
void jellyfin_report_playing(const char *, const char *, u64) { s_server_calls++; }
void jellyfin_report_stopped(const char *, const char *, u64) { s_server_calls++; }
void jellyfin_report_progress_async(const char *, const char *, u64, bool) { s_server_calls++; }
void jellyfin_stop_transcode(const char *) { s_server_calls++; }
void jellyfin_report_init(void) { s_server_calls++; }
void jellyfin_report_flush(void) { s_server_calls++; }
int http_request(int, const char *, const char *, const char *, char *, int) { s_server_calls++; return -1; }
int stream_open(const char *) { s_server_calls++; return -1; }
int stream_read(int, u8 *, int) { s_server_calls++; return -1; }
bool xmb_json_str_range(const char *, int, const char *, char *, int) { return false; }
long long xmb_json_ll_range(const char *, int, const char *, long long def) { return def; }
int xmb_json_int_range(const char *, int, const char *, int def) { return def; }
bool xmb_json_first_arr_str(const char *, int, const char *, char *, int) { return false; }
void decode_unicode_escapes(char *) {}

// the visualisers' taps
void music_viz_reset(void) {}
void music_viz_push(const float *, int) {}
void music_sv_reset(void) {}
void music_sv_push(const float *, int) {}
void wave_audio_push(const float *, int) {}

// ---- the sound card: the pump reads blocks of 256 pairs from the engine's source ----
static audio_avail_fn s_avail = NULL;
static audio_read_fn s_read = NULL;
static std::mutex s_cap_mtx;
static std::vector<float> s_captured;
static std::atomic<bool> s_card_open(true);       // false: the card consumes nothing (the test stages commands first)
static std::atomic<int> s_pace_us(0);             // sleep after each block

void audio_set_source(audio_avail_fn a, audio_read_fn r, audio_channels_fn) { s_avail = a; s_read = r; }
void audio_open(int) {}
void audio_set_paced(bool) {}
void audio_close(void) {}
bool audio_write_pcm(void) {
    if (!s_card_open || !s_avail) return false;
    if (s_avail() < 256) return false;
    float b[512];
    const int n = s_read(b, 256);
    {
        std::lock_guard<std::mutex> g(s_cap_mtx);
        s_captured.insert(s_captured.end(), b, b + n * 2);
    }
    if (s_pace_us > 0) usleep((useconds_t)s_pace_us.load());
    return true;
}

// ---- lfs: "usb0:/m/<file>" is tests/fixtures/music/<file> ----
static std::string s_fail_name;                    // reads of a file whose name contains this fail from fail_at on
static std::atomic<long> s_fail_at(0);
struct HFile { FILE *f; bool fails; };
static std::mutex s_lfs_mtx;
static std::vector<HFile> s_handles;

static std::string real_path(const char *p) {
    const char *slash = strrchr(p, '/');
    return std::string("fixtures/music/") + (slash ? slash + 1 : p);
}
int lfs_open(const char *path) {
    if (strstr(path, "missing")) return LFS_E_NOTFOUND;
    FILE *f = fopen(real_path(path).c_str(), "rb");
    if (!f) return LFS_E_NOTFOUND;
    std::lock_guard<std::mutex> g(s_lfs_mtx);
    s_handles.push_back({ f, !s_fail_name.empty() && strstr(path, s_fail_name.c_str()) != NULL });
    return (int)s_handles.size() - 1;
}
int lfs_read(int h, uint64_t off, void *buf, uint32_t n) {
    std::lock_guard<std::mutex> g(s_lfs_mtx);
    if (h < 0 || h >= (int)s_handles.size() || !s_handles[(size_t)h].f) return LFS_E_IO;
    HFile &f = s_handles[(size_t)h];
    if (f.fails && (long)off >= s_fail_at) return LFS_E_REMOVED;
    if (f.fails && (long)(off + n) > s_fail_at) n = (uint32_t)(s_fail_at - (long)off);
    if (fseek(f.f, (long)off, SEEK_SET)) return LFS_E_IO;
    return (int)fread(buf, 1, n, f.f);
}
uint64_t lfs_size(int h) {
    std::lock_guard<std::mutex> g(s_lfs_mtx);
    FILE *f = s_handles[(size_t)h].f;
    fseek(f, 0, SEEK_END);
    return (uint64_t)ftell(f);
}
void lfs_close(int h) {
    std::lock_guard<std::mutex> g(s_lfs_mtx);
    if (h >= 0 && h < (int)s_handles.size() && s_handles[(size_t)h].f) { fclose(s_handles[(size_t)h].f); s_handles[(size_t)h].f = NULL; }
}
static int open_handles() {
    std::lock_guard<std::mutex> g(s_lfs_mtx);
    int n = 0;
    for (const HFile &f : s_handles) if (f.f) n++;
    return n;
}

// ---------------------------------------------------------------------------
//  Helpers
// ---------------------------------------------------------------------------

typedef std::vector<float> Floats;

// What local_audio decodes from a fixture: the reference for what the pump should hear.
static Floats decode_file(const char *name) {
    Floats out;
    std::string p = std::string("fixtures/music/") + name;
    FILE *f = fopen(p.c_str(), "rb");
    if (!f) { CHECK(false); return out; }
    fseek(f, 0, SEEK_END);
    const long size = ftell(f);
    std::vector<uint8_t> data((size_t)size);
    fseek(f, 0, SEEK_SET);
    if (fread(data.data(), 1, (size_t)size, f) != (size_t)size) CHECK(false);
    fclose(f);
    struct Mem { const std::vector<uint8_t> *d; } mem = { &data };
    auto rd = [](void *c, uint64_t off, uint8_t *buf, int len) -> int {
        const std::vector<uint8_t> &d = *((Mem *)c)->d;
        if (off >= d.size()) return 0;
        size_t n = d.size() - (size_t)off;
        if (n > (size_t)len) n = (size_t)len;
        memcpy(buf, d.data() + off, n);
        return (int)n;
    };
    LaMeta m;
    if (!la_read_meta(rd, &mem, data.size(), la_kind_of(name), &m)) { CHECK(false); return out; }
    LaDecoder *d = la_open(rd, &mem, data.size(), &m);
    if (!d) { CHECK(false); return out; }
    float buf[4096 * 2];
    int n;
    while ((n = la_decode(d, buf, 4096)) > 0) out.insert(out.end(), buf, buf + n * 2);
    la_close(d);
    return out;
}

static std::string format_line_of(const char *name) {
    std::string p = std::string("fixtures/music/") + name;
    FILE *f = fopen(p.c_str(), "rb");
    std::vector<uint8_t> data(1 << 20);
    const size_t n = fread(data.data(), 1, data.size(), f);
    fclose(f);
    data.resize(n);
    struct Mem { const std::vector<uint8_t> *d; } mem = { &data };
    LaMeta m;
    char line[48] = "";
    if (la_read_meta([](void *c, uint64_t off, uint8_t *buf, int len) -> int {
            const std::vector<uint8_t> &d = *((Mem *)c)->d;
            if (off >= d.size()) return 0;
            size_t k = d.size() - (size_t)off;
            if (k > (size_t)len) k = (size_t)len;
            memcpy(buf, d.data() + off, k);
            return (int)k;
        }, &mem, data.size(), la_kind_of(name), &m))
        la_format_line(&m, line, sizeof line);
    return line;
}

static std::vector<MusicTrack> queue_of(const std::vector<const char *> &names) {
    std::vector<MusicTrack> q;
    int i = 0;
    for (const char *n : names) {
        MusicTrack t;
        memset(&t, 0, sizeof t);
        snprintf(t.id, sizeof t.id, "file:%d", i++);
        snprintf(t.name, sizeof t.name, "%s", n);
        snprintf(t.path, sizeof t.path, "usb0:/m/%s", n);
        t.duration_secs = 2;
        q.push_back(t);
    }
    return q;
}

static void sleep_ms(int ms) { usleep((useconds_t)ms * 1000); }

static bool wait_until(bool (*cond)(), int timeout_ms) {
    for (int t = 0; t < timeout_ms; t += 5) {
        if (cond()) return true;
        sleep_ms(5);
    }
    return cond();
}
static bool queue_done() { return !music_is_active(); }

static Floats take_captured() {
    std::lock_guard<std::mutex> g(s_cap_mtx);
    Floats f;
    f.swap(s_captured);
    return f;
}

// Starts a queue, lets it play to its end, stops it, and returns what the pump heard.
static Floats play_through(const std::vector<MusicTrack> &q, int start = 0, int timeout_ms = 20000) {
    take_captured();
    CHECK(music_start(q.data(), (int)q.size(), start));
    CHECK(wait_until(queue_done, timeout_ms));
    sleep_ms(20);
    music_stop();
    return take_captured();
}

static Floats cat(const Floats &a, const Floats &b) { Floats r = a; r.insert(r.end(), b.begin(), b.end()); return r; }

// `heard` is `want` less the last partial block the pump never takes (under 256 pairs), exactly.
static bool is_whole_prefix(const Floats &heard, const Floats &want) {
    if (heard.size() > want.size() || want.size() - heard.size() >= 256 * 2) return false;
    return memcmp(heard.data(), want.data(), heard.size() * sizeof(float)) == 0;
}

// ---------------------------------------------------------------------------
//  Tests
// ---------------------------------------------------------------------------

static void test_gapless_exact() {
    printf("- lossless and LAME-tagged files join with nothing cut\n");
    const Floats flac48 = decode_file("chirp48.flac");
    const Floats flac44 = decode_file("chirp44.flac");
    const Floats lame = decode_file("chirp44_v24.mp3");
    CHECK(flac48.size() == 96000 * 2 && flac44.size() > 90000 * 2 && lame.size() > 90000 * 2);

    // one track, to its end
    Floats heard = play_through(queue_of({ "chirp48.flac" }));
    CHECK(is_whole_prefix(heard, flac48));

    // two of the same, and one that is resampled, and a mix of every kind
    heard = play_through(queue_of({ "chirp48.flac", "chirp48.flac" }));
    CHECK(is_whole_prefix(heard, cat(flac48, flac48)));
    heard = play_through(queue_of({ "chirp44.flac", "chirp48.flac", "chirp44.flac" }));
    CHECK(is_whole_prefix(heard, cat(cat(flac44, flac48), flac44)));
    heard = play_through(queue_of({ "chirp44_v24.mp3", "chirp44_v23.mp3" }));
    CHECK(is_whole_prefix(heard, cat(lame, lame)));
    heard = play_through(queue_of({ "chirp44_v24.mp3", "chirp48.flac", "chirp44_v24.mp3" }));
    CHECK(is_whole_prefix(heard, cat(cat(lame, flac48), lame)));
    CHECK(open_handles() == 0);
    CHECK(s_server_calls == 0);                                   // no server was asked for anything

    // starting in the middle of the queue
    heard = play_through(queue_of({ "chirp48.flac", "chirp44.flac", "chirp48.flac" }), 1);
    CHECK(is_whole_prefix(heard, cat(flac44, flac48)));
}

static void test_padded_mp3() {
    printf("- an MP3 without a LAME header has its padding trimmed at the join\n");
    // silence at both ends: the join drops up to 2400 silent samples from the end of the first and from the start of
    // the second, and no more, however much silence there is
    const Floats pad = decode_file("silence_pad.mp3");
    CHECK(pad.size() > 40000 * 2);
    const Floats both = cat(pad, pad);
    s_pace_us = 1500;                                                 // the ring holds seconds at the join, as it does for real
    const Floats heard = play_through(queue_of({ "silence_pad.mp3", "silence_pad.mp3" }));
    const long lost = (long)(both.size() - heard.size()) / 2;
    CHECK(lost >= 2 * 2400 - 300 && lost <= 2 * 2400 + 256);
    // the first track is intact up to where it was cut, the second is intact from where it starts
    CHECK(memcmp(heard.data(), pad.data(), (pad.size() / 2 - 2400 - 300) * 2 * sizeof(float)) == 0);
    // one track alone loses nothing but the pump's last block
    CHECK(is_whole_prefix(play_through(queue_of({ "silence_pad.mp3" })), pad));
    // a track with exact information (LAME header) next to a padded one: nothing is cut from the exact one
    const Floats lame = decode_file("chirp44_v24.mp3");
    const Floats mixed = play_through(queue_of({ "chirp44_v24.mp3", "silence_pad.mp3" }));
    s_pace_us = 0;
    const long lost2 = (long)(lame.size() + pad.size() - mixed.size()) / 2;
    CHECK(lost2 >= 2400 - 300 && lost2 <= 2400 + 256);                 // only the padded one's start
    CHECK(memcmp(mixed.data(), lame.data(), lame.size() * sizeof(float)) == 0);
}

// Played at the speed of sound the whole of the next track is queued behind the one being heard when it is decoded:
// the handover is a boundary in the ring, and what is shown follows what is heard.
static void test_paced_handover() {
    printf("- the boundary in a full ring\n");
    const Floats a = decode_file("chirp48.flac");
    const Floats b = decode_file("chirp44.flac");
    const Floats c = decode_file("chirp44_v24.mp3");
    s_pace_us = 1500;
    take_captured();
    const std::vector<MusicTrack> q = queue_of({ "chirp48.flac", "chirp44.flac", "chirp44_v24.mp3" });
    CHECK(music_start(q.data(), 3, 0));
    std::vector<int> seen;
    std::vector<std::string> lines;
    const std::string want[3] = { format_line_of("chirp48.flac"), format_line_of("chirp44.flac"), format_line_of("chirp44_v24.mp3") };
    for (int t = 0; t < 20000 && music_is_active(); t += 2) {
        const int pos = music_current_pos();
        if (seen.empty() || seen.back() != pos) { seen.push_back(pos); lines.push_back(music_source_info()); }
        sleep_ms(2);
    }
    sleep_ms(20);
    music_stop();
    s_pace_us = 0;
    const Floats heard = take_captured();
    CHECK(is_whole_prefix(heard, cat(cat(a, b), c)));
    CHECK(seen.size() == 3 && seen[0] == 0 && seen[1] == 1 && seen[2] == 2);
    if (lines.size() == 3) for (int i = 0; i < 3; i++) CHECK(lines[(size_t)i] == want[i]);

    // tracks so short that several are in the ring at once: the screen still goes through every one, in order
    const Floats m = decode_file("mono22.mp3");
    s_pace_us = 1500;
    take_captured();
    const std::vector<MusicTrack> q5 = queue_of({ "mono22.mp3", "mono22.mp3", "mono22.mp3", "mono22.mp3", "mono22.mp3" });
    CHECK(music_start(q5.data(), 5, 0));
    seen.clear();
    for (int t = 0; t < 20000 && music_is_active(); t += 1) {
        const int pos = music_current_pos();
        if (seen.empty() || seen.back() != pos) seen.push_back(pos);
        usleep(1000);
    }
    sleep_ms(20);
    music_stop();
    s_pace_us = 0;
    Floats five;
    for (int i = 0; i < 5; i++) five = cat(five, m);
    CHECK(is_whole_prefix(take_captured(), five));
    CHECK(seen == std::vector<int>({ 0, 1, 2, 3, 4 }));
}

static void test_info() {
    printf("- the source line and the length\n");
    take_captured();
    s_card_open = false;
    const std::vector<MusicTrack> q = queue_of({ "chirp44.flac", "chirp44_v24.mp3" });
    CHECK(music_start(q.data(), 2, 0));
    sleep_ms(150);
    CHECK(music_is_active() && music_current_pos() == 0 && music_current_index() == 0);
    CHECK(format_line_of("chirp44.flac") == "FLAC 44.1 kHz / 16-bit");
    CHECK(!strcmp(music_source_info(), "FLAC 44.1 kHz / 16-bit"));
    CHECK(music_duration_secs() == 2 && music_elapsed_secs() == 0);
    music_next();
    sleep_ms(150);
    CHECK(music_current_pos() == 1 && !strcmp(music_source_info(), format_line_of("chirp44_v24.mp3").c_str()));
    music_stop();
    s_card_open = true;
    CHECK(!music_is_active());
}

static void test_commands() {
    printf("- seek, next, previous, jump\n");
    const Floats flac48 = decode_file("chirp48.flac");
    const Floats flac44 = decode_file("chirp44.flac");
    const Floats lame = decode_file("chirp44_v24.mp3");
    {   // a seek: the track goes on from a second in, exactly
        take_captured();
        s_card_open = false;
        const std::vector<MusicTrack> q = queue_of({ "chirp48.flac" });
        CHECK(music_start(q.data(), 1, 0));
        sleep_ms(100);
        music_seek(+1);
        sleep_ms(150);
        CHECK(music_elapsed_secs() == 1);
        s_card_open = true;
        CHECK(wait_until(queue_done, 10000));
        sleep_ms(20);
        music_stop();
        const Floats heard = take_captured();
        const Floats from_1s(flac48.begin() + 48000 * 2, flac48.end());
        CHECK(is_whole_prefix(heard, from_1s));
    }
    {   // next, jump, previous: where the queue is, and what plays from there
        take_captured();
        s_card_open = false;
        const std::vector<MusicTrack> q = queue_of({ "chirp48.flac", "chirp44.flac", "chirp44_v24.mp3" });
        CHECK(music_start(q.data(), 3, 0));
        sleep_ms(100);
        music_next();
        sleep_ms(120);
        CHECK(music_current_pos() == 1);
        music_jump(2);
        sleep_ms(120);
        CHECK(music_current_pos() == 2);
        music_prev();                                              // within the first seconds: the track before
        sleep_ms(120);
        CHECK(music_current_pos() == 1);
        s_card_open = true;
        CHECK(wait_until(queue_done, 10000));
        sleep_ms(20);
        music_stop();
        CHECK(is_whole_prefix(take_captured(), cat(flac44, lame)));
    }
    {   // next at the last track ends the queue; previous at the first stays on it
        take_captured();
        s_card_open = false;
        const std::vector<MusicTrack> q = queue_of({ "chirp48.flac", "chirp44.flac" });
        CHECK(music_start(q.data(), 2, 0));
        sleep_ms(100);
        music_prev();
        sleep_ms(120);
        CHECK(music_current_pos() == 0 && music_is_active());
        music_jump(1);
        sleep_ms(120);
        music_next();
        CHECK(wait_until(queue_done, 5000));
        music_stop();
        s_card_open = true;
        take_captured();
    }
}

static void test_failures() {
    printf("- files that cannot be opened or read\n");
    const Floats a = decode_file("chirp48.flac");
    const Floats b = decode_file("chirp44.flac");
    // a missing file in the middle is skipped without a gap
    Floats heard = play_through(queue_of({ "chirp48.flac", "missing.flac", "chirp44.flac" }));
    CHECK(is_whole_prefix(heard, cat(a, b)));
    // three in a row end the queue
    heard = play_through(queue_of({ "chirp48.flac", "missing1.flac", "missing2.flac", "missing3.flac", "chirp44.flac" }));
    CHECK(is_whole_prefix(heard, a));
    // a last file that is missing, with the ring full: the track before it plays out, it is not cut off by the end of the queue
    s_pace_us = 1500;
    heard = play_through(queue_of({ "chirp48.flac", "missing.flac" }));
    s_pace_us = 0;
    CHECK(is_whole_prefix(heard, a));
    s_pace_us = 1500;
    heard = play_through(queue_of({ "chirp48.flac", "missing1.flac", "missing2.flac", "missing3.flac", "chirp44.flac" }));
    s_pace_us = 0;
    CHECK(is_whole_prefix(heard, a));
    // a first file that is missing, the rest played
    heard = play_through(queue_of({ "missing.flac", "chirp44.flac" }));
    CHECK(is_whole_prefix(heard, b));
    // two bad ones and then good: the count starts over after a good one
    heard = play_through(queue_of({ "missing1.flac", "missing2.flac", "chirp48.flac", "missing3.flac", "missing4.flac", "chirp44.flac" }));
    CHECK(is_whole_prefix(heard, cat(a, b)));

    // a file that stops being readable part way (the drive is pulled): what was read is heard, the next file plays
    s_fail_name = "chirp48.flac";
    s_fail_at = 40000;                                                // of ~77 KB
    heard = play_through(queue_of({ "chirp48.flac", "chirp44.flac" }));
    s_fail_name.clear();
    // locate where the second file starts: its first samples
    size_t at = 0;
    bool found = false;
    for (size_t i = 0; i + 200 <= heard.size(); i += 2) {
        if (memcmp(&heard[i], b.data(), 200 * sizeof(float)) == 0 && i > 0) { at = i; found = true; break; }
    }
    CHECK(found);
    if (found) {
        CHECK(at / 2 > 10000 && at / 2 < 96000);                       // part of the first, not all of it
        CHECK(memcmp(heard.data(), a.data(), at * sizeof(float)) == 0); // and what there is of it is right
        const Floats second(heard.begin() + (long)at, heard.end());
        CHECK(is_whole_prefix(second, b));
    }
    CHECK(open_handles() == 0);

    // every file failing from the first byte: nothing plays, nothing hangs
    s_fail_name = "chirp";
    s_fail_at = 0;
    heard = play_through(queue_of({ "chirp48.flac", "chirp44.flac", "chirp48.flac", "chirp44.flac" }), 0, 15000);
    s_fail_name.clear();
    CHECK(heard.empty());
    CHECK(open_handles() == 0);
}

static void test_stop_and_restart() {
    printf("- stopping in the middle, and starting again\n");
    const Floats a = decode_file("chirp48.flac");
    s_pace_us = 3000;                                                 // about 1.8 times the speed of sound
    take_captured();
    const std::vector<MusicTrack> q = queue_of({ "chirp48.flac", "chirp48.flac" });
    CHECK(music_start(q.data(), 2, 0));
    sleep_ms(250);
    const u64 t0 = timing_get_us();
    music_stop();
    CHECK(timing_get_us() - t0 < 1500000);                           // returns at once
    CHECK(!music_is_active());
    const Floats part = take_captured();
    CHECK(part.size() > 1000 * 2 && part.size() < a.size());
    CHECK(memcmp(part.data(), a.data(), part.size() * sizeof(float)) == 0);
    music_stop();                                                     // twice is fine
    s_pace_us = 0;

    // a new session after a stop is a clean one: the same queue, heard whole
    const Floats heard = play_through(q);
    CHECK(is_whole_prefix(heard, cat(a, a)));
    CHECK(open_handles() == 0);

    // and after a session that ended in failure
    s_fail_name = "chirp48.flac";
    s_fail_at = 0;
    play_through(queue_of({ "chirp48.flac" }), 0, 5000);
    s_fail_name.clear();
    CHECK(is_whole_prefix(play_through(queue_of({ "chirp48.flac" })), a));
}

static void test_pause() {
    printf("- pause holds the sound\n");
    const Floats a = decode_file("chirp48.flac");
    s_pace_us = 2000;
    take_captured();
    const std::vector<MusicTrack> q = queue_of({ "chirp48.flac" });
    CHECK(music_start(q.data(), 1, 0));
    sleep_ms(120);
    music_toggle_pause();
    CHECK(music_is_paused());
    sleep_ms(60);
    const size_t at_pause = take_captured().size();
    sleep_ms(200);
    CHECK(take_captured().empty());                                  // nothing heard while paused
    music_toggle_pause();
    CHECK(!music_is_paused());
    CHECK(wait_until(queue_done, 10000));
    sleep_ms(20);
    music_stop();
    s_pace_us = 0;
    (void)at_pause;
    CHECK(s_server_calls == 0);
}

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "-v")) s_verbose = true;
    test_gapless_exact();
    test_padded_mp3();
    test_paced_handover();
    test_info();
    test_commands();
    test_failures();
    test_stop_and_restart();
    test_pause();
    printf("music engine: %d checks, %d failed\n", s_checks, s_failed);
    return s_failed ? 1 : 0;
}
