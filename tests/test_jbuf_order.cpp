// jbuf_push() must hand the display pictures in timestamp order even though
// VDEC returns a B-pyramid stream in decode order.  The PTS sequence below is
// the first 30 pictures of a tester log (player_log 2026-10-06, 480i CRT).

#include <stdio.h>
#include <stdlib.h>
#include <sys/mutex.h>

struct sys_mutex_attr_t { int attr_protocol, attr_recursive; };
#define SYS_LWMUTEX_ATTR_PROTOCOL 0
#define SYS_MUTEX_ATTR_RECURSIVE  0
#define sysMutexAttrInitialize(a) ((a).attr_protocol = 0, (a).attr_recursive = 0)
static int sysMutexCreate(sys_mutex_t *, sys_mutex_attr_t *) { return 0; }
static int sysMutexDestroy(sys_mutex_t)     { return 0; }
static int sysMutexLock(sys_mutex_t, unsigned long long) { return 0; }
static int sysMutexUnlock(sys_mutex_t)      { return 0; }
void plog(const char *) {}

#include "../source/video/jbuf.cpp"

static int s_fail = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); s_fail++; } } while (0)

static const u64 LOG_PTS[] = {
    1483000, 1650000, 1566000, 1525000, 1608000, 1817000, 1733000, 1691000,
    1775000, 1983000, 1900000, 1858000, 1942000, 2150000, 2067000, 2025000,
    2109000, 2317000, 2234000, 2192000, 2275000, 2484000, 2401000, 2359000,
    2442000, 2651000, 2567000, 2526000, 2609000, 2818000,
};
static const int N = sizeof(LOG_PTS) / sizeof(LOG_PTS[0]);

// Steady state: the decoder keeps the queue full and the display pops one.
static void full_queue(int cap_w, int cap_h) {
    jbuf_alloc(cap_w, cap_h);
    jbuf_set_front_locked(1);
    const int cap = jbuf_cap();
    u64 last = 0; int shown = 0, i = 0;
    while (shown < N - (cap - 1)) {
        while (i < N && !jbuf_full()) { jbuf_push(LOG_PTS[i], 41708); i++; }
        const u64 p = jbuf_peek_pts_us();
        CHECK(p > last, "cap %d: shown %llu after %llu", cap, (unsigned long long)p, (unsigned long long)last);
        last = p;
        jbuf_consume_dur(jbuf_peek_dur()); jbuf_advance(); shown++;
    }
    u32 ro = 0, late = 0; jbuf_order_stats(&ro, &late);
    CHECK(late == 0, "cap %d: %u pictures arrived too late", cap, late);
    CHECK(ro > 0, "cap %d: nothing was reordered", cap);
    jbuf_free();
}

// Buffers swap pointers, never pixels: each picture keeps its own buffer.
static void pixels_follow(void) {
    jbuf_alloc(64, 64);
    jbuf_set_front_locked(1);
    for (int i = 0; i < 8; i++) {
        *jbuf_write_ptr() = (u8)i;
        jbuf_push(LOG_PTS[i], 41708);
    }
    for (int k = 0; k < 8; k++) {
        const u64 p = jbuf_peek_pts_us();
        int src = -1;
        for (int i = 0; i < 8; i++) if (LOG_PTS[i] == p) src = i;
        CHECK(*jbuf_peek() == (u8)src, "picture %llu carries the pixels of %d", (unsigned long long)p, *jbuf_peek());
        jbuf_consume_dur(jbuf_peek_dur()); jbuf_advance();
    }
    jbuf_free();
}

// With blending on, the front two slots belong to the display and the
// upload thread.
static void front_two_untouched(void) {
    jbuf_alloc(64, 64);
    jbuf_set_front_locked(2);
    jbuf_push(2000000, 41708);
    jbuf_push(2100000, 41708);
    jbuf_push(1900000, 41708);   // earlier than both, but they are in use
    CHECK(jbuf_peek_pts_us() == 2000000, "front moved");
    jbuf_consume_dur(jbuf_peek_dur()); jbuf_advance();
    CHECK(jbuf_peek_pts_us() == 2100000, "second moved");
    jbuf_free();
}

// A live stream restarting jumps back by seconds: keep arrival order.
static void discontinuity(void) {
    jbuf_alloc(64, 64);
    jbuf_set_front_locked(1);
    const u64 pts[] = { 9000000, 9041000, 9083000, 9125000, 100000 };
    for (u64 p : pts) jbuf_push(p, 41708);
    u64 seen[5];
    for (int k = 0; k < 5; k++) { seen[k] = jbuf_peek_pts_us(); jbuf_consume_dur(jbuf_peek_dur()); jbuf_advance(); }
    CHECK(seen[4] == 100000, "restart picture was pulled forward");
    jbuf_free();
}

int main() {
    full_queue(1920, 1080);   // 8-slot 1080p ring
    full_queue(1280, 720);    // 24-slot ring
    pixels_follow();
    front_two_untouched();
    discontinuity();
    printf("test_jbuf_order: %s\n", s_fail ? "FAILED" : "ok");
    return s_fail ? 1 : 0;
}
