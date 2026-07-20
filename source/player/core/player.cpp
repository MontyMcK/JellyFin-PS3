// show_player — Jellyfin PS3 media player orchestrator: session setup,
// thread spawn, the main display loop, and teardown.  The heavy lifting
// lives in player_session / player_menu / player_seek / player_display.

#include <stdio.h>
#include <string.h>

#include <ppu-types.h>
#include <io/pad.h>
#include <sysutil/sysutil.h>
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
#include "thumbnail_cache.h"
#include "slog.h"

extern void crash_log(const char *msg);

// A startup status screen ("Initializing decoder…", etc.) followed by a long
// CPU-only stretch.  flip() is ASYNCHRONOUS — it queues the frame and returns
// while the RSX is still reading the glyph-blit source memory.  The work that
// runs right after each of these screens (thumb_cache_shutdown, vdec_open's
// 96 MB memalign + heap reorg, jbuf_alloc, the prefill) frees/moves the heap
// out from under those in-flight blits.  On real hardware the RSX has already
// drained by then; RPCS3 has not, so it reads the freed pages (poisoned to
// 0xfeed0000), fails to decode the NV3089 blit, and its FIFO watchdog kills the
// RSX thread ("Dead FIFO commands queue state") — which is exactly why movie
// playback took the whole emulator down.  Draining the FIFO before the blocking
// work removes the use-after-free.  Hardware is byte-for-byte unaffected: the
// rsxSync() is compiled out and this is a plain flip() there.
static inline void player_startup_flip(void) {
    flip();
#if BUILD_FOR_RPCS3
    rsxSync();   // block until the RSX has consumed the just-queued blits
#endif
}

// A cosmetic "loading" screen shown between the XMB and live video.
//
// The bitmap-font text path (drawHeader/drawText) renders each glyph with the
// RSX NV3089 transfer-scale engine.  On RPCS3 that engine is freeze-prone — the
// same reason thumbnail_cache blits its cards with the CPU instead — and firing
// it here, during the fragile XMB->player transition, wedges the RSX FIFO
// ("Dead FIFO ... nv3089 decode_transfer_registers: Timer expired") and kills
// playback before it starts.  So on the emulator we DROP the glyph blits and
// just present a drained frame; the status text is only cosmetic and hardware
// (where NV3089 is fine) still draws it in full.  The flip keeps the display
// alive through the long vdec_open/stream stalls that follow.
static inline void player_status_screen(const char *name, const char *msg) {
#if !BUILD_FOR_RPCS3
    drawHeader();
    drawTextf(40, 100, "%.70s", name);
    drawText(40, 130, msg);
#else
    (void)name; (void)msg;
#endif
    player_startup_flip();
}

// -------------------------------------------------------
// End-of-item auto-advance ("Next Episode")
// -------------------------------------------------------
// The UI arms this before show_player when the played item has a follower.
// In the last 90 s of playback the player shows the popup badge (just the
// label) with the instruction line drawn separately below it; SELECT ends
// playback with the next-request flag set, which the UI reads via
// player_take_next_request() to start the next item.  For episodes the
// last 30 s also run a countdown that fires the request automatically.
static bool s_next_armed     = false;
static bool s_next_requested = false;
static char s_next_label[32] = "NEXT EPISODE";
static char s_next_hint[64]  = "Press SELECT for next episode";

void player_arm_next(const char *label, const char *hint) {
    s_next_armed     = true;
    s_next_requested = false;
    if (label && label[0])
        snprintf(s_next_label, sizeof(s_next_label), "%s", label);
    if (hint && hint[0])
        snprintf(s_next_hint, sizeof(s_next_hint), "%s", hint);
}

bool player_take_next_request(void) {
    bool r = s_next_requested;
    s_next_requested = false;
    return r;
}

// Popup badge top-right; the instruction line is drawn separately below
// the badge, outside the box.  auto_secs >= 0 appends the auto-advance
// countdown to the instruction line.
static void player_draw_next_popup(int auto_secs) {
    const float label_px = 26.0f;
    const float hint_px  = 16.0f;
    const int   pad_x = 26, pad_y = 14, margin = 48;

    int tw = ttf_text_width(s_next_label, label_px);
    int bw = tw + 2 * pad_x;
    int bh = (int)label_px + 2 * pad_y;
    int bx = (int)display_width - bw - margin;
    int by = margin;

    drawRect((u32)(bx - 1), (u32)(by - 1), (u32)(bw + 2), (u32)(bh + 2),
             XMB_HAIRLINE);
    drawRect((u32)bx, (u32)by, (u32)bw, (u32)bh, XMB_PANEL);
    drawRect((u32)bx, (u32)by, 3, (u32)bh, XMB_ACCENT);
    drawTTF((u32)(bx + pad_x), (u32)(by + pad_y), s_next_label, label_px,
            XMB_TEXT, true);

    char hint[96];
    if (auto_secs >= 0)
        snprintf(hint, sizeof(hint), "%s \xB7 starting in %ds",
                 s_next_hint, auto_secs);
    else
        snprintf(hint, sizeof(hint), "%s", s_next_hint);
    int hw = ttf_text_width(hint, hint_px);
    drawTTF((u32)(bx + bw - hw), (u32)(by + bh + 14), hint, hint_px,
            XMB_TEXT_DIM);
}

void show_player(const JFItem *item, u32 resume_secs) {
    crash_log("p1 enter");
    plog("show_player: enter");
    plog("show_player: BUILD=seek-diag-1");
    init_btns();

#if BUILD_FOR_RPCS3
    // Drain the XMB's final frame off the RSX before the first long PPU stall.
    // The last grid frame still has its movie-poster thumbnail blits (128x48,
    // NV3089) queued; the network calls just below (get_play_session_id /
    // fetch_tracks) then block the PPU for ~seconds while thumb_cache_shutdown
    // is about to free those poster bitmaps.  On real hardware the RSX drains
    // in the background and waits happily; RPCS3 sees the FIFO not advance and
    // trips its dead-FIFO watchdog ("Timer expired" in nv3089), killing the RSX
    // and taking playback down before it starts.  Emptying the FIFO here means
    // every startup stall (network, then vdec_open) runs on an idle queue with
    // nothing for the watchdog to fire on.  Compiled out on hardware.
    rsxSync();
#endif
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

    // Consume the auto-advance arming one-shot so a later, unrelated
    // playback can never inherit a stale NEXT popup.
    bool have_next = s_next_armed;
    s_next_armed = false;

    // Auto-advance countdown applies only to episodes; movies keep the
    // manual SELECT prompt.
    bool auto_next      = have_next && strcmp(item->type, "Episode") == 0;
    bool in_auto_window = false;   // last frame was inside the countdown
    bool user_stopped   = false;   // START pressed (never auto-advance)

    // Session state shared with the player_* helpers.  ~2KB of JFTracks —
    // keep off the stack.
    static PlayerState ps;
    memset(&ps, 0, sizeof(ps));
    ps.item      = item;
    ps.playing   = true;
    ps.dec_run   = true;
    ps.cur_audio = -1;
    ps.cur_sub   = -1;               // subtitles start off
    ps.menu_kind = PLAYER_MENU_NONE;

    // H.264 level 3.1 caps at 1280×720 @ 30fps
    ps.req_w = display_width  < 1280 ? display_width  : 1280;
    ps.req_h = display_height < 720  ? display_height : 720;

    if (!jellyfin_get_play_session_id(item->id, ps.session_id,
                                      sizeof(ps.session_id), &ps.total_secs)) {
        plog("show_player: PlaybackInfo failed, streaming without PlaySessionId");
        ps.session_id[0] = '\0';
    }

    // Selectable audio/subtitle tracks (HUD AUDIO + CC buttons cycle these).
    ps.have_tracks = jellyfin_fetch_tracks(item->id, &ps.tracks);
    if (ps.have_tracks && ps.tracks.n_audio > 0)
        ps.cur_audio = ps.tracks.default_audio;

    // Continue Watching: open the transcode at the saved position.  The new
    // stream's PTS starts at 0, so play_base_us anchors the absolute clock —
    // the same mechanism every seek uses.
    if (ps.total_secs > 0 && resume_secs >= ps.total_secs)
        resume_secs = 0;             // stale position at/past the end: restart
    ps.play_base_us = (u64)resume_secs * 1000000ULL;
    if (resume_secs > 0) {
        char buf[48];
        snprintf(buf, sizeof(buf), "resume: start at %us", resume_secs);
        plog(buf);
    }

    char url[768];
    build_stream_url(url, sizeof(url), &ps, (u64)resume_secs * 10000000ULL);
    plog_url("url", url);

    player_status_screen(item->name, "Initializing decoder...");

    // Release the UI thumbnail cache (joins its fetch thread, frees ~15 MB
    // of card bitmaps) — the decoder + jitter buffer below need every MB,
    // and jbuf_alloc fails outright with the cache resident.  Re-created on
    // every exit path; the UI simply refetches its thumbs.
    thumb_cache_shutdown();

    // Claim the cached HUD overlay buffers before the big allocations so
    // they sit low in the heap once and forever (see player_hud.h).
    hud_overlay_alloc();

    crash_log("p2 vdec_open begin");
    plog("show_player: vdec_open");
    if (!vdec_open()) {
        plog("show_player: vdec_open FAILED");
        vdec_close();
        thumb_cache_init();
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

    player_status_screen(item->name, "Connecting to stream...");

    crash_log("p6 stream_open begin");
    plog("show_player: stream_open");
    ps.sock = stream_open(url);
    if (ps.sock < 0) {
        plog("show_player: stream_open FAILED");
        adec_stop();
        audio_close();
        vdec_close();
        thumb_cache_init();
        show_error("Stream connection failed.", url);
        ui_restore_rsx_state();
        return;
    }
    plog("show_player: stream_open OK");
    crash_log("p7 stream_open OK");

    player_status_screen(item->name, "Streaming... START=stop");

    video_reset();
    // Clear any avsync state (incl. the video-PTS base correction) left over
    // from a previous playback session so the first frame of THIS stream
    // re-latches cleanly.  Seeks/track-changes reset it via avsync_reset() in
    // the seek path; this covers session start.
    avsync_reset();

    if (!jbuf_alloc(ps.req_w, ps.req_h)) {
        plog("show_player: jbuf_alloc FAILED");
        netClose(ps.sock);
        adec_stop();
        audio_close();
        vdec_close();
        thumb_cache_init();
        ui_restore_rsx_state();
        return;
    }
    {
        char buf[72];
        snprintf(buf, sizeof(buf), "jbuf: %d slots %ux%u ~%u KB each",
                 JBUF_SIZE, ps.req_w, ps.req_h, ps.req_w * ps.req_h * 4 / 1024);
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
      setsockopt(ps.sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)); }

    // The button always reads "AUDIO" — track names are too long for the HUD
    // row; the selected track is plogged when cycled.
    hud_init(ps.total_secs, NULL);
    hud_set_title(item->name);
    plog("hud: prewarm start");
    ttf_prewarm_hud();
    plog("hud: prewarm done");

    // ---- Pre-fill: decode JBUF_PREFILL frames before display starts ----
    plog("jbuf: pre-fill start");
    player_prefill(&ps, true, 0x7FFFFFFF);

    if (!ps.playing) {
        vid_gpu_free();
        jbuf_free();
        netClose(ps.sock);
        adec_stop();
        audio_close();
        vdec_close();
        thumb_cache_init();
        return;
    }

    plog("jbuf: pre-fill done — starting threads");
    timing_register_vblank();

    // ---- Spawn decode thread ----
    player_spawn_decode(&ps);

    // ---- Spawn audio thread ----
    AudioCtx         aud_ctx = { &ps.playing, &ps.paused };
    sys_ppu_thread_t aud_tid = 0;
    if (ps.playing) {
        int trc = sysThreadCreate(&aud_tid, audio_thread_fn,
                                  (void *)&aud_ctx,
                                  700, 32 * 1024,
                                  0, "jf_audio");
        if (trc != 0) {
            char buf[64];
            snprintf(buf, sizeof(buf), "show_player: aud thread_create FAILED rc=%d", trc);
            plog(buf);
            ps.playing = false;
        }
    }

    // ---- Spawn upload thread — memcpy jbuf→RSX-local off the display thread ----
    UploadCtx        upl_ctx = { &ps.playing, jbuf_fw(), jbuf_fh() };
    sys_ppu_thread_t upl_tid = 0;
    if (ps.playing) {
        int trc = sysThreadCreate(&upl_tid, upload_thread_fn,
                                  (void *)&upl_ctx,
                                  850, 32 * 1024,
                                  0, "jf_upload");
        if (trc != 0) {
            char buf[64];
            snprintf(buf, sizeof(buf), "show_player: upl thread_create FAILED rc=%d", trc);
            plog(buf);
            ps.playing = false;
        }
    }

    // ---- Spawn progress reporter — keeps server resume position current ----
    jellyfin_report_playing(item->id, ps.session_id, ps.play_base_us * 10ULL);
    sys_ppu_thread_t prog_tid = 0;
    if (ps.playing) {
        int trc = sysThreadCreate(&prog_tid, progress_thread_fn,
                                  (void *)&ps,
                                  1100, 16 * 1024,
                                  0, "jf_progress");
        if (trc != 0) {
            plog("show_player: progress thread_create failed (non-fatal)");
            prog_tid = 0;
        }
    }

    crash_log("p9 threads started");
    slog_state("PLAYBACK_STARTED item_id=%s name=%.40s total=%us "
               "resume=%us w=%u h=%u",
               item->id, item->name, ps.total_secs,
               (unsigned)(ps.play_base_us / 1000000ULL), ps.req_w, ps.req_h);

    crash_log("p10 loop");
    // Paused-idle gate state (see below).  flip_queued: whether the previous
    // iteration queued a flip — waitflip() blocks forever if none is pending.
    bool flip_queued  = true;
    bool was_paused   = false;
    int  pause_settle = 0;   // frames still to draw after a pause-state change

    // ---- Main (display) loop ----
    while (running && ps.playing && !s_vdec_error) {
        if (flip_queued) waitflip();
        flip_queued = false;
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
            user_stopped = true;
            ps.playing = false;
        }

        // End-of-item auto-advance: within the last 90 s of a followed item,
        // show the NEXT popup; SELECT ends this session with the
        // next-request flag set so the UI starts the follower.  Episodes
        // also count down through the last 30 s and fire the request
        // automatically at zero.
        bool next_popup = false;
        int  auto_secs  = -1;
        in_auto_window  = false;
        if (have_next && ps.total_secs > 90) {
            u64 pos_secs = (ps.play_base_us + audio_get_clock_us()) / 1000000ULL;
            next_popup = (pos_secs + 90 >= (u64)ps.total_secs);
            if (next_popup && auto_next && pos_secs + 30 >= (u64)ps.total_secs) {
                s64 rem = (s64)ps.total_secs - (s64)pos_secs;
                auto_secs = rem > 0 ? (int)rem : 0;
                in_auto_window = true;
            }
        }
        if (next_popup && BTN_PRESSED(select)) {
            plog("playing=0 reason=next_item");
            s_next_requested = true;
            ps.playing = false;
        }
        if (auto_secs == 0 && ps.playing) {
            plog("playing=0 reason=next_auto");
            s_next_requested = true;
            ps.playing = false;
        }
        if (!ps.playing) break;

        // The HUD owns the D-pad (focus navigation) and X (activates the
        // focused control: play/pause, REW/FF, AUDIO, or CC), returning the
        // action to perform.
        HudAction act = hud_handle_input(l2_pressed, r2_pressed, ps.paused);

        // D-pad / focus-mode taps come through the HUD; queue them like any
        // tap.  R2/L2 are NOT handled here — the seek input machine owns them,
        // so their flickery digital edge can't fire spurious taps.
        if (act == HUD_ACTION_SEEK) {
            player_seek_queue_tap(&ps, hud_seek_delta());
            act = HUD_ACTION_NONE;
        }

        // AUDIO / CC popup menus; a track change comes back as a 0-delta
        // HUD_ACTION_SEEK — the reopen applies the new track.
        act = player_handle_menu_action(&ps, act);

        // R2/L2 tap/hold machine + commit gate.
        act = player_seek_input_update(&ps, act);

        if (act == HUD_ACTION_TOGGLE_PAUSE) {
            ps.paused = !ps.paused;
            { sys_ppu_thread_t cur_tid = 0;
              sysThreadGetId(&cur_tid);
              char buf[128];
              snprintf(buf, sizeof(buf),
                  "hud: %s clk=%lluus tid=%llu fr_dur=%lluus",
                  ps.paused ? "paused" : "resumed",
                  (unsigned long long)audio_get_clock_us(),
                  (unsigned long long)cur_tid,
                  (unsigned long long)s_loop_frame_dur);
              plog(buf); }
            slog_state("PLAYBACK_%s pos=%llus",
                       ps.paused ? "PAUSED" : "RESUMED",
                       (unsigned long long)((ps.play_base_us +
                           audio_get_clock_us()) / 1000000ULL));
        } else if (act == HUD_ACTION_SEEK) {
            if (!player_execute_seek(&ps)) break;
        }

        // Paused-idle gate.  While paused the seek bar stays up and every
        // redrawn frame is pixel-identical, yet redrawing costs the GPU
        // clear + video quad, three rsxSync stalls, and CPU alpha-blended
        // text into RSX VRAM — at 60 Hz that churn is what makes the whole
        // player lag whenever the bar is visible.  Draw only when something
        // can have changed (input, seek/scrub activity, a landed paused
        // seek, or the pause state itself); otherwise sleep one vblank and
        // just keep polling input.  Playback (!paused) is never gated.
        if (ps.paused != was_paused) { was_paused = ps.paused; pause_settle = 2; }
        bool any_input =
            btn_cur.up || btn_cur.down || btn_cur.left || btn_cur.right ||
            btn_cur.cross || btn_cur.circle || btn_cur.square || btn_cur.triangle ||
            btn_cur.start || btn_cur.select ||
            btn_cur.l1 || btn_cur.r1 || btn_cur.l2 || btn_cur.r2;
        if (ps.paused && !any_input && act == HUD_ACTION_NONE &&
            !ps.show_seek_frame && ps.seek.pending_secs == 0 &&
            ps.seek.state == SEEK_IDLE && pause_settle == 0) {
            usleep(16000);
            continue;
        }
        if (pause_settle > 0) pause_settle--;

#if BUILD_FOR_RPCS3
        // AV-sync heartbeat (~1 Hz) — the app already measures drift, so the
        // choppy-audio / desync bug is visible in the log as diff_us climbing
        // or jbuf underrunning, without needing to eyeball a screenshot.
        {
            static u64 s_avsync_next_us = 0;
            u64 hb_now = timing_get_us();
            if (!ps.paused && hb_now >= s_avsync_next_us) {
                s_avsync_next_us = hb_now + 1000000ULL;
                slog_state("AVSYNC diff_us=%lld locked=%d jbuf=%d pos=%llus "
                           "frame=%d",
                           (long long)avsync_get_smoothed_diff(),
                           (int)avsync_is_locked(), jbuf_count(),
                           (unsigned long long)((ps.play_base_us +
                               audio_get_clock_us()) / 1000000ULL),
                           ps.frame_count);
            }
        }
#endif

        player_display_frame(&ps);

        if (next_popup && ps.frame_count > 0) {
            // Fence the in-flight video draw before CPU framebuffer writes,
            // same as the HUD overlay does.
            rsxSync();
            player_draw_next_popup(auto_secs);
        }

        flip();
        flip_queued = true;
    }

    crash_log("p11 loop exit");
    {
        char buf[96];
        snprintf(buf, sizeof(buf),
            "show_player: loop exit running=%u playing=%d vdec_err=%d fr=%d",
            running, (int)ps.playing, (int)s_vdec_error, ps.frame_count);
        plog(buf);
    }

    // Transcoded streams often run slightly short of RunTimeTicks, so EOF
    // can land before the countdown reaches zero.  If playback ended on its
    // own inside the countdown window, still advance to the next episode.
    if (in_auto_window && !s_next_requested && !user_stopped &&
        !ps.playing && running && !s_vdec_error) {
        plog("next_auto: eof inside countdown window");
        s_next_requested = true;
    }

    timing_shutdown();
    hud_shutdown();

    // Final position for the server's resume bookmark — read before the
    // audio clock is torn down.
    u64 final_pos_ticks = (ps.play_base_us + audio_get_clock_us()) * 10ULL;

    // Signal all threads to stop, join in order: decode → audio → upload
    ps.playing = false;
    usleep(16000);

    if (ps.dec_tid) {
        u64 tret;
        sysThreadJoin(ps.dec_tid, &tret);
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
    if (prog_tid) {
        u64 tret;
        sysThreadJoin(prog_tid, &tret);
    }
    crash_log("p12 threads joined");

    // Tell the server where we stopped (also finalizes Continue Watching).
    jellyfin_report_stopped(item->id, ps.session_id, final_pos_ticks);

    // Free video GPU blit resources before releasing the jitter buffer
    vid_gpu_free();

    crash_log("p17 jbuf_free begin");
    jbuf_free();
    crash_log("p18 jbuf_free OK");
    netClose(ps.sock);
    adec_stop();
    crash_log("p15 audio_close begin");
    audio_close();
    crash_log("p16 audio_close OK");
    crash_log("p13 vdec_close begin");
    vdec_close();
    crash_log("p14 vdec_close OK");

    thumb_cache_init();
    ui_restore_rsx_state();
    crash_log("p19 done");
    plog("show_player: done");
    slog_state("PLAYBACK_STOPPED reason=%s frames=%d vdec_err=%d",
               user_stopped     ? "user_stop"
             : s_next_requested ? "next_item"
             : s_vdec_error     ? "vdec_error" : "eof",
               ps.frame_count, (int)s_vdec_error);
}
