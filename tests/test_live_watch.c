// Host test for source/player/core/live_watch.h: when a live stream has to be
// asked for again.  Times are microseconds on an arbitrary clock.
//
//   make -f Makefile.host test_live_watch && ./test_live_watch

#include "live_watch.h"

#include <stdio.h>

static int s_checks = 0, s_failed = 0;
#define CHECK(cond) do { s_checks++; if (!(cond)) { s_failed++; \
    printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

#define S(n) ((uint64_t)(n) * 1000000ULL)

static void pausing(void) {
    printf("- pause\n");
    LiveWatch w; live_watch_init(&w, S(100));
    // Playing: nothing.
    CHECK(live_watch_step(&w, S(101), false, 10, false) == LIVE_EV_NONE);
    // Paused for 10 s exactly is still a short pause; past it is stale.
    CHECK(live_watch_step(&w, S(102), true, 10, false) == LIVE_EV_NONE);
    CHECK(live_watch_step(&w, S(112), false, 10, false) == LIVE_EV_NONE);
    CHECK(live_watch_step(&w, S(120), true, 10, false) == LIVE_EV_NONE);
    CHECK(live_watch_step(&w, S(131), true, 10, false) == LIVE_EV_NONE);      // still paused
    CHECK(live_watch_step(&w, S(131) + 1, false, 10, false) == LIVE_EV_RESUME_STALE);
    CHECK(live_watch_step(&w, S(132), false, 10, false) == LIVE_EV_NONE);     // once
}

static void stalls(void) {
    printf("- stall\n");
    LiveWatch w; live_watch_init(&w, S(0));
    // No picture yet is not a stall, however long.
    CHECK(live_watch_step(&w, S(60), false, 0, false) == LIVE_EV_NONE);
    // Pictures arriving: fine.
    CHECK(live_watch_step(&w, S(61), false, 1, false) == LIVE_EV_NONE);
    CHECK(live_watch_step(&w, S(70), false, 200, false) == LIVE_EV_NONE);
    // Frozen count: 15 s, not before.
    CHECK(live_watch_step(&w, S(85), false, 200, false) == LIVE_EV_NONE);
    CHECK(live_watch_step(&w, S(85) + 1, false, 200, false) == LIVE_EV_STALLED);
    // A pause is not a stall: the count stands still while paused.
    live_watch_opened(&w, S(100));
    CHECK(live_watch_step(&w, S(101), false, 5, false) == LIVE_EV_NONE);
    CHECK(live_watch_step(&w, S(102), true, 5, false) == LIVE_EV_NONE);
    CHECK(live_watch_step(&w, S(112), false, 5, false) == LIVE_EV_NONE);      // 10 s pause, not stale
    CHECK(live_watch_step(&w, S(120), false, 5, false) == LIVE_EV_NONE);      // 8 s since resuming
    CHECK(live_watch_step(&w, S(127) + 1, false, 5, false) == LIVE_EV_STALLED);
}

static void ended(void) {
    printf("- ended\n");
    LiveWatch w; live_watch_init(&w, S(0));
    CHECK(live_watch_step(&w, S(1), false, 3, true) == LIVE_EV_ENDED);
    // While paused the end waits for the resume.
    live_watch_opened(&w, S(2));
    CHECK(live_watch_step(&w, S(3), true, 3, true) == LIVE_EV_NONE);
    CHECK(live_watch_step(&w, S(4), false, 3, true) == LIVE_EV_ENDED);
}

static void looping(void) {
    printf("- loop\n");
    LiveWatch w; live_watch_init(&w, S(0));
    // Reopens a minute apart never loop.
    for (int i = 1; i <= 6; i++) {
        CHECK(!live_watch_looping(&w, S(60 * i)));
        live_watch_opened(&w, S(60 * i));
    }
    // Three in a row inside 10 s each do.
    uint64_t t = S(1000);
    live_watch_opened(&w, t);
    t += S(5); CHECK(!live_watch_looping(&w, t)); live_watch_opened(&w, t);
    t += S(5); CHECK(!live_watch_looping(&w, t)); live_watch_opened(&w, t);
    t += S(5); CHECK(live_watch_looping(&w, t));
    // A long run in between resets the count.
    live_watch_opened(&w, t);
    t += S(30); CHECK(!live_watch_looping(&w, t));
    live_watch_opened(&w, t);
    t += S(5);  CHECK(!live_watch_looping(&w, t));
}

int main(void) {
    pausing();
    stalls();
    ended();
    looping();
    printf("live watch: %d checks, %d failed\n", s_checks, s_failed);
    return s_failed ? 1 : 0;
}
