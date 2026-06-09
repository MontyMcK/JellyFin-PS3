








// player.cpp — Jellyfin PS3 media player orchestrator

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <ppu-types.h>
#include <io/pad.h>
#include <sysutil/sysutil.h>
#include <sysutil/video.h>
#include <net/net.h>
#include <sys/socket.h>
#include <sys/thread.h>
#include <unistd.h>

#include "plog.h"
#include "stream.h"
#include "audio.h"
#include "adec.h"
#include "video.h"
#include "timing.h"
#include "player.h"
#include "player_hud.h"
#include "player_internal.h"
#include "ui.h"
#include "ui_visuals.h"
#include "jellyfin_api.h"
#include "rsxutil.h"

extern void crash_log(const char *msg);

// Use brute-force vdec_close/vdec_open on seek instead of EndSequence/
// StartSequence.  Slower (~200 ms) but guarantees clean SPU state — the
// in-place EndSequence/StartSequence flush leaves the decoder unable to
// produce frames after a seek (video freezes while audio keeps playing).
#define SEEK_REOPEN_VDEC 1

// Set true before the seek flush window, false after the decode thread is
// respawned.  Declared extern in player_internal.h; read by upload_thread_fn.
volatile bool s_seeking = false;

// -------------------------------------------------------
// Helper — wait for O with error message
// -------------------------------------------------------

static void show_error(const char *line1, const char *line2) {
    drawHeader();
    drawText(40, 100, line1);
    if (line2 && line2[0]) drawText(40, 130, line2);
    drawText(40, 160, "O: back");
    flip();
    init_btns();
    while (running) {
        sysUtilCheckCallback();
        poll_buttons();
        if (BTN_PRESSED(circle)) return;
    }
}

// plog() truncates every line at 127 chars, so a 400+ char stream URL is cut
// off well before StartTimeTicks/PlaySessionId.  Log it in ~100-char chunks so
// the full query string is visible on the wire.
static void plog_url(const char *tag, const char *url) {
    int len = (int)strlen(url);
    char buf[128];
    int part = 0;
    for (int off = 0; off < len; off += 100, part++) {
        snprintf(buf, sizeof(buf), "%s[%d]: %.100s", tag, part, url + off);
        plog(buf);
    }
}

// -------------------------------------------------------
// Stream URL builder — used for the initial open and for every seek.
// start_ticks is in Jellyfin's 100-ns units (seconds * 10,000,000).
// The query string is otherwise identical so the server keeps the same
// transcode session and we only change the start offset.
// -------------------------------------------------------
static void build_stream_url(char *url, int url_sz,
                             const JFItem *item, u32 req_w, u32 req_h,
                             const char *session_id, u64 start_ticks) {
    int n = snprintf(url, url_sz,
        "%s/Videos/%s/stream.ts"
        "?VideoCodec=h264"
        "&VideoProfile=baseline"
        "&VideoLevel=31"
        "&MaxWidth=%u&MaxHeight=%u"
        "&VideoBitrate=4000000"
        "&AudioCodec=mp3&AudioBitrate=192000&AudioSampleRate=48000"
        "&MaxAudioChannels=2"
        "&MaxFramerate=30"
        "&DeviceId=ps3&Static=false"
        "&MediaSourceId=%s"
        "&StartTimeTicks=%llu",
        g_server, item->id, req_w, req_h, item->id,
        (unsigned long long)start_ticks);
    if (session_id && session_id[0] && n > 0 && n < url_sz) {
        snprintf(url + n, url_sz - n, "&PlaySessionId=%s", session_id);
    }
}

// -------------------------------------------------------
// show_player — public entry point
// -------------------------------------------------------

void show_player(const JFItem *item) {
    crash_log("p1 enter");
    plog("show_player: enter");
    plog("show_player: BUILD=seek-diag-1");
    init_btns();
    {
        static bool s_logged_cbufs = false;
        if (!s_logged_cbufs) {
            s_logged_cbufs = true;
            char buf[128];
            snprintf(buf, sizeof(buf), "color_buffer[0]=%p", (void*)color_buffer[0]);
            plog(buf);
            snprintf(buf, sizeof(buf), "color_buffer[1]=%p", (void*)color_buffer[1]);
            plog(buf);
        }
    }

    // H.264 level 3.1 caps at 1280×720 @ 30fps
    u32 req_w = display_width  < 1280 ? display_width  : 1280;
    u32 req_h = display_height < 720  ? display_height : 720;

    char     session_id[64] = "";
    unsigned total_secs     = 0;
    if (!jellyfin_get_play_session_id(item->id, session_id, sizeof(session_id),
                                      &total_secs)) {
        plog("show_player: PlaybackInfo failed, streaming without PlaySessionId");
        session_id[0] = '\0';
    }

    char url[640];
    build_stream_url(url, sizeof(url), item, req_w, req_h, session_id, 0);
    plog_url("url", url);

    drawHeader();
    drawTextf(40, 100, "%.70s", item->name);
    drawText(40, 130, "Initializing decoder...");
    flip();

    crash_log("p2 vdec_open begin");
    plog("show_player: vdec_open");
    if (!vdec_open()) {
        plog("show_player: vdec_open FAILED");
        vdec_close();
        show_error("VDEC init failed.", "See /dev_hdd0/tmp/player_log.txt");
        ui_restore_rsx_state();
        return;
    }
    plog("show_player: vdec_open OK");
    crash_log("p3 vdec_open OK");

    crash_log("p4 audio_open begin");
    plog("show_player: audio_open");
    audio_open();
    adec_init();
    adec_start();
    plog("show_player: audio_open done");
    crash_log("p5 audio_open OK");

    drawHeader();
    drawTextf(40, 100, "%.70s", item->name);
    drawText(40, 130, "Connecting to stream...");
    flip();

    crash_log("p6 stream_open begin");
    plog("show_player: stream_open");
    int sock = stream_open(url);
    if (sock < 0) {
        plog("show_player: stream_open FAILED");
        adec_stop();
        audio_close();
        vdec_close();
        show_error("Stream connection failed.", url);
        ui_restore_rsx_state();
        return;
    }
    plog("show_player: stream_open OK");
    crash_log("p7 stream_open OK");

    drawHeader();
    drawTextf(40, 100, "%.70s", item->name);
    drawText(40, 130, "Streaming... START=stop");
    flip();

    video_reset();

    if (!jbuf_alloc(req_w, req_h)) {
        plog("show_player: jbuf_alloc FAILED");
        netClose(sock);
        adec_stop();
        audio_close();
        vdec_close();
        ui_restore_rsx_state();
        return;
    }
    {
        char buf[72];
        snprintf(buf, sizeof(buf), "jbuf: %d slots %ux%u ~%u KB each",
                 JBUF_SIZE, req_w, req_h, req_w * req_h * 4 / 1024);
        plog(buf);
    }
    for (int i = 0; i < JBUF_SIZE; i++) {
        char buf[64];
        snprintf(buf, sizeof(buf), "jbuf_addr: slot=%d ptr=%p", i, (void*)jbuf_slot_ptr(i));
        plog(buf);
    }
    crash_log("p8 jbuf alloc OK");

    // Video GPU blit init — allocate RSX buffers once per session
    vid_gpu_init(jbuf_fw(), jbuf_fh());

    // 5 ms socket receive timeout keeps the network thread responsive
    { struct { u32 sec; u32 usec; } tv = { 0, 5000 };
      setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)); }

    u8            ts_pkt[TS_PACKET_SIZE];
    volatile bool playing    = true;
    volatile bool paused     = false;
    volatile bool dec_run    = true;   // decode-thread stop flag (toggled on seek)
    bool          first_pkt  = true;
    int           frame_count = 0;
    int           rd          = 0;

    // Absolute media time (us) corresponding to the current stream's PTS 0.
    // Jellyfin restarts the transcode PTS at 0 on the initial open and on every
    // seek, so the audio clock alone is stream-relative. Absolute position is
    // play_base_us + audio_get_clock_us(); the base advances on each seek.
    u64           play_base_us   = 0;
    int           seek_dbg_frames = 0;   // >0: log HUD elapsed for this many frames

    hud_init(total_secs, NULL);
    plog("hud: prewarm start");
    ttf_prewarm_hud();
    plog("hud: prewarm done");

    // ---- Pre-fill: decode JBUF_PREFILL frames before display starts ----
    plog("jbuf: pre-fill start");
    while (jbuf_count() < JBUF_PREFILL && running && !s_vdec_error) {
        sysUtilCheckCallback();
        rd = stream_read(sock, ts_pkt, TS_PACKET_SIZE);
        if (rd < 0) {
            plog("playing=0 reason=net_error");
            playing = false; break;
        }
        if (rd > 0) {
            if (first_pkt) {
                char buf[56];
                snprintf(buf, sizeof(buf), "show_player: first pkt byte=0x%02x", ts_pkt[0]);
                plog(buf);
                first_pkt = false;
            }
            video_feed_ts(ts_pkt);
        }
    }

    if (!playing) {
        vid_gpu_free();
        jbuf_free();
        netClose(sock);
        adec_stop();
        audio_close();
        vdec_close();
        return;
    }

    plog("jbuf: pre-fill done — starting threads");
    timing_register_vblank();

    // ---- Spawn decode thread (Step 2) ----
    DecodeCtx        dec_ctx = { &playing, &frame_count, sock, &dec_run };
    sys_ppu_thread_t dec_tid = 0;
    if (playing) {
        int trc = sysThreadCreate(&dec_tid, decode_thread_fn,
                                  (void *)&dec_ctx,
                                  800, 128 * 1024,
                                  0, "jf_decode");
        if (trc != 0) {
            char buf[64];
            snprintf(buf, sizeof(buf), "show_player: dec thread_create FAILED rc=%d", trc);
            plog(buf);
            playing = false;
        }
    }

    // ---- Spawn audio thread (Step 6b) ----
    AudioCtx         aud_ctx = { &playing, &paused };
    sys_ppu_thread_t aud_tid = 0;
    if (playing) {
        int trc = sysThreadCreate(&aud_tid, audio_thread_fn,
                                  (void *)&aud_ctx,
                                  700, 32 * 1024,
                                  0, "jf_audio");
        if (trc != 0) {
            char buf[64];
            snprintf(buf, sizeof(buf), "show_player: aud thread_create FAILED rc=%d", trc);
            plog(buf);
            playing = false;
        }
    }

    // ---- Spawn upload thread — memcpy jbuf→RSX-local off the display thread ----
    UploadCtx        upl_ctx = { &playing, jbuf_fw(), jbuf_fh() };
    sys_ppu_thread_t upl_tid = 0;
    if (playing) {
        int trc = sysThreadCreate(&upl_tid, upload_thread_fn,
                                  (void *)&upl_ctx,
                                  850, 32 * 1024,
                                  0, "jf_upload");
        if (trc != 0) {
            char buf[64];
            snprintf(buf, sizeof(buf), "show_player: upl thread_create FAILED rc=%d", trc);
            plog(buf);
            playing = false;
        }
    }

    crash_log("p9 threads started");

    // Detection timeout tracking for Bresenham fallback initialisation
    u64 det_timeout_start = 0;

    // --- Seek (FF/REW) ---------------------------------------------------------
    // Driven by the plain DIGITAL r2/l2 bit (no analog pressure), grace-bridged to
    // ride out the bit's frame-to-frame flicker:
    //   * quick TAP  -> +10s skip, batched by a 1s gate so taps stack into one
    //                   reopen further ahead.
    //   * HOLD       -> pause and scrub the seek bar +25s/-25s every 250ms.  While
    //                   held NOTHING is fetched — only the bar moves; the single
    //                   reopen to the scrubbed spot happens when the user releases.
    // The D-pad also gives +10s taps via the HUD.
    enum SeekState { SEEK_IDLE, SEEK_PRESS, SEEK_SCRUB };
    const u64 SEEK_HOLD_DELAY_US = 400000ULL;   // held longer than this -> scrub
    const u64 SEEK_SCRUB_STEP_US = 250000ULL;   // one scrub step per 250ms
    const s32 SEEK_SCRUB_SECS    = 25;          // +25s per scrub step
    const s32 SEEK_TAP_SECS      = 10;          // +10s per quick tap
    const u64 SEEK_GRACE_US      = 250000ULL;   // bridge digital-bit flicker
    const u64 SEEK_TAP_GATE_US   = 1000000ULL;  // batch window for quick taps
    SeekState s_seek_state     = SEEK_IDLE;
    int  s_seek_dir            = 0;            // +1 = fwd, -1 = back
    bool s_seek_dpad           = false;        // this press is the D-pad (scrubs slower)
    u64  s_press_us            = 0;            // when the current press began
    u64  s_seek_active_us      = 0;            // last frame the trigger read as down
    u64  s_scrub_step_us       = 0;            // last scrub accumulation time
    s32  s_seek_pending_secs   = 0;            // queued offset, seconds
    u64  s_tap_gate_us         = 0;            // tap batch deadline (0 => none)
    bool s_commit_now          = false;        // release -> commit the reopen now
    bool s_scrub_resume        = false;        // was playing when this scrub began
    bool s_resume_after_seek   = false;        // unpause once the seek lands

    // When a seek lands while the video is PAUSED, the normal display gate (which
    // requires !paused) won't swap in the new frame, so the screen would stay
    // frozen on the pre-seek image.  This one-shot forces a single flip after a
    // paused seek so the user sees the target position.
    bool s_show_seek_frame     = false;

    crash_log("p10 loop");
    // ---- Main (display) loop ----
    while (running && playing && !s_vdec_error) {
        waitflip();
        sysUtilCheckCallback();

        static u64 s_loop_iter_us   = 0;
        static u64 s_loop_frame_dur = 0;
        { u64 now = timing_get_us();
          if (s_loop_iter_us) s_loop_frame_dur = now - s_loop_iter_us;
          s_loop_iter_us = now; }

        poll_buttons();
        bool l2_pressed = BTN_PRESSED(l2);
        bool r2_pressed = BTN_PRESSED(r2);
        if (BTN_PRESSED(start)) {
            plog("playing=0 reason=user_stop");
            playing = false;
        }
        if (!playing) break;

        {
            HudAction act = hud_handle_input(l2_pressed, r2_pressed, paused);

            // D-pad / focus-mode taps come through the HUD; queue them like any
            // tap.  R2/L2 are NOT handled here — the state machine below owns them
            // off the analog pressure, so their flickery digital edge can't fire
            // spurious taps.
            if (act == HUD_ACTION_SEEK) {
                s_seek_pending_secs += hud_seek_delta();
                s_tap_gate_us        = timing_get_us() + SEEK_TAP_GATE_US;
                act                  = HUD_ACTION_NONE;
            }

            // ---- R2/L2 + D-pad: quick tap = +10s skip; hold = pause + scrub ----
            // R2/L2 and the D-pad share one path; the D-pad just scrubs at half the
            // speed.  These are the ONLY player controls (plus START to stop).
            {
                int  dir   = 0;
                bool dpad  = false;
                if      (btn_cur.r2)    dir = +1;
                else if (btn_cur.l2)    dir = -1;
                else if (btn_cur.right) { dir = +1; dpad = true; }
                else if (btn_cur.left)  { dir = -1; dpad = true; }
                u64 now = timing_get_us();
                if (dir != 0) s_seek_active_us = now;
                // Ride over brief 1-frame dropouts so a real hold isn't chopped.
                if (dir == 0 && s_seek_dir != 0 && now - s_seek_active_us < SEEK_GRACE_US)
                    dir = s_seek_dir;

                switch (s_seek_state) {
                case SEEK_IDLE:
                    if (dir != 0) {
                        s_seek_state = SEEK_PRESS;
                        s_seek_dir   = dir;
                        s_seek_dpad  = dpad;
                        s_press_us   = now;
                    }
                    break;

                case SEEK_PRESS:
                    if (dir == 0) {
                        // Quick press/release = TAP: +10s, batched by the 1s gate.
                        s_seek_pending_secs += s_seek_dir * SEEK_TAP_SECS;
                        s_tap_gate_us = now + SEEK_TAP_GATE_US;
                        s_seek_state  = SEEK_IDLE;
                        s_seek_dir    = 0;
                    } else if (now - s_press_us >= SEEK_HOLD_DELAY_US) {
                        // Held: pause and scrub.  Nothing is fetched while held — the
                        // bar just moves; the reopen waits for release.
                        s_seek_state    = SEEK_SCRUB;
                        s_seek_dir      = dir;
                        s_scrub_resume  = !paused;
                        paused          = true;
                        s_scrub_step_us = now - SEEK_SCRUB_STEP_US;   // first step now
                        s_tap_gate_us   = 0;
                        plog("seek: scrub begin");
                    } else {
                        s_seek_dir = dir;             // allow F<->B before it commits
                    }
                    break;

                case SEEK_SCRUB:
                    if (dir == 0) {
                        // Released: NOW fetch — one reopen to the scrubbed spot, resume.
                        s_seek_state = SEEK_IDLE;
                        s_seek_dir   = 0;
                        if (s_seek_pending_secs != 0) {
                            s_commit_now        = true;
                            s_resume_after_seek = s_scrub_resume;
                        } else {
                            paused = !s_scrub_resume;
                        }
                        plog("seek: scrub end");
                    } else {
                        s_seek_dir = dir;
                        // D-pad scrubs at half speed (one step per 500ms vs 250ms).
                        u64 step_us = s_seek_dpad ? SEEK_SCRUB_STEP_US * 2
                                                  : SEEK_SCRUB_STEP_US;
                        if (now - s_scrub_step_us >= step_us) {
                            s_scrub_step_us = now;
                            s_seek_pending_secs += dir * SEEK_SCRUB_SECS;   // move bar +25s
                        }
                    }
                    break;
                }
            }

            // Commit one reopen: on scrub release, or when the tap gate expires.
            // Never mid-scrub — a hold only moves the bar until the user lets go.
            if (act == HUD_ACTION_NONE && s_seek_pending_secs != 0 &&
                s_seek_state != SEEK_SCRUB &&
                (s_commit_now ||
                 (s_tap_gate_us != 0 && timing_get_us() >= s_tap_gate_us))) {
                s_commit_now  = false;
                s_tap_gate_us = 0;
                act           = HUD_ACTION_SEEK;
            }

            if (act == HUD_ACTION_TOGGLE_PAUSE) {
                paused = !paused;
                { sys_ppu_thread_t cur_tid = 0;
                  sysThreadGetId(&cur_tid);
                  char buf[128];
                  snprintf(buf, sizeof(buf),
                      "hud: %s clk=%lluus tid=%llu fr_dur=%lluus",
                      paused ? "paused" : "resumed",
                      (unsigned long long)audio_get_clock_us(),
                      (unsigned long long)cur_tid,
                      (unsigned long long)s_loop_frame_dur);
                  plog(buf); }
            } else if (act == HUD_ACTION_SEEK) {
                int delta = s_seek_pending_secs;   // total accumulated during cooldown
                s_seek_pending_secs = 0;
                {
                    char buf[64];
                    snprintf(buf, sizeof(buf), "hud: seek %+d s (begin)", delta);
                    plog(buf);
                }
                crash_log("sk1 seek begin");

                // Compute target from the ABSOLUTE position (base + stream clock),
                // not the stream-relative audio clock — otherwise each seek would
                // be measured from the post-seek stream's PTS 0 and walk backward.
                s64 cur_us    = (s64)play_base_us + (s64)audio_get_clock_us();
                s64 target_us = cur_us + (s64)delta * 1000000LL;
                if (target_us < 0) target_us = 0;
                if (total_secs > 0 &&
                    target_us > (s64)total_secs * 1000000LL)
                    target_us = (s64)total_secs * 1000000LL;
                u64 start_ticks = (u64)target_us * 10ULL;  // us -> 100-ns ticks
                {
                    char buf[176];
                    snprintf(buf, sizeof(buf),
                        "seek_calc: base=%llus clk=%llus delta=%ds -> target=%llus "
                        "ticks=%llu total=%us",
                        (unsigned long long)(play_base_us / 1000000ULL),
                        (unsigned long long)(audio_get_clock_us() / 1000000ULL),
                        delta,
                        (unsigned long long)((u64)target_us / 1000000ULL),
                        (unsigned long long)start_ticks,
                        total_secs);
                    plog(buf);
                }

                // 1) Stop the decode thread so nothing feeds VDEC mid-flush.
                //    Use the decode-only flag so the audio + upload threads
                //    keep running (they idle on empty buffers), matching
                //    Movian's flush-while-primary model.
                bool was_paused = paused;
                paused    = true;              // gate audio output during the seek
                s_seeking = true;              // gate upload thread during flush window
                dec_run   = false;             // signal ONLY the decode thread
                __asm__ volatile("sync" ::: "memory");
                if (dec_tid) {
                    u64 tret;
                    sysThreadJoin(dec_tid, &tret);
                    dec_tid = 0;
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
                    s_seeking = false;
                    playing   = false;
                    break;
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
                netClose(sock);
                // Kill the existing transcode first, otherwise Jellyfin keeps
                // serving the in-progress job (which started at offset 0) and
                // the seek appears to reset to 0:00 instead of honouring the
                // new StartTimeTicks.
                jellyfin_stop_transcode(session_id);
                // Stopping the transcode is not enough on its own: re-requesting
                // stream.ts with the SAME PlaySessionId makes Jellyfin reuse the
                // existing transcode job (which began at offset 0), so
                // StartTimeTicks is silently ignored and the video restarts from
                // 0:00.  Mint a fresh PlaySessionId via PlaybackInfo so the server
                // spins up a brand-new transcode anchored at the seek target.
                {
                    char new_session[64] = "";
                    if (jellyfin_get_play_session_id(item->id, new_session,
                                                     sizeof(new_session), NULL) &&
                        new_session[0]) {
                        snprintf(session_id, sizeof(session_id), "%s", new_session);
                        char sb[96];
                        snprintf(sb, sizeof(sb), "seek: new session=%s", session_id);
                        plog(sb);
                    } else {
                        plog("seek: PlaybackInfo failed, reusing old session");
                    }
                }
                char surl[640];
                build_stream_url(surl, sizeof(surl), item, req_w, req_h,
                                 session_id, start_ticks);
                plog_url("surl", surl);
                int nsock = stream_open(surl);
                if (nsock < 0) {
                    plog("seek: stream_open FAILED");
                    crash_log("sk_fail reopen");
                    playing = false;           // give up cleanly; loop will exit
                    break;
                }
                sock = nsock;
                // The new stream's PTS restarts at ~0, so its clock now maps to
                // absolute media time target_us.
                play_base_us = (u64)target_us;
                { struct { u32 sec; u32 usec; } tv = { 0, 5000 };
                  setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)); }
                crash_log("sk4 reopened");

                // 4) Re-prime: decode a few frames before resuming display so
                //    the jitter buffer is non-empty (mirrors initial pre-fill).
                {
                    u8 spkt[TS_PACKET_SIZE];
                    int guard = 0;
                    while (jbuf_count() < JBUF_PREFILL && running &&
                           !s_vdec_error && guard < 20000) {
                        sysUtilCheckCallback();
                        int srd = stream_read(sock, spkt, TS_PACKET_SIZE);
                        if (srd < 0) { plog("seek: prefill eof"); break; }
                        if (srd > 0) video_feed_ts(spkt);
                        guard++;
                    }
                }
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
                        (unsigned long long)(play_base_us / 1000000ULL));
                    plog(buf);
                }

                // 5) Respawn the decode thread with the new socket.
                dec_run = true;
                dec_ctx.sock = sock;
                {
                    int trc = sysThreadCreate(&dec_tid, decode_thread_fn,
                                              (void *)&dec_ctx,
                                              800, 128 * 1024,
                                              0, "jf_decode");
                    if (trc != 0) {
                        char buf[64];
                        snprintf(buf, sizeof(buf),
                                 "seek: dec thread_create FAILED rc=%d", trc);
                        plog(buf);
                        playing = false;
                        break;
                    }
                }

                // 6) Resume (unless the user had paused before seeking).
                s_seeking = false;             // upload thread may resume
                paused    = was_paused;
                {
                    char buf[80];
                    snprintf(buf, sizeof(buf),
                             "hud: seek done target=%llds clk_was=%llds",
                             (long long)(target_us / 1000000),
                             (long long)(cur_us / 1000000));
                    plog(buf);
                }
                crash_log("sk6 seek done");
                seek_dbg_frames = 120;   // log resumed position for ~2s
                if (paused) s_show_seek_frame = true;  // reveal target while paused
                // A scrub paused the video to race the position; now that its seek
                // has landed, resume playback if it had been playing.
                if (s_resume_after_seek) {
                    paused = false;
                    s_resume_after_seek = false;
                }
            } else if (act == HUD_ACTION_AUDIO_TRACK) {
                plog("hud: audio track selected");
            } else if (act == HUD_ACTION_SUBTITLE) {
                plog("hud: subtitle toggled");
            }
        }

        // fps detection timeout — needed only to initialise the Bresenham fallback
        if (!s_timing_ready) {
            if (det_timeout_start == 0) det_timeout_start = timing_get_us();
            if (timing_get_us() - det_timeout_start >= 5000000ULL) {
                plog("fps_detect: timeout, fallback 30fps");
                timing_init(30, 1);
                s_timing_ready = true;
            }
        }

        // Refresh-rate stability diagnostic: poll videoGetState every 300 vblanks
        {
            static u64  s_rr_vblank   = 0;
            static u16  s_last_rr     = 0;
            static int  s_rr_changes  = 0;
            static int  s_rr_polls    = 0;
            s_rr_vblank++;
            if (s_rr_vblank % 300 == 0) {
                videoState vs;
                if (videoGetState(0, 0, &vs) == 0) {
                    u16 rr = vs.displayMode.refreshRates;
                    s_rr_polls++;
                    if (s_last_rr != 0 && rr != s_last_rr) {
                        s_rr_changes++;
                        char buf[96];
                        snprintf(buf, sizeof(buf),
                            "rr_change: poll#%d prev=0x%04x new=0x%04x changes=%d",
                            s_rr_polls, (unsigned)s_last_rr,
                            (unsigned)rr, s_rr_changes);
                        plog(buf);
                    }
                    s_last_rr = rr;
                    if (s_rr_polls % 12 == 0) {
                        char buf[80];
                        snprintf(buf, sizeof(buf),
                            "rr_stats: polls=%d changes=%d cur=0x%04x",
                            s_rr_polls, s_rr_changes, (unsigned)rr);
                        plog(buf);
                    }
                }
            }
        }

        // ---- Duration-consumption gate logic (Movian-style temporal blend) ----
        s64 vblank_period_us = avsync_biased_period(16683);
        static int  s_pure_count = 0;
        static int  s_mid_count  = 0;
        static s64  s_spill_max  = 0;

        bool  do_pop       = false;
        bool  render_blend = false;
        bool  silent_flip  = false;
        float blend_factor = 0.0f;

        // Log first 30 gate-logic vblanks after timing is ready
        {
            static int s_blend_init_n = 0;
            if (s_blend_init_n < 30 && s_timing_ready) {
                s_blend_init_n++;
                char buf[96];
                snprintf(buf, sizeof(buf),
                    "blend_init: tick=%d n=%d dur=%lldus",
                    s_blend_init_n, jbuf_count(),
                    (long long)jbuf_peek_dur());
                plog(buf);
            }
        }

        if (!paused && s_vid_frame_ready && s_timing_ready && jbuf_count() > 0) {
            // Step 4: measurement only — result discarded; EMA updated for logging.
            { u64 vpts = jbuf_peek_pts_us(); (void)avsync_compute_diff(vpts); }

            s64  dur_a = jbuf_peek_dur();
            bool b_ok  = (jbuf_peek_next() != NULL) && s_vid_b_present;

            if (!b_ok || dur_a >= vblank_period_us) {
                // Pure-A: consume one vblank period, pop if frame exhausted
                jbuf_consume_dur(vblank_period_us);
                jbuf_advance();
                silent_flip = true;
                s_pure_count++;
            } else {
                // Crossfade: dur_a < vblank, B available — blend A→B
                {
                    static u64 s_last_cf_us = 0;
                    u64 now_cf = timing_get_us();
                    u64 delta_cf = (s_last_cf_us != 0) ? (now_cf - s_last_cf_us) : 0;
                    s_last_cf_us = now_cf;
                    if (frame_count < 200) {
                        char buf[128];
                        snprintf(buf, sizeof(buf),
                            "cf: t=%lluus delta=%lluus blend=%.3f fr=%d dur_a=%lld",
                            (unsigned long long)now_cf,
                            (unsigned long long)delta_cf,
                            (double)((float)dur_a / (float)vblank_period_us),
                            frame_count,
                            (long long)dur_a);
                        plog(buf);
                    }
                }
                blend_factor  = (float)dur_a / (float)vblank_period_us;
                s64 spill     = vblank_period_us - dur_a;
                jbuf_consume_dur(dur_a);   // exhaust A
                jbuf_advance();            // pop A (dur now <= 0)
                jbuf_consume_dur(spill);   // consume spill from new front (old B)
                do_pop        = true;
                render_blend  = true;
                s_mid_count++;
                if (spill > s_spill_max) s_spill_max = spill;
            }
        }

        if (do_pop) {
            u32 disp_seq = jbuf_peek_seq();
            frame_count++;
            __asm__ volatile("sync" ::: "memory");
            s_vid_frame_ready = false;
            s_vid_disp_idx ^= 1;
            {
                static u64 s_fi_last_us = 0;
                static u64 s_fi_gaps[2] = {0, 0};
                u64 now_us = timing_get_us();
                if (s_fi_last_us != 0) {
                    s_fi_gaps[0] = s_fi_gaps[1];
                    s_fi_gaps[1] = now_us - s_fi_last_us;
                }
                s_fi_last_us = now_us;
                if (frame_count % 60 == 0) {
                    u64 audio_clk = audio_get_clock_us();
                    char buf[128];
                    snprintf(buf, sizeof(buf),
                        "frame_interval: fr=%d gap1=%lluus gap2=%lluus audio=%lluus",
                        frame_count,
                        (unsigned long long)s_fi_gaps[0],
                        (unsigned long long)s_fi_gaps[1],
                        (unsigned long long)audio_clk);
                    plog(buf);
                    char buf2[128];
                    snprintf(buf2, sizeof(buf2),
                        "blend_dist: fr=%d pure=%d mid=%d spill_max=%lldus",
                        frame_count, s_pure_count, s_mid_count,
                        (long long)s_spill_max);
                    plog(buf2);
                    s64  smooth = avsync_get_smoothed_diff();
                    bool locked = avsync_is_locked();
                    s64  biased = avsync_biased_period(16683);
                    char buf3[160];
                    snprintf(buf3, sizeof(buf3),
                        "avsync: smooth=%lldus locked=%d audio=%lluus video=%lluus bias=%lldus",
                        (long long)smooth,
                        locked ? 1 : 0,
                        (unsigned long long)audio_get_clock_us(),
                        (unsigned long long)jbuf_peek_pts_us(),
                        (long long)(biased - 16683));
                    plog(buf3);
                }
            }
            {
                int window_pos = frame_count % 240;
                if (window_pos >= 1 && window_pos <= 24) {
                    char buf[80];
                    snprintf(buf, sizeof(buf),
                        "disp_seq: fr=%d seq=%u window=%d",
                        frame_count, disp_seq, frame_count / 240);
                    plog(buf);
                }
            }
        } else if (silent_flip && s_vid_frame_ready) {
            __asm__ volatile("sync" ::: "memory");
            s_vid_frame_ready = false;
            s_vid_disp_idx ^= 1;
        } else if (s_show_seek_frame && paused && s_vid_frame_ready) {
            // Paused seek: display the target frame exactly once, staying paused.
            __asm__ volatile("sync" ::: "memory");
            s_vid_frame_ready = false;
            s_vid_disp_idx ^= 1;
            s_show_seek_frame = false;
        }
        if (do_pop) s_show_seek_frame = false;   // playing again; clear any leftover

        // Re-draw every vblank. s_vid_disp_idx advances on do_pop or silent_flip.
        if (frame_count > 0) {
            if (do_pop) rsxSync();
            u32 fw = jbuf_fw(), fh = jbuf_fh();
            vid_gpu_draw(render_blend, blend_factor, fw, fh);
        }

        {
            static int s_vb_log_n = 0;
            static int s_vb_last_fr = -1;
            // Log at most 200 lines, one per DISTINCT frame, after startup.
            // Guard against frame_count freezing (e.g. a post-seek stall) — that
            // would otherwise spam an identical line every loop iteration and
            // thrash the HDD with log writes.
            if (frame_count >= 60 && s_timing_ready &&
                s_vb_log_n < 200 && frame_count != s_vb_last_fr) {
                s_vb_last_fr = frame_count;
                u64 now_us = timing_get_us();
                char buf[160];
                snprintf(buf, sizeof(buf),
                    "vb: t=%lluus fr=%d disp=%d dur_a=%lld do_pop=%d render_blend=%d blend=%.3f q=%d",
                    (unsigned long long)now_us,
                    frame_count,
                    s_vid_disp_idx,
                    (long long)jbuf_peek_dur(),
                    do_pop ? 1 : 0,
                    render_blend ? 1 : 0,
                    (double)blend_factor,
                    jbuf_count());
                plog(buf);
                s_vb_log_n++;
            }
        }

        // Post-seek diagnostic: what position is the HUD actually showing?
        if (seek_dbg_frames > 0) {
            seek_dbg_frames--;
            if (seek_dbg_frames % 15 == 0) {
                u64 clk = audio_get_clock_us();
                char buf[128];
                snprintf(buf, sizeof(buf),
                    "seek_dbg: shown=%llus (base=%llus + clk=%llus) jbuf=%d paused=%d",
                    (unsigned long long)((play_base_us + clk) / 1000000ULL),
                    (unsigned long long)(play_base_us / 1000000ULL),
                    (unsigned long long)(clk / 1000000ULL),
                    jbuf_count(), (int)paused);
                plog(buf);
            }
        }

        // HUD overlay.
        // The HUD's draw_dim_rect() reprograms RSX vertex/fragment state and
        // rebinds attributes.  If the video draw above is still in flight when
        // that happens, the RSX wedges (the dim quad collides with the video
        // path's TEX0 fetch).  When playing, do_pop triggers the rsxSync() at
        // line 651, so the pipeline is already fenced.  When PAUSED, do_pop is
        // always false (the consume gate requires !paused), so that sync is
        // skipped and the HUD is the first thing to touch RSX after an unfenced
        // vid_gpu_draw -> hard freeze at "hud_draw: A pre dim_rect".
        // Fence unconditionally here so the HUD never races the video commands.
        if (hud_is_visible() && frame_count > 0) {
            static int s_hg = 0;
            if (s_hg < 12) { 
                char b[80]; snprintf(b,sizeof(b),"hud_gate: vis paused=%d fr=%d",(int)paused,frame_count);
                plog(b); s_hg++;
            }
            rsxSync();  // ensure prior vid_gpu_draw is complete before HUD reprograms RSX
            // While a seek is armed, show where the accumulated skip will land so
            // the user can keep tapping toward the right spot before it fires.
            u64 hud_elapsed = play_base_us + audio_get_clock_us();
            if (s_seek_pending_secs != 0) {   // tap armed or scrubbing: preview target
                s64 prev = (s64)hud_elapsed + (s64)s_seek_pending_secs * 1000000LL;
                hud_elapsed = prev < 0 ? 0 : (u64)prev;
            }
            hud_draw(hud_elapsed, paused);
            { static int s_hg2 = 0; if (s_hg2 < 12) { plog("hud_gate: draw returned"); s_hg2++; } }
        }

        flip();
    }

    crash_log("p11 loop exit");
    {
        char buf[96];
        snprintf(buf, sizeof(buf),
            "show_player: loop exit running=%u playing=%d vdec_err=%d fr=%d",
            running, (int)playing, (int)s_vdec_error, frame_count);
        plog(buf);
    }

    timing_shutdown();
    hud_shutdown();

    // Signal all threads to stop, join in order: network → decode → audio (Steps 5e, 6d)
    playing = false;
    usleep(16000);

    if (dec_tid) {
        u64 tret;
        sysThreadJoin(dec_tid, &tret);
        plog("show_player: decode thread joined");
    }
    if (aud_tid) {
        u64 tret;
        sysThreadJoin(aud_tid, &tret);
        plog("show_player: audio thread joined");
    }
    if (upl_tid) {
        u64 tret;
        sysThreadJoin(upl_tid, &tret);
        plog("show_player: upload thread joined");
    }
    crash_log("p12 threads joined");

    // Free video GPU blit resources before releasing the jitter buffer
    vid_gpu_free();

    crash_log("p17 jbuf_free begin");
    jbuf_free();
    crash_log("p18 jbuf_free OK");
    netClose(sock);
    adec_stop();
    crash_log("p15 audio_close begin");
    audio_close();
    crash_log("p16 audio_close OK");
    crash_log("p13 vdec_close begin");
    vdec_close();
    crash_log("p14 vdec_close OK");

    ui_restore_rsx_state();
    crash_log("p19 done");
    plog("show_player: done");
}
