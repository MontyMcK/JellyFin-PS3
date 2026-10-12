// rx_gap.h -- recognises a sender stuck in retransmission backoff from the
// receiving end.  Pure; host-tested by tests/test_rx_gap.cpp.
//
// The signature: a connection that has already delivered data goes completely
// quiet for seconds while the reader is waiting on it the whole time.  A LAN
// server that is merely slow still sends something every few hundred ms; a
// sender whose segments the console dropped sends nothing until its retransmit
// timer fires, and that timer doubles on every miss.
//
// Feed it every wait on the socket: a receive or a poll, with its start and end
// times and what it returned.  Waits closer together than RX_GAP_JOIN_US count
// as one unbroken wait; a longer break means the reader stopped asking (ring
// full, paused, busy decoding), and silence nobody was waiting through proves
// nothing.
#ifndef RX_GAP_H
#define RX_GAP_H

#include <stdint.h>

#define RX_GAP_MIN_BYTES  (1024u * 1024u)   // past the transcode's start-up
#define RX_GAP_SILENCE_US 3000000ull        // one unbroken wait this long trips it
#define RX_GAP_JOIN_US    250000ull

typedef struct {
    uint64_t bytes;      // delivered on this connection
    uint64_t run_start;  // start of the current unbroken wait; 0 = none
    uint64_t run_end;    // end of its latest piece
} RxGap;

static inline void rx_gap_reset(RxGap *g) {
    g->bytes = 0; g->run_start = 0; g->run_end = 0;
}

// One wait from t0 to t1 (microseconds) that returned n: > 0 bytes arrived,
// 0 nothing arrived (a timeout, or a poll with nothing ready).  Returns 1 when
// the unbroken wait has reached RX_GAP_SILENCE_US on a connection that had
// already delivered RX_GAP_MIN_BYTES.
static inline int rx_gap_note(RxGap *g, uint64_t t0, uint64_t t1, int n) {
    if (n > 0) {
        g->bytes += (uint64_t)n;
        g->run_start = 0;
        return 0;
    }
    if (t1 < t0) t1 = t0;
    if (g->run_start == 0 || t0 < g->run_end || t0 - g->run_end > RX_GAP_JOIN_US)
        g->run_start = t0 ? t0 : 1;
    g->run_end = t1;
    return g->bytes >= RX_GAP_MIN_BYTES && t1 - g->run_start >= RX_GAP_SILENCE_US;
}

#endif
