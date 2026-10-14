// Host test for the retransmission-backoff detector (source/player/stream/rx_gap.h).
//
//   make -f Makefile.host test_rx_gap && ./test_rx_gap
#include <stdio.h>

#include "rx_gap.h"

static int s_checks = 0, s_failed = 0;
#define CHECK(cond) do { s_checks++; if (!(cond)) { s_failed++; \
    printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

// Polls of 2 ms every 5 ms from t for dur microseconds; returns whether any tripped.
static int wait_polls(RxGap *g, uint64_t *t, uint64_t dur) {
    int hit = 0;
    const uint64_t end = *t + dur;
    while (*t < end) {
        hit |= rx_gap_note(g, *t, *t + 2000, 0);
        *t += 5000;
    }
    return hit;
}

int main() {
    RxGap g;
    uint64_t t;

    // The 2026-10-12 log: a 1.7 MB burst, then 6-9 s of nothing.
    rx_gap_reset(&g); t = 1000000;
    for (int i = 0; i < 7; i++) { rx_gap_note(&g, t, t + 1000, 256 * 1024); t += 2000; }
    CHECK(!wait_polls(&g, &t, 2900000));
    CHECK(wait_polls(&g, &t, 200000));

    // Before the first megabyte the server may still be starting its transcode.
    rx_gap_reset(&g); t = 1000000;
    rx_gap_note(&g, t, t + 1000, 512 * 1024); t += 2000;
    CHECK(!wait_polls(&g, &t, 20000000));

    // A slow server: something every 400 ms never trips it.
    rx_gap_reset(&g); t = 1000000;
    rx_gap_note(&g, t, t + 1000, 2 * 1024 * 1024); t += 2000;
    int hit = 0;
    for (int i = 0; i < 100; i++) {
        hit |= wait_polls(&g, &t, 400000);
        rx_gap_note(&g, t, t + 500, 64 * 1024); t += 1000;
    }
    CHECK(!hit);

    // A reader that stopped asking (ring full, paused): the silence between its
    // waits is not counted, however long.
    rx_gap_reset(&g); t = 1000000;
    rx_gap_note(&g, t, t + 1000, 2 * 1024 * 1024); t += 2000;
    hit = 0;
    for (int i = 0; i < 20; i++) {
        hit |= wait_polls(&g, &t, 1000000);
        t += 2000000;                        // not waiting
    }
    CHECK(!hit);

    // One long blocking receive counts the same as many short polls.
    rx_gap_reset(&g); t = 1000000;
    rx_gap_note(&g, t, t + 1000, 2 * 1024 * 1024); t += 2000;
    CHECK(rx_gap_note(&g, t, t + 3000000, 0));

    // A clock that steps backwards starts a new wait instead of a huge one.
    rx_gap_reset(&g); t = 5000000;
    rx_gap_note(&g, t, t + 1000, 2 * 1024 * 1024);
    CHECK(!rx_gap_note(&g, 6000000, 6002000, 0));
    CHECK(!rx_gap_note(&g, 100, 2100, 0));

    printf("%s: %d checks, %d failed\n", s_failed ? "FAIL" : "ok", s_checks, s_failed);
    return s_failed ? 1 : 0;
}
