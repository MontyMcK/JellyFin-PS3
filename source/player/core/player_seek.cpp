// Seek (FF/REW) — R2/L2 tap/hold input machine and the flush-and-reopen
// seek execution, modelled on Movian's mp_flush path.
//
// Input is driven by the plain DIGITAL r2/l2 bit (no analog pressure),
// grace-bridged to ride out the bit's frame-to-frame flicker:
//   * quick TAP  -> +10s skip, batched by a 1s gate so taps stack into one
//                   reopen further ahead.
//   * HOLD       -> pause and scrub the seek bar +25s/-25s every 250ms.  While
//                   held NOTHING is fetched — only the bar moves; the single
//                   reopen to the scrubbed spot happens when the user releases.
// The D-pad also gives +10s taps via the HUD.

#include <stdio.h>
#include <string.h>

#include <ppu-types.h>
#include <sys/thread.h>
#include <net/net.h>
#include <sys/socket.h>

#include "player_internal.h"
#include "stream.h"
#include "audio.h"
#include "adec.h"
#include "video.h"
#include "timing.h"
#include "plog.h"
#include "ui.h"
#include "jellyfin_api.h"
#include "slog.h"

extern void crash_log(const char *msg);

// Use brute-force vdec_close/vdec_open on seek instead of EndSequence/
// StartSequence.  Slower (~200 ms) but guarantees clean SPU state — the
// in-place EndSequence/StartSequence flush leaves the decoder unable to
// produce frames after a seek (video freezes while audio keeps playing).
#define SEEK_REOPEN_VDEC 1

// Set true before the seek flush window, false after the decode thread is
// respawned.
static volatile bool s_seeking = false;

static const u64 SEEK_HOLD_DELAY_US = 400000ULL;   // held longer than this -> scrub
static const u64 SEEK_SCRUB_STEP_US = 250000ULL;   // one scrub step per 250ms
static const s32 SEEK_SCRUB_SECS    = 25;          // +25s per scrub step
static const s32 SEEK_TAP_SECS      = 10;          // +10s per quick tap
static const u64 SEEK_GRACE_US      = 250000ULL;   // bridge digital-bit flicker
static const u64 SEEK_TAP_GATE_US   = 1000000ULL;  // batch window for quick taps

// -------------------------------------------------------
// Input
// -------------------------------------------------------

void player_seek_queue_tap(PlayerState *ps, int delta_secs) {
    ps->seek.pending_secs += delta_secs;
    ps->seek.tap_gate_us   = timing_get_us() + SEEK_TAP_GATE_US;
}

// ---- R2/L2: quick tap = +10s skip; hold = pause + scrub ----
// The D-pad no longer scrubs here — left/right drive HUD focus navigation
// (see hud_handle_input).  R2/L2 are the scrub controls.
HudAction player_seek_input_update(PlayerState *ps, HudAction act) {
    PlayerSeekInput *sk = &ps->seek;

    int dir = 0;
    if      (btn_cur.r2) dir = +1;
    else if (btn_cur.l2) dir = -1;
    u64 now = timing_get_us();
    if (dir != 0) sk->active_us = now;
    // Ride over brief 1-frame dropouts so a real hold isn't chopped.
    if (dir == 0 && sk->dir != 0 && now - sk->active_us < SEEK_GRACE_US)
        dir = sk->dir;

    switch (sk->state) {
    case SEEK_IDLE:
        if (dir != 0) {
            sk->state    = SEEK_PRESS;
            sk->dir      = dir;
            sk->press_us = now;
        }
        break;

    case SEEK_PRESS:
        if (dir == 0) {
            // Quick press/release = TAP: +10s, batched by the 1s gate.
            sk->pending_secs += sk->dir * SEEK_TAP_SECS;
            sk->tap_gate_us = now + SEEK_TAP_GATE_US;
            sk->state       = SEEK_IDLE;
            sk->dir         = 0;
        } else if (now - sk->press_us >= SEEK_HOLD_DELAY_US) {
            // Held: pause and scrub.  Nothing is fetched while held — the
            // bar just moves; the reopen waits for release.
            sk->state         = SEEK_SCRUB;
            sk->dir           = dir;
            sk->scrub_resume  = !ps->paused;
            ps->paused        = true;
            sk->scrub_step_us = now - SEEK_SCRUB_STEP_US;   // first step now
            sk->tap_gate_us   = 0;
            plog("seek: scrub begin");
        } else {
            sk->dir = dir;             // allow F<->B before it commits
        }
        break;

    case SEEK_SCRUB:
        if (dir == 0) {
            // Released: NOW fetch — one reopen to the scrubbed spot, resume.
            sk->state = SEEK_IDLE;
            sk->dir   = 0;
            if (sk->pending_secs != 0) {
                sk->commit_now        = true;
                sk->resume_after_seek = sk->scrub_resume;
            } else {
                ps->paused = !sk->scrub_resume;
            }
            plog("seek: scrub end");
        } else {
            sk->dir = dir;
            if (now - sk->scrub_step_us >= SEEK_SCRUB_STEP_US) {
                sk->scrub_step_us = now;
                sk->pending_secs += dir * SEEK_SCRUB_SECS;   // move bar +25s
            }
        }
        break;
    }

    // Commit one reopen: on scrub release, or when the tap gate expires.
    // Never mid-scrub — a hold only moves the bar until the user lets go.
    if (act == HUD_ACTION_NONE && sk->pending_secs != 0 &&
        sk->state != SEEK_SCRUB &&
        (sk->commit_now ||
         (sk->tap_gate_us != 0 && timing_get_us() >= sk->tap_gate_us))) {
        sk->commit_now  = false;
        sk->tap_gate_us = 0;
        act             = HUD_ACTION_SEEK;
    }
    return act;
}

// -------------------------------------------------------
// Execution — flush, stop transcode, reopen at the target
// -------------------------------------------------------

bool player_execute_seek(PlayerState *ps) {
    int delta = ps->seek.pending_secs;   // total accumulated during cooldown
    ps->seek.pending_secs = 0;
    {
        char buf[64];
        snprintf(buf, sizeof(buf), "hud: seek %+d s (begin)", delta);
        plog(buf);
    }
    crash_log("sk1 seek begin");

    // Compute target from the ABSOLUTE position (base + stream clock),
    // not the stream-relative audio clock — otherwise each seek would
    // be measured from the post-seek stream's PTS 0 and walk backward.
    s64 cur_us    = (s64)ps->play_base_us + (s64)audio_get_clock_us();
    s64 target_us = cur_us + (s64)delta * 1000000LL;
    if (target_us < 0) target_us = 0;
    if (ps->total_secs > 0 &&
        target_us > (s64)ps->total_secs * 1000000LL)
        target_us = (s64)ps->total_secs * 1000000LL;
    u64 start_ticks = (u64)target_us * 10ULL;  // us -> 100-ns ticks
    {
        char buf[176];
        snprintf(buf, sizeof(buf),
            "seek_calc: base=%llus clk=%llus delta=%ds -> target=%llus "
            "ticks=%llu total=%us",
            (unsigned long long)(ps->play_base_us / 1000000ULL),
            (unsigned long long)(audio_get_clock_us() / 1000000ULL),
            delta,
            (unsigned long long)((u64)target_us / 1000000ULL),
            (unsigned long long)start_ticks,
            ps->total_secs);
        plog(buf);
    }
    slog_state("SEEK delta=%d target=%llds total=%us",
               delta, (long long)(target_us / 1000000LL), ps->total_secs);

    // 1) Stop the decode thread so nothing feeds VDEC mid-flush.
    //    Use the decode-only flag so the audio + upload threads
    //    keep running (they idle on empty buffers), matching
    //    Movian's flush-while-primary model.
    bool was_paused = ps->paused;
    ps->paused  = true;            // gate audio output during the seek
    s_seeking   = true;            // gate upload thread during flush window
    ps->dec_run = false;           // signal ONLY the decode thread
    __asm__ volatile("sync" ::: "memory");
    if (ps->dec_tid) {
        u64 tret;
        sysThreadJoin(ps->dec_tid, &tret);
        ps->dec_tid = 0;
    }
    crash_log("sk2 dec joined");

    // 2) Flush decoder + audio + jitter buffer (Movian mp_flush).
#ifdef SEEK_REOPEN_VDEC
    // Brute-force path: tear down and rebuild the decoder instead
    // of EndSequence/StartSequence.  Avoids any stale SPU state.
    vdec_close();
    crash_log("sk2a vdec_close done");
    if (!vdec_open()) {
        crash_log("sk2b vdec_open FAILED");
        s_seeking   = false;
        ps->playing = false;
        return false;
    }
    crash_log("sk2c vdec_open done");
    vdec_reset_counters();
#else
    vdec_flush();
#endif
    adec_flush();
    crash_log("jc1 jbuf_clear");
    jbuf_clear();
    crash_log("jc2 jbuf_clear done");
    video_reset_demux();
    avsync_reset();
    s_vid_frame_ready = false;
    s_vid_b_present   = false;
    crash_log("sk3 flushed");

    // 3) Re-request the stream at the new offset.
    netClose(ps->sock);
    // Kill the existing transcode first, otherwise Jellyfin keeps
    // serving the in-progress job (which started at offset 0) and
    // the seek appears to reset to 0:00 instead of honouring the
    // new StartTimeTicks.
    jellyfin_stop_transcode(ps->session_id);
    // Stopping the transcode is not enough on its own: re-requesting
    // stream.ts with the SAME PlaySessionId makes Jellyfin reuse the
    // existing transcode job (which began at offset 0), so
    // StartTimeTicks is silently ignored and the video restarts from
    // 0:00.  Mint a fresh PlaySessionId via PlaybackInfo so the server
    // spins up a brand-new transcode anchored at the seek target.
    {
        char new_session[64] = "";
        if (jellyfin_get_play_session_id(ps->item->id, new_session,
                                         sizeof(new_session), NULL) &&
            new_session[0]) {
            snprintf(ps->session_id, sizeof(ps->session_id), "%s", new_session);
            char sb[96];
            snprintf(sb, sizeof(sb), "seek: new session=%s", ps->session_id);
            plog(sb);
        } else {
            plog("seek: PlaybackInfo failed, reusing old session");
        }
    }
    char surl[768];
    build_stream_url(surl, sizeof(surl), ps, start_ticks);
    plog_url("surl", surl);
    int nsock = stream_open(surl);
    if (nsock < 0) {
        plog("seek: stream_open FAILED");
        crash_log("sk_fail reopen");
        ps->playing = false;       // give up cleanly; loop will exit
        return false;
    }
    ps->sock = nsock;
    // The new stream's PTS restarts at ~0, so its clock now maps to
    // absolute media time target_us.
    ps->play_base_us = (u64)target_us;
    { struct { u32 sec; u32 usec; } tv = { 0, 5000 };
      setsockopt(ps->sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)); }
    crash_log("sk4 reopened");

    // 4) Re-prime: decode a few frames before resuming display so
    //    the jitter buffer is non-empty (mirrors initial pre-fill).
    player_prefill(ps, false, 20000);
    crash_log("sk5 prefilled");
    {
        // What timestamps did Jellyfin actually return for the new
        // stream?  If audio/video PTS come back near 0, the transcode
        // restarted at the offset with a reset clock (expected). If
        // they come back near `target`, Jellyfin kept absolute PTS.
        char buf[160];
        snprintf(buf, sizeof(buf),
            "seek_post: jbuf=%d aud_pts=%lluus vid_pts=%lluus base=%llus",
            jbuf_count(),
            (unsigned long long)audio_get_clock_us(),
            (unsigned long long)jbuf_peek_pts_us(),
            (unsigned long long)(ps->play_base_us / 1000000ULL));
        plog(buf);
    }

    // 5) Respawn the decode thread with the new socket.
    if (!player_spawn_decode(ps)) {
        plog("seek: dec thread_create FAILED");
        return false;
    }

    // 6) Resume (unless the user had paused before seeking).
    s_seeking  = false;            // flush window over
    ps->paused = was_paused;
    {
        char buf[80];
        snprintf(buf, sizeof(buf),
                 "hud: seek done target=%llds clk_was=%llds",
                 (long long)(target_us / 1000000),
                 (long long)(cur_us / 1000000));
        plog(buf);
    }
    crash_log("sk6 seek done");
    slog_state("SEEK_DONE target=%llds jbuf=%d",
               (long long)(target_us / 1000000LL), jbuf_count());
    ps->seek_dbg_frames = 120;   // log resumed position for ~2s
    if (ps->paused) ps->show_seek_frame = true;  // reveal target while paused
    // A scrub paused the video to race the position; now that its seek
    // has landed, resume playback if it had been playing.
    if (ps->seek.resume_after_seek) {
        ps->paused = false;
        ps->seek.resume_after_seek = false;
    }
    return true;
}
