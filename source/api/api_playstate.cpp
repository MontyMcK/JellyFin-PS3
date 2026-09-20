// Playback-state reporting — POST /Sessions/Playing{,/Progress,/Stopped}.
// Keeps the server's Continue Watching list and resume positions in sync
// with what the PS3 plays.  Responses are empty (204); use a small local
// buffer instead of the shared responseBuffer since the progress reports
// come from their own thread.

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <ppu-types.h>
#include <sys/thread.h>
#include <sys/mutex.h>

#include "jellyfin_api.h"
#include "plog.h"

static void post_playstate(const char *endpoint, const char *item_id,
                           const char *session_id, u64 pos_ticks, bool paused) {
    if (!g_server[0] || !g_token[0] || !item_id || !item_id[0]) return;

    char url[512];
    snprintf(url, sizeof(url), "%s%s", g_server, endpoint);

    char body[512];
    snprintf(body, sizeof(body),
        "{\"ItemId\":\"%s\",\"PlaySessionId\":\"%s\","
        "\"PositionTicks\":%llu,\"IsPaused\":%s,"
        "\"PlayMethod\":\"Transcode\",\"CanSeek\":true}",
        item_id, session_id ? session_id : "",
        (unsigned long long)pos_ticks, paused ? "true" : "false");

    char resp[256];
    int status = http_request(HTTP_POST, url, body, g_token, resp, sizeof(resp));

    // The first dozen, then one in every six, and every failure.
    //
    // It used to stop after twelve successes, which is why the log from the
    // last crash showed reports ending an hour before the app did -- they had
    // not stopped, they had stopped being written, and that read as a dead
    // thread.  A heartbeat that goes quiet when healthy is worse than no
    // heartbeat.
    static int s_log = 0, s_seen = 0;
    s_seen++;
    bool failed = (status != 200 && status != 204);
    if (s_log < 12 || failed || (s_seen % 6) == 0) {
        if (s_log < 400) {
            s_log++;
            char buf[112];
            snprintf(buf, sizeof(buf), "playstate: %s http=%d pos=%llus",
                     endpoint, status,
                     (unsigned long long)(pos_ticks / 10000000ULL));
            plog(buf);
        }
    }
}

void jellyfin_report_playing(const char *item_id, const char *session_id,
                             unsigned long long pos_ticks) {
    post_playstate("/Sessions/Playing", item_id, session_id, pos_ticks, false);
}

void jellyfin_report_progress(const char *item_id, const char *session_id,
                              unsigned long long pos_ticks, bool paused) {
    post_playstate("/Sessions/Playing/Progress", item_id, session_id,
                   pos_ticks, paused);
}

void jellyfin_report_stopped(const char *item_id, const char *session_id,
                             unsigned long long pos_ticks) {
    post_playstate("/Sessions/Playing/Stopped", item_id, session_id,
                   pos_ticks, false);
}

// ---------------------------------------------------------------------------
// Off-thread progress reporting
// ---------------------------------------------------------------------------
//
// post_playstate() is a full HTTP round trip: connect, POST, wait for the
// response.  That is fine on a thread whose only job is waiting, and it is
// WRONG on two threads that were calling it anyway:
//
//   * the music pump thread, which is the only thing refilling a PCM ring
//     that holds 683 ms.  Its 10-second progress heartbeat blocked there, so
//     a server that took longer than the ring's remaining margin to answer
//     produced an audible dropout -- once every ten seconds, which is exactly
//     what "it stops for a second sometimes" sounds like;
//   * the music screen's input handler, which runs inside the render loop.
//     Pressing play/pause posted the new state immediately "so the server UI
//     flips too", and froze every frame until the server replied.
//
// This is that call, moved onto a thread of its own.  It is a ONE-SLOT
// mailbox, not a queue: reports are absolute state, not events, so when two
// arrive before either is sent the older one is worthless -- and coalescing is
// what keeps a held-down pause button from queueing a dozen round trips.
//
// Deliberately not applied to jellyfin_report_playing/stopped: those are
// once-per-track, already on threads that can afford to wait, and `stopped`
// must land before the app tears the session down.
static sys_ppu_thread_t s_rep_tid;
static sys_mutex_t      s_rep_mtx;
static volatile int     s_rep_run     = 0;
static volatile int     s_rep_pending = 0;
static int              s_rep_started = 0;

static struct {
    char item[64];
    char sess[64];
    u64  ticks;
    bool paused;
} s_rep_slot;

static void report_thread(void *arg) {
    (void)arg;
    while (s_rep_run) {
        int have = 0;
        char item[64], sess[64];
        u64  ticks = 0;
        bool paused = false;

        sysMutexLock(s_rep_mtx, 0);
        if (s_rep_pending) {
            memcpy(item, s_rep_slot.item, sizeof item);
            memcpy(sess, s_rep_slot.sess, sizeof sess);
            ticks  = s_rep_slot.ticks;
            paused = s_rep_slot.paused;
            s_rep_pending = 0;
            have = 1;
        }
        sysMutexUnlock(s_rep_mtx);

        if (have)
            post_playstate("/Sessions/Playing/Progress", item, sess, ticks, paused);
        else
            usleep(50000);          // 50 ms; nothing is waiting on this
    }
    sysThreadExit(0);
}

// Start the worker.  EXPLICIT, from a single-threaded moment (music_start),
// rather than lazily on first use: the two callers are the stream thread's
// heartbeat and the render thread's pause handler, and if they had ever raced
// here on the first report they would each have created a mutex and a thread,
// leaving one of them locking a handle the other had overwritten.
//
// 64 KB of stack because this thread runs http_request(), whose frame alone is
// 5,376 bytes -- measured, with post_playstate above it and build_headers and
// newlib's printf below.  The other thread in this app that calls into the
// same path (thumb_fetch) is given exactly this, and the stream thread that
// used to make these calls had 128 KB.  16 KB was ~60% consumed at peak, which
// is not a margin.
void jellyfin_report_init(void) {
    if (s_rep_started) return;
    sys_mutex_attr_t mattr;
    sysMutexAttrInitialize(mattr);
    if (sysMutexCreate(&s_rep_mtx, &mattr) != 0) {
        // No mailbox, no thread: fall back to the blocking call rather
        // than silently dropping the report.  A stutter beats a server
        // session that thinks nothing is playing.
        plog("playstate: async mutex failed, reporting inline");
        s_rep_started = -1;
        return;
    }
    s_rep_pending = 0;
    s_rep_run     = 1;
    s_rep_started = 1;
    sysThreadCreate(&s_rep_tid, report_thread, NULL,
                    1000, 0x10000, THREAD_JOINABLE, (char*)"jf_report");
    plog("playstate: report thread up");
}

void jellyfin_report_progress_async(const char *item_id, const char *session_id,
                                    unsigned long long pos_ticks, bool paused) {
    if (!item_id || !item_id[0]) return;

    if (s_rep_started != 1) {
        post_playstate("/Sessions/Playing/Progress", item_id, session_id,
                       pos_ticks, paused);
        return;
    }

    sysMutexLock(s_rep_mtx, 0);
    snprintf(s_rep_slot.item, sizeof s_rep_slot.item, "%s", item_id);
    snprintf(s_rep_slot.sess, sizeof s_rep_slot.sess, "%s",
             session_id ? session_id : "");
    s_rep_slot.ticks  = pos_ticks;
    s_rep_slot.paused = paused;
    s_rep_pending     = 1;
    sysMutexUnlock(s_rep_mtx);
}

// Wait briefly for a queued report to go out.  It does NOT stop the thread.
//
// It used to join it, and that was a deadlock I walked into: this runs on the
// render thread on the way out of the music screen, while the stream thread is
// on ITS way out making blocking report_stopped/stop_transcode calls that hold
// the global HTTP mutex.  The worker wanting that mutex cannot reach its
// `while (s_rep_run)` check, so the join waits on a thread that is waiting on a
// thread the join is blocking behind.  After a long pause -- when the server
// has already discarded the transcode and those calls take their full timeout
// -- that is a visibly hung app during teardown.
//
// So the worker now lives for the process. It costs a 20 Hz poll of one int,
// and nothing has to be torn down at exactly the wrong moment. Not destroying
// the mutex also removes the other hazard here: any late report arriving after
// a destroy would have locked freed handle.
void jellyfin_report_flush(void) {
    if (s_rep_started != 1) return;
    for (int i = 0; i < 20 && s_rep_pending; i++) usleep(25000);
}
