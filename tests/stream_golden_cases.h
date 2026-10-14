// Inputs for the stream-request golden URLs (tests/test_stream_request.cpp);
// the expected URLs are in stream_golden_urls.inc, in the same order.
#pragma once

enum { GA_NONE = -1, GA_AC3 = 0, GA_TRUEHD, GA_DTS, GA_EAC3, GA_AAC };
enum { GS_NONE = -1, GS_TEXT = 0, GS_PGS, GS_BITMAP };
enum { GV_PLAIN = 0, GV_ALT, GV_LIVE, GV_NOSRC };

typedef struct {
    const char *name;
    int quality;                 // vquality_t
    int hd;                      // 1080p (Alpha) toggle
    int surround;                // surround_enabled()
    int surround_hd;             // surround_hd_preferred()
    unsigned disp_w, disp_h;     // output size
    int audio;                   // GA_*  (selected track)
    int sub;                     // GS_*  (selected subtitle)
    int version;                 // GV_*
    int session;                 // 1 = a PlaySessionId is known
    unsigned long long start;    // StartTimeTicks
} GoldenCase;

#define VQA 0   // Auto
#define VQ1080 1
#define VQ720 2
#define VQ480 3
#define VQ360 4
#define VQORIG 5
#define VQ1080_20 6
#define VQ1080_25 8

static const GoldenCase GOLDEN[] = {
    // Stereo
    {"stereo orig ac3",     VQORIG,    0, 0, 0, 1920,1080, GA_AC3,    GS_NONE, GV_PLAIN, 1, 0},
    {"stereo orig truehd",  VQORIG,    0, 0, 0, 1920,1080, GA_TRUEHD, GS_NONE, GV_PLAIN, 1, 0},
    {"stereo orig dts",     VQORIG,    0, 0, 0, 1920,1080, GA_DTS,    GS_NONE, GV_PLAIN, 1, 0},
    {"stereo 1080p25 ac3",  VQ1080_25, 0, 0, 0, 1920,1080, GA_AC3,    GS_NONE, GV_PLAIN, 1, 0},
    {"stereo 1080p25 truehd", VQ1080_25, 0, 0, 0, 1920,1080, GA_TRUEHD, GS_NONE, GV_PLAIN, 1, 0},
    {"stereo 1080p25 dts",  VQ1080_25, 0, 0, 0, 1920,1080, GA_DTS,    GS_NONE, GV_PLAIN, 1, 0},
    {"stereo 720p ac3",     VQ720,     0, 0, 0, 1920,1080, GA_AC3,    GS_NONE, GV_PLAIN, 1, 0},
    {"stereo 720p truehd",  VQ720,     0, 0, 0, 1920,1080, GA_TRUEHD, GS_NONE, GV_PLAIN, 1, 0},
    {"stereo 720p dts",     VQ720,     0, 0, 0, 1920,1080, GA_DTS,    GS_NONE, GV_PLAIN, 1, 0},
    {"stereo 480p ac3",     VQ480,     0, 0, 0, 1920,1080, GA_AC3,    GS_NONE, GV_PLAIN, 1, 0},
    {"stereo 480p truehd",  VQ480,     0, 0, 0, 1920,1080, GA_TRUEHD, GS_NONE, GV_PLAIN, 1, 0},
    {"stereo 480p dts",     VQ480,     0, 0, 0, 1920,1080, GA_DTS,    GS_NONE, GV_PLAIN, 1, 0},
    // 5.1 / 7.1 (surround on, HD preferred)
    {"5.1 orig ac3",        VQORIG,    0, 1, 1, 1920,1080, GA_AC3,    GS_NONE, GV_PLAIN, 1, 0},
    {"5.1 orig truehd",     VQORIG,    0, 1, 1, 1920,1080, GA_TRUEHD, GS_NONE, GV_PLAIN, 1, 0},
    {"5.1 orig dts",        VQORIG,    0, 1, 1, 1920,1080, GA_DTS,    GS_NONE, GV_PLAIN, 1, 0},
    {"5.1 1080p25 ac3",     VQ1080_25, 0, 1, 1, 1920,1080, GA_AC3,    GS_NONE, GV_PLAIN, 1, 0},
    {"5.1 1080p25 truehd",  VQ1080_25, 0, 1, 1, 1920,1080, GA_TRUEHD, GS_NONE, GV_PLAIN, 1, 0},
    {"5.1 1080p25 dts",     VQ1080_25, 0, 1, 1, 1920,1080, GA_DTS,    GS_NONE, GV_PLAIN, 1, 0},
    {"5.1 720p ac3",        VQ720,     0, 1, 1, 1920,1080, GA_AC3,    GS_NONE, GV_PLAIN, 1, 0},
    {"5.1 720p truehd",     VQ720,     0, 1, 1, 1920,1080, GA_TRUEHD, GS_NONE, GV_PLAIN, 1, 0},
    {"5.1 720p dts",        VQ720,     0, 1, 1, 1920,1080, GA_DTS,    GS_NONE, GV_PLAIN, 1, 0},
    {"5.1 480p ac3",        VQ480,     0, 1, 1, 1920,1080, GA_AC3,    GS_NONE, GV_PLAIN, 1, 0},
    {"5.1 480p truehd",     VQ480,     0, 1, 1, 1920,1080, GA_TRUEHD, GS_NONE, GV_PLAIN, 1, 0},
    {"5.1 480p dts",        VQ480,     0, 1, 1, 1920,1080, GA_DTS,    GS_NONE, GV_PLAIN, 1, 0},
    // Subtitles: console-drawn text/PGS add nothing, bitmap burns in
    {"5.1 1080p25 truehd +text", VQ1080_25, 0, 1, 1, 1920,1080, GA_TRUEHD, GS_TEXT,   GV_PLAIN, 1, 0},
    {"5.1 1080p25 truehd +pgs",  VQ1080_25, 0, 1, 1, 1920,1080, GA_TRUEHD, GS_PGS,    GV_PLAIN, 1, 0},
    {"5.1 1080p25 truehd +bitmap", VQ1080_25, 0, 1, 1, 1920,1080, GA_TRUEHD, GS_BITMAP, GV_PLAIN, 1, 0},
    {"stereo 720p ac3 +text",   VQ720, 0, 0, 0, 1920,1080, GA_AC3, GS_TEXT,   GV_PLAIN, 1, 0},
    {"stereo 720p ac3 +bitmap", VQ720, 0, 0, 0, 1920,1080, GA_AC3, GS_BITMAP, GV_PLAIN, 1, 0},
    // Versions
    {"live stream id",      VQ1080_25, 0, 1, 1, 1920,1080, GA_DTS,    GS_NONE, GV_LIVE,  1, 0},
    {"alternate version",   VQ720,     0, 0, 0, 1920,1080, GA_AC3,    GS_NONE, GV_ALT,   1, 0},
    {"no source chosen",    VQA,       0, 0, 0, 1920,1080, GA_NONE,   GS_NONE, GV_NOSRC, 1, 0},
    // Quality ladder corners
    {"auto hd off",         VQA,       0, 0, 0, 1920,1080, GA_AC3,    GS_NONE, GV_PLAIN, 1, 0},
    {"auto hd on",          VQA,       1, 0, 0, 1920,1080, GA_AC3,    GS_NONE, GV_PLAIN, 1, 0},
    {"1080p 10 Mbps",       VQ1080,    0, 1, 1, 1920,1080, GA_AC3,    GS_NONE, GV_PLAIN, 1, 0},
    {"1080p 20 Mbps dts",   VQ1080_20, 0, 1, 1, 1920,1080, GA_DTS,    GS_NONE, GV_PLAIN, 1, 0},
    {"360p",                VQ360,     0, 0, 0, 1920,1080, GA_AC3,    GS_NONE, GV_PLAIN, 1, 0},
    {"720p on a 480 output", VQ720,    0, 0, 0,  720, 480, GA_AC3,    GS_NONE, GV_PLAIN, 1, 0},
    // Audio track variants
    {"5.1 eac3 falls back to ac3", VQ1080_25, 0, 1, 1, 1920,1080, GA_EAC3, GS_NONE, GV_PLAIN, 1, 0},
    {"5.1 aac",             VQ1080_25, 0, 1, 1, 1920,1080, GA_AAC,    GS_NONE, GV_PLAIN, 1, 0},
    {"5.1 no track picked", VQ1080_25, 0, 1, 1, 1920,1080, GA_NONE,   GS_NONE, GV_PLAIN, 1, 0},
    {"5.1 hd disabled this session", VQ1080_25, 0, 1, 0, 1920,1080, GA_DTS, GS_NONE, GV_PLAIN, 1, 0},
    // Session id and seek offset
    {"no play session",     VQ720,     0, 0, 0, 1920,1080, GA_AC3,    GS_NONE, GV_PLAIN, 0, 0},
    {"seek offset",         VQ1080_25, 0, 1, 1, 1920,1080, GA_TRUEHD, GS_TEXT, GV_PLAIN, 1, 5400000000000ULL},
};
#define GOLDEN_N ((int)(sizeof(GOLDEN) / sizeof(GOLDEN[0])))
