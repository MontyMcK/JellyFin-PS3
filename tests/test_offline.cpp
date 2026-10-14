// Host tests for the offline download core (source/offline).
//
// The download manager, store, record format and HTTP pieces compiled here
// are the SAME files the PS3 build compiles; only dl_platform.h is backed by
// tests/dl_fake.cpp -- a real temp directory and a scripted fake server.
//
//   make -f Makefile.host test_offline && ./test_offline [-v]

#include "dl_fake.h"
#include "dl_http.h"
#include "dl_manager.h"
#include "dl_model.h"
#include "dl_platform.h"
#include "dl_store.h"
#include "dl_request.h"
#include "dl_service.h"
#include "dl_ts.h"
#include "dl_library.h"
#include "dl_ui.h"
#include "stream_local.h"
#include "dl_season.h"
#include "stream_request.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <map>
#include <string>
#include <unistd.h>   // truncate

static int s_checks = 0, s_failed = 0;
static const char *s_test = "";

#define CHECK(cond) do { s_checks++; if (!(cond)) { s_failed++; \
    printf("  FAIL %s:%d [%s]: %s\n", __FILE__, __LINE__, s_test, #cond); } } while (0)

static std::string s_tmp;    // per-test temp dir
static std::string s_root;   // the store root inside it
static DlConfig    s_cfg;

static void small_config(DlConfig *c) {
    dl_config_defaults(c);
    c->head_timeout_ms   = 10000;
    c->idle_timeout_ms   = 5000;
    c->checkpoint_ms     = 1000000;   // byte-driven in tests
    c->checkpoint_bytes  = 50000;
    c->space_check_bytes = 60000;
    c->progress_ms       = 0;         // every read visible to the tests
}

static void begin(const char *name) {
    s_test = name;
    printf("- %s\n", name);
    fake_reset();
    if (!s_tmp.empty()) fake_rmtree(s_tmp);
    s_tmp  = fake_mkdtemp();
    s_root = s_tmp + "/offline";
    small_config(&s_cfg);
    // A server with no artwork unless a test says otherwise, so image
    // requests never consume the media responses a test scripted.
    { FakeResp nf; nf.status = 404; g_fake_routes["/Images/"] = nf; }
    dl_set_suspended(false);
    dl_playback_end();
    dl_set_auth_header("X-Emby-Authorization: MediaBrowser Token=\"t\"");
    CHECK(dl_manager_init(s_root.c_str(), &s_cfg));
}

// Re-open the same root, as a restart of the app would.
static void restart_app(void) {
    dl_manager_shutdown();
    CHECK(dl_manager_init(s_root.c_str(), &s_cfg));
}

static DlMeta meta_for(const char *id, const char *title = "A Film") {
    DlMeta m;
    dl_meta_init(&m);
    snprintf(m.id, sizeof(m.id), "%s", id);
    snprintf(m.title, sizeof(m.title), "%s", title);
    snprintf(m.type, sizeof(m.type), "Movie");
    snprintf(m.container, sizeof(m.container), "ts");
    return m;
}

static const char *URL = "http://192.168.1.2:8096/Videos/abc/stream.ts?VideoCodec=h264";

static DlStatus status_of(const char *id) {
    DlStatus st;
    memset(&st, 0, sizeof(st));
    if (!dl_find(id, &st)) st.rec.state = DL_STATE_COUNT;   // "absent"
    return st;
}

static std::string item_file(const char *id, const char *leaf) {
    char p[DL_PATH_MAX];
    dl_store_item_file(p, sizeof(p), id, leaf);
    return p;
}

// Run the worker until nothing is due (bounded).
static int run_worker(int max_steps = 50) {
    int steps = 0;
    while (steps < max_steps && dl_manager_step()) steps++;
    return steps;
}

// Skip past any backoff and run one more attempt.
static bool step_after_backoff(void) {
    g_fake_now_ms += 400000;
    return dl_manager_step();
}

static bool last_request_has_range(uint64_t from) {
    if (g_fake_requests.empty()) return false;
    char want[64];
    snprintf(want, sizeof(want), "Range: bytes=%llu-\r\n", (unsigned long long)from);
    return g_fake_requests.back().find(want) != std::string::npos;
}

static bool last_request_has_no_range(void) {
    return !g_fake_requests.empty() &&
           g_fake_requests.back().find("Range:") == std::string::npos;
}

// =========================================================================
// State machine
// =========================================================================

static void test_state_machine(void) {
    s_test = "state machine"; printf("- %s\n", s_test);
    struct { DlState from; DlEvent ev; DlState to; } legal[] = {
        { DL_QUEUED,      DL_EV_START,       DL_DOWNLOADING },
        { DL_QUEUED,      DL_EV_PAUSE,       DL_PAUSED },
        { DL_QUEUED,      DL_EV_CANCEL,      DL_CANCELLED },
        { DL_DOWNLOADING, DL_EV_PAUSE,       DL_PAUSED },
        { DL_DOWNLOADING, DL_EV_CANCEL,      DL_CANCELLED },
        { DL_DOWNLOADING, DL_EV_COMPLETE,    DL_COMPLETED },
        { DL_DOWNLOADING, DL_EV_FAIL_RETRY,  DL_QUEUED },
        { DL_DOWNLOADING, DL_EV_INTERRUPTED, DL_QUEUED },
        { DL_DOWNLOADING, DL_EV_FAIL,        DL_FAILED },
        { DL_PAUSED,      DL_EV_RESUME,      DL_QUEUED },
        { DL_PAUSED,      DL_EV_CANCEL,      DL_CANCELLED },
        { DL_FAILED,      DL_EV_RETRY,       DL_QUEUED },
        { DL_FAILED,      DL_EV_CANCEL,      DL_CANCELLED },
        { DL_CANCELLED,   DL_EV_RETRY,       DL_QUEUED },
    };
    const int nlegal = (int)(sizeof(legal) / sizeof(legal[0]));
    for (int i = 0; i < nlegal; i++) {
        DlState out = DL_STATE_COUNT;
        CHECK(dl_next_state(legal[i].from, legal[i].ev, &out));
        CHECK(out == legal[i].to);
    }
    // Everything not in the table is refused and leaves *out untouched.
    int refused = 0;
    for (int s = 0; s < DL_STATE_COUNT; s++) {
        for (int e = DL_EV_START; e <= DL_EV_INTERRUPTED; e++) {
            bool is_legal = false;
            for (int i = 0; i < nlegal; i++)
                if (legal[i].from == s && legal[i].ev == e) is_legal = true;
            if (is_legal) continue;
            DlState out = DL_STATE_COUNT;
            CHECK(!dl_next_state((DlState)s, (DlEvent)e, &out));
            CHECK(out == DL_STATE_COUNT);
            refused++;
        }
    }
    CHECK(refused == DL_STATE_COUNT * 9 - nlegal);
    // COMPLETED has no way out at all.
    for (int e = DL_EV_START; e <= DL_EV_INTERRUPTED; e++) {
        DlState out;
        CHECK(!dl_next_state(DL_COMPLETED, (DlEvent)e, &out));
    }
    // Names round-trip.
    for (int s = 0; s < DL_STATE_COUNT; s++) {
        DlState back;
        CHECK(dl_state_from_name(dl_state_name((DlState)s), &back) && back == s);
    }
    for (int e = 0; e < DL_ERR_COUNT; e++) {
        DlError back;
        CHECK(dl_error_from_name(dl_error_name((DlError)e), &back) && back == e);
    }
    DlState dummy;
    CHECK(!dl_state_from_name("Downloading", &dummy));   // case-sensitive
    CHECK(!dl_state_from_name("", &dummy));
}

static void test_error_policy(void) {
    s_test = "error policy"; printf("- %s\n", s_test);
    CHECK(dl_error_for_http_status(200) == DL_ERR_NONE);
    CHECK(dl_error_for_http_status(206) == DL_ERR_NONE);
    CHECK(dl_error_for_http_status(401) == DL_ERR_AUTH);     // session: hold queue
    CHECK(dl_error_for_http_status(403) == DL_ERR_HTTP);     // account: permanent
    CHECK(dl_error_for_http_status(404) == DL_ERR_NOT_FOUND);
    CHECK(dl_error_for_http_status(410) == DL_ERR_NOT_FOUND);
    CHECK(dl_error_for_http_status(400) == DL_ERR_HTTP);
    CHECK(dl_error_for_http_status(429) == DL_ERR_SERVER);
    CHECK(dl_error_for_http_status(500) == DL_ERR_SERVER);
    CHECK(dl_error_for_http_status(503) == DL_ERR_SERVER);
    CHECK(dl_error_for_http_status(302) == DL_ERR_BAD_RESPONSE);
    CHECK(dl_error_retryable(DL_ERR_UNREACHABLE) && dl_error_retryable(DL_ERR_TIMEOUT));
    CHECK(dl_error_retryable(DL_ERR_PARTIAL) && dl_error_retryable(DL_ERR_SERVER));
    CHECK(!dl_error_retryable(DL_ERR_AUTH) && !dl_error_retryable(DL_ERR_NO_SPACE));
    CHECK(!dl_error_retryable(DL_ERR_NOT_FOUND) && !dl_error_retryable(DL_ERR_DISK));
    CHECK(dl_backoff_ms(0) == 0);
    CHECK(dl_backoff_ms(1) == 2000);
    CHECK(dl_backoff_ms(2) == 4000);
    CHECK(dl_backoff_ms(3) == 8000);
    CHECK(dl_backoff_ms(8) == 256000);
    CHECK(dl_backoff_ms(9) == 300000);
    CHECK(dl_backoff_ms(4000000000u) == 300000);
    for (int e = 1; e < DL_ERR_COUNT; e++) CHECK(dl_error_text((DlError)e)[0] != '\0');
}

static void test_progress_math(void) {
    s_test = "progress math"; printf("- %s\n", s_test);
    DlRecord r;
    dl_record_init(&r);
    CHECK(dl_progress_permille(&r) == -1);            // total unknown
    r.bytes_total = 1000; r.bytes_done = 0;   CHECK(dl_progress_permille(&r) == 0);
    r.bytes_done = 820;                       CHECK(dl_progress_permille(&r) == 820);
    r.bytes_done = 1000;                      CHECK(dl_progress_permille(&r) == 1000);
    r.bytes_total = 40ull << 30; r.bytes_done = 10ull << 30;   // 40 GB film
    CHECK(dl_progress_permille(&r) == 250);
    r.bytes_total = UINT64_MAX; r.bytes_done = UINT64_MAX / 2;
    int p = dl_progress_permille(&r);
    CHECK(p >= 499 && p <= 500);
    r.state = DL_COMPLETED; r.bytes_total = 0;
    CHECK(dl_progress_permille(&r) == 1000);
}

static void test_id_validation(void) {
    s_test = "id validation"; printf("- %s\n", s_test);
    CHECK(dl_id_valid("0123456789abcdef0123456789abcdef"));
    CHECK(dl_id_valid("a-b_C"));
    CHECK(!dl_id_valid(""));
    CHECK(!dl_id_valid("../etc"));
    CHECK(!dl_id_valid("a/b"));
    CHECK(!dl_id_valid("a.b"));
    CHECK(!dl_id_valid("a b"));
    std::string longid(DL_ID_MAX, 'a');
    CHECK(!dl_id_valid(longid.c_str()));
    CHECK(dl_id_valid(longid.substr(0, DL_ID_MAX - 1).c_str()));
}

// =========================================================================
// Persistence format
// =========================================================================

static void test_record_roundtrip(void) {
    s_test = "record round-trip"; printf("- %s\n", s_test);
    DlRecord r;
    dl_record_init(&r);
    snprintf(r.id, sizeof(r.id), "abc123");
    snprintf(r.title, sizeof(r.title), "Line one\nkey=forged\\ \r end");
    snprintf(r.url, sizeof(r.url), "%s", URL);
    r.state = DL_PAUSED; r.error = DL_ERR_PARTIAL; r.http_status = 206;
    r.seq = 42; r.attempts = 3;
    r.bytes_done = 5000000000ull; r.bytes_total = 9000000000ull; r.resumable = 1;
    char text[4096];
    int n = dl_record_format(&r, text, sizeof(text));
    CHECK(n > 0 && (int)strlen(text) == n);
    CHECK(strstr(text, "state=paused\n") != NULL);          // names, not digits
    CHECK(strstr(text, "\nkey=forged") == NULL);             // newline escaped
    DlRecord b;
    CHECK(dl_record_parse(text, &b));
    CHECK(strcmp(b.title, r.title) == 0);
    CHECK(strcmp(b.url, r.url) == 0);
    CHECK(b.state == DL_PAUSED && b.error == DL_ERR_PARTIAL && b.http_status == 206);
    CHECK(b.seq == 42 && b.attempts == 3 && b.resumable == 1);
    CHECK(b.bytes_done == 5000000000ull && b.bytes_total == 9000000000ull);
    // Too small a buffer fails cleanly instead of truncating.
    char tiny[40];
    CHECK(dl_record_format(&r, tiny, sizeof(tiny)) == -1);
    // CRLF line endings (a file edited on Windows over FTP) still load.
    std::string crlf;
    for (const char *p = text; *p; p++) { if (*p == '\n') crlf += "\r\n"; else crlf += *p; }
    // ...except the title had a real \r escaped as "\r", which stays intact.
    CHECK(dl_record_parse(crlf.c_str(), &b));
    CHECK(b.bytes_done == 5000000000ull);
}

static void test_record_malformed(void) {
    s_test = "malformed records"; printf("- %s\n", s_test);
    DlRecord b;
    const char *bad[] = {
        "",                                                         // empty
        "garbage",                                                  // no magic
        "jfdl-state 2\nid=a\nstate=queued\nend\n",                 // future version
        "jfdl-state 1\nid=a\nstate=queued\n",                      // torn: no end
        "jfdl-state 1\nstate=queued\nend\n",                       // no id
        "jfdl-state 1\nid=a\nend\n",                               // no state
        "jfdl-state 1\nid=a\nstate=exploded\nend\n",               // unknown state
        "jfdl-state 1\nid=../../x\nstate=queued\nend\n",           // traversal
        "jfdl-state 1\nid=a\nstate=queued\nbytes_done=12x\nend\n", // bad number
        "jfdl-state 1\nid=a\nstate=queued\nbytes_done=-1\nend\n",  // negative
        "jfdl-state 1\nid=a\nstate=queued\nseq=99999999999\nend\n",// overflow
        "jfdl-state 1\nid=a\nstate=queued\nbytes_done=10\nbytes_total=5\nend\n",
        "jfdl-state 1\nid=a\nstate=queued\nerror=melted\nend\n",
        "jfdl-state 1\nid=a\nstate=queued\nno equals sign\nend\n",
        "jfdl-state 1\nid=a\nstate=queued\n=value\nend\n",
        "jfdl-state 1\nid=a\nstate=queued\nresumable=2\nend\n",
    };
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        bool parsed = dl_record_parse(bad[i], &b);
        if (parsed) printf("    accepted #%u: %s\n", i, bad[i]);
        CHECK(!parsed);
    }
    // Unknown keys are tolerated (a newer build's file).
    CHECK(dl_record_parse("jfdl-state 1\nid=a\nstate=queued\nfuture_key=1\nend\n", &b));
    // An over-long url is refused, not truncated into a different request.
    std::string longurl = "jfdl-state 1\nid=a\nstate=queued\nurl=http://x/" +
                          std::string(DL_URL_MAX, 'q') + "\nend\n";
    CHECK(!dl_record_parse(longurl.c_str(), &b));
}

static void test_meta_roundtrip(void) {
    s_test = "metadata round-trip"; printf("- %s\n", s_test);
    DlMeta m = meta_for("ep01", "Pilot");
    snprintf(m.type, sizeof(m.type), "Episode");
    snprintf(m.series, sizeof(m.series), "Some Show");
    snprintf(m.series_id, sizeof(m.series_id), "show9");
    m.season = 1; m.episode = 3; m.year = 2008; m.runtime_secs = 2820;
    snprintf(m.overview, sizeof(m.overview), "Two lines\nof plot = spoilers");
    snprintf(m.video_codec, sizeof(m.video_codec), "h264");
    snprintf(m.audio_codec, sizeof(m.audio_codec), "ac3");
    m.width = 1920; m.height = 1080; m.audio_channels = 6; m.video_bitrate = 10000000;
    snprintf(m.media_source_id, sizeof(m.media_source_id), "src-a");
    m.audio_stream_index = 2;
    snprintf(m.quality, sizeof(m.quality), "High");
    snprintf(m.video_info, sizeof(m.video_info), "1080p H264 SDR");
    snprintf(m.audio_info, sizeof(m.audio_info), "English AC3 5.1");
    snprintf(m.poster, sizeof(m.poster), "poster.jpg");
    char text[4096];
    CHECK(dl_meta_format(&m, text, sizeof(text)) > 0);
    DlMeta b;
    CHECK(dl_meta_parse(text, &b));
    CHECK(strcmp(b.series, "Some Show") == 0 && b.season == 1 && b.episode == 3);
    CHECK(b.runtime_secs == 2820 && b.year == 2008);
    CHECK(strcmp(b.overview, m.overview) == 0);
    CHECK(b.width == 1920 && b.height == 1080 && b.audio_channels == 6);
    CHECK(b.video_bitrate == 10000000 && b.audio_stream_index == 2);
    CHECK(strcmp(b.poster, "poster.jpg") == 0 && b.backdrop[0] == '\0');
    // Defaults for a movie (no season/episode) survive a round trip.
    DlMeta mv = meta_for("mov1");
    CHECK(dl_meta_format(&mv, text, sizeof(text)) > 0 && dl_meta_parse(text, &b));
    CHECK(b.season == -1 && b.episode == -1 && b.audio_stream_index == -1);
}

static void test_meta_malformed(void) {
    s_test = "malformed metadata"; printf("- %s\n", s_test);
    DlMeta b;
    const char *bad[] = {
        "jfdl-meta 1\nid=a\nend\n",                               // no title
        "jfdl-meta 1\ntitle=x\nend\n",                            // no id
        "jfdl-meta 1\nid=a\ntitle=\nend\n",                       // empty title
        "jfdl-meta 1\nid=a\ntitle=x\n",                           // torn
        "jfdl-meta 1\nid=a\ntitle=x\nseason=one\nend\n",          // bad int
        "jfdl-meta 1\nid=a\ntitle=x\nposter=../../boot.jpg\nend\n", // traversal
        "jfdl-meta 1\nid=a\ntitle=x\nposter=/dev_flash/x\nend\n",
        "jfdl-meta 1\nid=a\ntitle=x\nbackdrop=a.b.c\nend\n",
        "jfdl-state 1\nid=a\ntitle=x\nend\n",                     // wrong kind
    };
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        bool parsed = dl_meta_parse(bad[i], &b);
        if (parsed) printf("    accepted #%u\n", i);
        CHECK(!parsed);
    }
}

// =========================================================================
// HTTP pieces
// =========================================================================

static void test_url_parse(void) {
    s_test = "url parse"; printf("- %s\n", s_test);
    DlUrl u;
    CHECK(dl_url_parse("http://192.168.1.2:8096/Videos/x/stream.ts?a=1", &u));
    CHECK(strcmp(u.host, "192.168.1.2") == 0 && u.port == 8096);
    CHECK(strcmp(u.path, "/Videos/x/stream.ts?a=1") == 0);
    CHECK(dl_url_parse("http://jelly.local/", &u) && u.port == 8096);  // app default
    CHECK(dl_url_parse("http://h:80", &u) && u.port == 80 && strcmp(u.path, "/") == 0);
    CHECK(dl_url_parse("http://h?x=1", &u) && strcmp(u.path, "/?x=1") == 0);
    CHECK(dl_url_parse("192.168.1.2:8096/Videos/x/stream.ts?a=1", &u) &&
          strcmp(u.host, "192.168.1.2") == 0 && u.port == 8096);   // scheme-less server
    CHECK(!dl_url_parse("ftp://h/x", &u));
    CHECK(!dl_url_parse("https://h/x", &u));          // no TLS on this path
    CHECK(!dl_url_parse("ftp://h/x", &u));
    CHECK(!dl_url_parse("http://", &u));
    CHECK(!dl_url_parse("http://h:0/", &u));
    CHECK(!dl_url_parse("http://h:65536/", &u));
    CHECK(!dl_url_parse("http://h:12a/", &u));
    CHECK(!dl_url_parse("http://h:/", &u));
    std::string longhost = "http://" + std::string(4096, 'x') + ".com/path";
    CHECK(!dl_url_parse(longhost.c_str(), &u));        // bounded, not overflowed
    std::string longpath = "http://h/" + std::string(DL_URL_MAX, 'p');
    CHECK(!dl_url_parse(longpath.c_str(), &u));
}

static void test_request_build(void) {
    s_test = "request build"; printf("- %s\n", s_test);
    DlUrl u;
    CHECK(dl_url_parse("http://10.0.0.5:8096/Videos/a/stream.ts?x=1", &u));
    char req[2048];
    int n = dl_http_build_get(req, sizeof(req), &u, 0, "X-Emby-Authorization: tok");
    CHECK(n > 0);
    const char *line1 = "GET /Videos/a/stream.ts?x=1 HTTP/1.1\r\n";
    CHECK(strncmp(req, line1, strlen(line1)) == 0);
    CHECK(strstr(req, "Host: 10.0.0.5:8096\r\n") != NULL);
    CHECK(strstr(req, "X-Emby-Authorization: tok\r\n") != NULL);
    CHECK(strstr(req, "Range:") == NULL);
    CHECK(strcmp(req + n - 4, "\r\n\r\n") == 0);
    n = dl_http_build_get(req, sizeof(req), &u, 5000000000ull, NULL);
    CHECK(strstr(req, "Range: bytes=5000000000-\r\n") != NULL);
    CHECK(strstr(req, "Authorization") == NULL);
    CHECK(dl_http_build_get(req, 32, &u, 0, NULL) == -1);
}

static bool head(const char *t, DlHttpHead *h) { return dl_http_parse_head(t, (int)strlen(t), h); }

static void test_head_parse(void) {
    s_test = "response head parse"; printf("- %s\n", s_test);
    DlHttpHead h;
    CHECK(head("HTTP/1.1 200 OK\r\nContent-Length: 1234\r\nContent-Type: Video/MP2T\r\n"
               "Accept-Ranges: bytes\r\n\r\n", &h));
    CHECK(h.status == 200 && h.content_length == 1234 && h.accept_ranges && !h.chunked);
    CHECK(strcmp(h.content_type, "video/mp2t") == 0);
    CHECK(head("HTTP/1.1 206 Partial\r\ncontent-range: bytes 100-199/1000\r\n"
               "CONTENT-LENGTH: 100\r\n\r\n", &h));
    CHECK(h.status == 206 && h.has_range && h.range_start == 100 && h.range_end == 199);
    CHECK(h.range_total == 1000 && h.content_length == 100);
    CHECK(head("HTTP/1.1 206 X\r\nContent-Range: bytes 0-9/*\r\n\r\n", &h));
    CHECK(h.range_total == -1);
    CHECK(head("HTTP/1.1 416 X\r\nContent-Range: bytes */5000\r\n\r\n", &h));
    CHECK(h.range_unsatisfied && h.range_total == 5000);
    // Chunked beats Content-Length (RFC 7230 3.3.3).
    CHECK(head("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nContent-Length: 5\r\n\r\n", &h));
    CHECK(h.chunked && h.content_length == -1);
    CHECK(head("HTTP/1.0 200\r\n\r\n", &h) && h.status == 200);
    // Malformed
    CHECK(!head("", &h));
    CHECK(!head("ICY 200 OK\r\n\r\n", &h));
    CHECK(!head("HTTP/1.1 2x0 OK\r\n\r\n", &h));
    CHECK(!head("HTTP/1.1 2000 OK\r\n\r\n", &h));
    CHECK(!head("HTTP/1.1 200 OK\r\nContent-Length: abc\r\n\r\n", &h));
    CHECK(!head("HTTP/1.1 200 OK\r\nContent-Length: 5\r\nContent-Length: 6\r\n\r\n", &h));
    CHECK(!head("HTTP/1.1 200 OK\r\nContent-Length: 99999999999999999999999\r\n\r\n", &h));
    CHECK(!head("HTTP/1.1 206 X\r\nContent-Range: bytes 9-3/10\r\n\r\n", &h));
    CHECK(!head("HTTP/1.1 206 X\r\nContent-Range: bytes 0-10/10\r\n\r\n", &h));
    CHECK(!head("HTTP/1.1 206 X\r\nContent-Range: pages 0-1/10\r\n\r\n", &h));
    CHECK(!head("HTTP/1.1 206 X\r\nContent-Range: bytes */*\r\n\r\n", &h));
    // Duplicate identical Content-Length is legal.
    CHECK(head("HTTP/1.1 200 OK\r\nContent-Length: 5\r\nContent-Length: 5\r\n\r\n", &h));
    CHECK(dl_http_content_type_is_error_page("text/html; charset=utf-8"));
    CHECK(dl_http_content_type_is_error_page("application/json"));
    CHECK(!dl_http_content_type_is_error_page("video/mp2t"));
    CHECK(!dl_http_content_type_is_error_page(""));
}

static void test_head_reader(void) {
    s_test = "head reader splits"; printf("- %s\n", s_test);
    const char *resp = "HTTP/1.1 200 OK\r\nContent-Length: 4\r\n\r\nBODY";
    const int len = (int)strlen(resp);
    const int head_len = len - 4;
    // Every split point and every read size delivers the same head.
    for (int step = 1; step <= len; step++) {
        DlHeadReader r;
        dl_head_reader_init(&r);
        int pos = 0, body_at = -1;
        while (pos < len && !r.done) {
            int n = step < len - pos ? step : len - pos;
            int used = dl_head_reader_feed(&r, (const uint8_t *)resp + pos, n);
            CHECK(used >= 0);
            pos += used;
            if (r.done) body_at = pos;
        }
        CHECK(r.done && body_at == head_len && r.n == head_len);
    }
    // A head that never ends is refused at DL_HEAD_MAX.
    static DlHeadReader r;
    dl_head_reader_init(&r);
    std::string junk(DL_HEAD_MAX + 10, 'a');
    CHECK(dl_head_reader_feed(&r, (const uint8_t *)junk.data(), (int)junk.size()) == -1);
}

static std::string make_chunked(const std::string &body, unsigned seed, bool ext) {
    std::string out;
    size_t i = 0;
    while (i < body.size()) {
        seed = seed * 1103515245u + 12345u;
        size_t n = 1 + (seed >> 16) % 300;
        if (n > body.size() - i) n = body.size() - i;
        char line[48];
        snprintf(line, sizeof(line), (seed & 1) ? "%zX" : "%zx", n);
        out += line;
        if (ext) out += ";name=val";
        out += "\r\n";
        out.append(body, i, n);
        out += "\r\n";
        i += n;
    }
    out += "0\r\nX-Trailer: 1\r\n\r\nGARBAGE-AFTER-END";
    return out;
}

static void test_chunked(void) {
    s_test = "chunked decoder"; printf("- %s\n", s_test);
    std::string body;
    for (int i = 0; i < 20000; i++) body.push_back((char)fake_byte((uint64_t)i));
    for (unsigned seed = 1; seed <= 40; seed++) {
        std::string enc = make_chunked(body, seed, seed % 3 == 0);
        // Decode in place with random read sizes.
        std::vector<uint8_t> buf(enc.begin(), enc.end());
        DlChunked c;
        dl_chunked_init(&c);
        std::string got;
        size_t pos = 0;
        unsigned rs = seed * 7;
        while (pos < buf.size() && !c.done) {
            rs = rs * 1664525u + 1013904223u;
            int n = (int)(1 + (rs >> 20) % 97);
            if ((size_t)n > buf.size() - pos) n = (int)(buf.size() - pos);
            int out = dl_chunked_decode(&c, buf.data() + pos, n, buf.data() + pos);
            CHECK(out >= 0);
            if (out < 0) break;
            got.append((const char *)buf.data() + pos, (size_t)out);
            pos += (size_t)n;
        }
        CHECK(c.done && !c.error);
        CHECK(got == body);
    }
    // Byte-at-a-time also works.
    {
        std::string enc = make_chunked(body.substr(0, 3000), 99, true);
        DlChunked c; dl_chunked_init(&c);
        std::string got;
        for (size_t i = 0; i < enc.size() && !c.done; i++) {
            uint8_t b = (uint8_t)enc[i], o = 0;
            int n = dl_chunked_decode(&c, &b, 1, &o);
            CHECK(n >= 0);
            if (n == 1) got.push_back((char)o);
        }
        CHECK(c.done && got == body.substr(0, 3000));
    }
    // Malformed framing is refused.
    const char *bad[] = { "zz\r\nhello\r\n", "\r\n", "5\r\nhelloXX", "5\rhello",
                          "ffffffffffffffffff\r\n" };
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        DlChunked c; dl_chunked_init(&c);
        uint8_t out[64];
        int r = dl_chunked_decode(&c, (const uint8_t *)bad[i], (int)strlen(bad[i]), out);
        CHECK(r < 0 && c.error);
        CHECK(dl_chunked_decode(&c, (const uint8_t *)"0\r\n\r\n", 5, out) < 0);   // sticky
    }
}

// =========================================================================
// Manager: queue, transfer, persistence
// =========================================================================

static void test_enqueue_and_complete(void) {
    begin("enqueue -> download -> complete");
    DlMeta m = meta_for("item1", "The Film");
    CHECK(dl_enqueue(&m, URL, 0) == DL_OK);
    DlStatus st = status_of("item1");
    CHECK(st.rec.state == DL_QUEUED && st.rec.seq == 1);
    // Persisted before any network activity.
    DlRecord disk;
    CHECK(dl_store_load_record("item1", &disk) && disk.state == DL_QUEUED);
    DlMeta dm;
    CHECK(dl_store_load_meta("item1", &dm) && strcmp(dm.title, "The Film") == 0);
    CHECK(strstr(disk.url, "Token") == NULL);

    CHECK(run_worker() == 1);
    st = status_of("item1");
    CHECK(st.rec.state == DL_COMPLETED && st.rec.error == DL_ERR_NONE);
    CHECK(st.rec.bytes_done == (uint64_t)g_fake_total && st.rec.bytes_total == (uint64_t)g_fake_total);
    CHECK(dl_progress_permille(&st.rec) == 1000);
    CHECK(fake_file_matches(item_file("item1", DL_FILE_MEDIA).c_str(), g_fake_total));
    CHECK(dl_plat_file_size(item_file("item1", DL_FILE_PART).c_str()) == -1);
    CHECK(dl_store_load_record("item1", &disk) && disk.state == DL_COMPLETED);
    // Token went in the header, never in the URL or the record.
    CHECK(g_fake_requests.size() == 1);
    CHECK(g_fake_requests[0].find("X-Emby-Authorization: MediaBrowser Token=\"t\"\r\n")
          != std::string::npos);
    CHECK(last_request_has_no_range());
    char path[DL_PATH_MAX];
    CHECK(dl_media_path("item1", path, sizeof(path)));
    CHECK(strstr(path, "/items/item1/media.ts") != NULL);
    CHECK(run_worker() == 0);   // nothing left to do
}

static void test_queue_order_and_duplicates(void) {
    begin("queue order, duplicates, limits");
    DlMeta a = meta_for("aaa", "A"), b = meta_for("bbb", "B"), c = meta_for("ccc", "C");
    CHECK(dl_enqueue(&b, URL, 0) == DL_OK);
    CHECK(dl_enqueue(&a, URL, 0) == DL_OK);
    CHECK(dl_enqueue(&c, URL, 0) == DL_OK);
    CHECK(dl_enqueue(&a, URL, 0) == DL_E_EXISTS);
    DlStatus list[8];
    CHECK(dl_list(list, 8, false) == 3);
    CHECK(!strcmp(list[0].rec.id, "bbb") && !strcmp(list[1].rec.id, "aaa") &&
          !strcmp(list[2].rec.id, "ccc"));
    CHECK(dl_list(list, 2, false) == 2);            // honours max
    CHECK(dl_list(list, 8, true) == 0);             // nothing complete yet
    CHECK(dl_manager_step());
    CHECK(status_of("bbb").rec.state == DL_COMPLETED);   // first in, first done
    CHECK(status_of("aaa").rec.state == DL_QUEUED);
    CHECK(dl_list(list, 8, true) == 1 && !strcmp(list[0].rec.id, "bbb"));
    CHECK(dl_enqueue(&b, URL, 0) == DL_E_EXISTS);   // already downloaded
    // Invalid input
    DlMeta bad = meta_for("../x");
    CHECK(dl_enqueue(&bad, URL, 0) == DL_E_INVALID);
    DlMeta notitle = meta_for("ok1", "");
    CHECK(dl_enqueue(&notitle, URL, 0) == DL_E_INVALID);
    DlMeta ok = meta_for("ok2");
    CHECK(dl_enqueue(&ok, "https://h/x", 0) == DL_E_INVALID);
    CHECK(dl_enqueue(&ok, "not a url", 0) == DL_E_INVALID);
    CHECK(dl_enqueue(NULL, URL, 0) == DL_E_INVALID);
    CHECK(dl_pause("nope") == DL_E_NOT_FOUND);
    // Table capacity
    int added = 3;
    for (int i = 0; added < DL_MAX_ITEMS; i++, added++) {
        char id[16]; snprintf(id, sizeof(id), "fill%d", i);
        DlMeta f = meta_for(id);
        CHECK(dl_enqueue(&f, URL, 0) == DL_OK);
    }
    DlMeta extra = meta_for("onetoomany");
    CHECK(dl_enqueue(&extra, URL, 0) == DL_E_FULL);
    CHECK(dl_remove("fill0") == DL_OK);
    CHECK(dl_enqueue(&extra, URL, 0) == DL_OK);     // room again
}

static int64_t s_hook_at = -1;
static const char *s_hook_id = "";
static int s_hook_action = 0;   // 1 pause, 2 cancel, 3 remove, 4 suspend, 5 cancel+pause
static void hook(int64_t body_sent) {
    if (s_hook_at < 0 || body_sent < s_hook_at) return;
    s_hook_at = -1;
    switch (s_hook_action) {
    case 1: CHECK(dl_pause(s_hook_id) == DL_OK); break;
    case 2: CHECK(dl_cancel(s_hook_id) == DL_OK); break;
    case 3: CHECK(dl_remove(s_hook_id) == DL_OK); break;
    case 4: dl_set_suspended(true); break;
    case 5: CHECK(dl_cancel(s_hook_id) == DL_OK); CHECK(dl_pause(s_hook_id) == DL_OK); break;
    case 6: CHECK(dl_pause(s_hook_id) == DL_OK); CHECK(dl_resume(s_hook_id) == DL_OK); break;
    }
}

static void arm_hook(const char *id, int action, int64_t at) {
    s_hook_id = id; s_hook_action = action; s_hook_at = at;
    g_fake_default.on_body = hook;
}

static void test_progress_checkpoints(void) {
    begin("progress + checkpoints");
    // Watch progress as the worker sees it, from inside the transfer.
    static std::vector<uint64_t> seen_mem, seen_disk;
    seen_mem.clear(); seen_disk.clear();
    g_fake_default.on_body = [](int64_t) {
        DlStatus st;
        if (dl_find("prog", &st)) seen_mem.push_back(st.rec.bytes_done);
        DlRecord d;
        if (dl_store_load_record("prog", &d)) seen_disk.push_back(d.bytes_done);
    };
    DlMeta m = meta_for("prog");
    CHECK(dl_enqueue(&m, URL, 0) == DL_OK);
    CHECK(dl_manager_step());
    CHECK(status_of("prog").rec.state == DL_COMPLETED);
    CHECK(seen_mem.size() > 10);
    bool mono = true;
    for (size_t i = 1; i < seen_mem.size(); i++) if (seen_mem[i] < seen_mem[i - 1]) mono = false;
    CHECK(mono);
    // Disk lags memory, but only by one checkpoint interval.
    bool bounded = true;
    for (size_t i = 0; i < seen_mem.size() && i < seen_disk.size(); i++) {
        if (seen_disk[i] > seen_mem[i]) bounded = false;
        if (seen_mem[i] - seen_disk[i] > s_cfg.checkpoint_bytes + 7919) bounded = false;
    }
    CHECK(bounded);
    int distinct = 0;
    for (size_t i = 1; i < seen_disk.size(); i++) if (seen_disk[i] != seen_disk[i - 1]) distinct++;
    CHECK(distinct >= 4);   // 300 KB / 50 KB checkpoints
}

static void test_interrupted_resume(void) {
    begin("interrupted download resumes with Range");
    FakeResp drop; drop.drop_after = 123457;
    g_fake_queue.push_back(drop);
    DlMeta m = meta_for("res1");
    CHECK(dl_enqueue(&m, URL, 0) == DL_OK);
    CHECK(dl_manager_step());
    DlStatus st = status_of("res1");
    CHECK(st.rec.state == DL_QUEUED && st.rec.error == DL_ERR_PARTIAL);
    CHECK(st.rec.attempts == 1 && st.retry_in_ms == 2000);
    CHECK(st.rec.bytes_done == 123457 && st.rec.resumable == 1);
    CHECK(dl_plat_file_size(item_file("res1", DL_FILE_PART).c_str()) == 123457);
    // Not due yet: the worker leaves it alone during the backoff.
    CHECK(!dl_manager_step());
    CHECK(dl_next_wake_ms() == 1000);             // capped, so control is noticed
    g_fake_now_ms += 2000;
    CHECK(dl_next_wake_ms() == 0);
    CHECK(dl_manager_step());
    CHECK(last_request_has_range(123457));
    st = status_of("res1");
    CHECK(st.rec.state == DL_COMPLETED && st.rec.attempts == 0);
    CHECK(fake_file_matches(item_file("res1", DL_FILE_MEDIA).c_str(), g_fake_total));
}

static void test_reset_and_timeout(void) {
    begin("connection reset and idle timeout");
    FakeResp reset; reset.reset_after = 50000;
    FakeResp stall; stall.stall_after = 90000;
    g_fake_queue.push_back(reset);
    g_fake_queue.push_back(stall);
    DlMeta m = meta_for("rt");
    CHECK(dl_enqueue(&m, URL, 0) == DL_OK);
    CHECK(dl_manager_step());
    CHECK(status_of("rt").rec.error == DL_ERR_NETWORK);
    CHECK(status_of("rt").rec.bytes_done == 50000);
    uint64_t t0 = g_fake_now_ms;
    CHECK(step_after_backoff());
    DlStatus st = status_of("rt");
    CHECK(st.rec.error == DL_ERR_TIMEOUT && st.rec.state == DL_QUEUED);
    CHECK(st.rec.bytes_done == 50000 + 90000);    // limits count per response
    CHECK(g_fake_now_ms - t0 >= 400000 + s_cfg.idle_timeout_ms);
    CHECK(step_after_backoff());
    CHECK(last_request_has_range(140000));
    CHECK(status_of("rt").rec.state == DL_COMPLETED);
    CHECK(fake_file_matches(item_file("rt", DL_FILE_MEDIA).c_str(), g_fake_total));
}

static void test_head_timeout(void) {
    begin("server never answers");
    FakeResp s; s.head_stall = true;
    g_fake_queue.push_back(s);
    DlMeta m = meta_for("ht");
    CHECK(dl_enqueue(&m, URL, 0) == DL_OK);
    uint64_t t0 = g_fake_now_ms;
    CHECK(dl_manager_step());
    CHECK(status_of("ht").rec.error == DL_ERR_TIMEOUT);
    CHECK(g_fake_now_ms - t0 >= s_cfg.head_timeout_ms);
    CHECK(g_fake_now_ms - t0 < s_cfg.head_timeout_ms + 5000);   // bounded
}

static void test_range_ignored(void) {
    begin("server ignores Range (live transcode)");
    FakeResp drop; drop.drop_after = 100000; drop.accept_ranges = false;
    FakeResp full; full.honor_range = false; full.accept_ranges = false;
    g_fake_queue.push_back(drop);
    g_fake_queue.push_back(full);
    DlMeta m = meta_for("tr");
    CHECK(dl_enqueue(&m, URL, 0) == DL_OK);
    CHECK(dl_manager_step());
    CHECK(status_of("tr").rec.bytes_done == 100000);
    CHECK(status_of("tr").rec.resumable == 0);
    CHECK(step_after_backoff());
    CHECK(last_request_has_range(100000));        // asked...
    DlStatus st = status_of("tr");                 // ...got 200, started over
    CHECK(st.rec.state == DL_COMPLETED);
    CHECK(fake_file_matches(item_file("tr", DL_FILE_MEDIA).c_str(), g_fake_total));
}

static void test_range_inconsistent(void) {
    begin("206 from the wrong offset / file changed");
    FakeResp drop; drop.drop_after = 100000;
    FakeResp lie;  lie.range_start_lie = 5;
    g_fake_queue.push_back(drop);
    g_fake_queue.push_back(lie);
    DlMeta m = meta_for("ri");
    CHECK(dl_enqueue(&m, URL, 0) == DL_OK);
    CHECK(dl_manager_step());
    CHECK(step_after_backoff());
    DlStatus st = status_of("ri");
    CHECK(st.rec.error == DL_ERR_BAD_RESPONSE && st.rec.bytes_done == 0);
    CHECK(dl_plat_file_size(item_file("ri", DL_FILE_PART).c_str()) == 0);   // discarded
    CHECK(step_after_backoff());
    CHECK(last_request_has_no_range());
    CHECK(status_of("ri").rec.state == DL_COMPLETED);
    CHECK(fake_file_matches(item_file("ri", DL_FILE_MEDIA).c_str(), g_fake_total));

    // The file on the server changed size between attempts.
    begin("206 with a different total");
    FakeResp d2; d2.drop_after = 70000;
    FakeResp grown; grown.total_lie = 999999;
    g_fake_queue.push_back(d2);
    g_fake_queue.push_back(grown);
    CHECK(dl_enqueue(&m, URL, 0) == DL_OK);
    CHECK(dl_manager_step());
    CHECK(status_of("ri").rec.bytes_total == (uint64_t)g_fake_total);
    CHECK(step_after_backoff());
    CHECK(status_of("ri").rec.error == DL_ERR_BAD_RESPONSE);
    CHECK(status_of("ri").rec.bytes_done == 0);
}

static void test_416_already_complete(void) {
    begin("416 when everything is already here");
    // Crash after the last byte was written but before verify/rename: the
    // partial is complete, and the next request asks for bytes past the end.
    DlMeta m = meta_for("full");
    CHECK(dl_enqueue(&m, URL, 0) == DL_OK);
    FILE *f = fopen(item_file("full", DL_FILE_PART).c_str(), "wb");
    for (int64_t i = 0; i < g_fake_total; i++) fputc(fake_byte((uint64_t)i), f);
    fclose(f);
    CHECK(dl_manager_step());
    CHECK(last_request_has_range((uint64_t)g_fake_total));
    CHECK(status_of("full").rec.state == DL_COMPLETED);
    CHECK(fake_file_matches(item_file("full", DL_FILE_MEDIA).c_str(), g_fake_total));

    // 416 that does NOT match what we have: start over.
    begin("416 that does not match");
    CHECK(dl_enqueue(&m, URL, 0) == DL_OK);
    f = fopen(item_file("full", DL_FILE_PART).c_str(), "wb");
    for (int64_t i = 0; i < g_fake_total + 10; i++) fputc(fake_byte((uint64_t)i), f);
    fclose(f);
    CHECK(dl_manager_step());
    CHECK(status_of("full").rec.error == DL_ERR_BAD_RESPONSE);
    CHECK(status_of("full").rec.http_status == 416);
    CHECK(step_after_backoff());
    CHECK(status_of("full").rec.state == DL_COMPLETED);
    CHECK(fake_file_matches(item_file("full", DL_FILE_MEDIA).c_str(), g_fake_total));
}

static void test_chunked_transcode(void) {
    begin("chunked transcode body");
    g_fake_default.chunked = true;
    g_fake_default.accept_ranges = false;
    g_fake_default.honor_range = false;
    g_fake_total = 1500000;
    DlMeta m = meta_for("chk");
    CHECK(dl_enqueue(&m, URL, 0) == DL_OK);
    CHECK(dl_manager_step());
    DlStatus st = status_of("chk");
    CHECK(st.rec.state == DL_COMPLETED);
    CHECK(st.rec.bytes_total == 1500000);       // learned at the end
    CHECK(fake_file_matches(item_file("chk", DL_FILE_MEDIA).c_str(), 1500000));

    begin("chunked body cut short");
    g_fake_default.chunked = true;
    g_fake_default.honor_range = false;
    g_fake_default.accept_ranges = false;
    FakeResp cut = g_fake_default; cut.drop_after = 40000;
    g_fake_queue.push_back(cut);
    CHECK(dl_enqueue(&m, URL, 0) == DL_OK);
    CHECK(dl_manager_step());
    st = status_of("chk");
    CHECK(st.rec.error == DL_ERR_PARTIAL && st.rec.state == DL_QUEUED);
    CHECK(st.rec.bytes_total == 0);             // a transcode never says
    CHECK(step_after_backoff());
    CHECK(status_of("chk").rec.state == DL_COMPLETED);
    CHECK(fake_file_matches(item_file("chk", DL_FILE_MEDIA).c_str(), g_fake_total));

    begin("close-delimited body (no length)");
    g_fake_default.no_length = true;
    CHECK(dl_enqueue(&m, URL, 0) == DL_OK);
    CHECK(dl_manager_step());
    CHECK(status_of("chk").rec.state == DL_COMPLETED);
    CHECK(fake_file_matches(item_file("chk", DL_FILE_MEDIA).c_str(), g_fake_total));
}

static void test_pause_resume(void) {
    begin("pause mid-transfer, resume later");
    arm_hook("pz", 1, 80000);
    DlMeta m = meta_for("pz");
    CHECK(dl_enqueue(&m, URL, 0) == DL_OK);
    CHECK(dl_manager_step());
    DlStatus st = status_of("pz");
    CHECK(st.rec.state == DL_PAUSED && !st.active);
    CHECK(st.rec.bytes_done >= 80000 && st.rec.bytes_done < (uint64_t)g_fake_total);
    uint64_t kept = st.rec.bytes_done;
    CHECK(dl_plat_file_size(item_file("pz", DL_FILE_PART).c_str()) == (int64_t)kept);
    CHECK(!dl_manager_step());                   // paused items are not picked
    CHECK(dl_pause("pz") == DL_E_STATE);         // already paused
    CHECK(dl_resume("pz") == DL_OK);
    CHECK(status_of("pz").rec.state == DL_QUEUED);
    CHECK(dl_manager_step());
    CHECK(last_request_has_range(kept));
    CHECK(status_of("pz").rec.state == DL_COMPLETED);
    CHECK(fake_file_matches(item_file("pz", DL_FILE_MEDIA).c_str(), g_fake_total));
    CHECK(dl_resume("pz") == DL_E_STATE);        // completed is terminal

    begin("pause then resume before the worker notices");
    arm_hook("pr", 6, 60000);
    DlMeta m2 = meta_for("pr");
    CHECK(dl_enqueue(&m2, URL, 0) == DL_OK);
    CHECK(dl_manager_step());
    CHECK(status_of("pr").rec.state == DL_COMPLETED);

    begin("pause a queued item");
    DlMeta m3 = meta_for("pq");
    CHECK(dl_enqueue(&m3, URL, 0) == DL_OK);
    CHECK(dl_pause("pq") == DL_OK);
    CHECK(!dl_manager_step());
    CHECK(g_fake_connects == 0);
    DlRecord d;
    CHECK(dl_store_load_record("pq", &d) && d.state == DL_PAUSED);
}

static void test_cancel(void) {
    begin("cancel mid-transfer");
    arm_hook("cx", 2, 100000);
    DlMeta m = meta_for("cx");
    CHECK(dl_enqueue(&m, URL, 0) == DL_OK);
    CHECK(dl_manager_step());
    DlStatus st = status_of("cx");
    CHECK(st.rec.state == DL_CANCELLED && st.rec.bytes_done == 0);
    CHECK(dl_plat_file_size(item_file("cx", DL_FILE_PART).c_str()) == -1);   // deleted
    CHECK(dl_plat_file_size(item_file("cx", DL_FILE_MEDIA).c_str()) == -1);
    DlRecord d;
    CHECK(dl_store_load_record("cx", &d) && d.state == DL_CANCELLED);         // entry kept
    CHECK(!dl_manager_step());
    // Retry starts over from zero.
    CHECK(dl_retry("cx") == DL_OK);
    CHECK(dl_manager_step());
    CHECK(last_request_has_no_range());
    CHECK(status_of("cx").rec.state == DL_COMPLETED);

    begin("cancel is not undone by a later pause");
    arm_hook("cp", 5, 50000);
    DlMeta m2 = meta_for("cp");
    CHECK(dl_enqueue(&m2, URL, 0) == DL_OK);
    CHECK(dl_manager_step());
    CHECK(status_of("cp").rec.state == DL_CANCELLED);

    begin("cancel paused / queued / failed items");
    DlMeta q = meta_for("cq");
    CHECK(dl_enqueue(&q, URL, 0) == DL_OK);
    CHECK(dl_cancel("cq") == DL_OK);
    CHECK(status_of("cq").rec.state == DL_CANCELLED);
    CHECK(dl_cancel("cq") == DL_E_STATE);
    // Re-enqueueing a cancelled item is allowed (same as retry, new url).
    CHECK(dl_enqueue(&q, URL, 0) == DL_OK);
    CHECK(status_of("cq").rec.state == DL_QUEUED);
}

static void test_failure_retry(void) {
    begin("server errors retry with backoff, then fail");
    FakeResp e503; e503.status = 503; e503.content_type = "application/json";
    g_fake_default = e503;
    DlMeta m = meta_for("f5");
    CHECK(dl_enqueue(&m, URL, 0) == DL_OK);
    uint32_t expected_wait = 2000;
    for (int i = 1; i < DL_MAX_ATTEMPTS; i++) {
        CHECK(dl_manager_step());
        DlStatus st = status_of("f5");
        CHECK(st.rec.state == DL_QUEUED && st.rec.error == DL_ERR_SERVER);
        CHECK(st.rec.http_status == 503 && st.rec.attempts == (uint32_t)i);
        CHECK(st.retry_in_ms == expected_wait);
        CHECK(!dl_manager_step());               // waiting
        g_fake_now_ms += st.retry_in_ms;
        expected_wait = expected_wait * 2 > 300000 ? 300000 : expected_wait * 2;
    }
    CHECK(dl_manager_step());
    DlStatus st = status_of("f5");
    CHECK(st.rec.state == DL_FAILED && st.rec.attempts == DL_MAX_ATTEMPTS);
    CHECK(!step_after_backoff());                // failed items stay put
    // Manual retry resets the count; the server is back.
    g_fake_default = FakeResp();
    CHECK(dl_retry("f5") == DL_OK);
    st = status_of("f5");
    CHECK(st.rec.state == DL_QUEUED && st.rec.attempts == 0 && st.rec.error == DL_ERR_NONE);
    CHECK(dl_manager_step());
    CHECK(status_of("f5").rec.state == DL_COMPLETED);
    CHECK(dl_retry("f5") == DL_E_STATE);

    begin("progress resets the attempt count");
    for (int i = 0; i < DL_MAX_ATTEMPTS + 3; i++) {
        FakeResp d; d.drop_after = 20000;         // 20 KB per attempt, forever
        g_fake_queue.push_back(d);
    }
    DlMeta m2 = meta_for("flaky");
    CHECK(dl_enqueue(&m2, URL, 0) == DL_OK);
    for (int i = 0; i < DL_MAX_ATTEMPTS + 3; i++) {
        step_after_backoff();
        CHECK(status_of("flaky").rec.state != DL_FAILED);
        CHECK(status_of("flaky").rec.attempts <= 1);
    }
    CHECK(step_after_backoff());
    CHECK(status_of("flaky").rec.state == DL_COMPLETED);
    CHECK(fake_file_matches(item_file("flaky", DL_FILE_MEDIA).c_str(), g_fake_total));

    begin("permanent HTTP errors fail at once");
    struct { int status; DlError err; } perm[] = {
        { 403, DL_ERR_HTTP }, { 404, DL_ERR_NOT_FOUND }, { 410, DL_ERR_NOT_FOUND },
        { 400, DL_ERR_HTTP },
    };
    for (unsigned i = 0; i < sizeof(perm) / sizeof(perm[0]); i++) {
        char id[16]; snprintf(id, sizeof(id), "p%d", perm[i].status);
        FakeResp r; r.status = perm[i].status;
        g_fake_queue.push_back(r);
        DlMeta pm = meta_for(id);
        CHECK(dl_enqueue(&pm, URL, 0) == DL_OK);
        CHECK(dl_manager_step());
        DlStatus ps = status_of(id);
        CHECK(ps.rec.state == DL_FAILED && ps.rec.error == perm[i].err);
        CHECK(ps.rec.http_status == perm[i].status && ps.rec.attempts == 0);
    }
}

static void test_invalid_responses(void) {
    begin("invalid responses");
    struct { const char *name; FakeResp r; DlError err; } cases[4];
    cases[0].name = "html error page with 200";
    cases[0].r.content_type = "text/html"; cases[0].err = DL_ERR_BAD_RESPONSE;
    cases[1].name = "not HTTP at all";
    cases[1].r.raw = "SSH-2.0-OpenSSH_9.6\r\n\r\n"; cases[1].err = DL_ERR_BAD_RESPONSE;
    cases[2].name = "empty 200";
    cases[2].r.total = 0; cases[2].err = DL_ERR_BAD_RESPONSE;
    cases[3].name = "closed before any byte";
    cases[3].r.close_now = true; cases[3].err = DL_ERR_NETWORK;
    for (int i = 0; i < 4; i++) {
        char id[16]; snprintf(id, sizeof(id), "inv%d", i);
        g_fake_queue.push_back(cases[i].r);
        DlMeta m = meta_for(id);
        CHECK(dl_enqueue(&m, URL, 0) == DL_OK);
        CHECK(dl_manager_step());
        DlStatus st = status_of(id);
        if (st.rec.error != cases[i].err)
            printf("    %s: got %s\n", cases[i].name, dl_error_name(st.rec.error));
        CHECK(st.rec.error == cases[i].err);
        CHECK(st.rec.state == DL_QUEUED);                   // retryable
        CHECK(dl_plat_file_size(item_file(id, DL_FILE_MEDIA).c_str()) == -1);
    }
    begin("more body than declared");
    FakeResp liar; liar.total_lie = 1000;   // Content-Length 1000, sends 300000
    g_fake_queue.push_back(liar);
    DlMeta m = meta_for("over");
    CHECK(dl_enqueue(&m, URL, 0) == DL_OK);
    CHECK(dl_manager_step());
    // Content-Length is authoritative: the first 1000 bytes are the file.
    DlStatus st = status_of("over");
    CHECK(st.rec.state == DL_COMPLETED && st.rec.bytes_done == 1000);

    // fewer body bytes than declared
    FakeResp shortr; shortr.total_lie = 400000;   // says 400000, sends 300000
    g_fake_queue.push_back(shortr);
    CHECK(dl_remove("over") == DL_OK);
    CHECK(dl_enqueue(&m, URL, 0) == DL_OK);
    CHECK(dl_manager_step());
    CHECK(status_of("over").rec.error == DL_ERR_PARTIAL);

    begin("https refused");
    // enqueue refuses it; a hand-edited record with one fails permanently.
    DlMeta hm = meta_for("tls");
    CHECK(dl_enqueue(&hm, URL, 0) == DL_OK);
    DlRecord r;
    CHECK(dl_store_load_record("tls", &r));
    snprintf(r.url, sizeof(r.url), "https://secure/x");
    CHECK(dl_store_save_record(&r));
    restart_app();
    CHECK(dl_manager_step());
    CHECK(status_of("tls").rec.state == DL_FAILED);
    CHECK(status_of("tls").rec.error == DL_ERR_UNSUPPORTED);
}

static void test_unreachable(void) {
    begin("server unreachable");
    g_fake_default.refuse = true;
    DlMeta m = meta_for("un");
    CHECK(dl_enqueue(&m, URL, 0) == DL_OK);
    CHECK(dl_manager_step());
    DlStatus st = status_of("un");
    CHECK(st.rec.state == DL_QUEUED && st.rec.error == DL_ERR_UNREACHABLE);
    CHECK(st.rec.attempts == 1);
    g_fake_default.refuse = false;
    CHECK(step_after_backoff());
    CHECK(status_of("un").rec.state == DL_COMPLETED);
}

static void test_suspend(void) {
    begin("suspend for playback");
    arm_hook("sp", 4, 90000);
    DlMeta m = meta_for("sp");
    CHECK(dl_enqueue(&m, URL, 0) == DL_OK);
    CHECK(dl_manager_step());
    DlStatus st = status_of("sp");
    CHECK(st.rec.state == DL_QUEUED && st.rec.error == DL_ERR_NONE);   // not a failure
    CHECK(st.rec.attempts == 0 && st.retry_in_ms == 0);
    CHECK(!dl_manager_step());                   // nothing starts while suspended
    dl_set_suspended(false);
    CHECK(dl_manager_step());
    CHECK(last_request_has_range(st.rec.bytes_done));
    CHECK(status_of("sp").rec.state == DL_COMPLETED);
}

static void test_deletion(void) {
    begin("remove a completed item");
    DlMeta m = meta_for("del1");
    snprintf(m.poster, sizeof(m.poster), DL_FILE_POSTER);
    CHECK(dl_enqueue(&m, URL, 0) == DL_OK);
    CHECK(dl_manager_step());
    FILE *f = fopen(item_file("del1", DL_FILE_POSTER).c_str(), "wb");
    fputs("jpeg", f); fclose(f);
    char dir[DL_PATH_MAX];
    dl_store_item_dir(dir, sizeof(dir), "del1");
    CHECK(dl_plat_exists(dir));
    CHECK(dl_remove("del1") == DL_OK);
    CHECK(!dl_plat_exists(dir));
    CHECK(status_of("del1").rec.state == DL_STATE_COUNT);
    CHECK(dl_remove("del1") == DL_E_NOT_FOUND);
    restart_app();
    CHECK(status_of("del1").rec.state == DL_STATE_COUNT);   // stays gone

    begin("remove the active item");
    arm_hook("del2", 3, 30000);
    DlMeta m2 = meta_for("del2");
    CHECK(dl_enqueue(&m2, URL, 0) == DL_OK);
    CHECK(dl_manager_step());
    dl_store_item_dir(dir, sizeof(dir), "del2");
    CHECK(!dl_plat_exists(dir));
    CHECK(status_of("del2").rec.state == DL_STATE_COUNT);

    begin("remove never touches unknown files");
    DlMeta m3 = meta_for("del3");
    CHECK(dl_enqueue(&m3, URL, 0) == DL_OK);
    f = fopen(item_file("del3", "user-notes.txt").c_str(), "wb");
    fputs("mine", f); fclose(f);
    CHECK(dl_remove("del3") == DL_OK);
    CHECK(dl_plat_file_size(item_file("del3", "user-notes.txt").c_str()) == 4);
    CHECK(dl_plat_file_size(item_file("del3", DL_FILE_STATE).c_str()) == -1);
}

static const uint64_t GIB   = 1ull << 30;
static const uint64_t FLOOR = 10 * GIB;     // DL_RESERVE_MIN

static void test_storage_limits(void) {
    begin("the floor is 10 GiB and the reserve can only raise it");
    CHECK(DL_RESERVE_MIN == FLOOR);
    CHECK(dl_reserve_bytes() == FLOOR);
    s_cfg.reserve_bytes = 1000;                     // a lower configured value...
    restart_app();
    CHECK(dl_reserve_bytes() == FLOOR);             // ...is lifted to the floor
    g_fake_reserve_gb = 3;                          // the setting cannot lower it
    restart_app();
    CHECK(dl_reserve_bytes() == FLOOR);
    g_fake_reserve_gb = 25;                         // but it can raise it
    restart_app();
    CHECK(dl_reserve_bytes() == 25 * GIB);
    g_fake_reserve_gb = -4;                         // nonsense is ignored
    restart_app();
    CHECK(dl_reserve_bytes() == FLOOR);

    begin("refused up front when it cannot fit above the floor");
    g_fake_free = FLOOR + 100000;
    DlMeta m = meta_for("big");
    CHECK(dl_enqueue(&m, URL, 300000) == DL_E_NO_SPACE);
    CHECK(status_of("big").rec.state == DL_STATE_COUNT);
    DlSpaceReport rep = dl_last_space_report();     // the numbers behind the refusal
    CHECK(rep.state == DL_SPACE_LOW && rep.need_bytes == 300000 &&
          rep.avail_bytes == 100000 && rep.reserve_bytes == FLOOR);
    char dir[DL_PATH_MAX];
    dl_store_item_dir(dir, sizeof(dir), "big");
    CHECK(!dl_plat_exists(dir));                   // nothing left behind
    CHECK(dl_enqueue(&m, URL, 99000) == DL_OK);    // fits above the floor

    begin("at the floor exactly, nothing fits");
    g_fake_free = FLOOR;
    DlMeta mz = meta_for("zero");
    CHECK(dl_enqueue(&mz, URL, 1000) == DL_E_NO_SPACE);
    g_fake_free = FLOOR - 1;                        // below it: avail is 0, not negative
    CHECK(dl_enqueue(&mz, URL, 1000) == DL_E_NO_SPACE);
    CHECK(dl_last_space_report().avail_bytes == 0);

    begin("the queue's own commitments count against the next item");
    g_fake_free = FLOOR + 250000;
    DlExtras ex; memset(&ex, 0, sizeof(ex));
    ex.estimate_bytes = 100000;                    // an estimate, not an exact size
    DlMeta qa = meta_for("qa"), qb = meta_for("qb"), qc = meta_for("qc");
    CHECK(dl_enqueue(&qa, URL, 0, &ex) == DL_OK);
    CHECK(dl_enqueue(&qb, URL, 0, &ex) == DL_OK);
    CHECK(dl_enqueue(&qc, URL, 0, &ex) == DL_E_NO_SPACE);   // 300000 > 250000
    rep = dl_last_space_report();
    CHECK(rep.committed_bytes == 200000 && rep.need_bytes == 300000);
    CHECK(dl_remove("qb") == DL_OK);                // room again
    CHECK(dl_enqueue(&qc, URL, 0, &ex) == DL_OK);
    // The same query without enqueuing:
    rep = dl_space_report(100000);
    CHECK(rep.state == DL_SPACE_LOW && rep.committed_bytes == 200000);

    begin("unknown free space: nothing is accepted");
    g_fake_free = DL_FREE_UNKNOWN;
    DlMeta mu = meta_for("unk");
    CHECK(dl_enqueue(&mu, URL, 1000) == DL_E_SPACE_UNKNOWN);
    CHECK(status_of("unk").rec.state == DL_STATE_COUNT);
    CHECK(dl_space_report(1).state == DL_SPACE_UNKNOWN);

    begin("an implausible reading counts as unknown");
    g_fake_free = 9ull * 1024 * GIB;                // 9 TiB: no console drive reads this
    CHECK(dl_enqueue(&mu, URL, 1000) == DL_E_SPACE_UNKNOWN);
    g_fake_free = UINT64_MAX - 1;
    CHECK(dl_enqueue(&mu, URL, 1000) == DL_E_SPACE_UNKNOWN);

    begin("size unknown still has to leave room for the next chunk");
    g_fake_free = FLOOR + 1000;                     // less than one chunk above the floor
    DlMeta ms = meta_for("nosize");
    CHECK(dl_enqueue(&ms, URL, 0) == DL_E_NO_SPACE);
    g_fake_free = FLOOR + (1ull << 30);
    CHECK(dl_enqueue(&ms, URL, 0) == DL_OK);

    begin("an item that cannot start pauses, it does not fail");
    g_fake_free = FLOOR + (1ull << 30);
    DlMeta m2 = meta_for("late");
    CHECK(dl_enqueue(&m2, URL, 0) == DL_OK);
    g_fake_free = FLOOR + 10;                       // the disk filled up meanwhile
    CHECK(dl_manager_step());
    DlStatus st = status_of("late");
    CHECK(st.rec.state == DL_PAUSED && st.rec.error == DL_ERR_NO_SPACE);
    CHECK(st.rec.bytes_done == 0);
    CHECK(dl_space_held());
    g_fake_free = FLOOR + (1ull << 30);             // the user freed space
    CHECK(dl_resume("late") == DL_OK);
    CHECK(dl_manager_step());
    CHECK(status_of("late").rec.state == DL_COMPLETED);
    CHECK(!dl_space_held());

    begin("crossing the floor mid-transfer pauses with the partial kept");
    g_fake_free = FLOOR + (1ull << 30);
    g_fake_default.chunked = true;                  // size unknown up front
    g_fake_default.accept_ranges = true;
    static bool dropped; dropped = false;
    g_fake_default.on_body = [](int64_t sent) {
        if (!dropped && sent > 30000) { dropped = true; g_fake_free = FLOOR + 100; }
    };
    DlMeta m3 = meta_for("drain");
    CHECK(dl_enqueue(&m3, URL, 0) == DL_OK);
    DlMeta m3b = meta_for("waiting");
    CHECK(dl_enqueue(&m3b, URL, 0) == DL_OK);
    CHECK(dl_manager_step());
    st = status_of("drain");
    CHECK(st.rec.state == DL_PAUSED && st.rec.error == DL_ERR_NO_SPACE);
    CHECK(st.rec.bytes_done > 0 && st.rec.bytes_done < (uint64_t)g_fake_total);
    CHECK(dl_plat_file_size(item_file("drain", DL_FILE_PART).c_str()) ==
          (int64_t)st.rec.bytes_done);              // kept, and what the record says
    CHECK(!dl_manager_step());                      // nothing else starts below the floor
    CHECK(status_of("waiting").rec.state == DL_QUEUED);
    // Nothing was written inside the reserve: what is on disk plus the floor
    // never exceeded what was free when the last check passed.
    g_fake_free = FLOOR + (1ull << 30);             // space comes back
    g_fake_default.on_body = nullptr;
    CHECK(dl_resume("drain") == DL_OK);
    CHECK(dl_manager_step());                       // the held queue is released
    CHECK(last_request_has_range(st.rec.bytes_done));   // a resume, by Range
    CHECK(status_of("drain").rec.state == DL_COMPLETED);

    begin("a failed write pauses for space, not as a disk error");
    g_fake_free = FLOOR + (1ull << 30);
    g_fake_default.chunked = true;
    g_fake_default.accept_ranges = false;
    g_fake_write_budget = 150000;                   // the fake zeroes free space when this runs out
    DlMeta m4 = meta_for("fill");
    CHECK(dl_enqueue(&m4, URL, 0) == DL_OK);
    CHECK(dl_manager_step());
    st = status_of("fill");
    CHECK(st.rec.state == DL_PAUSED && st.rec.error == DL_ERR_NO_SPACE);
    CHECK(dl_plat_file_size(item_file("fill", DL_FILE_MEDIA).c_str()) == -1);

    begin("free space becomes unreadable mid-transfer");
    g_fake_free = FLOOR + (1ull << 30);
    g_fake_default.on_body = [](int64_t sent) {
        if (sent > 30000) g_fake_free = DL_FREE_UNKNOWN;
    };
    DlMeta m5 = meta_for("blind");
    CHECK(dl_enqueue(&m5, URL, 0) == DL_OK);
    CHECK(dl_manager_step());
    st = status_of("blind");
    CHECK(st.rec.state == DL_PAUSED && st.rec.error == DL_ERR_SPACE_UNKNOWN);
    CHECK(st.rec.bytes_done > 0);
    CHECK(!dl_manager_step());                      // still unreadable: still held
    g_fake_default.on_body = nullptr;
    g_fake_free = FLOOR + (1ull << 30);
    CHECK(dl_resume("blind") == DL_OK);
    CHECK(dl_manager_step());
    CHECK(status_of("blind").rec.state == DL_COMPLETED);

    begin("the pause reason and the estimate survive a restart");
    g_fake_free = FLOOR + (1ull << 30);
    ex.estimate_bytes = 123456;
    DlMeta m6 = meta_for("keep");
    CHECK(dl_enqueue(&m6, URL, 0, &ex) == DL_OK);
    g_fake_free = FLOOR + 5;
    CHECK(dl_manager_step());
    restart_app();
    st = status_of("keep");
    CHECK(st.rec.state == DL_PAUSED && st.rec.error == DL_ERR_NO_SPACE &&
          st.rec.est_bytes == 123456);

    begin("how many of a season fit");
    DlSpaceReport sr;
    memset(&sr, 0, sizeof(sr));
    sr.state = DL_SPACE_OK; sr.avail_bytes = 1000; sr.committed_bytes = 100;
    const uint64_t eps[5] = { 300, 300, 300, 300, 300 };
    CHECK(dl_space_fit_prefix(&sr, eps, 5) == 3);   // 100 + 900 = 1000: exactly fits
    sr.committed_bytes = 101;
    CHECK(dl_space_fit_prefix(&sr, eps, 5) == 2);
    sr.avail_bytes = 0;
    CHECK(dl_space_fit_prefix(&sr, eps, 5) == 0);
    sr.state = DL_SPACE_UNKNOWN; sr.avail_bytes = 1000000;
    CHECK(dl_space_fit_prefix(&sr, eps, 5) == 0);   // unknown: nothing fits
}

// =========================================================================
// HDD floor: estimates and the words
// =========================================================================

static void test_space_estimates_and_text(void) {
    s_test = "space: estimates"; printf("- %s\n", s_test);
    StreamRequest d;
    memset(&d, 0, sizeof(d));
    d.vbitrate = 25000000; d.vreq = 24000000; d.abitrate = 640000; d.achans = 6;
    uint64_t est = 0;
    // A ceiling: (video + audio) over the runtime, plus 15%.
    CHECK(dl_estimate_bytes(&d, 3600, 0, &est));
    const uint64_t base = (24000000ull + 640000) / 8 * 3600;
    CHECK(est == base + base / 100 * 15);
    // A ceiling with no runtime: nothing to estimate from.
    CHECK(!dl_estimate_bytes(&d, 0, 5ull << 30, &est));
    // An HD audio copy is charged what the budget charged.
    d.hd_codec = "truehd"; d.audio_cost = 4500000;
    uint64_t est_hd = 0;
    CHECK(dl_estimate_bytes(&d, 3600, 0, &est_hd) && est_hd > est);
    // Direct play: the server's size, else the disc ceiling over the runtime.
    StreamRequest o;
    memset(&o, 0, sizeof(o));
    CHECK(dl_estimate_bytes(&o, 0, 10000000000ull, &est));
    CHECK(est > 10000000000ull && est < 12000000000ull);
    CHECK(dl_estimate_bytes(&o, 7200, 0, &est));
    CHECK(est > 50000000ull / 8 * 7200);
    CHECK(!dl_estimate_bytes(&o, 0, 0, &est));                // neither: refuse

    s_test = "space: what the screens say"; printf("- %s\n", s_test);
    char buf[160];
    DlSpaceReport rep;
    memset(&rep, 0, sizeof(rep));
    rep.state = DL_SPACE_LOW;
    rep.need_bytes = 6ull * GIB + 200ull * 1024 * 1024;     // 6.1 GB
    rep.avail_bytes = 3ull * GIB + 100ull * 1024 * 1024;    // 3.0 GB
    rep.reserve_bytes = FLOOR;
    dl_ui_result_message(DL_E_NO_SPACE, &rep, buf, sizeof(buf));
    CHECK(!strcmp(buf, "Not enough space: needs 6.1 GB, 3.0 GB available (10 GB is always kept free)"));
    dl_ui_result_message(DL_E_SPACE_UNKNOWN, &rep, buf, sizeof(buf));
    CHECK(!strcmp(buf, "Can't check free HDD space: downloads paused"));
    dl_ui_result_message(DL_E_SIZE_UNKNOWN, NULL, buf, sizeof(buf));
    CHECK(!strcmp(buf, "Size unknown, can't check space"));
    dl_ui_result_message(DL_E_NO_SPACE, NULL, buf, sizeof(buf));   // no numbers: the plain text
    CHECK(!strcmp(buf, dl_ui_result_text(DL_E_NO_SPACE)));

    rep.free_bytes = 42ull * GIB;
    dl_ui_hdd_header(&rep, buf, sizeof(buf));
    CHECK(!strcmp(buf, "HDD: 42 GB free \xC2\xB7 downloads keep 10 GB free"));
    dl_ui_store_header(&rep, "", buf, sizeof(buf));                 // no drive: the HDD's header
    CHECK(!strcmp(buf, "HDD: 42 GB free \xC2\xB7 downloads keep 10 GB free"));
    dl_ui_store_header(&rep, "MOVIES", buf, sizeof(buf));
    CHECK(!strcmp(buf, "MOVIES: 42 GB free \xC2\xB7 downloads keep 10 GB free"));
    rep.state = DL_SPACE_UNKNOWN;
    dl_ui_hdd_header(&rep, buf, sizeof(buf));
    CHECK(strstr(buf, "unknown") != NULL);

    DlUiContext cx = { false, false, true };
    rep.state = DL_SPACE_LOW;
    dl_ui_banner(&cx, &rep, true, buf, sizeof(buf));
    CHECK(!strcmp(buf, "Paused: the HDD is down to 10 GB free"));
    rep.state = DL_SPACE_UNKNOWN;
    dl_ui_banner(&cx, &rep, true, buf, sizeof(buf));
    CHECK(!strcmp(buf, "Can't check free HDD space: downloads paused"));
    dl_ui_banner(&cx, &rep, false, buf, sizeof(buf));           // not held: no banner
    CHECK(buf[0] == '\0');
    cx.auth_held = true;                                        // another hold wins
    dl_ui_banner(&cx, &rep, true, buf, sizeof(buf));
    CHECK(!strcmp(buf, "Sign in to continue downloads"));

    // A row paused for space says why, as a warning.
    DlStatus st;
    memset(&st, 0, sizeof(st));
    snprintf(st.rec.id, sizeof(st.rec.id), "x");
    st.rec.state = DL_PAUSED; st.rec.error = DL_ERR_NO_SPACE;
    DlUiRow row;
    DlUiContext cx2 = { false, false, true };
    dl_ui_row(&st, &cx2, &row);
    CHECK(row.warning && strstr(row.status, "Paused") != NULL);
    st.rec.error = DL_ERR_NONE;                                 // a user pause is not a warning
    dl_ui_row(&st, &cx2, &row);
    CHECK(!row.warning && !strcmp(row.status, "Paused"));
}

// =========================================================================
// Whole-season download
// =========================================================================

struct SeasonScript {
    int  n = 0;                    // episodes listed (-1: the list fails)
    int  fail_at = -1;             // queue() answers fail_with here
    int  fail_with = DL_E_INVALID;
    int  cancel_after = -1;        // cancel once this many queue() calls were made
    int  calls = 0;
    std::string queued;            // ids, in the order queue() saw them
};

static int season_list(void *ctx, const char *, const char *, DlSeasonEp *out, int max) {
    SeasonScript *sc = (SeasonScript *)ctx;
    if (sc->n < 0) return -1;
    const int n = sc->n < max ? sc->n : max;
    for (int i = 0; i < n; i++) {
        memset(&out[i], 0, sizeof(out[i]));
        snprintf(out[i].id, sizeof(out[i].id), "ep%02d", i + 1);
        snprintf(out[i].name, sizeof(out[i].name), "Episode %d", i + 1);
        out[i].runtime_secs = 1800;
        out[i].identity.season = 2;
        out[i].identity.episode = i + 1;
    }
    return n;
}

static int season_queue(void *ctx, const DlSeasonEp *ep) {
    SeasonScript *sc = (SeasonScript *)ctx;
    const int call = sc->calls++;
    if (sc->cancel_after >= 0 && sc->calls >= sc->cancel_after) dl_season_cancel();
    if (call == sc->fail_at) return sc->fail_with;
    DlMeta m = meta_for(ep->id, ep->name);
    const DlResult r = dl_enqueue(&m, URL, 0);
    if (r == DL_OK) sc->queued += std::string(ep->id) + " ";
    return r;
}

static void test_season(void) {
    const DlSeasonOps ops = { season_list, season_queue };
    DlSeasonStatus st;

    begin("season: every episode is queued, in order");
    SeasonScript a; a.n = 6;
    dl_season_run(&ops, &a, "series", "season2", 0);
    dl_season_status(&st);
    CHECK(st.state == DL_SEASON_DONE && st.total == 6 && st.added == 6 && st.skipped == 0 && st.failed == 0);
    CHECK(a.queued == "ep01 ep02 ep03 ep04 ep05 ep06 ");
    CHECK(status_of("ep03").rec.state == DL_QUEUED);
    CHECK(!dl_season_busy());

    begin("season: run twice adds nothing");
    SeasonScript b1; b1.n = 4;
    dl_season_run(&ops, &b1, "series", "season2", 0);
    SeasonScript b2; b2.n = 4;
    dl_season_run(&ops, &b2, "series", "season2", 0);
    dl_season_status(&st);
    CHECK(st.state == DL_SEASON_DONE && st.added == 0 && st.skipped == 4 && b2.calls == 0);

    begin("season: a failed or cancelled episode is queued again, the rest are skipped");
    SeasonScript c1; c1.n = 3;
    dl_season_run(&ops, &c1, "series", "season2", 0);
    CHECK(dl_cancel("ep02") == DL_OK);
    SeasonScript c2; c2.n = 3;
    dl_season_run(&ops, &c2, "series", "season2", 0);
    dl_season_status(&st);
    CHECK(st.added == 1 && st.skipped == 2 && c2.queued == "ep02 ");

    begin("season: cancel stops between episodes");
    SeasonScript d; d.n = 6; d.cancel_after = 2;
    dl_season_run(&ops, &d, "series", "season2", 0);
    dl_season_status(&st);
    CHECK(st.state == DL_SEASON_CANCELLED && st.added == 2 && d.calls == 2);
    CHECK(status_of("ep03").rec.state == DL_STATE_COUNT);       // never queued
    SeasonScript d2; d2.n = 2;                                   // the flag does not stick
    dl_season_run(&ops, &d2, "series", "season2", 0);
    dl_season_status(&st);
    CHECK(st.state == DL_SEASON_DONE && st.added == 0 && st.skipped == 2);

    begin("season: no space stops the run, what was queued stays");
    SeasonScript e; e.n = 6; e.fail_at = 2; e.fail_with = DL_E_NO_SPACE;
    dl_season_run(&ops, &e, "series", "season2", 0);
    dl_season_status(&st);
    CHECK(st.state == DL_SEASON_NO_SPACE && st.added == 2 && e.calls == 3);
    CHECK(status_of("ep01").rec.state == DL_QUEUED && status_of("ep02").rec.state == DL_QUEUED);
    CHECK(status_of("ep04").rec.state == DL_STATE_COUNT);
    begin("season: space that cannot be read stops it too");
    SeasonScript e2; e2.n = 4; e2.fail_at = 0; e2.fail_with = DL_E_SPACE_UNKNOWN;
    dl_season_run(&ops, &e2, "series", "season2", 0);
    dl_season_status(&st);
    CHECK(st.state == DL_SEASON_NO_SPACE && st.added == 0 && e2.calls == 1);

    begin("season: another refusal skips that episode and carries on");
    SeasonScript f; f.n = 4; f.fail_at = 1; f.fail_with = DL_E_INVALID;
    dl_season_run(&ops, &f, "series", "season2", 0);
    dl_season_status(&st);
    CHECK(st.state == DL_SEASON_DONE && st.added == 3 && st.failed == 1);
    CHECK(f.queued == "ep01 ep03 ep04 ");

    begin("season: only the first N");
    SeasonScript g; g.n = 8;
    dl_season_run(&ops, &g, "series", "season2", 3);
    dl_season_status(&st);
    CHECK(st.total == 3 && st.added == 3 && g.queued == "ep01 ep02 ep03 ");

    begin("season: the episode list failing");
    SeasonScript h; h.n = -1;
    dl_season_run(&ops, &h, "series", "season2", 0);
    dl_season_status(&st);
    CHECK(st.state == DL_SEASON_FAILED && st.total == 0 && h.calls == 0);

    begin("season: estimates, and how many fit");
    StreamPrefs p;
    memset(&p, 0, sizeof(p));
    p.quality = VQ_1080P_25; p.display_w = 1920; p.display_h = 1080; p.budget = true;
    StreamSelection sel;
    memset(&sel, 0, sizeof(sel));
    sel.item_id = "x"; sel.cur_audio = -1; sel.cur_sub = -1;
    StreamRequest rq;
    stream_request_resolve(&p, &sel, &rq);
    DlSeasonEp eps[3];
    memset(eps, 0, sizeof(eps));
    eps[0].runtime_secs = 3600; eps[1].runtime_secs = 1800; eps[2].runtime_secs = 0;
    uint64_t est[3];
    dl_season_estimates(&rq, eps, 3, est);
    CHECK(est[0] > est[1] && est[1] > 0);
    CHECK(est[2] == est[0]);                                    // none reported: an hour
    g_fake_free = FLOOR + est[0] + est[1] + 1000;               // room for two of the three
    DlSpaceReport rep = dl_space_report(0);
    CHECK(dl_space_fit_prefix(&rep, est, 3) == 2);
    g_fake_free = FLOOR + est[0] - 1;
    rep = dl_space_report(0);
    CHECK(dl_space_fit_prefix(&rep, est, 3) == 0);
}

// =========================================================================
// Restore / offline startup
// =========================================================================

static void test_restore_interrupted(void) {
    begin("app killed mid-download resumes after restart");
    // Stop the transfer the hard way: the app "dies" part way, leaving the
    // record at its last checkpoint (DOWNLOADING) and more bytes on disk.
    DlMeta m = meta_for("crash");
    CHECK(dl_enqueue(&m, URL, 0) == DL_OK);
    FakeResp drop; drop.drop_after = 170000;
    g_fake_queue.push_back(drop);
    CHECK(dl_manager_step());
    DlRecord r;
    CHECK(dl_store_load_record("crash", &r));
    r.state = DL_DOWNLOADING;
    r.bytes_done = 100000;            // record lagging the file
    CHECK(dl_store_save_record(&r));
    restart_app();
    DlStatus st = status_of("crash");
    CHECK(st.rec.state == DL_QUEUED);
    CHECK(st.rec.bytes_done == 170000);            // file size wins
    CHECK(st.retry_in_ms == 0);                    // no backoff after restart
    CHECK(dl_manager_step());
    CHECK(last_request_has_range(170000));
    CHECK(status_of("crash").rec.state == DL_COMPLETED);
    CHECK(fake_file_matches(item_file("crash", DL_FILE_MEDIA).c_str(), g_fake_total));

    begin("killed between rename and record save");
    DlMeta m2 = meta_for("rn");
    CHECK(dl_enqueue(&m2, URL, 0) == DL_OK);
    CHECK(dl_manager_step());
    CHECK(dl_store_load_record("rn", &r));
    r.state = DL_DOWNLOADING;                      // the save that never happened
    CHECK(dl_store_save_record(&r));
    restart_app();
    CHECK(status_of("rn").rec.state == DL_COMPLETED);

    begin("state survives restart in every state");
    DlMeta p = meta_for("s_paused"), f = meta_for("s_failed"), q = meta_for("s_queued");
    CHECK(dl_enqueue(&p, URL, 0) == DL_OK);
    CHECK(dl_enqueue(&f, URL, 0) == DL_OK);
    CHECK(dl_enqueue(&q, URL, 0) == DL_OK);
    CHECK(dl_pause("s_paused") == DL_OK);
    FakeResp nf; nf.status = 404;
    g_fake_queue.push_back(nf);
    CHECK(dl_manager_step());                      // s_failed (first unpaused)
    CHECK(status_of("s_failed").rec.state == DL_FAILED);
    restart_app();
    CHECK(status_of("s_paused").rec.state == DL_PAUSED);
    CHECK(status_of("s_failed").rec.state == DL_FAILED);
    CHECK(status_of("s_failed").rec.error == DL_ERR_NOT_FOUND);
    CHECK(status_of("s_queued").rec.state == DL_QUEUED);
    DlStatus list[16];
    int n = dl_list(list, 16, false);
    CHECK(n == 3);
    bool ordered = true;
    for (int i = 1; i < n; i++) if (list[i].rec.seq <= list[i - 1].rec.seq) ordered = false;
    CHECK(ordered);
    // New items continue the sequence rather than reusing numbers.
    DlMeta nx = meta_for("s_next");
    CHECK(dl_enqueue(&nx, URL, 0) == DL_OK);
    CHECK(status_of("s_next").rec.seq > list[n - 1].rec.seq);
}

static void write_file(const std::string &path, const char *text) {
    FILE *f = fopen(path.c_str(), "wb");
    fputs(text, f);
    fclose(f);
}

static void test_restore_malformed(void) {
    begin("damaged state.txt, media intact -> recovered");
    DlMeta m = meta_for("dmg1", "Recover Me");
    CHECK(dl_enqueue(&m, URL, 0) == DL_OK);
    CHECK(dl_manager_step());
    write_file(item_file("dmg1", DL_FILE_STATE), "jfdl-state 1\nid=dmg1\nstate=compl");
    restart_app();
    DlStatus st = status_of("dmg1");
    CHECK(st.rec.state == DL_COMPLETED);
    CHECK(st.rec.bytes_done == (uint64_t)g_fake_total);
    CHECK(strcmp(st.rec.title, "Recover Me") == 0);
    DlRecord disk;
    CHECK(dl_store_load_record("dmg1", &disk) && disk.state == DL_COMPLETED);   // rewritten

    begin("damaged state.txt falls back to .tmp");
    DlMeta mt = meta_for("dmgtmp");
    CHECK(dl_enqueue(&mt, URL, 0) == DL_OK);
    CHECK(dl_pause("dmgtmp") == DL_OK);
    // A crash between remove(old) and rename(tmp) leaves only the .tmp.
    std::string st_path = item_file("dmgtmp", DL_FILE_STATE);
    CHECK(rename(st_path.c_str(), (st_path + ".tmp").c_str()) == 0);
    restart_app();
    CHECK(status_of("dmgtmp").rec.state == DL_PAUSED);

    begin("damaged state and meta -> visible, removable");
    DlMeta m2 = meta_for("dmg2");
    CHECK(dl_enqueue(&m2, URL, 0) == DL_OK);
    write_file(item_file("dmg2", DL_FILE_STATE), "\x01\x02 binary junk");
    write_file(item_file("dmg2", DL_FILE_META), "");
    write_file(item_file("dmg2", DL_FILE_PART), "12345");
    restart_app();
    st = status_of("dmg2");
    CHECK(st.rec.state == DL_FAILED && st.rec.error == DL_ERR_CORRUPT);
    CHECK(st.rec.bytes_done == 5 && strcmp(st.rec.title, "dmg2") == 0);
    CHECK(dl_retry("dmg2") == DL_E_STATE);         // no request to repeat
    DlMeta fresh = meta_for("dmg2", "Fresh");
    CHECK(dl_enqueue(&fresh, URL, 0) == DL_OK);    // but can be re-downloaded
    CHECK(status_of("dmg2").rec.state == DL_QUEUED);
    // ...from scratch: a partial whose record was lost cannot be trusted.
    CHECK(status_of("dmg2").rec.bytes_done == 0);
    CHECK(dl_plat_file_size(item_file("dmg2", DL_FILE_PART).c_str()) == -1);
    CHECK(dl_cancel("dmg2") == DL_OK);
    CHECK(dl_remove("dmg2") == DL_OK);

    begin("completed record with missing media");
    DlMeta m3 = meta_for("gone");
    CHECK(dl_enqueue(&m3, URL, 0) == DL_OK);
    CHECK(dl_manager_step());
    CHECK(remove(item_file("gone", DL_FILE_MEDIA).c_str()) == 0);
    restart_app();
    st = status_of("gone");
    CHECK(st.rec.state == DL_FAILED && st.rec.error == DL_ERR_CORRUPT);
    char path[DL_PATH_MAX];
    CHECK(!dl_media_path("gone", path, sizeof(path)));
    CHECK(dl_retry("gone") == DL_OK);              // url is known: fetch again
    CHECK(dl_manager_step());
    CHECK(status_of("gone").rec.state == DL_COMPLETED);

    // ...and one whose media is there but the wrong size
    CHECK(truncate(item_file("gone", DL_FILE_MEDIA).c_str(), 1000) == 0);
    restart_app();
    CHECK(status_of("gone").rec.state == DL_FAILED);
    CHECK(status_of("gone").rec.error == DL_ERR_CORRUPT);

    begin("record filed under the wrong directory");
    DlMeta a = meta_for("dira");
    CHECK(dl_enqueue(&a, URL, 0) == DL_OK);
    std::string other = item_file("dira", DL_FILE_STATE);
    write_file(other, "jfdl-state 1\nid=dirb\nstate=completed\nend\n");
    restart_app();
    CHECK(status_of("dirb").rec.state == DL_STATE_COUNT);
    CHECK(status_of("dira").rec.state == DL_FAILED);

    begin("junk directories are ignored");
    CHECK(dl_plat_mkdir((s_root + "/items/not.an.id").c_str()));
    CHECK(dl_plat_mkdir((s_root + "/items/has space").c_str()));
    restart_app();
    DlStatus list[64];
    int n = dl_list(list, 64, false);
    for (int i = 0; i < n; i++) CHECK(dl_id_valid(list[i].rec.id));

    begin("unwritable root is refused");
    dl_manager_shutdown();
    CHECK(!dl_manager_init("/proc/definitely/not/writable", &s_cfg));
    CHECK(!dl_manager_ready());
    DlMeta x = meta_for("x");
    CHECK(dl_enqueue(&x, URL, 0) == DL_E_NOT_READY);
    CHECK(dl_list(list, 64, false) == 0);
    CHECK(!dl_manager_step());
}

static void test_offline_startup(void) {
    begin("offline startup: library usable with no server");
    DlMeta a = meta_for("offa", "Downloaded A"), b = meta_for("offb", "Downloaded B");
    snprintf(a.series, sizeof(a.series), "Show");
    a.season = 2; a.episode = 5; a.runtime_secs = 1800;
    CHECK(dl_enqueue(&a, URL, 0) == DL_OK);
    CHECK(dl_enqueue(&b, URL, 0) == DL_OK);
    CHECK(run_worker() == 2);
    DlMeta pending = meta_for("offc", "Still Queued");
    CHECK(dl_enqueue(&pending, URL, 0) == DL_OK);

    // The server goes away and the console reboots.
    g_fake_default.refuse = true;
    g_fake_connects = 0;
    restart_app();
    CHECK(g_fake_connects == 0);                   // init touched no network
    DlStatus lib[8];
    int n = dl_list(lib, 8, true);
    CHECK(n == 2);
    CHECK(!strcmp(lib[0].rec.id, "offa") && !strcmp(lib[1].rec.id, "offb"));
    DlMeta got;
    CHECK(dl_load_meta("offa", &got));
    CHECK(!strcmp(got.title, "Downloaded A") && !strcmp(got.series, "Show"));
    CHECK(got.season == 2 && got.episode == 5 && got.runtime_secs == 1800);
    char path[DL_PATH_MAX];
    CHECK(dl_media_path("offa", path, sizeof(path)));
    CHECK(fake_file_matches(path, g_fake_total));

    // The queued one tries, fails as unreachable, and leaves the library alone.
    CHECK(dl_manager_step());
    CHECK(status_of("offc").rec.error == DL_ERR_UNREACHABLE);
    CHECK(dl_list(lib, 8, true) == 2);
    CHECK(dl_media_path("offb", path, sizeof(path)));
    CHECK(!dl_media_path("offc", path, sizeof(path)));
}


// =========================================================================
// Playback: downloads yield to streaming
// =========================================================================

// Stream URLs in exactly the shape build_stream_url() produces
// (player/core/player_session.cpp), one per quality step / audio mode.
static std::string stream_url(int w, int h, long vbitrate, bool hd_copy,
                              const char *acodec = "mp3", long abitrate = 192000) {
    char v[96], a[128];
    if (vbitrate > 0)
        snprintf(v, sizeof(v), "&VideoBitrate=%ld&AllowVideoStreamCopy=true", vbitrate);
    else
        snprintf(v, sizeof(v), "&AllowVideoStreamCopy=true");
    if (hd_copy)
        snprintf(a, sizeof(a), "&AudioCodec=ac3,truehd,mp3&AudioSampleRate=48000"
                               "&MaxAudioChannels=8");
    else
        snprintf(a, sizeof(a), "&AudioCodec=%s&AudioBitrate=%ld&AudioSampleRate=48000"
                               "&MaxAudioChannels=%d", acodec, abitrate,
                 strcmp(acodec, "ac3") == 0 ? 6 : 2);
    char url[768];
    snprintf(url, sizeof(url),
             "http://192.168.1.2:8096/Videos/abc/stream.ts?VideoCodec=h264"
             "&Profile=baseline&Level=31&MaxWidth=%d&MaxHeight=%d%s%s"
             "&MaxFramerate=30&AllowAudioStreamCopy=%s&DeviceId=dev"
             "&Static=false&MediaSourceId=abc&StartTimeTicks=0&PlaySessionId=ps1",
             w, h, v, a, hd_copy ? "true" : "false");
    return url;
}

static const std::string &url_480p(void)  { static std::string u = stream_url(854, 480, 1500000, false); return u; }
static const std::string &url_1080p(void) { static std::string u = stream_url(1920, 1080, 10000000, false); return u; }

static void test_stream_classifier(void) {
    s_test = "light/heavy stream classifier"; printf("- %s\n", s_test);
    CHECK(dl_stream_is_light(stream_url(854, 480, 1500000, false).c_str()));   // 480p
    CHECK(dl_stream_is_light(stream_url(640, 360, 700000, false).c_str()));    // 360p
    CHECK(dl_stream_is_light(stream_url(854, 480, 1500000, false, "ac3", 640000).c_str())); // 5.1
    CHECK(!dl_stream_is_light(stream_url(1280, 720, 4000000, false).c_str()));   // 720p
    CHECK(!dl_stream_is_light(stream_url(1920, 1080, 10000000, false).c_str())); // High
    CHECK(!dl_stream_is_light(stream_url(1920, 1080, 25000000, false).c_str())); // Max
    CHECK(!dl_stream_is_light(stream_url(1920, 1080, 0, false).c_str()));        // Original
    CHECK(!dl_stream_is_light(stream_url(854, 480, 0, false).c_str()));   // copy, no ceiling
    CHECK(!dl_stream_is_light(stream_url(854, 480, 1500000, true).c_str()));  // TrueHD copy
    CHECK(!dl_stream_is_light(stream_url(854, 576, 1500000, false).c_str())); // 576 lines
    CHECK(!dl_stream_is_light(stream_url(854, 480, 1500001, false).c_str()));
    CHECK(!dl_stream_is_light(stream_url(854, 480, 1500000, false, "flac", 1411000).c_str()));
    // Fail closed on anything odd.
    CHECK(!dl_stream_is_light(NULL));
    CHECK(!dl_stream_is_light(""));
    CHECK(!dl_stream_is_light("http://h/stream.ts"));
    CHECK(!dl_stream_is_light("http://h/s.ts?MaxHeight=480"));                   // no ceiling
    CHECK(!dl_stream_is_light("http://h/s.ts?MaxHeight=abc&VideoBitrate=700000"));
    CHECK(!dl_stream_is_light("http://h/s.ts?MaxHeight=480&VideoBitrate=0"));
    CHECK(!dl_stream_is_light("http://h/s.ts?MaxHeight=480&VideoBitrate=700000"
                              "&AllowAudioStreamCopy=yes"));
    CHECK(dl_stream_is_light("http://h/s.ts?MaxHeight=480&VideoBitrate=700000"));
    // Names match whole keys only.
    CHECK(!dl_stream_is_light("http://h/s.ts?XMaxHeight=480&MaxVideoBitrate=700000"));
    CHECK(!dl_stream_is_light("http://h/s.ts?MediaSourceId=a&MaxHeight=480"
                              "&SourceVideoBitrate=700000"));
    char v[16];
    CHECK(dl_url_query_get("http://h/p?a=1&bb=22&b=3", "b", v, sizeof(v)) && !strcmp(v, "3"));
    CHECK(dl_url_query_get("http://h/p?a=1&bb=22&b=3", "a", v, sizeof(v)) && !strcmp(v, "1"));
    CHECK(!dl_url_query_get("http://h/p?a=1", "c", v, sizeof(v)));
    CHECK(!dl_url_query_get("http://h/p?a=12345678901234567890", "a", v, sizeof(v)));
    CHECK(!dl_url_query_get("http://h/p", "a", v, sizeof(v)));
}

static void test_playback_heavy_stops(void) {
    begin("heavy stream stops downloads");
    // Nothing starts while a heavy stream plays.
    dl_playback_begin(url_1080p().c_str());
    CHECK(dl_playback_blocking());
    DlMeta m = meta_for("hv");
    CHECK(dl_enqueue(&m, URL, 0) == DL_OK);        // queueing is still fine
    CHECK(!dl_manager_step());
    CHECK(g_fake_connects == 0);
    dl_playback_end();
    CHECK(!dl_playback_blocking());

    // A running download stops the moment a heavy stream opens...
    static const char *s_heavy;
    s_heavy = url_1080p().c_str();
    g_fake_default.on_body = [](int64_t sent) {
        static bool done; if (sent == 0) done = false;
        if (!done && sent >= 60000) { done = true; dl_playback_begin(s_heavy); }
    };
    CHECK(dl_manager_step());
    DlStatus st = status_of("hv");
    CHECK(st.rec.state == DL_QUEUED && st.rec.error == DL_ERR_NONE);  // not a failure
    CHECK(st.rec.attempts == 0 && st.retry_in_ms == 0);
    CHECK(st.rec.bytes_done >= 60000 && st.rec.bytes_done < (uint64_t)g_fake_total);
    // ...keeps what it had on disk...
    CHECK(dl_plat_file_size(item_file("hv", DL_FILE_PART).c_str()) == (int64_t)st.rec.bytes_done);
    // ...waits for as long as playback lasts...
    g_fake_now_ms += 3600000;
    CHECK(!dl_manager_step());
    // ...and resumes from where it stopped once playback ends.
    g_fake_default.on_body = nullptr;
    dl_playback_end();
    CHECK(dl_manager_step());
    CHECK(last_request_has_range(st.rec.bytes_done));
    CHECK(status_of("hv").rec.state == DL_COMPLETED);
    CHECK(fake_file_matches(item_file("hv", DL_FILE_MEDIA).c_str(), g_fake_total));

    begin("track change to an HD copy stops a paced download");
    static std::string keep;
    keep = stream_url(854, 480, 1500000, true);    // same 480p, TrueHD copy
    s_heavy = keep.c_str();
    dl_playback_begin(url_480p().c_str());
    CHECK(!dl_playback_blocking());
    g_fake_default.on_body = [](int64_t sent) {
        static bool done; if (sent == 0) done = false;
        if (!done && sent >= 40000) { done = true; dl_playback_begin(s_heavy); }
    };
    DlMeta m2 = meta_for("tc");
    CHECK(dl_enqueue(&m2, URL, 0) == DL_OK);
    CHECK(dl_manager_step());
    CHECK(status_of("tc").rec.state == DL_QUEUED);
    CHECK(dl_playback_blocking());
    dl_playback_end();

    begin("ending playback does not lift a service suspension");
    dl_set_suspended(true);
    dl_playback_begin(url_480p().c_str());
    dl_playback_end();
    DlMeta m3 = meta_for("ss");
    CHECK(dl_enqueue(&m3, URL, 0) == DL_OK);
    CHECK(!dl_manager_step());
    dl_set_suspended(false);
    CHECK(dl_manager_step());
    CHECK(status_of("ss").rec.state == DL_COMPLETED);
}

static void test_playback_light_paced(void) {
    begin("light stream: downloads continue, paced");
    s_cfg.stream_share_bps = 800000;               // 100 KB/s, for a quick test
    restart_app();
    DlMeta m = meta_for("lt");
    CHECK(dl_enqueue(&m, URL, 0) == DL_OK);

    // Unpaced baseline: 300 KB in a few fake milliseconds.
    uint64_t t0 = g_fake_now_ms;
    CHECK(dl_manager_step());
    const uint64_t unpaced = g_fake_now_ms - t0;
    CHECK(status_of("lt").rec.state == DL_COMPLETED);
    CHECK(unpaced < 500);

    // Beside a 480p stream: same file, held to ~100 KB/s => ~3 s.
    CHECK(dl_remove("lt") == DL_OK);
    CHECK(dl_enqueue(&m, URL, 0) == DL_OK);
    dl_playback_begin(url_480p().c_str());
    CHECK(!dl_playback_blocking());
    t0 = g_fake_now_ms;
    CHECK(dl_manager_step());
    const uint64_t paced = g_fake_now_ms - t0;
    CHECK(status_of("lt").rec.state == DL_COMPLETED);
    CHECK(fake_file_matches(item_file("lt", DL_FILE_MEDIA).c_str(), g_fake_total));
    const uint64_t ideal = (uint64_t)g_fake_total * 8000 / 800000;   // 3000 ms
    if (paced < ideal * 8 / 10 || paced > ideal * 15 / 10)
        printf("    paced %llu ms, ideal %llu ms\n", (unsigned long long)paced,
               (unsigned long long)ideal);
    CHECK(paced >= ideal * 8 / 10);     // actually held back...
    CHECK(paced <= ideal * 15 / 10);    // ...but not starved
    // Pacing never counts as the server being idle (idle timeout is 5 s,
    // the paced transfer takes 3 s of sleeps: no false timeout).
    CHECK(status_of("lt").rec.error == DL_ERR_NONE);

    begin("a long pacing wait is not an idle server");
    // One 64 KB read at 10 KB/s means a 6.5 s wait -- longer than the 5 s
    // idle timeout -- and the next read then waits on the network once.
    s_cfg.stream_share_bps = 80000;
    restart_app();
    g_fake_default.recv_chunk = 65536;
    g_fake_default.timeout_every = 1;
    g_fake_total = 200000;
    DlMeta mi = meta_for("idle");
    CHECK(dl_enqueue(&mi, URL, 0) == DL_OK);
    dl_playback_begin(url_480p().c_str());
    CHECK(dl_manager_step());
    CHECK(status_of("idle").rec.state == DL_COMPLETED);
    CHECK(status_of("idle").rec.error == DL_ERR_NONE);
    dl_playback_end();

    begin("playback ends mid-transfer: pacing lifts");
    s_cfg.stream_share_bps = 80000;                // 10 KB/s: 30 s if it stayed
    restart_app();
    DlMeta m2 = meta_for("lift");
    CHECK(dl_enqueue(&m2, URL, 0) == DL_OK);
    dl_playback_begin(url_480p().c_str());
    g_fake_default.on_body = [](int64_t sent) { if (sent >= 30000) dl_playback_end(); };
    t0 = g_fake_now_ms;
    CHECK(dl_manager_step());
    CHECK(status_of("lift").rec.state == DL_COMPLETED);
    CHECK(g_fake_now_ms - t0 < 10000);

    begin("pause lands during pacing");
    s_cfg.stream_share_bps = 8000;                 // 1 KB/s: would take minutes
    restart_app();
    DlMeta m3 = meta_for("pp");
    CHECK(dl_enqueue(&m3, URL, 0) == DL_OK);
    dl_playback_begin(url_480p().c_str());
    g_fake_default.recv_chunk = 65536;             // 64 s of pacing per read
    // Pause from "the UI" while the worker is inside a pacing sleep.
    g_fake_on_sleep = []() { dl_pause("pp"); };
    t0 = g_fake_now_ms;
    CHECK(dl_manager_step());
    CHECK(status_of("pp").rec.state == DL_PAUSED);
    CHECK(g_fake_now_ms - t0 < 2000);              // noticed within one slice
    g_fake_on_sleep = nullptr;
    dl_playback_end();
}

static void test_write_batching(void) {
    begin("writes are batched, not one per read");
    s_cfg.checkpoint_bytes = 64ull << 20;          // no checkpoint interference
    restart_app();
    g_fake_total = 3 * 1024 * 1024;
    DlMeta m = meta_for("wb");
    CHECK(dl_enqueue(&m, URL, 0) == DL_OK);
    CHECK(dl_manager_step());
    CHECK(status_of("wb").rec.state == DL_COMPLETED);
    CHECK(fake_file_matches(item_file("wb", DL_FILE_MEDIA).c_str(), g_fake_total));
    const int writes = (int)g_fake_write_sizes.size();
    printf("    %d reads -> %d writes\n", g_fake_data_recvs, writes);
    CHECK(g_fake_data_recvs > 300);                // 7919-byte reads
    CHECK(writes <= 8);                            // ~450 KB each
    for (int i = 0; i + 1 < writes; i++) CHECK(g_fake_write_sizes[i] >= 448 * 1024);

    begin("batched bytes still land on a drop");
    s_cfg.checkpoint_bytes = 64ull << 20;
    restart_app();
    FakeResp d; d.drop_after = 200001;             // well inside one batch
    g_fake_queue.push_back(d);
    DlMeta m2 = meta_for("wd");
    CHECK(dl_enqueue(&m2, URL, 0) == DL_OK);
    CHECK(dl_manager_step());
    CHECK(status_of("wd").rec.bytes_done == 200001);
    CHECK(dl_plat_file_size(item_file("wd", DL_FILE_PART).c_str()) == 200001);
    CHECK(step_after_backoff());
    CHECK(last_request_has_range(200001));
    CHECK(fake_file_matches(item_file("wd", DL_FILE_MEDIA).c_str(), g_fake_total));
}


// =========================================================================
// Stage 3 fixtures: a Jellyfin item as the item page holds it
// =========================================================================

static const char *SERVER = "http://192.168.1.2:8096";
static const char *DEVICE = "dev-1234";

static JFTracks fixture_tracks(int default_audio) {
    JFTracks t;
    memset(&t, 0, sizeof(t));
    const char *labels[] = { "English - DTS-HD MA - 7.1 - Default", "English - TRUEHD - 7.1",
                             "English - AC3 - 5.1", "Japanese - EAC3 - 5.1",
                             "Commentary - AAC - Stereo" };
    for (int i = 0; i < 5; i++) {
        t.audio[i].index = i + 1;
        snprintf(t.audio[i].label, sizeof(t.audio[i].label), "%s", labels[i]);
    }
    t.n_audio = 5;
    t.default_audio = default_audio;
    t.subs[0].index = 9;
    snprintf(t.subs[0].label, sizeof(t.subs[0].label), "English - SRT");
    t.n_subs = 1;
    return t;
}

static JFItem fixture_item(const char *id = "0123456789abcdef0123456789abcdef",
                           const char *type = "Episode") {
    JFItem it;
    memset(&it, 0, sizeof(it));
    snprintf(it.id, sizeof(it.id), "%s", id);
    snprintf(it.name, sizeof(it.name), "Pilot");
    snprintf(it.type, sizeof(it.type), "%s", type);
    return it;
}

static JFMediaSource fixture_source(int default_audio, const char *id = "src-b a&b",
                                    const char *live = "") {
    JFMediaSource s;
    memset(&s, 0, sizeof(s));
    snprintf(s.id, sizeof(s.id), "%s", id);
    snprintf(s.live_stream_id, sizeof(s.live_stream_id), "%s", live);
    snprintf(s.label, sizeof(s.label), "1080p H.264");
    s.runtime_secs = 2820;
    s.tracks = fixture_tracks(default_audio);
    return s;
}

static const char kEpisodeJson[] =
    "{\"Name\":\"Pilot\",\"Id\":\"0123456789abcdef0123456789abcdef\","
    "\"RunTimeTicks\":28200000000,\"ProductionYear\":2008,\"IndexNumber\":3,"
    "\"ParentIndexNumber\":1,\"SeriesName\":\"Some Show\",\"SeriesId\":\"show9\","
    "\"ParentBackdropItemId\":\"show9\",\"BackdropImageTags\":[],"
    "\"MediaStreams\":[{\"Index\":0,\"Type\":\"Video\"}],"
    "\"People\":[{\"Name\":\"A Person\",\"Id\":\"p1\"}]}";

static XMBItemDetail fixture_detail(void) {
    XMBItemDetail d;
    memset(&d, 0, sizeof(d));
    snprintf(d.overview, sizeof(d.overview), "Two lines\nof plot");
    snprintf(d.video_info, sizeof(d.video_info), "1080p H264 SDR");
    snprintf(d.audio_info, sizeof(d.audio_info), "English DTS 7.1");
    jellyfin_parse_item_identity(kEpisodeJson, &d.identity);
    return d;
}

static StreamPrefs prefs_of(int q, bool hd, int surround_mode, u32 dw = 1920, u32 dh = 1080) {
    StreamPrefs p;
    p.quality = vquality_sanitize(q);   // what vquality_get() would hold
    p.hd1080 = hd;
    p.surround = surround_mode > 0;
    p.surround_hd = surround_mode == 2;
    p.display_w = dw;
    p.display_h = dh;
    p.budget = true;
    p.passthrough = false;
    return p;
}

// What show_player() streams for this item on Play, from the start: its
// initial track rule, the shared decision, its session.
static std::string playback_url(const JFItem &it, const JFMediaSource &src,
                                const StreamPrefs &p, const char *session,
                                unsigned long long ticks, StreamRequest *out_rq = NULL) {
    StreamSelection sel = {};
    sel.item_id = it.id;
    sel.source = &src;
    sel.tracks = &src.tracks;
    stream_select_initial(&src.tracks, true, &sel.cur_audio, &sel.cur_sub);
    StreamRequest rq;
    stream_request_resolve(&p, &sel, &rq);
    char url[1024];
    stream_url_build(url, sizeof(url), &rq, SERVER, DEVICE, session, ticks);
    if (out_rq) *out_rq = rq;
    return url;
}

static bool build_request(const JFItem &it, const XMBItemDetail *d, const JFMediaSource *src,
                          const StreamPrefs &p, DlRequest *out, const char *server = NULL) {
    DlRequestInput in = { server ? server : SERVER, DEVICE, &it, d, src, &p };
    return dl_request_build(&in, out);
}

static std::map<std::string, std::string> query_of(const std::string &url, std::string *path) {
    std::map<std::string, std::string> q;
    size_t qm = url.find('?');
    if (path) *path = url.substr(0, qm);
    if (qm == std::string::npos) return q;
    std::string rest = url.substr(qm + 1);
    size_t i = 0;
    while (i <= rest.size()) {
        size_t amp = rest.find('&', i);
        if (amp == std::string::npos) amp = rest.size();
        std::string kv = rest.substr(i, amp - i);
        size_t eq = kv.find('=');
        if (!kv.empty()) q[kv.substr(0, eq)] = eq == std::string::npos ? "" : kv.substr(eq + 1);
        i = amp + 1;
    }
    return q;
}

// =========================================================================
// Stream selection: shared, unchanged, identical for downloads
// =========================================================================

static void test_download_matches_playback(void) {
    s_test = "download = playback's stream decision"; printf("- %s\n", s_test);
    JFItem it = fixture_item();
    XMBItemDetail d = fixture_detail();
    u32 disp[][2] = { { 1920, 1080 }, { 1280, 720 }, { 720, 576 } };
    int combos = 0;
    for (int q = 0; q < VQ_COUNT; q++)
    for (int hd = 0; hd < 2; hd++)
    for (int sm = 0; sm < 3; sm++)
    for (int di = 0; di < 3; di++)
    for (int da = 0; da < 5; da++)            // every default-track kind
    for (int live = 0; live < 2; live++) {
        StreamPrefs p = prefs_of(q, hd, sm, disp[di][0], disp[di][1]);
        JFMediaSource src = fixture_source(da, "src-a", live ? "live-1" : "");
        StreamRequest prq;
        std::string purl = playback_url(it, src, p, "sess-9", 0, &prq);
        DlRequest dr;
        CHECK(build_request(it, &d, &src, p, &dr));
        // The decision itself: identical in every field.
        const StreamRequest &drq = dr.decision;
        CHECK(drq.max_w == prq.max_w && drq.max_h == prq.max_h);
        CHECK(!strcmp(drq.profile, prq.profile) && !strcmp(drq.level, prq.level));
        CHECK(drq.vbitrate == prq.vbitrate);                    // copy vs transcode
        CHECK(drq.vreq == prq.vreq);                            // the ceiling asked for
        CHECK(!strcmp(drq.acodec, prq.acodec) && drq.abitrate == prq.abitrate);
        CHECK(drq.achans == prq.achans);
        CHECK((drq.hd_codec == NULL) == (prq.hd_codec == NULL));
        if (drq.hd_codec && prq.hd_codec) CHECK(!strcmp(drq.hd_codec, prq.hd_codec));
        CHECK(drq.audio_idx == prq.audio_idx && drq.sub_idx == prq.sub_idx);
        CHECK(!strcmp(drq.source_id, prq.source_id));
        CHECK(!strcmp(drq.live_stream_id, prq.live_stream_id));
        // The URL: same path, same parameters, except the session.
        std::string ppath, dpath;
        auto pq = query_of(purl, &ppath);
        auto dq = query_of(dr.url, &dpath);
        CHECK(ppath == dpath);
        CHECK(pq.count("PlaySessionId") == 1 && dq.count("PlaySessionId") == 0);
        pq.erase("PlaySessionId");
        CHECK(pq == dq);
        CHECK(dq["StartTimeTicks"] == "0");
        combos++;
    }
    printf("    %d combinations identical\n", combos);
    // A playback that resumes differs in StartTimeTicks and nothing else.
    StreamPrefs p = prefs_of(VQ_1080P_25, false, 2);
    JFMediaSource src = fixture_source(1);
    auto pq = query_of(playback_url(it, src, p, "", 36000000000ULL), NULL);
    DlRequest dr;
    CHECK(build_request(it, &d, &src, p, &dr));
    auto dq = query_of(dr.url, NULL);
    CHECK(pq["StartTimeTicks"] == "36000000000" && dq["StartTimeTicks"] == "0");
    pq.erase("StartTimeTicks"); dq.erase("StartTimeTicks");
    CHECK(pq == dq);
}

static void test_download_dolby_digital(void) {
    s_test = "download in Dolby Digital mode carries the AC-3 track"; printf("- %s\n", s_test);
    JFItem it = fixture_item();
    XMBItemDetail d = fixture_detail();
    StreamPrefs p = prefs_of(VQ_1080P_25, false, 2);
    p.passthrough = true;
    // Default track = "English - AC3 - 5.1": copied, no AudioBitrate.
    JFMediaSource ac3 = fixture_source(2);
    DlRequest dr;
    CHECK(build_request(it, &d, &ac3, p, &dr));
    auto q = query_of(dr.url, NULL);
    CHECK(q["AudioCodec"] == "ac3" && q["AllowAudioStreamCopy"] == "true");
    CHECK(q.count("AudioBitrate") == 0 && q["MaxAudioChannels"] == "6");
    CHECK(!strcmp(dr.meta.audio_codec, "ac3") && dr.decision.ac3_copy);
    // Default track = DTS-HD: transcoded to AC-3, never an HD copy.
    JFMediaSource dts = fixture_source(0);
    CHECK(build_request(it, &d, &dts, p, &dr));
    q = query_of(dr.url, NULL);
    CHECK(q["AudioCodec"] == "ac3" && q["AllowAudioStreamCopy"] == "false");
    CHECK(q["AudioBitrate"] == "640000" && !dr.decision.ac3_copy && !dr.decision.hd_codec);
}

static void test_download_url_shape(void) {
    s_test = "download URL: whole title, no session, no token"; printf("- %s\n", s_test);
    JFItem it = fixture_item();
    XMBItemDetail d = fixture_detail();
    JFMediaSource src = fixture_source(2);
    DlRequest dr;
    CHECK(build_request(it, &d, &src, prefs_of(VQ_720P, false, 0), &dr));
    std::string path;
    auto q = query_of(dr.url, &path);
    CHECK(path == std::string(SERVER) + "/Videos/src-b%20a%26b/stream.ts");   // MPEG-TS, the version's id
    CHECK(q["VideoCodec"] == "h264" && q["Static"] == "false");
    CHECK(q["StartTimeTicks"] == "0");
    CHECK(q.count("PlaySessionId") == 0);
    CHECK(q["DeviceId"] == DEVICE);
    CHECK(q["MediaSourceId"] == "src-b%20a%26b");
    CHECK(q["AudioStreamIndex"] == "3");                  // the default track
    CHECK(q.count("SubtitleStreamIndex") == 0);           // subtitles off, as on Play
    CHECK(strstr(dr.url, "Token") == NULL && strstr(dr.url, "api_key") == NULL);
    // Nothing guessed: without the version (and so its tracks) there is no
    // request, because Play would have used those tracks.
    CHECK(!build_request(it, &d, NULL, prefs_of(VQ_720P, false, 0), &dr));
    JFItem bad = fixture_item("../etc");
    CHECK(!build_request(bad, &d, &src, prefs_of(VQ_720P, false, 0), &dr));
    JFItem noname = fixture_item(); noname.name[0] = '\0';
    CHECK(!build_request(noname, &d, &src, prefs_of(VQ_720P, false, 0), &dr));
    CHECK(!build_request(it, &d, &src, prefs_of(VQ_720P, false, 0), &dr, ""));
    std::string huge = "http://" + std::string(DL_URL_MAX, 'h');
    CHECK(!build_request(it, &d, &src, prefs_of(VQ_720P, false, 0), &dr, huge.c_str()));
    // An https server builds, but the manager refuses it (no TLS here).
    CHECK(build_request(it, &d, &src, prefs_of(VQ_720P, false, 0), &dr, "https://secure.host"));
    begin("request with an https server is refused");
    CHECK(dl_enqueue(&dr.meta, dr.url, 0, &dr.extras) == DL_E_INVALID);
    CHECK(build_request(it, &d, &src, prefs_of(VQ_720P, false, 0), &dr));
    DlExtras bad_x = dr.extras;
    bad_x.container = "mpegts-too-long";
    CHECK(dl_enqueue(&dr.meta, dr.url, 0, &bad_x) == DL_E_INVALID);
    bad_x = dr.extras;
    bad_x.poster_url = "https://no.tls/x.jpg";
    CHECK(dl_enqueue(&dr.meta, dr.url, 0, &bad_x) == DL_E_INVALID);
    CHECK(dl_enqueue(&dr.meta, dr.url, 0, &dr.extras) == DL_OK);
}

static void test_request_gating(void) {
    s_test = "playback gating on URLs from the shared builder"; printf("- %s\n", s_test);
    JFItem it = fixture_item();
    JFMediaSource aac = fixture_source(4);     // default track: AAC stereo
    JFMediaSource thd = fixture_source(1);     // default track: TrueHD
    for (int q = 0; q < VQ_COUNT; q++) {
        vquality_t vq = vquality_sanitize(q);
        const bool low = vq == VQ_480P || vq == VQ_360P;
        CHECK(dl_stream_is_light(playback_url(it, aac, prefs_of(q, false, 0), "s", 0).c_str()) == low);
        CHECK(dl_stream_is_light(playback_url(it, aac, prefs_of(q, true, 1), "s", 0).c_str()) == low);
        // 5.1 with an HD track copied: heavy at every quality, 480p included.
        CHECK(!dl_stream_is_light(playback_url(it, thd, prefs_of(q, false, 2), "s", 0).c_str()));
    }
}

// =========================================================================
// Metadata
// =========================================================================

static void test_item_identity(void) {
    s_test = "item identity parse"; printf("- %s\n", s_test);
    JFItemIdentity id;
    CHECK(jellyfin_parse_item_identity(kEpisodeJson, &id));
    CHECK(!strcmp(id.series_name, "Some Show") && !strcmp(id.series_id, "show9"));
    CHECK(id.season == 1 && id.episode == 3);
    CHECK(id.year == 2008 && id.runtime_secs == 2820);
    CHECK(!id.has_backdrop && !strcmp(id.parent_backdrop_id, "show9"));
    // A movie: no series, no numbers -- and "ParentIndexNumber" must not be
    // read as the episode number.
    const char *movie = "{\"Name\":\"Film\",\"ProductionYear\":1999,\"RunTimeTicks\":81000000000,"
                        "\"BackdropImageTags\":[\"abc\"]}";
    CHECK(jellyfin_parse_item_identity(movie, &id));
    CHECK(id.series_name[0] == '\0' && id.season == -1 && id.episode == -1);
    CHECK(id.year == 1999 && id.runtime_secs == 8100 && id.has_backdrop);
    CHECK(jellyfin_parse_item_identity("{\"ParentIndexNumber\":4}", &id));
    CHECK(id.season == 4 && id.episode == -1);
    CHECK(jellyfin_parse_item_identity("{}", &id));
    CHECK(id.runtime_secs == 0 && id.year == 0 && id.season == -1);
    CHECK(!jellyfin_parse_item_identity("", &id));
    CHECK(!jellyfin_parse_item_identity(NULL, &id));
}

static void test_request_metadata(void) {
    s_test = "request metadata"; printf("- %s\n", s_test);
    JFItem it = fixture_item();
    XMBItemDetail d = fixture_detail();
    JFMediaSource src = fixture_source(1);    // TrueHD default
    DlRequest dr;
    CHECK(build_request(it, &d, &src, prefs_of(VQ_1080P, false, 2), &dr));
    const DlMeta &m = dr.meta;
    CHECK(!strcmp(m.id, it.id) && !strcmp(m.title, "Pilot") && !strcmp(m.type, "Episode"));
    CHECK(!strcmp(m.series, "Some Show") && !strcmp(m.series_id, "show9"));
    CHECK(m.season == 1 && m.episode == 3 && m.year == 2008 && m.runtime_secs == 2820);
    CHECK(!strcmp(m.overview, "Two lines\nof plot"));
    // Media information describes the FILE the request will produce.
    CHECK(!strcmp(m.container, "ts") && !strcmp(m.video_codec, "h264"));
    CHECK(!strcmp(m.audio_codec, "truehd") && m.audio_channels == 8);
    CHECK(m.width == 1920 && m.height == 1080 && m.video_bitrate == 10000000);
    CHECK(!strcmp(m.media_source_id, "src-b a&b") && m.audio_stream_index == 2);
    CHECK(!strcmp(m.quality, "High"));
    CHECK(!strcmp(m.audio_info, "English - TRUEHD - 7.1"));   // the track downloaded
    CHECK(!strcmp(m.video_info, "1080p H264 SDR"));
    CHECK(m.poster[0] == '\0' && m.backdrop[0] == '\0');      // until they exist
    // Artwork: the episode's own Primary; the series' backdrop it inherits.
    CHECK(strstr(dr.poster_url, "/Items/0123456789abcdef0123456789abcdef/Images/Primary?") != NULL);
    CHECK(strstr(dr.backdrop_url, "/Items/show9/Images/Backdrop?") != NULL);
    CHECK(strstr(dr.poster_url, "Token") == NULL);
    CHECK(dr.extras.expect_secs == 2820 && !strcmp(dr.extras.container, "ts"));
    // Round trip through meta.txt.
    char text[4096];
    DlMeta back;
    CHECK(dl_meta_format(&m, text, sizeof(text)) > 0 && dl_meta_parse(text, &back));
    CHECK(back.season == 1 && back.episode == 3 && !strcmp(back.series, "Some Show"));

    s_test = "request metadata, optional fields missing"; printf("- %s\n", s_test);
    JFItem mv = fixture_item("fedcba9876543210fedcba9876543210", "Movie");
    JFMediaSource msrc = fixture_source(4);
    msrc.runtime_secs = 5400;
    CHECK(build_request(mv, NULL, &msrc, prefs_of(VQ_720P, false, 0), &dr));
    CHECK(dr.meta.series[0] == '\0' && dr.meta.season == -1 && dr.meta.episode == -1);
    CHECK(dr.meta.year == 0 && dr.meta.overview[0] == '\0');
    CHECK(dr.meta.runtime_secs == 5400);                  // from the version
    CHECK(!strcmp(dr.meta.audio_codec, "mp3") && dr.meta.audio_channels == 2);
    CHECK(dr.backdrop_url[0] == '\0' && dr.extras.backdrop_url == NULL);
    CHECK(dr.poster_url[0] != '\0');
}

// =========================================================================
// Artwork
// =========================================================================

static FakeResp jpeg_resp(int bytes) {
    FakeResp r;
    r.body = fake_jpeg(bytes);
    r.has_body = true;
    r.content_type = "image/jpeg";
    return r;
}

static FakeResp ts_resp(int packets, double secs, bool chunked = true) {
    FakeResp r;
    r.body = fake_ts(packets, secs);
    r.has_body = true;
    r.chunked = chunked;
    r.accept_ranges = !chunked;
    r.honor_range = !chunked;      // a live transcode ignores Range
    return r;
}

static std::string file_bytes(const std::string &path) {
    std::string out;
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) return out;
    int c;
    while ((c = fgetc(f)) != EOF) out.push_back((char)c);
    fclose(f);
    return out;
}

// Enqueue the fixture episode exactly as dl_download_item() does.
static DlResult enqueue_fixture(DlRequest *dr, int quality = VQ_720P, double runtime = -1) {
    JFItem it = fixture_item();
    XMBItemDetail d = fixture_detail();
    if (runtime >= 0) d.identity.runtime_secs = (unsigned)runtime;
    JFMediaSource src = fixture_source(2);
    if (runtime >= 0) src.runtime_secs = (unsigned)runtime;   // the fallback too
    if (!build_request(it, &d, &src, prefs_of(quality, false, 0), dr)) return DL_E_INVALID;
    return dl_enqueue(&dr->meta, dr->url, 0, &dr->extras);
}

static const char *FX = "0123456789abcdef0123456789abcdef";

static void test_artwork(void) {
    begin("artwork stored with the item");
    g_fake_routes.erase("/Images/");
    g_fake_routes["/Images/Primary"]  = jpeg_resp(20000);
    g_fake_routes["/Images/Backdrop"] = jpeg_resp(90000);
    g_fake_default = ts_resp(3000, 600);
    DlRequest dr;
    CHECK(enqueue_fixture(&dr, VQ_720P, 600) == DL_OK);
    CHECK(dl_manager_step());
    DlStatus st = status_of(FX);
    CHECK(st.rec.state == DL_COMPLETED);
    CHECK(file_bytes(item_file(FX, DL_FILE_POSTER)) == fake_jpeg(20000));
    CHECK(file_bytes(item_file(FX, DL_FILE_BACKDROP)) == fake_jpeg(90000));
    DlMeta m;
    CHECK(dl_load_meta(FX, &m));
    CHECK(!strcmp(m.poster, DL_FILE_POSTER) && !strcmp(m.backdrop, DL_FILE_BACKDROP));
    CHECK(st.rec.poster_url[0] == '\0' && st.rec.backdrop_url[0] == '\0');
    CHECK(g_fake_route_hits == 2);
    // Artwork requests carry the session, like everything else.
    int authed = 0;
    for (auto &r : g_fake_requests)
        if (r.find("/Images/") != std::string::npos &&
            r.find("X-Emby-Authorization: ") != std::string::npos) authed++;
    CHECK(authed == 2);

    begin("artwork failures never fail the film");
    g_fake_routes.erase("/Images/");
    FakeResp e500; e500.status = 500;
    g_fake_routes["/Images/Primary"] = e500;
    FakeResp html; html.content_type = "text/html"; html.body = "<html>nope</html>"; html.has_body = true;
    g_fake_routes["/Images/Backdrop"] = html;
    g_fake_default = ts_resp(3000, 600);
    CHECK(enqueue_fixture(&dr, VQ_720P, 600) == DL_OK);
    CHECK(dl_manager_step());
    st = status_of(FX);
    CHECK(st.rec.state == DL_COMPLETED);
    CHECK(dl_plat_file_size(item_file(FX, DL_FILE_POSTER).c_str()) == -1);
    CHECK(dl_plat_file_size(item_file(FX, DL_FILE_BACKDROP).c_str()) == -1);
    CHECK(st.rec.art_tries == 2);
    CHECK(dl_load_meta(FX, &m) && m.poster[0] == '\0');

    begin("artwork the server does not have is not retried");
    g_fake_routes.erase("/Images/");
    FakeResp nf; nf.status = 404;
    g_fake_routes["/Images/Primary"] = nf;
    g_fake_routes["/Images/Backdrop"] = nf;
    FakeResp drop = ts_resp(3000, 600); drop.drop_after = 100000;
    g_fake_queue.push_back(drop);
    g_fake_default = ts_resp(3000, 600);
    CHECK(enqueue_fixture(&dr, VQ_720P, 600) == DL_OK);
    CHECK(dl_manager_step());
    st = status_of(FX);
    CHECK(st.rec.poster_url[0] == '\0' && st.rec.backdrop_url[0] == '\0');
    CHECK(st.rec.art_tries == 0);
    const int hits = g_fake_route_hits;
    CHECK(step_after_backoff());
    CHECK(g_fake_route_hits == hits);                    // not asked again
    CHECK(status_of(FX).rec.state == DL_COMPLETED);

    begin("not an image / too big");
    g_fake_routes.erase("/Images/");
    FakeResp notimg; notimg.content_type = "image/jpeg"; notimg.body = "GIF89a-not-jpeg"; notimg.has_body = true;
    g_fake_routes["/Images/Primary"] = notimg;
    g_fake_routes["/Images/Backdrop"] = jpeg_resp(600 * 1024);   // past the 512 KB buffer
    g_fake_default = ts_resp(3000, 600);
    CHECK(enqueue_fixture(&dr, VQ_720P, 600) == DL_OK);
    CHECK(dl_manager_step());
    CHECK(status_of(FX).rec.state == DL_COMPLETED);
    CHECK(dl_plat_file_size(item_file(FX, DL_FILE_POSTER).c_str()) == -1);
    CHECK(dl_plat_file_size(item_file(FX, DL_FILE_BACKDROP).c_str()) == -1);

    begin("artwork gives up after its tries, across restarts");
    g_fake_routes.erase("/Images/");
    g_fake_routes["/Images/Primary"] = e500;
    g_fake_routes["/Images/Backdrop"] = e500;
    for (int i = 0; i < 6; i++) { FakeResp d2 = ts_resp(3000, 600); d2.drop_after = 50000; g_fake_queue.push_back(d2); }
    g_fake_default = ts_resp(3000, 600);
    CHECK(enqueue_fixture(&dr, VQ_720P, 600) == DL_OK);
    CHECK(dl_manager_step());                            // tries 2 (both images)
    restart_app();
    CHECK(status_of(FX).rec.art_tries == 2);             // persisted
    CHECK(dl_manager_step());                            // tries 4 -> given up
    st = status_of(FX);
    CHECK(st.rec.poster_url[0] == '\0' && st.rec.backdrop_url[0] == '\0');
    const int h2 = g_fake_route_hits;
    CHECK(step_after_backoff());
    CHECK(g_fake_route_hits == h2);

    begin("artwork saved but record not updated (crash) is not re-fetched");
    g_fake_routes.erase("/Images/");
    g_fake_routes["/Images/Primary"]  = jpeg_resp(20000);
    g_fake_routes["/Images/Backdrop"] = jpeg_resp(20000);
    g_fake_default = ts_resp(3000, 600);
    CHECK(enqueue_fixture(&dr, VQ_720P, 600) == DL_OK);
    {   // both images already there, urls still pending in the record
        std::string j = fake_jpeg(1234);
        CHECK(dl_store_save_blob(FX, DL_FILE_POSTER, (const uint8_t *)j.data(), (int)j.size()));
        CHECK(dl_store_save_blob(FX, DL_FILE_BACKDROP, (const uint8_t *)j.data(), (int)j.size()));
    }
    CHECK(dl_manager_step());
    CHECK(g_fake_route_hits == 0);
    CHECK(status_of(FX).rec.poster_url[0] == '\0');
    CHECK(dl_load_meta(FX, &m) && !strcmp(m.poster, DL_FILE_POSTER) &&
          !strcmp(m.backdrop, DL_FILE_BACKDROP));
    CHECK(file_bytes(item_file(FX, DL_FILE_POSTER)) == fake_jpeg(1234));

    begin("artwork already on disk is never re-fetched");
    g_fake_routes.erase("/Images/");
    g_fake_routes["/Images/Primary"]  = jpeg_resp(20000);
    g_fake_routes["/Images/Backdrop"] = jpeg_resp(20000);
    FakeResp d3 = ts_resp(3000, 600); d3.drop_after = 60000;
    g_fake_queue.push_back(d3);
    g_fake_default = ts_resp(3000, 600);
    CHECK(enqueue_fixture(&dr, VQ_720P, 600) == DL_OK);
    CHECK(dl_manager_step());                            // art fetched, media dropped
    CHECK(g_fake_route_hits == 2);
    restart_app();                                       // "the console rebooted"
    CHECK(step_after_backoff());
    CHECK(g_fake_route_hits == 2);                       // not again
    CHECK(status_of(FX).rec.state == DL_COMPLETED);
    CHECK(dl_load_meta(FX, &m) && !strcmp(m.poster, DL_FILE_POSTER));
}

// =========================================================================
// Completion validation (MPEG-TS)
// =========================================================================

static void test_ts_scan(void) {
    s_test = "TS scan"; printf("- %s\n", s_test);
    std::string ts = fake_ts(2000, 600);
    DlTsScan t;
    dl_ts_init(&t);
    // Fed in odd pieces, as flushes do.
    for (size_t i = 0; i < ts.size(); ) {
        size_t n = 1 + (i * 7919) % 5000;
        if (n > ts.size() - i) n = ts.size() - i;
        dl_ts_feed(&t, (const uint8_t *)ts.data() + i, (int)n);
        i += n;
    }
    CHECK(t.packets == 2000 && t.sync_errors == 0);
    CHECK(dl_ts_span_secs(&t) >= 599 && dl_ts_span_secs(&t) <= 600);
    bool shrt;
    CHECK(dl_ts_plausible(&t, ts.size(), 600, &shrt) && !shrt);
    CHECK(dl_ts_plausible(&t, ts.size(), 0, &shrt));
    CHECK(dl_ts_plausible(&t, ts.size(), 670, &shrt));      // within the slack
    CHECK(!dl_ts_plausible(&t, ts.size(), 7200, &shrt) && shrt);   // ended early
    CHECK(!dl_ts_plausible(&t, ts.size() + 1, 600, &shrt) && !shrt);
    // Garbage is not a TS.
    std::string junk(188 * 100, 'x');
    dl_ts_init(&t);
    dl_ts_feed(&t, (const uint8_t *)junk.data(), (int)junk.size());
    CHECK(t.sync_errors == 100 && !dl_ts_plausible(&t, junk.size(), 0, &shrt));
    // A TS without video PTS cannot be played.
    std::string nopts = fake_ts(10, 0);
    for (size_t i = 0; i < nopts.size(); i += 188) nopts[i + 1] &= ~0x40;
    dl_ts_init(&t);
    dl_ts_feed(&t, (const uint8_t *)nopts.data(), (int)nopts.size());
    CHECK(dl_ts_span_secs(&t) == -1 && !dl_ts_plausible(&t, nopts.size(), 0, &shrt));
    // Audio PES do not count: only the video decides what time is covered.
    std::string av = fake_ts(200, 10);
    {
        uint8_t *e = (uint8_t *)&av[(size_t)150 * 188 + 4];   // last PES start
        e[3] = 0xC0;                                            // make it audio...
        uint64_t pts = 126000 + 90000ULL * 5000;                // ...far in the future
        e[9]  = (uint8_t)(0x21 | ((pts >> 29) & 0x0E));
        e[10] = (uint8_t)(pts >> 22);
        e[11] = (uint8_t)(((pts >> 14) & 0xFE) | 1);
        e[12] = (uint8_t)(pts >> 7);
        e[13] = (uint8_t)(((pts << 1) & 0xFE) | 1);
    }
    dl_ts_init(&t);
    dl_ts_feed(&t, (const uint8_t *)av.data(), (int)av.size());
    CHECK(dl_ts_span_secs(&t) <= 10);
    // 33-bit PTS wrap mid-file does not collapse the span.
    std::string wrap = fake_ts(200, 0);
    for (int k = 0; k < 4; k++) {
        uint64_t pts = ((1ULL << 33) - 90000ULL * 2 + 90000ULL * k) & ((1ULL << 33) - 1);
        uint8_t *e = (uint8_t *)&wrap[(size_t)k * 50 * 188 + 4];
        e[9]  = (uint8_t)(0x21 | ((pts >> 29) & 0x0E));
        e[10] = (uint8_t)(pts >> 22);
        e[11] = (uint8_t)(((pts >> 14) & 0xFE) | 1);
        e[12] = (uint8_t)(pts >> 7);
        e[13] = (uint8_t)(((pts << 1) & 0xFE) | 1);
    }
    dl_ts_init(&t);
    dl_ts_feed(&t, (const uint8_t *)wrap.data(), (int)wrap.size());
    CHECK(dl_ts_span_secs(&t) == 3);
}

static void test_ts_validation(void) {
    begin("complete transcode verified and kept");
    g_fake_default = ts_resp(4000, 2820);
    DlRequest dr;
    CHECK(enqueue_fixture(&dr, VQ_720P, 2820) == DL_OK);
    CHECK(dl_manager_step());
    DlStatus st = status_of(FX);
    CHECK(st.rec.state == DL_COMPLETED);
    CHECK(file_bytes(item_file(FX, DL_FILE_MEDIA)) == fake_ts(4000, 2820));

    begin("transcode that died part way is not completed");
    g_fake_default = ts_resp(4000, 1200);      // 20 of 47 minutes, cleanly chunked
    CHECK(enqueue_fixture(&dr, VQ_720P, 2820) == DL_OK);
    CHECK(dl_manager_step());
    st = status_of(FX);
    CHECK(st.rec.state == DL_QUEUED && st.rec.error == DL_ERR_BAD_MEDIA);
    CHECK(dl_plat_file_size(item_file(FX, DL_FILE_MEDIA).c_str()) == -1);
    CHECK(dl_plat_file_size(item_file(FX, DL_FILE_PART).c_str()) == 0);   // not resumable
    // Once more, then it is the user's call -- progress does not buy retries.
    CHECK(step_after_backoff());
    st = status_of(FX);
    CHECK(st.rec.state == DL_FAILED && st.rec.error == DL_ERR_BAD_MEDIA);
    CHECK(st.rec.attempts == DL_MEDIA_MAX_ATTEMPTS);
    CHECK(!step_after_backoff());
    g_fake_default = ts_resp(4000, 2820);      // the server is fixed
    CHECK(dl_retry(FX) == DL_OK);
    CHECK(dl_manager_step());
    CHECK(status_of(FX).rec.state == DL_COMPLETED);

    begin("not a transport stream");
    FakeResp junk;                              // the byte pattern: no 0x47 sync
    junk.total = 188 * 1000;
    g_fake_default = junk;
    CHECK(enqueue_fixture(&dr, VQ_720P, 0) == DL_OK);
    CHECK(dl_manager_step());
    CHECK(status_of(FX).rec.error == DL_ERR_BAD_MEDIA);

    begin("TS with a torn last packet");
    FakeResp torn = ts_resp(1000, 600, false);
    torn.body.resize(torn.body.size() - 5);
    g_fake_default = torn;
    CHECK(enqueue_fixture(&dr, VQ_720P, 600) == DL_OK);
    CHECK(dl_manager_step());
    CHECK(status_of(FX).rec.error == DL_ERR_BAD_MEDIA);

    begin("runtime unknown: structure still checked, length not");
    g_fake_default = ts_resp(1000, 60);
    CHECK(enqueue_fixture(&dr, VQ_720P, 0) == DL_OK);
    CHECK(dl_manager_step());
    CHECK(status_of(FX).rec.state == DL_COMPLETED);

    begin("byte-resumed TS (Range honoured)");
    FakeResp first = ts_resp(3000, 600, false);
    first.drop_after = 188 * 700 + 91;         // mid-packet
    g_fake_queue.push_back(first);
    g_fake_default = ts_resp(3000, 600, false);
    CHECK(enqueue_fixture(&dr, VQ_720P, 600) == DL_OK);
    CHECK(dl_manager_step());
    CHECK(status_of(FX).rec.bytes_done == 188 * 700 + 91);
    CHECK(step_after_backoff());
    CHECK(last_request_has_range(188 * 700 + 91));
    CHECK(status_of(FX).rec.state == DL_COMPLETED);
    CHECK(file_bytes(item_file(FX, DL_FILE_MEDIA)) == fake_ts(3000, 600));

    begin("byte-resumed TS with a torn tail");
    FakeResp tfirst = ts_resp(3000, 600, false);
    tfirst.body.resize(tfirst.body.size() - 7);
    FakeResp tsecond = tfirst;
    tfirst.drop_after = 188 * 500;
    g_fake_queue.push_back(tfirst);
    g_fake_default = tsecond;
    CHECK(enqueue_fixture(&dr, VQ_720P, 600) == DL_OK);
    CHECK(dl_manager_step());
    CHECK(step_after_backoff());
    CHECK(last_request_has_range(188 * 500));
    CHECK(status_of(FX).rec.error == DL_ERR_BAD_MEDIA);
    CHECK(dl_plat_file_size(item_file(FX, DL_FILE_MEDIA).c_str()) == -1);

    begin("transcode restarted after Range ignored, then validated");
    FakeResp cut = ts_resp(3000, 600); cut.drop_after = 50000;
    g_fake_queue.push_back(cut);
    g_fake_default = ts_resp(3000, 600);
    CHECK(enqueue_fixture(&dr, VQ_720P, 600) == DL_OK);
    CHECK(dl_manager_step());
    CHECK(status_of(FX).rec.error == DL_ERR_PARTIAL);
    CHECK(step_after_backoff());
    CHECK(status_of(FX).rec.state == DL_COMPLETED);   // scanned from byte 0 again
    CHECK(file_bytes(item_file(FX, DL_FILE_MEDIA)) == fake_ts(3000, 600));

    begin("server transcode failure (500) retried");
    FakeResp e500; e500.status = 500;
    g_fake_queue.push_back(e500);
    g_fake_default = ts_resp(1000, 600);
    CHECK(enqueue_fixture(&dr, VQ_720P, 600) == DL_OK);
    CHECK(dl_manager_step());
    CHECK(status_of(FX).rec.error == DL_ERR_SERVER && status_of(FX).rec.state == DL_QUEUED);
    CHECK(step_after_backoff());
    CHECK(status_of(FX).rec.state == DL_COMPLETED);
}

static void test_request_heavy_playback(void) {
    begin("heavy playback parks a requested download; it resumes after");
    JFItem it = fixture_item();
    JFMediaSource src = fixture_source(2);
    static std::string heavy;
    heavy = playback_url(it, src, prefs_of(VQ_1080P_25, false, 0), "s", 0);
    g_fake_default = ts_resp(3000, 600, false);
    g_fake_default.on_body = [](int64_t sent) {
        static bool done; if (sent < 1000) done = false;
        if (!done && sent >= 100000) { done = true; dl_playback_begin(heavy.c_str()); }
    };
    DlRequest dr;
    CHECK(enqueue_fixture(&dr, VQ_720P, 600) == DL_OK);
    CHECK(dl_manager_step());
    DlStatus st = status_of(FX);
    CHECK(st.rec.state == DL_QUEUED && st.rec.error == DL_ERR_NONE);
    CHECK(dl_plat_file_size(item_file(FX, DL_FILE_PART).c_str()) == (int64_t)st.rec.bytes_done);
    DlRecord disk;
    CHECK(dl_store_load_record(FX, &disk) && disk.state == DL_QUEUED);   // persisted
    CHECK(!dl_manager_step());
    g_fake_default.on_body = nullptr;
    dl_playback_end();
    CHECK(dl_manager_step());
    CHECK(last_request_has_range(st.rec.bytes_done));
    CHECK(status_of(FX).rec.state == DL_COMPLETED);
    CHECK(file_bytes(item_file(FX, DL_FILE_MEDIA)) == fake_ts(3000, 600));
}

// =========================================================================
// Session / auth
// =========================================================================

static void test_auth_hold(void) {
    begin("no session: nothing transfers");
    dl_set_auth_header("");
    CHECK(dl_auth_held());
    DlMeta m = meta_for("noauth");
    CHECK(dl_enqueue(&m, URL, 0) == DL_OK);
    CHECK(!dl_manager_step());
    CHECK(g_fake_connects == 0);

    begin("sign-in expired: queue held, nothing failed, resumes after login");
    FakeResp e401; e401.status = 401;
    g_fake_queue.push_back(e401);
    DlMeta a = meta_for("auth_a"), b = meta_for("auth_b");
    CHECK(dl_enqueue(&a, URL, 0) == DL_OK);
    CHECK(dl_enqueue(&b, URL, 0) == DL_OK);
    CHECK(dl_manager_step());
    DlStatus st = status_of("auth_a");
    CHECK(st.rec.state == DL_QUEUED && st.rec.error == DL_ERR_AUTH);
    CHECK(st.rec.attempts == 0 && st.rec.http_status == 401);
    CHECK(dl_auth_held());
    const int connects = g_fake_connects;
    CHECK(!dl_manager_step());                          // b is not tried either
    CHECK(!step_after_backoff());
    CHECK(g_fake_connects == connects);
    CHECK(status_of("auth_b").rec.state == DL_QUEUED);
    char line[512];
    CHECK(dl_svc_auth_header(line, sizeof(line), "fresh-token", DEVICE));
    dl_set_auth_header(line);                           // logged in again
    CHECK(!dl_auth_held());
    CHECK(run_worker() == 2);
    CHECK(status_of("auth_a").rec.state == DL_COMPLETED);
    CHECK(status_of("auth_b").rec.state == DL_COMPLETED);
    CHECK(g_fake_requests.back().find("Token=\"fresh-token\"") != std::string::npos);
    // Held across a restart too: the record says queued, and a restart
    // without a session transfers nothing.
    s_test = "auth header"; printf("- %s\n", s_test);
    CHECK(dl_svc_auth_header(line, sizeof(line), "tok", "dev-9"));
    CHECK(!strcmp(line, "X-Emby-Authorization: MediaBrowser Client=\"PS3\", Device=\"PS3\","
                        " DeviceId=\"dev-9\", Version=\"0.1\", Token=\"tok\""));
    CHECK(!dl_svc_auth_header(line, sizeof(line), "", "dev-9") && line[0] == '\0');
    CHECK(!dl_svc_auth_header(line, sizeof(line), NULL, "dev-9"));
    CHECK(!dl_svc_auth_header(line, 20, "tok", "dev-9") && line[0] == '\0');
}

// =========================================================================
// Service lifecycle
// =========================================================================

static void seed_store(const std::string &root) {
    // An earlier session of the app: one completed, one mid-download.
    CHECK(dl_manager_init(root.c_str(), &s_cfg));
    dl_set_auth_header("X-Emby-Authorization: MediaBrowser Token=\"t\"");
    DlMeta done = meta_for("seed_done", "Done Film"), part = meta_for("seed_part", "Half Film");
    CHECK(dl_enqueue(&done, URL, 0) == DL_OK);
    CHECK(dl_enqueue(&part, URL, 0) == DL_OK);
    CHECK(dl_manager_step());
    FakeResp drop; drop.drop_after = 120000;
    g_fake_queue.push_back(drop);
    CHECK(dl_manager_step());
    DlRecord r;
    CHECK(dl_store_load_record("seed_part", &r));
    r.state = DL_DOWNLOADING;                       // the app died mid-transfer
    CHECK(dl_store_save_record(&r));
    dl_manager_shutdown();
    dl_set_auth_header("");
}

static void test_service_lifecycle(void) {
    begin("service: start once, restore on the worker, session gate");
    const std::string root = s_root;
    seed_store(root);
    const char *roots[] = { "/proc/not/writable/offline", root.c_str() };
    g_fake_thread_starts = g_fake_thread_joins = 0;
    CHECK(dl_svc_start(roots, 2));
    CHECK(dl_svc_start(roots, 2));                  // idempotent
    CHECK(g_fake_thread_starts == 1);               // one worker, ever
    CHECK(dl_svc_started());
    CHECK(!dl_manager_ready());                     // restore is the worker's job
    CHECK(dl_svc_root()[0] == '\0');
    g_fake_default.refuse = true;                   // the server is down at boot
    g_fake_connects = 0;
    CHECK(dl_svc_tick() == 0);                      // restore
    CHECK(dl_manager_ready());
    CHECK(!strcmp(dl_svc_root(), root.c_str()));    // fell back past the bad root
    CHECK(g_fake_connects == 0);                    // restore touched no network
    // Completed item usable without the server.
    DlStatus lib[8];
    CHECK(dl_list(lib, 8, true) == 1 && !strcmp(lib[0].rec.id, "seed_done"));
    char path[DL_PATH_MAX];
    CHECK(dl_media_path("seed_done", path, sizeof(path)));
    CHECK(fake_file_matches(path, g_fake_total));
    // The interrupted one is back in the queue with its bytes.
    DlStatus st = status_of("seed_part");
    CHECK(st.rec.state == DL_QUEUED && st.rec.bytes_done == 120000);
    // No session yet: ticks idle, no connection attempted.
    for (int i = 0; i < 3; i++) CHECK(dl_svc_tick() > 0);
    CHECK(g_fake_connects == 0);
    // Session arrives, server still down: tries, fails as unreachable.
    dl_svc_set_session("tok", DEVICE);
    CHECK(dl_svc_tick() == 0);
    CHECK(status_of("seed_part").rec.error == DL_ERR_UNREACHABLE);
    CHECK(dl_list(lib, 8, true) == 1);              // library untouched
    // Server back: resumes from its bytes.
    g_fake_default.refuse = false;
    g_fake_now_ms += 400000;
    CHECK(dl_svc_tick() == 0);
    CHECK(last_request_has_range(120000));
    CHECK(g_fake_requests.back().find("Token=\"tok\"") != std::string::npos);
    CHECK(status_of("seed_part").rec.state == DL_COMPLETED);
    CHECK(dl_svc_tick() > 0);                       // idle again
    // Logout holds the queue.
    DlMeta later = meta_for("after_logout");
    CHECK(dl_enqueue(&later, URL, 0) == DL_OK);
    dl_svc_set_session("", DEVICE);
    const int c = g_fake_connects;
    CHECK(dl_svc_tick() > 0 && g_fake_connects == c);

    s_test = "service: stop and restart"; printf("- %s\n", s_test);
    // While stop waits for the worker, the worker must be told to stop:
    // an attempt it would start now is refused.
    static bool refused_during_join;
    refused_during_join = false;
    g_fake_on_join = []() { refused_during_join = !dl_manager_step(); };
    dl_svc_set_session("tok", DEVICE);                 // queue live again
    dl_svc_stop();
    g_fake_on_join = nullptr;
    CHECK(refused_during_join);
    CHECK(g_fake_thread_joins == 1);
    CHECK(!dl_svc_started() && !dl_manager_ready());
    CHECK(dl_svc_root()[0] == '\0');
    CHECK(dl_enqueue(&later, URL, 0) == DL_E_NOT_READY);
    dl_svc_stop();                                  // twice: harmless
    CHECK(g_fake_thread_joins == 1);
    CHECK(dl_svc_start(roots, 2));                  // a new launch
    CHECK(g_fake_thread_starts == 2);
    CHECK(dl_svc_tick() == 0 && dl_manager_ready());
    CHECK(status_of("seed_done").rec.state == DL_COMPLETED);
    CHECK(status_of("after_logout").rec.state == DL_QUEUED);
    dl_svc_stop();

    s_test = "service: thread cannot start / no writable root"; printf("- %s\n", s_test);
    g_fake_thread_fail = true;
    CHECK(!dl_svc_start(roots, 2) && !dl_svc_started());
    g_fake_thread_fail = false;
    const char *bad[] = { "/proc/nope/a", "/proc/nope/b" };
    CHECK(dl_svc_start(bad, 2));
    CHECK(dl_svc_tick() == 0);
    CHECK(!dl_manager_ready() && dl_svc_root()[0] == '\0');
    CHECK(dl_svc_tick() == 1000);                   // idles, does not spin
    dl_svc_stop();
    // Leave the manager as begin() expects to find it.
    CHECK(dl_manager_init(s_root.c_str(), &s_cfg));
}


// =========================================================================
// Stage 4: offline library, startup path, local playback entry/seek
// =========================================================================

// A TS shaped like ffmpeg's mpegts output: PAT + PMT right before every
// video keyframe (random_access_indicator set), frames at `fps`, a GOP every
// `gop_s` seconds, and a bitrate that swings with a slow pattern (VBR), so
// byte offset and time are NOT proportional.
struct TsFile { std::string data; std::vector<uint64_t> key_off; std::vector<double> key_t; };

static void put_pts(uint8_t *e, uint64_t pts) {
    e[9]  = (uint8_t)(0x21 | ((pts >> 29) & 0x0E));
    e[10] = (uint8_t)(pts >> 22);
    e[11] = (uint8_t)(((pts >> 14) & 0xFE) | 1);
    e[12] = (uint8_t)(pts >> 7);
    e[13] = (uint8_t)(((pts << 1) & 0xFE) | 1);
}

static TsFile make_vbr_ts(double secs, double fps = 5, double gop_s = 2,
                          uint64_t pts0 = 126000, bool rai = true,
                          bool reorder = false, bool pat_every_key = true) {
    TsFile f;
    const int frames = (int)(secs * fps);
    const int gop = (int)(gop_s * fps);
    for (int fr = 0; fr <= frames; fr++) {
        const bool key = fr % gop == 0;
        // reorder: decode order I P B B, so the frame after a P has a LOWER
        // PTS -- and the file's last frames are B-frames.
        int pfr = fr;
        if (reorder && !key) { int k = fr % 3; pfr = k == 1 ? fr + 2 : fr - 1; }
        if (pfr > frames) pfr = frames;
        const uint64_t pts = (pts0 + (uint64_t)((double)pfr * 90000.0 / fps)) & ((1ULL << 33) - 1);
        uint8_t p[188];
        if (key && (pat_every_key || fr == 0)) {
            memset(p, 0xFF, 188); p[0] = 0x47; p[1] = 0x40; p[2] = 0x00; p[3] = 0x10;   // PAT
            f.data.append((const char *)p, 188);
            memset(p, 0xFF, 188); p[0] = 0x47; p[1] = 0x50; p[2] = 0x00; p[3] = 0x10;   // PMT 0x1000
            f.data.append((const char *)p, 188);
            f.key_off.push_back(f.data.size() - 2 * 188);
            f.key_t.push_back((double)fr / fps);
        } else if (key) {
            f.key_off.push_back(f.data.size());      // the keyframe itself
            f.key_t.push_back((double)fr / fps);
        }
        // Frame start: video PES with PTS; keyframes carry an adaptation
        // field with random_access_indicator.
        memset(p, 0xFF, 188);
        p[0] = 0x47; p[1] = 0x41; p[2] = 0x00;
        int off = 4;
        if (key && rai) { p[3] = 0x30; p[4] = 1; p[5] = 0x40; off = 6; }
        else            { p[3] = 0x10; }
        uint8_t *e = p + off;
        e[0] = 0; e[1] = 0; e[2] = 1; e[3] = 0xE0; e[4] = 0; e[5] = 0;
        e[6] = 0x80; e[7] = 0x80; e[8] = 5;
        put_pts(e, pts);
        f.data.append((const char *)p, 188);
        // Payload: 2..40 packets, big on keyframes, slowly varying otherwise.
        int n = key ? 40 : 2 + (int)(18.0 * (1.0 + ((fr / 37) % 5)) / 5.0);
        for (int k = 0; k < n; k++) {
            memset(p, 0x11, 188); p[0] = 0x47; p[1] = 0x01; p[2] = 0x00; p[3] = 0x10;
            f.data.append((const char *)p, 188);
        }
        // An audio packet between frames (PES start, PTS far ahead: must be ignored).
        memset(p, 0x22, 188); p[0] = 0x47; p[1] = 0x41; p[2] = 0x01; p[3] = 0x10;
        e = p + 4; e[0] = 0; e[1] = 0; e[2] = 1; e[3] = 0xC0; e[6] = 0x80; e[7] = 0x80; e[8] = 5;
        put_pts(e, pts + 90000ULL * 3600);
        f.data.append((const char *)p, 188);
    }
    return f;
}

struct MemFile { const std::string *d; int reads; bool fail; };
static int mem_read_at(void *ctx, uint64_t off, uint8_t *buf, int len) {
    MemFile *m = (MemFile *)ctx;
    m->reads++;
    if (m->fail) return -1;
    if (off >= m->d->size()) return 0;
    size_t n = m->d->size() - (size_t)off;
    if (n > (size_t)len) n = (size_t)len;
    memcpy(buf, m->d->data() + off, n);
    return (int)n;
}

static uint8_t s_scratch[256 * 1024];   // the size of the stream's buffer

static void test_local_index(void) {
    s_test = "local file index"; printf("- %s\n", s_test);
    TsFile f = make_vbr_ts(600);
    MemFile m = { &f.data, 0, false };
    StreamLocalIndex idx;
    CHECK(stream_local_index(mem_read_at, &m, f.data.size(), s_scratch, sizeof(s_scratch), &idx));
    CHECK(idx.ok && idx.size == f.data.size());
    CHECK(idx.first_pts == 126000);
    CHECK(idx.duration_secs == 600);        // from the file, not metadata
    // A stream that does not start at PTS ~0 (copyts) measures the same.
    TsFile late = make_vbr_ts(120, 5, 2, 90000ULL * 50000);
    MemFile ml = { &late.data, 0, false };
    CHECK(stream_local_index(mem_read_at, &ml, late.data.size(), s_scratch, sizeof(s_scratch), &idx));
    CHECK(idx.duration_secs == 120);
    // Across a 33-bit PTS wrap.
    TsFile wrap = make_vbr_ts(60, 5, 2, (1ULL << 33) - 90000ULL * 30);
    MemFile mw = { &wrap.data, 0, false };
    CHECK(stream_local_index(mem_read_at, &mw, wrap.data.size(), s_scratch, sizeof(s_scratch), &idx));
    CHECK(idx.duration_secs == 60);
    // Nothing to enter: no video, garbage, empty, unreadable.
    std::string junk(188 * 500, 'x');
    MemFile mj = { &junk, 0, false };
    CHECK(!stream_local_index(mem_read_at, &mj, junk.size(), s_scratch, sizeof(s_scratch), &idx));
    std::string empty;
    MemFile me = { &empty, 0, false };
    CHECK(!stream_local_index(mem_read_at, &me, 0, s_scratch, sizeof(s_scratch), &idx));
    MemFile mf = { &f.data, 0, true };
    CHECK(!stream_local_index(mem_read_at, &mf, f.data.size(), s_scratch, sizeof(s_scratch), &idx));
    CHECK(!idx.ok);
}

static void test_local_seek(void) {
    s_test = "local file seek"; printf("- %s\n", s_test);
    TsFile f = make_vbr_ts(3600, 5, 2);     // an hour, ~50 MB of VBR
    MemFile m = { &f.data, 0, false };
    StreamLocalIndex idx;
    CHECK(stream_local_index(mem_read_at, &m, f.data.size(), s_scratch, sizeof(s_scratch), &idx));
    // Start: offset 0 is the first PAT, position 0.
    uint64_t off, us;
    CHECK(stream_local_seek(mem_read_at, &m, &idx, 0, s_scratch, sizeof(s_scratch), &off, &us));
    CHECK(off == 0 && us == 0);
    int worst_err_ms = 0, worst_reads = 0;
    const uint64_t targets[] = { 1, 7, 61, 599, 1234, 1800, 2999, 3500, 3598 };
    for (uint64_t t : targets) {
        m.reads = 0;
        CHECK(stream_local_seek(mem_read_at, &m, &idx, t * 1000000ULL, s_scratch,
                                sizeof(s_scratch), &off, &us));
        // Lands on a PAT that precedes a keyframe...
        bool is_entry = false;
        for (size_t k = 0; k < f.key_off.size(); k++)
            if (f.key_off[k] == off) {
                is_entry = true;
                // ...whose position is exactly what was reported.
                CHECK(us == (uint64_t)(f.key_t[k] * 1000000.0));
            }
        CHECK(is_entry);
        CHECK(off % 188 == 0);
        // ...never more than half a second past the target, and within a
        // couple of GOPs before it.
        CHECK(us <= t * 1000000ULL + 500000ULL);
        const int err_ms = (int)(((int64_t)t * 1000000LL - (int64_t)us) / 1000);
        if (err_ms > 4500 || !is_entry) printf("    target %llus -> %.2fs off=%llu entry=%d reads=%d\n", (unsigned long long)t, us / 1e6, (unsigned long long)off, (int)is_entry, m.reads);
        CHECK(err_ms <= 4500);
        if (err_ms > worst_err_ms) worst_err_ms = err_ms;
        if (m.reads > worst_reads) worst_reads = m.reads;
    }
    printf("    worst landing %d ms before target, %d reads of 256 KB\n", worst_err_ms, worst_reads);
    CHECK(worst_reads <= 12);
    // Past the end clamps to the last entry, not an error.
    CHECK(stream_local_seek(mem_read_at, &m, &idx, 99999ULL * 1000000ULL, s_scratch,
                            sizeof(s_scratch), &off, &us));
    CHECK(us <= 3600ULL * 1000000ULL && us >= 3590ULL * 1000000ULL);
    // Deterministic: the same seek twice lands on the same byte.
    uint64_t off2, us2;
    CHECK(stream_local_seek(mem_read_at, &m, &idx, 1234 * 1000000ULL, s_scratch, sizeof(s_scratch), &off, &us));
    CHECK(stream_local_seek(mem_read_at, &m, &idx, 1234 * 1000000ULL, s_scratch, sizeof(s_scratch), &off2, &us2));
    CHECK(off == off2 && us == us2);

    s_test = "local seek without keyframe flags"; printf("- %s\n", s_test);
    TsFile nr = make_vbr_ts(300, 5, 2, 126000, false);
    MemFile mn = { &nr.data, 0, false };
    CHECK(stream_local_index(mem_read_at, &mn, nr.data.size(), s_scratch, sizeof(s_scratch), &idx));
    CHECK(stream_local_seek(mem_read_at, &mn, &idx, 100 * 1000000ULL, s_scratch, sizeof(s_scratch), &off, &us));
    CHECK(off % 188 == 0 && us <= 100500000ULL && us >= 90000000ULL);
    // Enters at a PAT (the demuxer's reset needs one), not mid-table.
    CHECK((uint8_t)nr.data[off + 1] == 0x40 && (uint8_t)nr.data[off + 2] == 0x00);
}


static void test_local_seek_edges(void) {
    s_test = "local seek edge cases"; printf("- %s\n", s_test);
    StreamLocalIndex idx;
    uint64_t off, us;
    // An entry search that starts between a PAT and its keyframe still
    // enters at the PAT (the demuxer needs it after its reset).
    TsFile f = make_vbr_ts(120);
    MemFile m = { &f.data, 0, false };
    CHECK(stream_local_index(mem_read_at, &m, f.data.size(), s_scratch, sizeof(s_scratch), &idx));
    for (size_t k = 1; k < 5; k++) {
        CHECK(stream_local_entry(mem_read_at, &m, &idx, f.key_off[k] + 188, s_scratch,
                                 sizeof(s_scratch), &off, &us));
        CHECK(off == f.key_off[k]);
        CHECK(us == (uint64_t)(f.key_t[k] * 1e6));
    }
    // Small reads: the bracket narrows and the forward finish has to walk
    // several windows -- same accuracy.
    static uint8_t tiny[STREAM_LOCAL_MIN_SCRATCH - 1];
    CHECK(!stream_local_seek(mem_read_at, &m, &idx, 10000000ULL, tiny, sizeof(tiny), &off, &us));
    CHECK(!stream_local_index(mem_read_at, &m, f.data.size(), tiny, sizeof(tiny), &idx) && !idx.ok);
    CHECK(stream_local_index(mem_read_at, &m, f.data.size(), s_scratch, sizeof(s_scratch), &idx));
    static uint8_t small[STREAM_LOCAL_MIN_SCRATCH];
    for (uint64_t t : { 13ULL, 59ULL, 111ULL }) {
        CHECK(stream_local_seek(mem_read_at, &m, &idx, t * 1000000ULL, small, sizeof(small), &off, &us));
        CHECK(us <= t * 1000000ULL + 500000ULL && t * 1000000ULL - us <= 2000000ULL);
    }
    // Short GOPs (0.4 s) with small reads: only the forward finish, walking
    // window after window, gets within one GOP of the target.
    TsFile sg = make_vbr_ts(120, 5, 0.4);
    MemFile ms = { &sg.data, 0, false };
    CHECK(stream_local_index(mem_read_at, &ms, sg.data.size(), s_scratch, sizeof(s_scratch), &idx));
    for (uint64_t t : { 17ULL, 63ULL, 101ULL }) {
        CHECK(stream_local_seek(mem_read_at, &ms, &idx, t * 1000000ULL, small, sizeof(small), &off, &us));
        const int64_t d = (int64_t)us - (int64_t)(t * 1000000ULL);   // landing - target
        CHECK(d <= 500000 && d >= -400000);
    }
    CHECK(stream_local_index(mem_read_at, &m, f.data.size(), s_scratch, sizeof(s_scratch), &idx));
    // B-frames at the end: the duration is the highest PTS, not the last.
    // (62 s at 1 fps: the last frame in decode order is a B-frame whose PTS
    // is a second BELOW the one before it.)
    TsFile b = make_vbr_ts(62, 1, 6, 126000, true, true);
    MemFile mb = { &b.data, 0, false };
    CHECK(stream_local_index(mem_read_at, &mb, b.data.size(), s_scratch, sizeof(s_scratch), &idx));
    CHECK(idx.duration_secs == 62);
    // Positions across a 33-bit PTS wrap are still exact.
    TsFile w = make_vbr_ts(60, 5, 2, (1ULL << 33) - 90000ULL * 30);
    MemFile mw = { &w.data, 0, false };
    CHECK(stream_local_index(mem_read_at, &mw, w.data.size(), s_scratch, sizeof(s_scratch), &idx));
    CHECK(stream_local_seek(mem_read_at, &mw, &idx, 45 * 1000000ULL, s_scratch, sizeof(s_scratch), &off, &us));
    CHECK(us <= 45500000ULL && us >= 43000000ULL);
    // Keyframes flagged, but the PAT only at the start (another muxer): the
    // entry is the keyframe itself.
    TsFile np = make_vbr_ts(120, 5, 2, 126000, true, false, false);
    MemFile mn = { &np.data, 0, false };
    CHECK(stream_local_index(mem_read_at, &mn, np.data.size(), s_scratch, sizeof(s_scratch), &idx));
    CHECK(idx.has_rai);
    CHECK(stream_local_seek(mem_read_at, &mn, &idx, 60 * 1000000ULL, s_scratch, sizeof(s_scratch), &off, &us));
    bool at_key = false;
    for (size_t k = 0; k < np.key_off.size(); k++) if (np.key_off[k] == off) at_key = true;
    CHECK(at_key && us <= 60500000ULL && us >= 58000000ULL);
}

static void make_completed(const char *id, const char *title, bool ts = false,
                           int packets = 0) {
    DlMeta m = meta_for(id, title);
    DlExtras x = {};
    if (ts) {
        FakeResp r;
        r.body = fake_ts(packets, 60);
        r.has_body = true;
        g_fake_queue.push_back(r);
        x.container = "ts";
    }
    CHECK(dl_enqueue(&m, URL, 0, ts ? &x : NULL) == DL_OK);
    CHECK(dl_manager_step());
    CHECK(status_of(id).rec.state == DL_COMPLETED);
}

static void test_library(void) {
    begin("offline library: only complete, verified media");
    make_completed("lib_a", "Alpha", true, 1000);
    make_completed("lib_b", "Beta", true, 1200);
    // Everything that is NOT complete: queued, paused, failed, cancelled,
    // mid-download (active), and a partial left by a drop.
    DlMeta q = meta_for("lib_q", "Queued"), p = meta_for("lib_p", "Paused"),
           fl = meta_for("lib_f", "Failed"), c = meta_for("lib_c", "Cancelled"),
           pr = meta_for("lib_partial", "Partial");
    CHECK(dl_enqueue(&pr, URL, 0) == DL_OK);
    FakeResp drop; drop.drop_after = 50000;
    g_fake_queue.push_back(drop);
    CHECK(dl_manager_step());                             // lib_partial: part file only
    CHECK(dl_enqueue(&p, URL, 0) == DL_OK);
    CHECK(dl_pause("lib_p") == DL_OK);
    CHECK(dl_enqueue(&c, URL, 0) == DL_OK);
    CHECK(dl_cancel("lib_c") == DL_OK);
    FakeResp nf; nf.status = 404;
    g_fake_queue.push_back(nf);
    CHECK(dl_enqueue(&fl, URL, 0) == DL_OK);
    CHECK(dl_retry("lib_partial") == DL_E_STATE);         // (queued, not failed)
    g_fake_now_ms += 400000;
    // lib_partial is due first; make it fail again so lib_f gets its 404.
    FakeResp drop2; drop2.drop_after = 10;
    g_fake_queue.insert(g_fake_queue.begin(), drop2);
    CHECK(dl_manager_step());
    CHECK(dl_manager_step());
    CHECK(status_of("lib_f").rec.state == DL_FAILED);
    CHECK(dl_enqueue(&q, URL, 0) == DL_OK);

    char ids[16][DL_ID_MAX];
    int n = dl_library_ids(ids, 16);
    CHECK(n == 2 && !strcmp(ids[0], "lib_a") && !strcmp(ids[1], "lib_b"));   // download order
    CHECK(dl_library_ids(ids, 1) == 1);                  // honours max
    const char *not_playable[] = { "lib_q", "lib_p", "lib_f", "lib_c", "lib_partial", "nope", "../x" };
    for (const char *id : not_playable) {
        DlLibraryEntry e;
        CHECK(!dl_library_has(id));
        CHECK(!dl_library_get(id, &e));
    }
    CHECK(!dl_library_has(NULL));
    DlLibraryEntry e;
    CHECK(dl_library_get("lib_a", &e));
    CHECK(e.meta_ok && !strcmp(e.meta.title, "Alpha"));
    CHECK(e.bytes == 188ull * 1000 && e.bytes % 188 == 0);
    CHECK(strstr(e.media_path, "/items/lib_a/media.ts") != NULL);
    CHECK(fake_file_matches(e.media_path, 0) == false);   // it is a TS, not the pattern
    CHECK(e.poster_path[0] == '\0' && e.backdrop_path[0] == '\0');   // no artwork

    s_test = "offline library: artwork only when the file is there"; printf("- %s\n", s_test);
    std::string j = fake_jpeg(500);
    CHECK(dl_store_save_blob("lib_a", DL_FILE_POSTER, (const uint8_t *)j.data(), (int)j.size()));
    CHECK(dl_library_get("lib_a", &e));
    CHECK(strstr(e.poster_path, "/items/lib_a/poster.jpg") != NULL && e.backdrop_path[0] == '\0');
    CHECK(remove(e.poster_path) == 0);
    CHECK(dl_library_get("lib_a", &e) && e.poster_path[0] == '\0');   // deleted since

    s_test = "offline library: media that changed under a running app"; printf("- %s\n", s_test);
    CHECK(dl_library_get("lib_b", &e));
    std::string media_b = e.media_path;
    CHECK(truncate(media_b.c_str(), 188 * 1000) == 0);   // shorter than verified
    CHECK(!dl_library_has("lib_b") && !dl_library_get("lib_b", &e));
    n = dl_library_ids(ids, 16);
    CHECK(n == 1 && !strcmp(ids[0], "lib_a"));
    CHECK(remove(media_b.c_str()) == 0);                  // gone entirely
    CHECK(!dl_library_has("lib_b"));
    // A torn TS (not whole packets) at the recorded size is refused too.
    DlRecord r;
    CHECK(dl_store_load_record("lib_a", &r));
    CHECK(dl_library_get("lib_a", &e));
    FILE *f = fopen(e.media_path, "ab"); fputs("xyz", f); fclose(f);
    CHECK(!dl_library_has("lib_a"));
    CHECK(truncate(e.media_path, 188 * 1000) == 0);
    CHECK(dl_library_has("lib_a"));

    // A TS whose verified size is not whole packets is never playable, even
    // when the file matches the record byte for byte.
    CHECK(dl_store_load_record("lib_a", &r));
    {
        FILE *g = fopen(e.media_path, "ab"); fputs("xyz", g); fclose(g);
        r.bytes_total += 3;
        CHECK(dl_store_save_record(&r));
        restart_app();
        CHECK(status_of("lib_a").rec.state == DL_COMPLETED);   // record and file agree...
        CHECK(!dl_library_has("lib_a"));                       // ...but it is not a TS
        CHECK(truncate(e.media_path, 188 * 1000) == 0);
        r.bytes_total -= 3;
        CHECK(dl_store_save_record(&r));
        restart_app();
        CHECK(dl_library_has("lib_a"));
    }
    // The manager's completed-id query lists completed items only.
    {
        char cids[16][DL_ID_MAX];
        int nc = dl_completed_ids(cids, 16);
        for (int i = 0; i < nc; i++) CHECK(status_of(cids[i]).rec.state == DL_COMPLETED);
        CHECK(nc == 1 && !strcmp(cids[0], "lib_a"));          // lib_b failed at restore
        CHECK(dl_completed_ids(cids, 0) == 0);
    }

    s_test = "offline library: stale / missing metadata"; printf("- %s\n", s_test);
    write_file(item_file("lib_a", DL_FILE_META), "jfdl-meta 1\nid=lib_a\ntitle=Alpha");   // torn
    CHECK(dl_library_get("lib_a", &e));
    CHECK(!e.meta_ok && !strcmp(e.meta.title, "Alpha") && !strcmp(e.meta.id, "lib_a"));
    CHECK(!strcmp(e.meta.container, "ts"));                // from the verified record
    CHECK(e.meta.season == -1 && e.meta.runtime_secs == 0);
    write_file(item_file("lib_a", DL_FILE_META), "jfdl-meta 1\nid=lib_b\ntitle=Wrong\nend\n");
    CHECK(dl_library_get("lib_a", &e) && !e.meta_ok && !strcmp(e.meta.title, "Alpha"));
    CHECK(remove(item_file("lib_a", DL_FILE_META).c_str()) == 0);
    CHECK(dl_library_get("lib_a", &e) && !e.meta_ok);

    s_test = "offline library: survives a restart, stays consistent"; printf("- %s\n", s_test);
    restart_app();
    n = dl_library_ids(ids, 16);
    CHECK(n == 1 && !strcmp(ids[0], "lib_a"));
    CHECK(status_of("lib_b").rec.state == DL_FAILED);      // restore saw it missing
    CHECK(status_of("lib_b").rec.error == DL_ERR_CORRUPT);
    CHECK(status_of("lib_partial").rec.state == DL_QUEUED);
    CHECK(!dl_library_has("lib_partial"));

    s_test = "offline library: before restore"; printf("- %s\n", s_test);
    dl_manager_shutdown();
    CHECK(dl_library_ids(ids, 16) == 0 && !dl_library_has("lib_a"));
    CHECK(dl_manager_init(s_root.c_str(), &s_cfg));
    CHECK(dl_library_has("lib_a"));
}

static void test_local_plan(void) {
    s_test = "local playback plan"; printf("- %s\n", s_test);
    DlLibraryEntry e;
    memset(&e, 0, sizeof(e));
    dl_meta_init(&e.meta);
    e.meta_ok = true;
    e.meta.width = 1280; e.meta.height = 720; e.meta.runtime_secs = 2820;
    e.meta.video_bitrate = 4000000;
    snprintf(e.meta.audio_codec, sizeof(e.meta.audio_codec), "ac3");
    DlLocalPlan p;
    dl_library_plan(&e, &p);
    // The file's frame size, whatever the quality setting says now.
    CHECK(p.req_w == 1280 && p.req_h == 720 && p.runtime_secs == 2820);
    CHECK(!p.light);                                    // 720p: heavy
    e.meta.width = 854; e.meta.height = 480; e.meta.video_bitrate = 1500000;
    dl_library_plan(&e, &p);
    CHECK(p.light && p.req_h == 480);
    snprintf(e.meta.audio_codec, sizeof(e.meta.audio_codec), "truehd");
    CHECK(!dl_meta_is_light(&e.meta, true));             // HD audio: heavy
    snprintf(e.meta.audio_codec, sizeof(e.meta.audio_codec), "mp3");
    CHECK(dl_meta_is_light(&e.meta, true));
    e.meta.video_bitrate = 0;                            // copied at source rate
    CHECK(!dl_meta_is_light(&e.meta, true));
    e.meta.video_bitrate = 700000; e.meta.height = 0;
    CHECK(!dl_meta_is_light(&e.meta, true));             // unknown size: heavy
    e.meta.height = 360; e.meta.audio_codec[0] = '\0';
    CHECK(!dl_meta_is_light(&e.meta, true));             // unknown audio: heavy
    snprintf(e.meta.audio_codec, sizeof(e.meta.audio_codec), "mp3");
    CHECK(dl_meta_is_light(&e.meta, true));
    CHECK(!dl_meta_is_light(&e.meta, false));            // stale meta: heavy
    // Stale or out-of-range dimensions: the largest ceiling, never too small.
    e.meta_ok = false;
    dl_library_plan(&e, &p);
    CHECK(p.req_w == 1920 && p.req_h == 1080 && p.runtime_secs == 0 && !p.light);
    e.meta_ok = true; e.meta.width = 3840; e.meta.height = 2160;
    dl_library_plan(&e, &p);
    CHECK(p.req_w == 1920 && p.req_h == 1080);
    // The shared thresholds: a request's metadata and its URL agree.
    JFItem it = fixture_item();
    JFMediaSource src = fixture_source(4);   // AAC default track
    for (int q = 0; q < VQ_COUNT; q++) {
        DlRequest dr;
        CHECK(build_request(it, NULL, &src, prefs_of(q, false, 0), &dr));
        CHECK(dl_meta_is_light(&dr.meta, true) == dl_stream_is_light(dr.url));
        CHECK(build_request(it, NULL, &src, prefs_of(q, true, 1), &dr));
        CHECK(dl_meta_is_light(&dr.meta, true) == dl_stream_is_light(dr.url));
    }
    JFMediaSource thd = fixture_source(1);
    DlRequest dr;
    CHECK(build_request(it, NULL, &thd, prefs_of(VQ_480P, false, 2), &dr));
    CHECK(!dl_meta_is_light(&dr.meta, true) && !dl_stream_is_light(dr.url));
}

static void test_local_playback_gate(void) {
    begin("local playback gates downloads by the file's weight");
    DlMeta m = meta_for("lg");
    CHECK(dl_enqueue(&m, URL, 0) == DL_OK);
    dl_playback_begin_local(false);                    // a heavy file playing
    CHECK(dl_playback_blocking());
    CHECK(!dl_manager_step() && g_fake_connects == 0);
    dl_playback_end();
    s_cfg.stream_share_bps = 800000;
    restart_app();
    dl_playback_begin_local(true);                     // a 480p file playing
    CHECK(!dl_playback_blocking());
    uint64_t t0 = g_fake_now_ms;
    CHECK(dl_manager_step());
    CHECK(status_of("lg").rec.state == DL_COMPLETED);
    CHECK(g_fake_now_ms - t0 >= 2400);                 // paced (300 KB at 100 KB/s)
    dl_playback_end();
    // A heavy file starting mid-download parks it, like a heavy stream.
    DlMeta m2 = meta_for("lg2");
    CHECK(dl_enqueue(&m2, URL, 0) == DL_OK);
    g_fake_default.on_body = [](int64_t sent) { if (sent > 50000) dl_playback_begin_local(false); };
    CHECK(dl_manager_step());
    CHECK(status_of("lg2").rec.state == DL_QUEUED && status_of("lg2").rec.error == DL_ERR_NONE);
    g_fake_default.on_body = nullptr;
    dl_playback_end();
    CHECK(dl_manager_step() && status_of("lg2").rec.state == DL_COMPLETED);
}

static void test_offline_startup_path(void) {
    begin("offline startup path: library ready without server or login");
    const std::string root = s_root;
    make_completed("boot_a", "Boot A", true, 800);
    dl_manager_shutdown();
    dl_set_auth_header("");                            // not logged in
    g_fake_default.refuse = true;                      // no server either
    g_fake_connects = 0;
    const char *roots[] = { root.c_str() };
    CHECK(dl_svc_start(roots, 1));
    CHECK(!dl_svc_restored());                         // not until the worker has run
    char ids[4][DL_ID_MAX];
    CHECK(dl_library_ids(ids, 4) == 0);                // "not restored", not "empty"
    CHECK(dl_svc_tick() == 0);
    CHECK(dl_svc_restored());
    CHECK(dl_library_ids(ids, 4) == 1 && !strcmp(ids[0], "boot_a"));
    DlLibraryEntry e;
    CHECK(dl_library_get("boot_a", &e));
    CHECK(g_fake_connects == 0);                       // no network anywhere
    dl_svc_stop();
    CHECK(!dl_svc_restored());                         // a stopped service is not "restored"
    const char *bad[] = { "/proc/nope/offline" };
    CHECK(dl_svc_start(bad, 1));
    CHECK(dl_svc_tick() == 0);
    CHECK(dl_svc_restored() && !dl_manager_ready());   // restored: nothing to show
    CHECK(dl_library_ids(ids, 4) == 0);
    dl_svc_stop();
    CHECK(dl_manager_init(s_root.c_str(), &s_cfg));
}


// =========================================================================
// Stage 5: what the screens say and do
// =========================================================================

static DlStatus st_of(DlState state, uint64_t done = 0, uint64_t total = 0,
                      uint32_t retry_ms = 0, DlError err = DL_ERR_NONE,
                      const char *url = "http://h/x") {
    DlStatus st;
    memset(&st, 0, sizeof(st));
    dl_record_init(&st.rec);
    snprintf(st.rec.id, sizeof(st.rec.id), "abc");
    snprintf(st.rec.title, sizeof(st.rec.title), "A Film");
    snprintf(st.rec.url, sizeof(st.rec.url), "%s", url);
    st.rec.state = state;
    st.rec.bytes_done = done;
    st.rec.bytes_total = total;
    st.rec.error = err;
    st.retry_in_ms = retry_ms;
    st.active = state == DL_DOWNLOADING;
    return st;
}

static std::string label_of(const DlStatus *st, bool can, DlUiContext cx) {
    char b[64];
    dl_ui_item_label(st, can, &cx, b, sizeof(b));
    return b;
}

static void test_ui_item_button(void) {
    s_test = "item page button"; printf("- %s\n", s_test);
    DlUiContext ok = { false, false, true };
    // Not in the list.
    CHECK(dl_ui_item_action(NULL, true, &ok) == DL_UI_START);
    CHECK(label_of(NULL, true, ok) == "Download");
    CHECK(dl_ui_item_action(NULL, false, &ok) == DL_UI_NONE);   // no version loaded
    CHECK(label_of(NULL, false, ok) == "Can't download");
    // Service not usable: nothing is offered, whatever the state.
    DlUiContext down = { false, false, false };
    DlStatus done = st_of(DL_COMPLETED, 900, 900);
    CHECK(dl_ui_item_action(NULL, true, &down) == DL_UI_NONE);
    CHECK(dl_ui_item_action(&done, true, &down) == DL_UI_NONE);
    CHECK(label_of(NULL, true, down) == "Downloads unavailable");
    // Each state.
    DlStatus q = st_of(DL_QUEUED);
    CHECK(dl_ui_item_action(&q, true, &ok) == DL_UI_PAUSE && label_of(&q, true, ok) == "Queued");
    DlUiContext held = { false, true, true }, streaming = { true, false, true };
    CHECK(label_of(&q, true, held) == "Queued (sign in)");
    CHECK(label_of(&q, true, streaming) == "Queued (streaming)");
    DlStatus qr = st_of(DL_QUEUED, 0, 0, 4000, DL_ERR_UNREACHABLE);
    CHECK(label_of(&qr, true, ok) == "Retrying...");
    DlStatus d = st_of(DL_DOWNLOADING, 820, 1000);
    CHECK(dl_ui_item_action(&d, true, &ok) == DL_UI_PAUSE);
    CHECK(label_of(&d, true, ok) == "Downloading 82%");
    DlStatus dt = st_of(DL_DOWNLOADING, 5000, 0);            // a transcode: no total
    CHECK(label_of(&dt, true, ok) == "Downloading");
    DlStatus p = st_of(DL_PAUSED, 420, 1000);
    CHECK(dl_ui_item_action(&p, true, &ok) == DL_UI_RESUME && label_of(&p, true, ok) == "Paused 42%");
    DlStatus p0 = st_of(DL_PAUSED);
    CHECK(label_of(&p0, true, ok) == "Paused");
    CHECK(dl_ui_item_action(&done, true, &ok) == DL_UI_PLAY_OFFLINE);
    CHECK(dl_ui_item_action(&done, false, &ok) == DL_UI_PLAY_OFFLINE);   // needs no version
    CHECK(label_of(&done, true, ok) == "Play offline");
    DlStatus f = st_of(DL_FAILED, 0, 0, 0, DL_ERR_NO_SPACE), c = st_of(DL_CANCELLED);
    CHECK(dl_ui_item_action(&f, true, &ok) == DL_UI_RETRY && label_of(&f, true, ok) == "Retry download");
    CHECK(dl_ui_item_action(&c, true, &ok) == DL_UI_RETRY);
    CHECK(dl_ui_item_action(&f, false, &ok) == DL_UI_NONE && label_of(&f, false, ok) == "Can't download");
    // Every request error has words; OK has none.
    CHECK(dl_ui_result_text(DL_OK)[0] == '\0');
    for (int r = DL_E_NOT_READY; r <= DL_E_STATE; r++) CHECK(dl_ui_result_text(r)[0] != '\0');
    CHECK(!strcmp(dl_ui_result_text(DL_E_NO_SPACE), "Not enough HDD space"));
    CHECK(dl_ui_result_text(999)[0] != '\0');
}

static DlUiRow row_of(const DlStatus &st, DlUiContext cx = { false, false, true }) {
    DlUiRow r;
    dl_ui_row(&st, &cx, &r);
    return r;
}

static void test_ui_rows(void) {
    s_test = "Downloads list rows"; printf("- %s\n", s_test);
    const uint64_t GB = 1024ull * 1024 * 1024, MB = 1024ull * 1024;
    DlUiRow r = row_of(st_of(DL_DOWNLOADING, 1288490188ull, 3650722201ull));
    CHECK(!strcmp(r.status, "Downloading") && r.emphasis && !r.warning);
    CHECK(!strcmp(r.size, "1.1 GB of 3.3 GB"));
    CHECK(r.permille == 352);
    r = row_of(st_of(DL_DOWNLOADING, 340 * MB, 0));             // transcode
    CHECK(r.permille == -1 && !strcmp(r.size, "340 MB"));        // no bar, no total
    r = row_of(st_of(DL_QUEUED));
    CHECK(!strcmp(r.status, "Waiting...") && r.permille == -1 && r.size[0] == '\0');
    r = row_of(st_of(DL_QUEUED, 0, 0, 7200, DL_ERR_UNREACHABLE));
    CHECK(!strcmp(r.status, "Retrying in 8s -- Server unreachable") && r.warning);
    r = row_of(st_of(DL_QUEUED, 100, 1000), { false, true, true });
    CHECK(!strcmp(r.status, "Waiting for sign-in") && r.warning && r.permille == 100);
    r = row_of(st_of(DL_QUEUED, 100, 1000), { true, false, true });
    CHECK(!strcmp(r.status, "Paused while streaming") && !r.warning);
    r = row_of(st_of(DL_PAUSED, 2 * GB, 4 * GB));
    CHECK(!strcmp(r.status, "Paused") && r.permille == 500 && !strcmp(r.size, "2.0 GB of 4.0 GB"));
    r = row_of(st_of(DL_COMPLETED, 18 * GB, 18 * GB));
    CHECK(!strcmp(r.status, "Downloaded") && !strcmp(r.size, "18 GB") && r.permille == -1);
    r = row_of(st_of(DL_FAILED, 0, 0, 0, DL_ERR_BAD_MEDIA));
    CHECK(!strcmp(r.status, "Failed -- Server sent an incomplete video") && r.warning);
    r = row_of(st_of(DL_CANCELLED, 5, 10));
    CHECK(!strcmp(r.status, "Cancelled") && r.size[0] == '\0');
    DlStatus untitled = st_of(DL_QUEUED);
    untitled.rec.title[0] = '\0';
    r = row_of(untitled);
    CHECK(!strcmp(r.title, "abc"));                              // falls back to the id
    // Every error the manager can record reads as words in a failed row.
    for (int e = 1; e < DL_ERR_COUNT; e++) {
        r = row_of(st_of(DL_FAILED, 0, 0, 0, (DlError)e));
        CHECK(strlen(r.status) > strlen("Failed -- "));
    }

    s_test = "Downloads list actions"; printf("- %s\n", s_test);
    struct { DlState s; DlUiAction p, q; } t[] = {
        { DL_QUEUED,      DL_UI_PAUSE,        DL_UI_CANCEL },
        { DL_DOWNLOADING, DL_UI_PAUSE,        DL_UI_CANCEL },
        { DL_PAUSED,      DL_UI_RESUME,       DL_UI_CANCEL },
        { DL_COMPLETED,   DL_UI_PLAY_OFFLINE, DL_UI_REMOVE },
        { DL_FAILED,      DL_UI_RETRY,        DL_UI_REMOVE },
        { DL_CANCELLED,   DL_UI_RETRY,        DL_UI_REMOVE },
    };
    for (auto &x : t) {
        DlStatus st = st_of(x.s);
        CHECK(dl_ui_row_primary(&st) == x.p && dl_ui_row_secondary(&st) == x.q);
        CHECK(dl_ui_action_label(x.p)[0] && dl_ui_action_label(x.q)[0]);
    }
    DlStatus corrupt = st_of(DL_FAILED, 0, 0, 0, DL_ERR_CORRUPT, "");
    CHECK(dl_ui_row_primary(&corrupt) == DL_UI_NONE);           // no request to repeat
    CHECK(dl_ui_action_label(DL_UI_NONE)[0] == '\0');
    // Asking first: deleting a finished film, or cancelling with data in hand.
    DlStatus fin = st_of(DL_COMPLETED, 10, 10), half = st_of(DL_DOWNLOADING, 10, 20),
             empty = st_of(DL_QUEUED), failed = st_of(DL_FAILED);
    CHECK(dl_ui_action_needs_confirm(DL_UI_REMOVE, &fin));
    CHECK(!dl_ui_action_needs_confirm(DL_UI_REMOVE, &failed));
    CHECK(dl_ui_action_needs_confirm(DL_UI_CANCEL, &half));
    CHECK(!dl_ui_action_needs_confirm(DL_UI_CANCEL, &empty));
    CHECK(!dl_ui_action_needs_confirm(DL_UI_PAUSE, &half));
    // Banner priority: unusable > signed out > streaming > nothing.
    DlUiContext b1 = { true, true, false }, b2 = { true, true, true }, b3 = { true, false, true },
                b4 = { false, false, true };
    CHECK(strstr(dl_ui_queue_banner(&b1), "unavailable") != NULL);
    CHECK(strstr(dl_ui_queue_banner(&b2), "Sign in") != NULL);
    CHECK(strstr(dl_ui_queue_banner(&b3), "stream") != NULL);
    CHECK(dl_ui_queue_banner(&b4)[0] == '\0');
}

static void test_ui_offline_and_helpers(void) {
    s_test = "Offline library lines"; printf("- %s\n", s_test);
    DlMeta m;
    dl_meta_init(&m);
    snprintf(m.id, sizeof(m.id), "ep1");
    snprintf(m.title, sizeof(m.title), "Pilot");
    snprintf(m.series, sizeof(m.series), "Some Show");
    m.season = 1; m.episode = 3; m.runtime_secs = 2820;
    char t[128], sub[160];
    dl_ui_offline_lines(&m, true, 1288490188ull, t, sizeof(t), sub, sizeof(sub));
    CHECK(!strcmp(t, "Pilot"));
    CHECK(!strcmp(sub, "Some Show  S1 E3  \xC2\xB7  47 min  \xC2\xB7  1.1 GB"));   // UTF-8 middle dot
    DlMeta mv;
    dl_meta_init(&mv);
    snprintf(mv.id, sizeof(mv.id), "mv1");
    snprintf(mv.title, sizeof(mv.title), "A Film");
    mv.year = 1999; mv.runtime_secs = 8100;
    dl_ui_offline_lines(&mv, true, 812 * 1024, t, sizeof(t), sub, sizeof(sub));
    CHECK(!strcmp(sub, "1999  \xC2\xB7  2h 15m  \xC2\xB7  812 KB"));
    // Stale metadata: title from the record, only what is certain.
    dl_ui_offline_lines(&m, false, 1024, t, sizeof(t), sub, sizeof(sub));
    CHECK(!strcmp(t, "Pilot") && !strcmp(sub, "1 KB"));
    m.title[0] = '\0';
    dl_ui_offline_lines(&m, false, 0, t, sizeof(t), sub, sizeof(sub));
    CHECK(!strcmp(t, "ep1"));
    m.season = -1;
    dl_ui_offline_lines(&m, true, 0, t, sizeof(t), sub, sizeof(sub));
    CHECK(strncmp(sub, "Some Show  \xC2\xB7", 13) == 0);             // no S-1 E3

    s_test = "byte formatting"; printf("- %s\n", s_test);
    char b[24];
    struct { uint64_t v; const char *s; } fb[] = {
        { 0, "0 KB" }, { 1, "1 KB" }, { 1024, "1 KB" }, { 1025, "2 KB" },
        { 1024ull * 1024 - 1, "1024 KB" }, { 1024ull * 1024, "1 MB" },
        { 1024ull * 1024 * 1024 - 1, "1023 MB" }, { 1024ull * 1024 * 1024, "1.0 GB" },
        { 1610612736ull, "1.5 GB" }, { 10ull << 30, "10 GB" }, { 5000ull << 30, "5000 GB" },
    };
    for (auto &x : fb) {
        dl_ui_format_bytes(x.v, b, sizeof(b));
        if (strcmp(b, x.s)) printf("    %llu -> %s (want %s)\n", (unsigned long long)x.v, b, x.s);
        CHECK(!strcmp(b, x.s));
    }

    s_test = "list selection window"; printf("- %s\n", s_test);
    int top = 0;
    CHECK(dl_ui_clamp_selection(5, 0, 6, &top) == 0 && top == 0);          // empty
    CHECK(dl_ui_clamp_selection(-3, 10, 6, &top) == 0 && top == 0);
    CHECK(dl_ui_clamp_selection(7, 10, 6, &top) == 7 && top == 2);         // scroll down
    CHECK(dl_ui_clamp_selection(1, 10, 6, &top) == 1 && top == 1);         // back up
    top = 9;
    CHECK(dl_ui_clamp_selection(9, 3, 6, &top) == 2 && top == 0);          // list shrank
    top = 0;
    CHECK(dl_ui_clamp_selection(4, 5, 0, &top) == 4 && top == 4);          // tiny window
}

static void test_ui_flow(void) {
    begin("item page -> Downloads -> Offline, through the real manager");
    // The item page's DOWNLOAD, exactly as dl_download_item builds it.
    DlUiContext cx = { false, false, true };
    DlStatus st;
    CHECK(!dl_find(FX, &st));
    CHECK(dl_ui_item_action(NULL, true, &cx) == DL_UI_START);
    g_fake_default = ts_resp(3000, 600);
    DlRequest dr;
    CHECK(enqueue_fixture(&dr, VQ_720P, 600) == DL_OK);
    CHECK(dl_find(FX, &st) && label_of(&st, true, cx) == "Queued");
    // Pressed again while queued: Pause (not a second copy).
    CHECK(dl_ui_item_action(&st, true, &cx) == DL_UI_PAUSE);
    CHECK(dl_pause(FX) == DL_OK);
    CHECK(dl_find(FX, &st) && dl_ui_item_action(&st, true, &cx) == DL_UI_RESUME);
    CHECK(dl_resume(FX) == DL_OK);
    // Mid-transfer, the list row shows live progress.
    static bool checked;
    checked = false;
    g_fake_default.on_body = [](int64_t sent) {
        if (checked || sent < 200000) return;
        checked = true;
        DlStatus s2;
        DlUiContext c2 = { false, false, true };
        CHECK(dl_find(FX, &s2) && s2.rec.state == DL_DOWNLOADING);
        CHECK(label_of(&s2, true, c2).rfind("Downloading", 0) == 0);
        DlUiRow r;
        dl_ui_row(&s2, &c2, &r);
        CHECK(r.emphasis && !strcmp(r.status, "Downloading"));
    };
    CHECK(dl_manager_step());
    CHECK(checked);
    CHECK(dl_find(FX, &st) && st.rec.state == DL_COMPLETED);
    CHECK(label_of(&st, true, cx) == "Play offline");
    CHECK(dl_ui_row_primary(&st) == DL_UI_PLAY_OFFLINE);
    // ...and the Offline library lists it with its lines.
    char ids[4][DL_ID_MAX];
    CHECK(dl_library_ids(ids, 4) == 1 && !strcmp(ids[0], FX));
    DlLibraryEntry e;
    CHECK(dl_library_get(FX, &e));
    char t[128], sub[160];
    dl_ui_offline_lines(&e.meta, e.meta_ok, e.bytes, t, sizeof(t), sub, sizeof(sub));
    CHECK(!strcmp(t, "Pilot") && strstr(sub, "Some Show  S1 E3") != NULL);
    // Settings tallies.
    int a = -1, c = -1, f = -1;
    dl_counts(&a, &c, &f);
    CHECK(a == 0 && c == 1 && f == 0);
    DlMeta q = meta_for("queued1");
    CHECK(dl_enqueue(&q, URL, 0) == DL_OK);
    FakeResp nf; nf.status = 404;
    g_fake_queue.push_back(nf);
    DlMeta bad = meta_for("bad1");
    CHECK(dl_enqueue(&bad, URL, 0) == DL_OK);
    CHECK(dl_pause("queued1") == DL_OK);
    CHECK(dl_manager_step());                               // bad1 -> 404 -> failed
    dl_counts(&a, &c, &f);
    CHECK(a == 1 && c == 1 && f == 1);
    dl_counts(NULL, NULL, NULL);                            // NULLs are fine
    char all[8][DL_ID_MAX];
    CHECK(dl_ids(all, 8) == 3 && !strcmp(all[0], FX) && !strcmp(all[1], "queued1") &&
          !strcmp(all[2], "bad1"));                         // queue order
    CHECK(dl_ids(all, 2) == 2);
    // Delete from Offline: asks first, then it is gone everywhere.
    CHECK(dl_ui_action_needs_confirm(DL_UI_REMOVE, &st));
    CHECK(dl_remove(FX) == DL_OK);
    CHECK(dl_library_ids(ids, 4) == 0 && !dl_find(FX, &st));
    CHECK(dl_ui_item_action(NULL, true, &cx) == DL_UI_START);   // Download again
    dl_manager_shutdown();
    dl_counts(&a, &c, &f);
    CHECK(a == 0 && c == 0 && f == 0 && dl_ids(all, 8) == 0);
    CHECK(dl_manager_init(s_root.c_str(), &s_cfg));
}

int main(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "-v") == 0) g_fake_verbose = true;

    test_state_machine();
    test_error_policy();
    test_progress_math();
    test_id_validation();
    test_record_roundtrip();
    test_record_malformed();
    test_meta_roundtrip();
    test_meta_malformed();
    test_url_parse();
    test_request_build();
    test_head_parse();
    test_head_reader();
    test_chunked();

    test_enqueue_and_complete();
    test_queue_order_and_duplicates();
    test_progress_checkpoints();
    test_interrupted_resume();
    test_reset_and_timeout();
    test_head_timeout();
    test_range_ignored();
    test_range_inconsistent();
    test_416_already_complete();
    test_chunked_transcode();
    test_pause_resume();
    test_cancel();
    test_failure_retry();
    test_invalid_responses();
    test_unreachable();
    test_suspend();
    test_deletion();
    test_storage_limits();
    test_space_estimates_and_text();
    test_season();
    test_restore_interrupted();
    test_restore_malformed();
    test_offline_startup();
    test_stream_classifier();
    test_playback_heavy_stops();
    test_playback_light_paced();
    test_write_batching();

    // Stage 3
    test_download_matches_playback();
    test_download_dolby_digital();
    test_download_url_shape();
    test_request_gating();
    test_item_identity();
    test_request_metadata();
    test_artwork();
    test_ts_scan();
    test_ts_validation();
    test_request_heavy_playback();
    test_auth_hold();
    test_service_lifecycle();

    // Stage 4
    test_local_index();
    test_local_seek();
    test_local_seek_edges();
    test_library();
    test_local_plan();
    test_local_playback_gate();
    test_offline_startup_path();

    // Stage 5
    test_ui_item_button();
    test_ui_rows();
    test_ui_offline_and_helpers();
    test_ui_flow();

    if (!s_tmp.empty()) fake_rmtree(s_tmp);
    printf("offline downloads: %d checks, %d failed\n", s_checks, s_failed);
    return s_failed ? 1 : 0;
}
