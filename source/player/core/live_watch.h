#pragma once
#include <stdbool.h>
#include <stdint.h>

// -------------------------------------------------------------------------
//  Live TV: when the player has to ask for a fresh stream
// -------------------------------------------------------------------------
//  Pure (no PS3 headers) so tests/test_live_watch.c compiles the same code the
//  player runs.  The player calls live_watch_step() once per loop iteration
//  and acts on what it returns; the clock is timing_get_us().
//
//  A live stream is only good for as long as somebody reads it:
//    * paused longer than LIVE_PAUSE_REOPEN_US, the server has stopped
//      producing and what is buffered is stale -> resume on a fresh stream;
//    * the decode thread reached the stream's end -> the server dropped it;
//    * no new picture for LIVE_STALL_US while playing -> it stopped sending.
//  A reopen that is itself followed by another within LIVE_QUICK_REOPEN_US,
//  LIVE_QUICK_REOPEN_MAX times running, is a stream that will not stay up:
//  playback ends instead of asking the server again and again.

#define LIVE_PAUSE_REOPEN_US   10000000ULL
#define LIVE_STALL_US          15000000ULL
#define LIVE_QUICK_REOPEN_US   10000000ULL
#define LIVE_QUICK_REOPEN_MAX  3

typedef struct {
    uint64_t paused_at_us;     // 0 = not paused
    int      stall_frames;     // the frame count last seen, -1 = none yet
    uint64_t stall_since_us;   // when it last changed
    uint64_t last_open_us;     // when the stream was last (re)opened
    int      quick_opens;      // consecutive automatic reopens inside the window
} LiveWatch;

typedef enum {
    LIVE_EV_NONE = 0,
    LIVE_EV_RESUME_STALE,      // un-paused after too long a pause
    LIVE_EV_STALLED,           // playing, but no new picture
    LIVE_EV_ENDED              // the stream ended
} LiveEvent;

static inline void live_watch_init(LiveWatch *w, uint64_t now_us) {
    w->paused_at_us   = 0;
    w->stall_frames   = -1;
    w->stall_since_us = now_us;
    w->last_open_us   = now_us;
    w->quick_opens    = 0;
}

// One loop iteration.  frame_count is the player's pictures shown so far; it
// is 0 until the first, and a stream with no picture yet is not stalled.
static inline LiveEvent live_watch_step(LiveWatch *w, uint64_t now_us, bool paused,
                                        int frame_count, bool ended) {
    LiveEvent ev = LIVE_EV_NONE;
    if (paused) {
        if (!w->paused_at_us) w->paused_at_us = now_us;
    } else if (w->paused_at_us) {
        if (now_us - w->paused_at_us > LIVE_PAUSE_REOPEN_US) ev = LIVE_EV_RESUME_STALE;
        w->paused_at_us = 0;
        w->stall_since_us = now_us;        // a pause is not a stall
    }
    if (!paused && frame_count > 0) {
        if (frame_count != w->stall_frames) {
            w->stall_frames   = frame_count;
            w->stall_since_us = now_us;
        } else if (ev == LIVE_EV_NONE && now_us - w->stall_since_us > LIVE_STALL_US) {
            ev = LIVE_EV_STALLED;
        }
    } else {
        w->stall_since_us = now_us;
    }
    if (ev == LIVE_EV_NONE && ended && !paused) ev = LIVE_EV_ENDED;
    return ev;
}

// A reopen the player decided on (not one the viewer asked for) is about to
// run.  True when the stream keeps dropping and playback should end instead.
static inline bool live_watch_looping(LiveWatch *w, uint64_t now_us) {
    if (now_us - w->last_open_us < LIVE_QUICK_REOPEN_US) w->quick_opens++;
    else                                                 w->quick_opens = 0;
    return w->quick_opens >= LIVE_QUICK_REOPEN_MAX;
}

// Any reopen has finished (opened or not): measure the next from here.
static inline void live_watch_opened(LiveWatch *w, uint64_t now_us) {
    w->last_open_us   = now_us;
    w->stall_frames   = -1;
    w->stall_since_us = now_us;
    w->paused_at_us   = 0;
}
