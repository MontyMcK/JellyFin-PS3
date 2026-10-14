// Host test for source/video/h264_sps.c against real x264 SPS NAL units (fixtures/h264/, made by
// make_sps.py, expectations from ffprobe).
//
//   make -f Makefile.host test_h264_sps && ./test_h264_sps

#include "h264_sps.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static int s_checks = 0, s_failed = 0;
#define CHECK(cond) do { s_checks++; if (!(cond)) { s_failed++; \
    printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

struct Fixture {
    const char *name;
    unsigned char nal[64];
    int len, width, height, level, depth, chroma, frame_mbs_only;
    double fps;
    const char *profile;
};

static const Fixture FIX[] = {
#include "fixtures/h264/sps_fixtures.inc"
};
#define NFIX ((int)(sizeof FIX / sizeof FIX[0]))

static int profile_idc_of(const char *p) {
    if (strstr(p, "Baseline")) return 66;
    if (!strcmp(p, "Main")) return 77;
    if (!strcmp(p, "High")) return 100;
    if (!strcmp(p, "High 10")) return 110;
    if (!strcmp(p, "High 4:2:2")) return 122;
    return -1;
}

static void fixtures() {
    printf("- x264 SPS units\n");
    for (int i = 0; i < NFIX; i++) {
        const Fixture &f = FIX[i];
        H264SpsInfo s;
        const bool ok = h264_parse_sps(f.nal, f.len, &s);
        CHECK(ok);
        if (!ok) { printf("  %s\n", f.name); continue; }
        if (s.width != f.width || s.height != f.height) printf("  %s: %dx%d, want %dx%d\n", f.name, s.width, s.height, f.width, f.height);
        CHECK(s.width == f.width && s.height == f.height);       // cropping applied (1080 is coded as 1088)
        CHECK(s.level_idc == f.level);
        CHECK(s.profile_idc == profile_idc_of(f.profile));
        CHECK(s.bit_depth == f.depth);
        CHECK(s.chroma_format_idc == f.chroma);
        CHECK(s.frame_mbs_only == (f.frame_mbs_only != 0));
        CHECK(s.has_timing && fabs(s.fps - f.fps) < 0.001);
    }
}

static void playability() {
    printf("- what the console can play\n");
    struct { const char *name; bool ok; const char *why; } want[] = {
        { "baseline_64", true, "" }, { "main_720p_24", true, "" }, { "high_1080p_24", true, "" },
        { "high_1080p_60", true, "" }, { "high_1080i", true, "" }, { "main_480p_ntsc", true, "" },
        { "baseline_768", true, "" },
        { "high10_1080p", false, "10-bit" }, { "high422_1080p", false, "4:2:2" },
        { "high_4k", false, "4K" }, { "high_1080p_l51", false, "Level 5.1" },
    };
    for (auto &w : want) {
        const Fixture *f = nullptr;
        for (int i = 0; i < NFIX; i++) if (!strcmp(FIX[i].name, w.name)) f = &FIX[i];
        CHECK(f != nullptr);
        if (!f) continue;
        H264SpsInfo s;
        char why[48] = "x";
        CHECK(h264_parse_sps(f->nal, f->len, &s));
        const bool ok = h264_playable(&s, why, sizeof why);
        if (ok != w.ok || strcmp(why, w.why)) printf("  %s: ok=%d why=\"%s\"\n", w.name, ok, why);
        CHECK(ok == w.ok && !strcmp(why, w.why));
    }
    H264SpsInfo s;
    memset(&s, 0, sizeof s);
    s.profile_idc = 100; s.level_idc = 41; s.chroma_format_idc = 1; s.bit_depth = 8; s.width = 1920; s.height = 1080;
    CHECK(h264_playable(&s, nullptr, 0));                                  // the reason is optional
    s.profile_idc = 66; s.width = 1921;
    CHECK(!h264_playable(&s, nullptr, 0));
    s.width = 1920; s.height = 1089;
    CHECK(!h264_playable(&s, nullptr, 0));
    s.height = 1080; s.profile_idc = 83;
    char why[48];
    CHECK(!h264_playable(&s, why, sizeof why) && !strcmp(why, "profile 83"));
    s.profile_idc = 100; s.bit_depth = 10;
    CHECK(!h264_playable(&s, why, sizeof why) && !strcmp(why, "10-bit"));
    s.bit_depth = 8; s.chroma_format_idc = 3;
    CHECK(!h264_playable(&s, why, sizeof why));
    char tiny[4];
    s.chroma_format_idc = 1; s.level_idc = 51;
    CHECK(!h264_playable(&s, tiny, sizeof tiny) && strlen(tiny) == 3);     // truncated, still terminated
}

static void garbage() {
    printf("- not an SPS\n");
    H264SpsInfo s;
    CHECK(!h264_parse_sps(nullptr, 0, &s));
    unsigned char pps[] = { 0x68, 0xce, 0x3c, 0x80 };
    CHECK(!h264_parse_sps(pps, sizeof pps, &s));                            // a PPS
    CHECK(!h264_parse_sps(FIX[0].nal, 3, &s));                              // cut short
    unsigned char zero[32] = { 0x67 };
    CHECK(!h264_parse_sps(zero, sizeof zero, &s));

    // every prefix of every fixture: no crash, no read past the end (run under ASan/valgrind for the proof)
    int parsed = 0;
    for (int i = 0; i < NFIX; i++)
        for (int n = 0; n <= FIX[i].len; n++) {
            unsigned char copy[64];
            memcpy(copy, FIX[i].nal, (size_t)n);
            parsed += h264_parse_sps(copy, n, &s) ? 1 : 0;
        }
    CHECK(parsed >= NFIX);                                                 // at least the whole ones
}

int main() {
    fixtures();
    playability();
    garbage();
    printf("h264 sps: %d checks, %d failed\n", s_checks, s_failed);
    return s_failed ? 1 : 0;
}
