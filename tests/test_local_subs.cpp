// Host test for the subtitles of a Matroska file, end to end as the player runs them: a file with an SRT, an ASS and
// a PGS track is opened with video/mkv_ts (mkv_ts_open_subs), read to the end, and every block of the chosen track
// reaches local/local_subs.cpp (the real sink), local/sub_conv.c and the player's own subtitles.cpp (compiled in
// with its console dependencies stubbed, as tests/test_subtitles.cpp does).  What the player would draw at a given
// moment is then looked up.
//
//   make -f Makefile.host test_local_subs && ./test_local_subs

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef unsigned int       u32;
typedef unsigned long long u64;

// ---- stubs for subtitles.cpp's console-only dependencies ------------------------------------------------------
#define RESPONSE_SIZE (384 * 1024)
#define HTTP_GET 0
char g_server[256] = "127.0.0.1:8096";
char g_token[256]  = "testtoken";
static int http_request(int, const char *, const char *, const char *, char *, int) { return -1; }
static int http_fetch_binary(const char *, const char *, unsigned char *, int) { return -1; }
static void plog(const char *) {}
void plog_stub_keep(void) { plog(""); }

#define JF_SUBTITLES_TEST 1
#include "../source/player/subtitles.cpp"

#include "mkv_builder.h"
#include "mkv_ts.h"
#include "local_subs.h"

static int s_checks = 0, s_failed = 0;
#define CHECK(cond) do { s_checks++; if (!(cond)) { s_failed++; \
    printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

static Bytes seg(uint8_t type, const Bytes &payload) {
    Bytes b;
    b.push_back((char)type); b.push_back((char)(payload.size() >> 8)); b.push_back((char)payload.size());
    return b + payload;
}

// A PGS display set as Matroska stores it: the segments without "PG" headers (a 2x2 bitmap at (5,5), palette index 9).
static Bytes pgs_block() {
    static const unsigned char pcs[] = { 0x07,0x80,0x04,0x38, 0x10, 0x00,0x00, 0x80, 0x00, 0x01, 0x01, 0x00,0x01, 0x00, 0x00, 0x00,0x05, 0x00,0x05 };
    static const unsigned char pds[] = { 0x01,0x00, 0x09,150,128,128,255 };
    static const unsigned char ods[] = { 0x00,0x01, 0x00, 0xC0, 0x00,0x00,0x0E, 0x00,0x02, 0x00,0x02, 0x00,0x82,0x09, 0x00,0x00, 0x00,0x82,0x09, 0x00,0x00 };
    return seg(0x16, Bytes((const char *)pcs, sizeof pcs)) + seg(0x14, Bytes((const char *)pds, sizeof pds)) +
           seg(0x15, Bytes((const char *)ods, sizeof ods)) + seg(0x80, Bytes());
}

static Bytes timed(int track, int rel_ms, const Bytes &frame, int dur_ms) {
    Bytes g = elem(ID_BLOCK, block_payload(track, rel_ms, 0, { frame }, 0));
    if (dur_ms) g += uint_el(ID_BLOCKDURATION, (uint64_t)dur_ms);
    return elem(ID_BLOCKGROUP, g);
}

static Bytes nal(size_t n, int seed) {
    Bytes b;
    for (int i = 3; i >= 0; i--) b.push_back((char)((n >> (8 * i)) & 0xFF));
    Bytes body = frame_of(seed, n);
    body[0] = (char)0x65;
    return b + body;
}

static void drain(MkvTs *t) {
    static uint8_t buf[188 * 64];
    while (mkv_ts_read(t, buf, (int)sizeof buf) > 0) {}
}

int main() {
    Bytes tracks = track_video(1, avcc()) + track_audio(2, "A_AC3", "eng", 2) + track_sub(3, "S_TEXT/UTF8", "English", false) +
                   track_sub(4, "S_TEXT/ASS", "Signs", false) + track_sub(5, "S_HDMV/PGS", "Disc", false);
    std::vector<Bytes> c(2);
    // cluster 0 (0 ms): the picture, a sound frame, and the subtitles that start in it
    c[0] += simple_block(1, 0, true, { nal(300, 1) });
    c[0] += simple_block(2, 0, true, { frame_of(1, 100) });
    c[0] += timed(5, 500, pgs_block(), 0);                                          // PGS display set at 500 ms
    c[0] += timed(3, 1000, Bytes("<i>Hello</i>\nworld"), 1500);                     // SRT 1000 - 2500
    c[0] += timed(4, 1200, Bytes("0,0,Default,,0,0,0,,{\\an8}Top\\Nline"), 2000);   // ASS 1200 - 3200
    c[0] += simple_block(1, 40, false, { nal(100, 2) });
    c[0] += timed(3, 4000, Bytes("Later"), 1000);                                   // SRT 4000 - 5000
    c[0] += timed(3, 4500, Bytes("  \n"), 500);                                     // nothing to show
    // cluster 1 (5000 ms): a key frame to enter on, and its own cues
    c[1] += simple_block(1, 0, true, { nal(300, 3) });
    c[1] += simple_block(2, 0, true, { frame_of(2, 100) });
    c[1] += timed(3, 200, Bytes("Third"), 400);                                     // SRT 5200 - 5600
    c[1] += timed(3, 700, Bytes("No duration"), 0);                                 // SRT 5700, no BlockDuration: 2.5 s
    Built b = build(tracks, c, { 0, 5000 }, Opts());
    Mem m = { &b.file, -1, 0 };
    MkvFile f;
    CHECK(mkv_open(&f, mem_read, &m, b.file.size()) == 0);

    printf("- the SRT track\n");
    subs_local_begin(false);
    MkvTs *t = mkv_ts_open_subs(&f, 1, 2, 3, local_sub_sink, (void *)(intptr_t)LS_SRT, 0, nullptr, nullptr, 0);
    CHECK(t != nullptr);
    if (t) drain(t);
    if (t) mkv_ts_close(t);
    CHECK(subs_active() && !subs_is_pgs());
    CHECK(subs_text_at(900) == nullptr);
    CHECK(subs_text_at(1500) && !strcmp(subs_text_at(1500), "Hello\nworld"));                // tags gone, lines kept
    CHECK(subs_text_at(2600) == nullptr);
    CHECK(subs_text_at(4500) && !strcmp(subs_text_at(4500), "Later"));                      // (the empty block at 4500 added nothing)
    CHECK(subs_text_at(5300) && !strcmp(subs_text_at(5300), "Third"));                      // from the second cluster
    CHECK(subs_text_at(5800) && !strcmp(subs_text_at(5800), "No duration"));
    CHECK(subs_text_at(8100) && subs_text_at(8300) == nullptr);                             // 5700 + 2500 = 8200 is the end
    CHECK(subs_text_at(8199) && !strcmp(subs_text_at(8199), "No duration"));

    printf("- the ASS track\n");
    subs_local_begin(false);
    t = mkv_ts_open_subs(&f, 1, 2, 4, local_sub_sink, (void *)(intptr_t)LS_ASS, 0, nullptr, nullptr, 0);
    CHECK(t != nullptr);
    if (t) { drain(t); mkv_ts_close(t); }
    CHECK(subs_text_at(1500) && !strcmp(subs_text_at(1500), "Top\nline"));                  // override block gone, \N a break
    CHECK(subs_text_at(3300) == nullptr && subs_text_at(1100) == nullptr);
    CHECK(subs_text_at(4500) == nullptr);                                                   // the SRT track's cues are not here

    printf("- the PGS track\n");
    subs_local_begin(true);
    t = mkv_ts_open_subs(&f, 1, 2, 5, local_sub_sink, (void *)(intptr_t)LS_PGS, 0, nullptr, nullptr, 0);
    CHECK(t != nullptr);
    if (t) { drain(t); mkv_ts_close(t); }
    CHECK(subs_active() && subs_is_pgs());
    const PgsBitmap *bmp = subs_pgs_at(600);
    CHECK(bmp && bmp->width == 2 && bmp->height == 2 && bmp->x == 5 && bmp->y == 5 && bmp->frame_w == 1920);
    CHECK(subs_pgs_at(400) == nullptr);                                                     // before the display set's time
    CHECK(subs_text_at(1500) == nullptr);

    printf("- entering the file part way, and no subtitles\n");
    subs_local_begin(false);
    t = mkv_ts_open_subs(&f, 1, 2, 3, local_sub_sink, (void *)(intptr_t)LS_SRT, 5000ull * 1000000ull, nullptr, nullptr, 0);
    CHECK(t != nullptr);
    if (t) { drain(t); mkv_ts_close(t); }
    CHECK(subs_text_at(1500) == nullptr && subs_text_at(4500) == nullptr);                  // the first cluster was not read
    CHECK(subs_text_at(5300) && !strcmp(subs_text_at(5300), "Third"));
    subs_local_begin(false);
    t = mkv_ts_open(&f, 1, 2, 0, nullptr, nullptr, 0);                                      // no subtitle track asked for
    if (t) { drain(t); mkv_ts_close(t); }
    CHECK(subs_text_at(1500) == nullptr && subs_text_at(5300) == nullptr);
    // a sink given the wrong kind or no sink at all does no harm
    subs_local_begin(false);
    t = mkv_ts_open_subs(&f, 1, 2, 3, local_sub_sink, (void *)(intptr_t)LS_OTHER, 0, nullptr, nullptr, 0);
    if (t) { drain(t); mkv_ts_close(t); }
    t = mkv_ts_open_subs(&f, 1, 2, 3, nullptr, nullptr, 0, nullptr, nullptr, 0);
    if (t) { drain(t); mkv_ts_close(t); }
    CHECK(subs_text_at(1500) == nullptr);
    CHECK(local_sub_kind_playable(LS_SRT) && local_sub_kind_playable(LS_ASS) && local_sub_kind_playable(LS_PGS) && !local_sub_kind_playable(LS_OTHER));

    mkv_ts_release();
    mkv_close(&f);
    subs_clear();
    printf("local subs: %d checks, %d failed\n", s_checks, s_failed);
    return s_failed ? 1 : 0;
}
