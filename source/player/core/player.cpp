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

extern void crash_log(const char *msg);

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

    char url[768];
    build_stream_url(url, sizeof(url), &ps, 0);
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
    ps.sock = stream_open(url);
    if (ps.sock < 0) {
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

    if (!jbuf_alloc(ps.req_w, ps.req_h)) {
        plog("show_player: jbuf_alloc FAILED");
        netClose(ps.sock);
        adec_stop();
        audio_close();
        vdec_close();
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

    crash_log("p9 threads started");

    crash_log("p10 loop");
    // ---- Main (display) loop ----
    while (running && ps.playing && !s_vdec_error) {
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
        } else if (act == HUD_ACTION_SEEK) {
            if (!player_execute_seek(&ps)) break;
        }

        player_display_frame(&ps);

        flip();
    }

    crash_log("p11 loop exit");
    {
        char buf[96];
        snprintf(buf, sizeof(buf),
            "show_player: loop exit running=%u playing=%d vdec_err=%d fr=%d",
            running, (int)ps.playing, (int)s_vdec_error, ps.frame_count);
        plog(buf);
    }

    timing_shutdown();
    hud_shutdown();

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
    crash_log("p12 threads joined");

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

    ui_restore_rsx_state();
    crash_log("p19 done");
    plog("show_player: done");
}
