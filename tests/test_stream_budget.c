// Host tests for source/player/core/stream_budget.h -- what a quality step
// asks the server for once the audio that rides with it is accounted for.
//
//   cc -std=c99 -Wall -Wextra -I../source/player/core -o test_stream_budget test_stream_budget.c

#include <stdio.h>
#include "stream_budget.h"

static int g_fail = 0, g_checks = 0;
#define CHECK(c, ...) do { g_checks++; if (!(c)) { g_fail++;                 \
    printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__);         \
    printf("\n"); } } while (0)

// What actually arrives: video + audio + TS framing on both.
static double wire_mbps(unsigned video_bps, unsigned audio_actual_bps)
{
    return (double)(video_bps + audio_actual_bps) * 100.0 /
           (100.0 - STREAM_TS_OVERHEAD_PCT) / 1e6;
}

int main(void)
{
    // Original: no ceiling, whatever the audio.
    CHECK(stream_video_budget(0, 5000000) == 0, "Original must stay uncapped");

    // THE MEASURED FAILURE: 25 Mbps step, DTS-HD MA 5.1 copied.  The server
    // reported its core rate (1509 kbps); the old request put 25 Mbps of
    // video on top and the wire carried 27.6 Mbps against a ~25 Mbps ceiling.
    {
        const unsigned a = stream_audio_cost(STREAM_AUDIO_COPY_DTS, 1509000u);
        const unsigned v = stream_video_budget(25000000u, a);
        CHECK(a == STREAM_EST_DTS_HD_BPS, "DTS-HD core rate must not be trusted (%u)", a);
        CHECK(v < 25000000u && v > 20000000u, "25 Mbps step with DTS-HD: video %u", v);
        // The track in that session actually cost ~1.9 Mbps: the budgeted
        // stream fits the ceiling with margin, the old one did not.
        CHECK(wire_mbps(v, 1900000u) <= 25.0, "budgeted wire %.2f Mbps", wire_mbps(v, 1900000u));
        CHECK(wire_mbps(25000000u, 1900000u) > 27.0, "the old request was over the ceiling");
        printf("25 Mbps + DTS-HD MA copy: video %.2f Mbps (was 25.00), wire %.2f Mbps (was %.2f)\n",
               v / 1e6, wire_mbps(v, 1900000u), wire_mbps(25000000u, 1900000u));
    }
    // TrueHD: reserve more.
    {
        const unsigned a = stream_audio_cost(STREAM_AUDIO_COPY_TRUEHD, 0);
        const unsigned v = stream_video_budget(25000000u, a);
        CHECK(a == STREAM_EST_TRUEHD_BPS, "TrueHD estimate");
        CHECK(wire_mbps(v, a) <= 25.01, "TrueHD budget fits: %.2f", wire_mbps(v, a));
    }
    // A reported rate above the estimate wins (an Atmos track that says so).
    CHECK(stream_audio_cost(STREAM_AUDIO_COPY_TRUEHD, 6500000u) == 6500000u,
          "higher reported rate must be used");
    // Plain transcoded AC-3: loses only its own 640 kbps and the framing.
    {
        const unsigned v = stream_video_budget(25000000u,
                               stream_audio_cost(STREAM_AUDIO_TRANSCODE, 640000u));
        CHECK(v == 25000000u * 97u / 100u - 640000u, "AC-3 budget %u", v);
        CHECK(wire_mbps(v, 640000u) <= 25.0001, "AC-3 wire fits");
    }
    // Every step keeps its promise: wire <= step for the budgeted audio.
    {
        static const unsigned steps[] = { 4000000u, 8000000u, 10000000u, 15000000u,
                                          20000000u, 25000000u, 30000000u };
        static const int kinds[] = { STREAM_AUDIO_TRANSCODE, STREAM_AUDIO_COPY_DTS,
                                     STREAM_AUDIO_COPY_TRUEHD };
        for (unsigned i = 0; i < sizeof steps / sizeof steps[0]; i++)
            for (unsigned k = 0; k < 3; k++) {
                const unsigned a = stream_audio_cost(kinds[k], kinds[k] ? 0u : 640000u);
                const unsigned v = stream_video_budget(steps[i], a);
                CHECK(v >= steps[i] / 2, "floor at step %u kind %d", steps[i], kinds[k]);
                if (v > steps[i] / 2)   // unless clamped by the floor
                    CHECK(wire_mbps(v, a) <= steps[i] / 1e6 + 0.001,
                          "step %u kind %d: wire %.3f", steps[i], kinds[k], wire_mbps(v, a));
            }
    }
    // The floor: an absurd audio claim cannot starve the picture.
    CHECK(stream_video_budget(10000000u, 50000000u) == 5000000u, "video floor");

    printf("test_stream_budget: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
