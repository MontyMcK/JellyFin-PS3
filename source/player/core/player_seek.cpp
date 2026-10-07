// Seek (FF/REW) — R2/L2 tap/hold input machine and the flush-and-reopen
// seek execution, modelled on Movian's mp_flush path.
//
// Input is driven by the plain DIGITAL r2/l2 bit (no analog pressure),
// grace-bridged to ride out the bit's frame-to-frame flicker:
//   * quick TAP  -> +10s skip, batched by a 1s gate so taps stack into one
//                   reopen further ahead.
//   * HOLD       -> pause and scrub the seek bar +25s/-25s every 250ms. The
//                   STREAM itself is never re-fetched while held — the
//                   single reopen to the scrubbed spot happens on release —
//                   but a trickplay preview thumbnail is (see trickplay.h),
//                   since that is a separate, much cheaper request the
//                   server already has sitting on disk.
// The D-pad also gives +10s taps via the HUD.

#include <stdio.h>
#include <string.h>

#include <ppu-types.h>
#include <sys/thread.h>
#include <net/net.h>
#include <sys/socket.h>
#include <sys/time.h>

#include "player_internal.h"
#include "stream.h"
#include "audio.h"
#include "adec.h"
#include "video.h"
#include "timing.h"
#include "plog.h"
#include "subtitles.h"
#include "ui.h"
#include "jellyfin_api.h"
#include "slog.h"
#include "trickplay.h"

extern void crash_log(const char *msg);

// Use brute-force vdec_close/vdec_open on seek instead of EndSequence/
// StartSequence.  Slower (~200 ms) but guarantees clean SPU state — the
// in-place EndSequence/StartSequence flush leaves the decoder unable to
// produce frames after a seek (video freezes while audio keeps playing).
#define SEEK_REOPEN_VDEC 1

// Set true before the seek flush window, false after the decode thread is
// respawned.
static volatile bool s_seeking = false;

// A seek moves the clock backwards as often as forwards, and the cue lookup
// walks forward from where it last was.  Telling it to start over costs one
// binary search and keeps the wrong line from lingering after a jump.
static void subs_after_seek(void) { subs_reset_cursor(); }

// Absolute media time (ms) the currently-accumulated scrub offset points
// at -- same base+clock+pending_secs arithmetic player_execute_seek() uses
// to compute its real seek target, just not yet committed to a reopen.
static u32 scrub_target_ms(const PlayerState *ps) {
    s64 cur_us    = (s64)ps->play_base_us + (s64)audio_get_clock_us();
    s64 target_us = cur_us + (s64)ps->seek.pending_secs * 1000000LL;
    if (target_us < 0) target_us = 0;
    return (u32)(target_us / 1000);
}

static const u64 SEEK_HOLD_DELAY_US = 400000ULL;   // held longer than this -> scrub
static const u64 SEEK_SCRUB_STEP_US = 250000ULL;   // one scrub step per 250ms
static const s32 SEEK_SCRUB_SECS    = 25;          // +25s per scrub step
static const s32 SEEK_TAP_SECS      = 10;          // +10s per quick tap
static const u64 SEEK_GRACE_US      = 250000ULL;   // bridge digital-bit flicker
static const u64 SEEK_TAP_GATE_US   = 450000ULL;   // batch window for quick taps (was 1 s: felt laggy)

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
            trickplay_scrub_update(ps->item->id, ps->source.id,
                                   scrub_target_ms(ps));
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
            trickplay_release_sheet();   // give its memory back -- see trickplay.cpp
        } else {
            sk->dir = dir;
            if (now - sk->scrub_step_us >= SEEK_SCRUB_STEP_US) {
                sk->scrub_step_us = now;
                sk->pending_secs += dir * SEEK_SCRUB_SECS;   // move bar +25s
                trickplay_scrub_update(ps->item->id, ps->source.id,
                                       scrub_target_ms(ps));
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
        plog("playing=0 reason=seek_vdec_open_failed");
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
    // The old transcode is stopped AFTER the new session exists, off this
    // thread (2026-09-27: it cost a whole HTTP round trip per seek).  Only if
    // no new session can be had is it stopped here, first -- the same id
    // would make Jellyfin reuse the old job and ignore StartTimeTicks.
    char old_sid[64];
    snprintf(old_sid, sizeof old_sid, "%s", ps->session_id);
    // Stopping the transcode is not enough on its own: re-requesting
    // stream.ts with the SAME PlaySessionId makes Jellyfin reuse the
    // existing transcode job (which began at offset 0), so
    // StartTimeTicks is silently ignored and the video restarts from
    // 0:00.  Mint a fresh PlaySessionId via PlaybackInfo so the server
    // spins up a brand-new transcode anchored at the seek target.
    {
        char new_session[64] = "";
        JFMediaSource opened;
        const JFMediaSource *chosen = player_current_source(ps);
        char source_id[96] = "";
        if (chosen) snprintf(source_id, sizeof(source_id), "%s", chosen->id);
        unsigned source_runtime = 0;
        if (jellyfin_get_playback_info(ps->item->id, source_id,
                                       new_session, sizeof(new_session),
                                       &source_runtime, NULL, &opened) &&
            new_session[0]) {
            snprintf(ps->session_id, sizeof(ps->session_id), "%s", new_session);
            char sb[96];
            snprintf(sb, sizeof(sb), "seek: new session=%s", ps->session_id);
            plog(sb);

            // AutoOpenLiveStream may resolve a plugin source to a LiveStreamId
            // (and occasionally a new MediaSourceId).  Carry that resolution
            // into the stream.ts request while preserving the user's current
            // audio/subtitle selections across an ordinary seek.
            if (opened.id[0]) {
                JFMediaSource *dst = &ps->source;
                char old_label[128];
                snprintf(old_label, sizeof(old_label), "%s", dst->label);
                snprintf(dst->id, sizeof(dst->id), "%s", opened.id);
                snprintf(dst->live_stream_id, sizeof(dst->live_stream_id),
                         "%s", opened.live_stream_id);
                if (!dst->label[0])
                    snprintf(dst->label, sizeof(dst->label), "%s", opened.label);
                else
                    snprintf(dst->label, sizeof(dst->label), "%s", old_label);
                if (source_runtime > 0) dst->runtime_secs = source_runtime;
                if (dst->runtime_secs > 0) ps->total_secs = dst->runtime_secs;
            }
        } else {
            plog("seek: PlaybackInfo failed, reusing old session");
            jellyfin_stop_transcode(old_sid);     // the same id: that job must die first
        }
    }
    // The old transcode is stopped FIRST, and its source connection given a
    // moment to close (2026-09-27).  Stopping it off-thread after the new one
    // had started left two connections on the same debrid file for a moment,
    // and TorBox answered the second with a 5 s placeholder clip: the new
    // transcode encoded nothing and playback ended (an audio-track switch,
    // and a fast-forward before it).
    if (strcmp(old_sid, ps->session_id) != 0) {
        jellyfin_stop_transcode(old_sid);
        usleep(600000);
    }
    char surl[768];
    build_stream_url(surl, sizeof(surl), ps, start_ticks);
    plog_url("surl", surl);
    // 3b/4) Open and re-prime, retrying with a growing wait.
    //
    // A debrid host rate-limits opens of the same file: the server's ffmpeg
    // got "HTTP 429 Too Many Requests" after four opens in ~90 s (start,
    // audio switch, seek, retry), answered the reopen with a 200 and no body,
    // then a 500 -- and one 2 s retry later playback was dumped back at the
    // menu (2026-09-27).  So: up to four tries, 5 / 10 / 15 s apart, the last
    // picture and the spinner up the whole time, Circle to give up.
    static const unsigned RETRY_WAIT_MS[] = { 5000, 10000, 15000 };
    const int max_tries = 1 + (int)(sizeof RETRY_WAIT_MS / sizeof RETRY_WAIT_MS[0]);
    int nsock = -1;
    for (int attempt = 0; ; attempt++) {
        stream_set_wait_cb(player_seek_wait);      // spinner + Circle while the server thinks
        nsock = stream_open(surl);
        stream_set_wait_cb(NULL);
        const bool cancelled = nsock < 0 &&
            strstr(stream_last_error(), "Cancelled") != NULL;
        if (nsock >= 0) {
            ps->sock = nsock;
            // The new stream's PTS restarts at ~0, so its clock now maps to
            // absolute media time target_us.
            ps->play_base_us = (u64)target_us;
            { struct timeval tv; tv.tv_sec = 0; tv.tv_usec = 5000;   // the lv2 layout (16 bytes)
              netSetSockOpt(ps->sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)); }
            crash_log("sk4 reopened");
            if (attempt > 0) video_reset_demux();
            // Re-prime: decode a few frames before resuming display so the
            // jitter buffer is non-empty (mirrors initial pre-fill).
            player_prefill(ps, false, 20000);
            if (jbuf_count() > 0 || !ps->playing || !running) break;
            plog("seek: reopen gave no picture (the server found nothing to send)");
            netClose(ps->sock);
            ps->sock = -1;
        } else {
            char b[160];
            snprintf(b, sizeof b, "seek: stream_open FAILED (%.100s)", stream_last_error());
            plog(b);
        }
        if (cancelled || attempt + 1 >= max_tries || !running) {
            plog("playing=0 reason=seek_stream_open_failed");
            crash_log("sk_fail reopen");
            ps->sock = -1;
            ps->playing = false;       // give up cleanly; loop will exit
            return false;
        }
        // Kill the job that produced nothing (the same PlaySessionId would be
        // handed it again) and give the source host time to calm down.
        jellyfin_stop_transcode(ps->session_id);
        const unsigned wait_ms = RETRY_WAIT_MS[attempt];
        {
            char b[96];
            snprintf(b, sizeof b, "seek: asking again in %u s (try %d of %d)",
                     wait_ms / 1000, attempt + 2, max_tries);
            plog(b);
        }
        const u64 w0 = timing_get_us();
        bool gave_up = false;
        while (timing_get_us() - w0 < (u64)wait_ms * 1000ULL) {
            if (!player_seek_wait((unsigned)((timing_get_us() - w0) / 1000ULL))) { gave_up = true; break; }
            usleep(30000);
        }
        if (gave_up) {
            plog("playing=0 reason=seek_cancelled_by_user");
            ps->sock = -1;
            ps->playing = false;
            return false;
        }
    }
    subs_after_seek();      // the cue cursor must not walk on from the old spot
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
