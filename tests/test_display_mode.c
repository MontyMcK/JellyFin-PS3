// Host test for source/video/display_mode.c -- the 24p decision record.
//
// The fixtures are the REAL mode list this console's TV reported
// (outputs/player_log.latest.txt, 2026-09-22), not invented values, so the
// "advertised" verdict is tested against what the firmware actually said.
#include <stdio.h>
#include <string.h>
#include "display_mode.h"

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)
#define CHECK_STR(a, b) do { if (strcmp((a), (b)) != 0) { \
    printf("FAIL %s:%d  \"%s\" != \"%s\"\n", __FILE__, __LINE__, (a), (b)); fails++; } } while (0)

// Panasonic UT30 via the CECH-2503, videoGetDeviceInfo, verbatim.
static const dm_mode kUT30[] = {
    {1, 0x0033}, {2, 0x0003}, {1, 0x0003}, {10, 0x0003}, {10, 0x0003},
    {11, 0x0003}, {11, 0x0003}, {12, 0x0003}, {12, 0x0003}, {13, 0x0003},
    {13, 0x0003}, {4, 0x0001}, {4, 0x0001}, {4, 0x0001}, {4, 0x0001},
    {5, 0x0002}, {5, 0x0002}, {5, 0x0002}, {5, 0x0002}, {129, 0x0003},
    {130, 0x0040}, {131, 0x0030}, {136, 0x0001}, {137, 0x0001}, {138, 0x0001},
    {139, 0x0001}, {146, 0x0003}, {146, 0x0003},
};
#define NUT30 ((int)(sizeof(kUT30) / sizeof(kUT30[0])))

// A plain 60/50 panel: same list with every 24Hz bit removed.
static dm_mode kNo24[NUT30];

static void test_rates(void)
{
    char b[96];
    uint32_t n, d;
    CHECK(dm_display_rate(0x01, &n, &d) == 1 && n == 60000 && d == 1001);
    CHECK(dm_display_rate(0x02, &n, &d) == 1 && n == 50 && d == 1);
    CHECK(dm_display_rate(0x04, &n, &d) == 1 && n == 60 && d == 1);
    CHECK(dm_display_rate(0x08, &n, &d) == 1 && n == 30 && d == 1);
    // An unknown bit alone falls back to 59.94 -- and says so.
    CHECK(dm_display_rate(0x10, &n, &d) == 0 && n == 60000 && d == 1001);
    CHECK(dm_display_rate(0x40, &n, &d) == 0);

    dm_rates_str(0x33, b, sizeof(b)); CHECK_STR(b, "59.94|50|24fam(0x10)|24fam(0x20)");
    dm_rates_str(0x40, b, sizeof(b)); CHECK_STR(b, "unk(0x40)");
    dm_rates_str(0x00, b, sizeof(b)); CHECK_STR(b, "none");
    dm_rates_str(0x181, b, sizeof(b)); CHECK_STR(b, "59.94|other(0x0180)");
    // Truncation must not overrun.
    char tiny[6];
    dm_rates_str(0x33, tiny, sizeof(tiny)); CHECK(strlen(tiny) < sizeof(tiny));
}

static void test_film(void)
{
    CHECK(dm_classify_film(24000, 1001) == DM_FILM_23976);
    CHECK(dm_classify_film(48000, 2002) == DM_FILM_23976);
    CHECK(dm_classify_film(24, 1) == DM_FILM_24);
    CHECK(dm_classify_film(24000, 1000) == DM_FILM_24);
    // 23.976 NOT snapped is not film: it would mean the snap broke upstream.
    CHECK(dm_classify_film(23976, 1000) == DM_FILM_NONE);
    CHECK(dm_classify_film(25, 1) == DM_FILM_NONE);
    CHECK(dm_classify_film(30000, 1001) == DM_FILM_NONE);
    CHECK(dm_classify_film(30, 1) == DM_FILM_NONE);
    CHECK(dm_classify_film(50, 1) == DM_FILM_NONE);
    CHECK(dm_classify_film(60000, 1001) == DM_FILM_NONE);
    CHECK(dm_classify_film(24, 0) == DM_FILM_NONE);

    CHECK(dm_fps_confident(DM_FPS_VDEC_FRC));
    CHECK(dm_fps_confident(DM_FPS_SERVER));
    CHECK(!dm_fps_confident(DM_FPS_DEFAULT));
    CHECK(!dm_fps_confident(DM_FPS_TIMEOUT));
}

static void test_support(void)
{
    CHECK(dm_display_24p_support(kUT30, NUT30, DM_RES_1080) == DM_SUPPORT_YES);
    // 720 is listed on the UT30, but only at 59.94/50.
    CHECK(dm_display_24p_support(kUT30, NUT30, DM_RES_720) == DM_SUPPORT_NO);
    CHECK(dm_display_24p_support(kNo24, NUT30, DM_RES_1080) == DM_SUPPORT_NO);
    CHECK(dm_display_24p_support(kUT30, -1, DM_RES_1080) == DM_SUPPORT_UNKNOWN);
    CHECK(dm_display_24p_support(NULL, 3, DM_RES_1080) == DM_SUPPORT_UNKNOWN);
    // Not listed at all: unknown, never "yes".
    CHECK(dm_display_24p_support(kUT30, NUT30, 7) == DM_SUPPORT_UNKNOWN);
    // The 0x40-only mode (res 130) is NOT 24Hz support.
    const dm_mode only40[] = { {1, 0x0041} };
    CHECK(dm_display_24p_support(only40, 1, DM_RES_1080) == DM_SUPPORT_NO);
}

static void test_cadence(void)
{
    char b[48];
    dm_cadence_str(60000, 1001, 24000, 1001, b, sizeof(b)); CHECK_STR(b, "3:2 pulldown");
    dm_cadence_str(60000, 1001, 30000, 1001, b, sizeof(b)); CHECK_STR(b, "2:2");
    dm_cadence_str(60000, 1001, 60000, 1001, b, sizeof(b)); CHECK_STR(b, "1:1");
    dm_cadence_str(50, 1, 25, 1, b, sizeof(b));             CHECK_STR(b, "2:2");
    dm_cadence_str(50, 1, 50, 1, b, sizeof(b));             CHECK_STR(b, "1:1");
    // The target state: 23.976 content on a 23.976 output.
    dm_cadence_str(24000, 1001, 24000, 1001, b, sizeof(b)); CHECK_STR(b, "1:1");
    // ...and why 23.976 vs 24.000 matters: 24.000 content on a 23.976 output
    // is NOT 1:1, it drops a frame every ~41.7 s.
    dm_cadence_str(24000, 1001, 24, 1, b, sizeof(b));       CHECK_STR(b, "uneven x0.999");
    // 24.000 on 59.94 is not clean 3:2 either.
    dm_cadence_str(60000, 1001, 24, 1, b, sizeof(b));       CHECK_STR(b, "uneven x2.498");
    dm_cadence_str(60000, 1001, 25, 1, b, sizeof(b));       CHECK_STR(b, "uneven x2.398");
    dm_cadence_str(50, 1, 24000, 1001, b, sizeof(b));       CHECK_STR(b, "uneven x2.085");
    dm_cadence_str(0, 1, 24, 1, b, sizeof(b));              CHECK_STR(b, "unknown");
}

static void test_decide(void)
{
    const dm_support yes = DM_SUPPORT_YES, no = DM_SUPPORT_NO;
    const int P = 1, ON = 1, OFF = 0;
    dm_decision d;

    // A. 1080p 23.976 on a 24p TV, enabled: the one path that switches.
    d = dm_decide(DM_FILM_23976, 1, 1920, 1080, DM_RES_1080, DM_RATE_59_94, P, yes, ON);
    CHECK(d.candidate == 1 && d.attempt == 1);
    CHECK_STR(d.path, "cellVideoOutConfigure2");
    CHECK_STR(d.result, "pending");

    // Same, but the user has not opted in: nothing happens, and it says why.
    d = dm_decide(DM_FILM_23976, 1, 1920, 1080, DM_RES_1080, DM_RATE_59_94, P, yes, OFF);
    CHECK(d.candidate == 1 && d.attempt == 0);
    CHECK_STR(d.result, "not_attempted");
    CHECK(strstr(d.result_why, "off") != NULL);

    // B. same content, TV without 24p: never attempted, even when enabled.
    d = dm_decide(DM_FILM_23976, 1, 1920, 1080, DM_RES_1080, DM_RATE_59_94, P, no, ON);
    CHECK(d.candidate == 1 && d.attempt == 0);
    CHECK(strstr(d.result_why, "does not advertise") != NULL);
    d = dm_decide(DM_FILM_23976, 1, 1920, 1080, DM_RES_1080, DM_RATE_59_94, P, DM_SUPPORT_UNKNOWN, ON);
    CHECK(d.attempt == 0);

    // C-F. 30, 25, 50, 60 fps are never candidates.
    d = dm_decide(dm_classify_film(30, 1), 1, 1920, 1080, DM_RES_1080, DM_RATE_59_94, P, yes, ON);
    CHECK(d.candidate == 0 && d.attempt == 0);
    d = dm_decide(dm_classify_film(25, 1), 1, 1920, 1080, DM_RES_1080, DM_RATE_50, P, yes, ON);
    CHECK(d.candidate == 0 && d.attempt == 0);
    d = dm_decide(dm_classify_film(50, 1), 1, 1920, 1080, DM_RES_1080, DM_RATE_50, P, yes, ON);
    CHECK(d.candidate == 0 && d.attempt == 0);
    d = dm_decide(dm_classify_film(60000, 1001), 1, 1920, 1080, DM_RES_1080, DM_RATE_59_94, P, yes, ON);
    CHECK(d.candidate == 0 && d.attempt == 0);

    // A guessed rate never qualifies, even if it happened to be 24.
    d = dm_decide(DM_FILM_24, 0, 1920, 1080, DM_RES_1080, DM_RATE_59_94, P, yes, ON);
    CHECK(d.attempt == 0 && strstr(d.candidate_why, "guessed") != NULL);

    // 720p output configurations behave as today.
    d = dm_decide(DM_FILM_23976, 1, 1280, 720, DM_RES_720, DM_RATE_59_94, P, yes, ON);
    CHECK(d.attempt == 0 && strstr(d.candidate_why, "1920x1080") != NULL);

    // 1080i output: left alone.
    d = dm_decide(DM_FILM_23976, 1, 1920, 1080, DM_RES_1080, DM_RATE_59_94, 0, yes, ON);
    CHECK(d.attempt == 0 && strstr(d.candidate_why, "interlaced") != NULL);

    // PAL 50Hz 1080p output with film: a candidate like any other.
    d = dm_decide(DM_FILM_24, 1, 1920, 1080, DM_RES_1080, DM_RATE_50, P, yes, ON);
    CHECK(d.candidate == 1 && d.attempt == 1);

    // SD content on a 1080 output: not a candidate.
    d = dm_decide(DM_FILM_23976, 1, 720, 480, DM_RES_1080, DM_RATE_59_94, P, yes, ON);
    CHECK(d.attempt == 0);

    // Already on a 24Hz-family output: nothing to do.
    d = dm_decide(DM_FILM_23976, 1, 1920, 1080, DM_RES_1080, DM_RATE_24FAM_A, P, yes, ON);
    CHECK(d.attempt == 0);

    // attempt requires ALL of: film, confident, supported, enabled.
    for (int f = 0; f <= 2; f++)
        for (int c = 0; c <= 1; c++)
            for (int s = 0; s <= 2; s++)
                for (int e = 0; e <= 1; e++) {
                    d = dm_decide((dm_film)f, c, 1920, 1080, DM_RES_1080, DM_RATE_59_94,
                                  P, (dm_support)s, e);
                    CHECK(d.attempt == (f != 0 && c && s == DM_SUPPORT_YES && e));
                    CHECK(d.candidate_why != NULL && d.result_why != NULL);
                }
}

static void test_clock(void)
{
    uint32_t n = 0, d = 0;
    // Ideal periods in ns.
    CHECK(dm_classify_period(41708333, &n, &d) == DM_CLK_23976 && n == 24000 && d == 1001);
    CHECK(dm_classify_period(41666667, &n, &d) == DM_CLK_24 && n == 24 && d == 1);
    CHECK(dm_classify_period(16683333, &n, &d) == DM_CLK_5994 && n == 60000 && d == 1001);
    CHECK(dm_classify_period(16666667, &n, &d) == DM_CLK_60);
    CHECK(dm_classify_period(20000000, &n, &d) == DM_CLK_50);
    // Measurement noise of +/-100 ppm must not flip 23.976 into 24 or back.
    CHECK(dm_classify_period(41708333 + 4170, 0, 0) == DM_CLK_23976);
    CHECK(dm_classify_period(41708333 - 4170, 0, 0) == DM_CLK_23976);
    CHECK(dm_classify_period(41666667 + 4166, 0, 0) == DM_CLK_24);
    CHECK(dm_classify_period(41666667 - 4166, 0, 0) == DM_CLK_24);
    // Halfway between them (500 ppm off both) is refused, not guessed.
    CHECK(dm_classify_period(41687500, 0, 0) == DM_CLK_UNKNOWN);
    CHECK(dm_classify_period(0, 0, 0) == DM_CLK_UNKNOWN);
    CHECK(dm_classify_period(35000000, 0, 0) == DM_CLK_UNKNOWN);
    CHECK_STR(dm_clock_name(DM_CLK_23976), "23.976");
    CHECK_STR(dm_clock_name(DM_CLK_UNKNOWN), "unknown");
}

int main(void)
{
    for (int i = 0; i < NUT30; i++) {
        kNo24[i] = kUT30[i];
        kNo24[i].rates &= (uint16_t)~DM_RATE_24FAM;
    }
    test_rates();
    test_film();
    test_support();
    test_cadence();
    test_decide();
    test_clock();
    if (fails) { printf("test_display_mode: %d FAILED\n", fails); return 1; }
    printf("test_display_mode: all passed\n");
    return 0;
}
