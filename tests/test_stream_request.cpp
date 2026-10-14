// Host test for the stream request decision (source/player/stream/
// stream_request.cpp): the one place a stream.ts URL is built, for playback
// and for downloads.
//
// The goldens are byte-exact URLs.  Parameter order is part of the contract:
// the server treats the query as a set, but a reordering is the first sign
// that a decision changed.  Each case's inputs are in stream_golden_cases.h,
// its expected URL in stream_golden_urls.inc.
//
//   make -f Makefile.host test_stream_request && ./test_stream_request

#include "stream_request.h"
#include "stream_golden_cases.h"

#include <stdio.h>
#include <string.h>
#include <string>

static int s_checks = 0, s_failed = 0;
#define CHECK(cond) do { s_checks++; if (!(cond)) { s_failed++; \
    printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

static const char *EXPECT[] = {
#include "stream_golden_urls.inc"
};

static const char *SERVER = "http://server:8096";
static const char *DEVICE = "DEVID123";

static void fill_tracks(JFTracks *t) {
    memset(t, 0, sizeof *t);
    struct { int idx; const char *label; unsigned br; } a[] = {
        {1, "English - Dolby Digital - 5.1 - Default", 640000},
        {2, "English - Dolby TrueHD - 7.1 - Atmos", 0},
        {3, "English - DTS-HD MA - 5.1", 1509000},
        {4, "English - Dolby Digital Plus - 5.1", 768000},
        {5, "English - AAC - Stereo", 192000},
    };
    for (int i = 0; i < 5; i++) {
        t->audio[i].index = a[i].idx;
        snprintf(t->audio[i].label, sizeof t->audio[i].label, "%s", a[i].label);
        t->audio[i].bitrate = a[i].br;
    }
    t->n_audio = 5;
    t->default_audio = 0;
    struct { int idx; const char *label; const char *codec; } s[] = {
        {6, "English - SRT", "subrip"},
        {7, "English - PGS", "pgssub"},
        {8, "English - VobSub", "dvdsub"},
    };
    for (int i = 0; i < 3; i++) {
        t->subs[i].index = s[i].idx;
        snprintf(t->subs[i].label, sizeof t->subs[i].label, "%s", s[i].label);
        snprintf(t->subs[i].codec, sizeof t->subs[i].codec, "%s", s[i].codec);
    }
    t->n_subs = 3;
}

struct Built { StreamPrefs p; StreamSelection sel; JFMediaSource src; JFTracks tracks; };

static void build_case(const GoldenCase *c, Built *b) {
    memset(b, 0, sizeof *b);
    b->p.quality     = (vquality_t)c->quality;
    b->p.hd1080      = c->hd != 0;
    b->p.surround    = c->surround != 0;
    b->p.surround_hd = c->surround_hd != 0;
    b->p.display_w   = c->disp_w;
    b->p.display_h   = c->disp_h;
    b->p.budget      = true;
    fill_tracks(&b->tracks);
    b->sel.item_id   = "ITEM0001";
    b->sel.tracks    = &b->tracks;
    b->sel.cur_audio = c->audio;
    b->sel.cur_sub   = c->sub;
    b->sel.sub_on_console = (c->sub == GS_TEXT || c->sub == GS_PGS);
    if (c->version != GV_NOSRC) {
        fill_tracks(&b->src.tracks);
        snprintf(b->src.id, sizeof b->src.id, "%s",
                 c->version == GV_ALT ? "VERSION0002" : "ITEM0001");
        if (c->version == GV_LIVE)
            snprintf(b->src.live_stream_id, sizeof b->src.live_stream_id, "live stream/1");
        b->sel.source = &b->src;
    }
}

static void golden(void) {
    printf("- golden URLs\n");
    CHECK(GOLDEN_N == (int)(sizeof EXPECT / sizeof EXPECT[0]));
    for (int i = 0; i < GOLDEN_N && i < (int)(sizeof EXPECT / sizeof EXPECT[0]); i++) {
        Built b; build_case(&GOLDEN[i], &b);
        StreamRequest rq;
        stream_request_resolve(&b.p, &b.sel, &rq);
        char url[1024];
        stream_url_build(url, sizeof url, &rq, SERVER, DEVICE,
                         GOLDEN[i].session ? "sess1234" : "", GOLDEN[i].start);
        if (strcmp(url, EXPECT[i]) != 0)
            printf("  %s:\n    got  %s\n    want %s\n", GOLDEN[i].name, url, EXPECT[i]);
        CHECK(strcmp(url, EXPECT[i]) == 0);
    }
}

// What the budget does, stated as properties rather than as URLs.
static void budget(void) {
    printf("- budget\n");
    for (int i = 0; i < GOLDEN_N; i++) {
        Built b; build_case(&GOLDEN[i], &b);
        StreamRequest rq;
        stream_request_resolve(&b.p, &b.sel, &rq);
        // Budgeted exactly when there is a step to take the audio out of.
        CHECK(rq.budgeted == (rq.vbitrate != 0));
        if (rq.budgeted) CHECK(rq.vreq <= rq.vbitrate && rq.vreq >= rq.vbitrate / 2);
        else             CHECK(rq.vreq == 0);

        // With the budget switched off the request carries the step as it is.
        b.p.budget = false;
        StreamRequest raw;
        stream_request_resolve(&b.p, &b.sel, &raw);
        CHECK(!raw.budgeted && raw.vreq == raw.vbitrate);
    }
}

static void subtitles(void) {
    printf("- subtitles\n");
    Built b; build_case(&GOLDEN[0], &b);
    StreamRequest rq;
    b.sel.cur_sub = 2; b.sel.sub_on_console = false;      // a bitmap track: burned in
    stream_request_resolve(&b.p, &b.sel, &rq);
    CHECK(rq.sub_idx == 8);
    b.sel.cur_sub = 0; b.sel.sub_on_console = true;       // drawn here: not requested
    stream_request_resolve(&b.p, &b.sel, &rq);
    CHECK(rq.sub_idx == -1);
    b.sel.cur_sub = 9; b.sel.sub_on_console = false;      // out of range: none
    stream_request_resolve(&b.p, &b.sel, &rq);
    CHECK(rq.sub_idx == -1);
}

// Dolby Digital output.  Written out by hand from the rules, not generated:
// a DD track is copied (no AudioBitrate: a ceiling below the track's rate
// demotes a copy to a transcode); every other track takes the AC-3 640 kbps
// transcode; never a TrueHD or DTS copy.  The video ceiling is the step less
// 3% framing less the audio (25 Mbps: 24.25 Mbps - 0.64 Mbps = 23.61 Mbps).
static std::string dd_url(int quality, int audio, bool surround, bool surround_hd) {
    GoldenCase c = { "dd", quality, 0, surround, surround_hd, 1920, 1080, audio,
                     GS_NONE, GV_PLAIN, 1, 0 };
    Built b; build_case(&c, &b);
    b.p.passthrough = true;
    StreamRequest rq;
    stream_request_resolve(&b.p, &b.sel, &rq);
    char url[1024];
    stream_url_build(url, sizeof url, &rq, SERVER, DEVICE, "sess1234", 0);
    return url;
}

static void dolby_digital(void) {
    printf("- dolby digital\n");
    const std::string head =
        "http://server:8096/Videos/ITEM0001/stream.ts?VideoCodec=h264&Profile=high"
        "&Level=42&MaxWidth=1920&MaxHeight=1080&VideoBitrate=23610000"
        "&AllowVideoStreamCopy=true";
    const std::string tail =
        "&MaxFramerate=30";
    const std::string ids =
        "&DeviceId=DEVID123&Static=false&MediaSourceId=ITEM0001&StartTimeTicks=0";

    // A Dolby Digital track is copied.
    CHECK(dd_url(VQ1080_25, GA_AC3, true, true) ==
          head + "&AudioCodec=ac3&AudioSampleRate=48000&MaxAudioChannels=6" + tail +
          "&AllowAudioStreamCopy=true" + ids + "&AudioStreamIndex=1&PlaySessionId=sess1234");

    // Any other track is transcoded to AC-3, whatever the 5.1/7.1 prefs say.
    const std::string transcode =
        "&AudioCodec=ac3&AudioBitrate=640000&AudioSampleRate=48000&MaxAudioChannels=6" +
        tail + "&AllowAudioStreamCopy=false" + ids;
    const int others[] = { GA_TRUEHD, GA_DTS, GA_EAC3, GA_AAC };
    const int idx[]    = { 2, 3, 4, 5 };
    for (int i = 0; i < 4; i++) {
        const std::string want = head + transcode + "&AudioStreamIndex=" +
                                 std::to_string(idx[i]) + "&PlaySessionId=sess1234";
        CHECK(dd_url(VQ1080_25, others[i], true, true) == want);    // HD copy preferred, still no copy
        CHECK(dd_url(VQ1080_25, others[i], false, false) == want);  // and with the surround flags off
    }

    // Direct play: no video ceiling at all, the DD track still copied.
    CHECK(dd_url(VQORIG, GA_AC3, true, false) ==
          "http://server:8096/Videos/ITEM0001/stream.ts?VideoCodec=h264&Profile=high"
          "&Level=42&MaxWidth=1920&MaxHeight=1080&AllowVideoStreamCopy=true"
          "&AudioCodec=ac3&AudioSampleRate=48000&MaxAudioChannels=6&MaxFramerate=30"
          "&AllowAudioStreamCopy=true" + ids + "&AudioStreamIndex=1&PlaySessionId=sess1234");

    // The request flags: only a DD track is a copy, never an HD codec.
    Built b; GoldenCase c = { "dd", VQ1080_25, 0, 1, 1, 1920, 1080, GA_TRUEHD,
                              GS_NONE, GV_PLAIN, 1, 0 };
    build_case(&c, &b); b.p.passthrough = true;
    StreamRequest rq;
    stream_request_resolve(&b.p, &b.sel, &rq);
    CHECK(!rq.ac3_copy && rq.hd_codec == NULL && !strcmp(rq.acodec, "ac3"));
    c.audio = GA_AC3; build_case(&c, &b); b.p.passthrough = true;
    stream_request_resolve(&b.p, &b.sel, &rq);
    CHECK(rq.ac3_copy && rq.hd_codec == NULL);
    b.p.passthrough = false;                    // output back on 5.1: as it always was
    stream_request_resolve(&b.p, &b.sel, &rq);
    CHECK(!rq.ac3_copy);
}

// A live channel, written out by hand from the rules: the path is the
// channel (the opened source goes in MediaSourceId only), the video is never
// copied, and "Original" quality is a 10 Mbps ceiling instead of none.
// 10 Mbps: 9.7 Mbps after framing, less the 192 kbps MP3 = 9.508 Mbps.
// 25 Mbps: 24.25 Mbps less 192 kbps = 24.058 Mbps.
static std::string live_url(int quality, bool surround, bool passthrough) {
    GoldenCase c = { "live", quality, 0, surround, 0, 1920, 1080, -1,
                     GS_NONE, GV_LIVE, 1, 0 };
    Built b; build_case(&c, &b);
    b.p.passthrough = passthrough;
    snprintf(b.src.id, sizeof b.src.id, "LIVESRC1");
    b.sel.item_id = "CHAN0001";
    b.sel.live    = true;
    StreamRequest rq;
    stream_request_resolve(&b.p, &b.sel, &rq);
    char url[1024];
    stream_url_build(url, sizeof url, &rq, SERVER, DEVICE, "sess1234", 0);
    return url;
}

static void live_tv(void) {
    printf("- live tv\n");
    const std::string head = "http://server:8096/Videos/CHAN0001/stream.ts?VideoCodec=h264"
                             "&Profile=high&Level=42&MaxWidth=1920&MaxHeight=1080";
    const std::string ids  = "&DeviceId=DEVID123&Static=false&MediaSourceId=LIVESRC1"
                             "&StartTimeTicks=0&LiveStreamId=live%20stream%2F1"
                             "&PlaySessionId=sess1234";
    const std::string mp3  = "&AudioCodec=mp3&AudioBitrate=192000&AudioSampleRate=48000"
                             "&MaxAudioChannels=2&MaxFramerate=30&AllowAudioStreamCopy=false";
    CHECK(live_url(VQORIG, false, false) ==
          head + "&VideoBitrate=9508000&AllowVideoStreamCopy=false" + mp3 + ids);
    CHECK(live_url(VQ1080_25, false, false) ==
          head + "&VideoBitrate=24058000&AllowVideoStreamCopy=false" + mp3 + ids);
    // 5.1 output and Dolby Digital output both ask for AC-3, never a copy.
    const std::string ac3 = "&AudioCodec=ac3&AudioBitrate=640000&AudioSampleRate=48000"
                            "&MaxAudioChannels=6&MaxFramerate=30&AllowAudioStreamCopy=false";
    CHECK(live_url(VQORIG, true, false) ==
          head + "&VideoBitrate=9060000&AllowVideoStreamCopy=false" + ac3 + ids);
    CHECK(live_url(VQORIG, false, true) ==
          head + "&VideoBitrate=9060000&AllowVideoStreamCopy=false" + ac3 + ids);

    // Not live: the same selection copies the video and puts the version in the path.
    GoldenCase c = { "plain", VQORIG, 0, 0, 0, 1920, 1080, -1, GS_NONE, GV_PLAIN, 1, 0 };
    Built b; build_case(&c, &b);
    StreamRequest rq;
    stream_request_resolve(&b.p, &b.sel, &rq);
    CHECK(!rq.live && rq.vbitrate == 0 && rq.vreq == 0);
}

// Settings > Direct Stream = Off: the same requests, with the copy forbidden.
// Only AllowVideoStreamCopy changes; a live channel never copied anyway.
static std::string url_for(const GoldenCase *c, bool no_copy) {
    Built b; build_case(c, &b);
    b.p.no_video_copy = no_copy;
    StreamRequest rq;
    stream_request_resolve(&b.p, &b.sel, &rq);
    char url[1024];
    stream_url_build(url, sizeof url, &rq, SERVER, DEVICE, "sess1234", 0);
    return url;
}

static void direct_stream_off(void) {
    printf("- direct stream off\n");
    for (int i = 0; i < GOLDEN_N; i++) {
        std::string on  = url_for(&GOLDEN[i], false);
        std::string off = url_for(&GOLDEN[i], true);
        size_t at = on.find("AllowVideoStreamCopy=true");
        if (at == std::string::npos) { CHECK(on == off); continue; }
        on.replace(at, strlen("AllowVideoStreamCopy=true"), "AllowVideoStreamCopy=false");
        CHECK(on == off);
        CHECK(off.find("AllowVideoStreamCopy=true") == std::string::npos);
    }
}

// Jellyfin's live stream id is three 32-digit ids joined by underscores (98
// characters).  A 96-byte copy cut off the last three and the server could not
// find the stream (HTTP 500 on the console): the whole id must reach the URL.
static void live_stream_id_length(void) {
    printf("- live stream id length\n");
    const char *id = "e2329f4997b378e64ccf8fa396deb76e_af999c25a00715699361240d4c6c7a53_c955cfc46ce79741b9adcab51b75776d";
    CHECK(strlen(id) == 98);
    GoldenCase c = { "live", VQORIG, 0, 0, 0, 1920, 1080, -1, GS_NONE, GV_LIVE, 1, 0 };
    Built b; build_case(&c, &b);
    snprintf(b.src.live_stream_id, sizeof b.src.live_stream_id, "%s", id);
    b.sel.item_id = "CHAN0001";
    b.sel.live    = true;
    StreamRequest rq;
    stream_request_resolve(&b.p, &b.sel, &rq);
    CHECK(strcmp(rq.live_stream_id, id) == 0);
    char url[1024];
    stream_url_build(url, sizeof url, &rq, SERVER, DEVICE, "sess1234", 0);
    const std::string want = std::string("&LiveStreamId=") + id + "&PlaySessionId=sess1234";
    CHECK(std::string(url).find(want) != std::string::npos);
}

static void initial_selection(void) {
    printf("- initial selection\n");
    JFTracks t; fill_tracks(&t);
    t.default_audio = 2;
    int a = -9, s = -9;
    stream_select_initial(&t, true, &a, &s);
    CHECK(a == 2 && s == -1);
    stream_select_initial(&t, false, &a, &s);
    CHECK(a == -1 && s == -1);
    JFTracks none; memset(&none, 0, sizeof none);
    stream_select_initial(&none, true, &a, &s);
    CHECK(a == -1 && s == -1);
}

int main(void) {
    golden();
    budget();
    subtitles();
    dolby_digital();
    live_tv();
    live_stream_id_length();
    direct_stream_off();
    initial_selection();
    printf("%d checks, %d failed\n", s_checks, s_failed);
    return s_failed ? 1 : 0;
}
