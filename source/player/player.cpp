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
    padInfo pi; padData pd;
    while (running) {
        sysUtilCheckCallback();
        ioPadGetInfo(&pi);
        for (int i = 0; i < MAX_PADS; i++) {
            if (!pi.status[i]) continue;
            ioPadGetData(i, &pd); update_buttons(&pd);
            if (BTN_PRESSED(circle)) return;
        }
    }
}

// -------------------------------------------------------
// show_player — public entry point
// -------------------------------------------------------

void show_player(const JFItem *item) {
    crash_log("p1 enter");
    plog("show_player: enter");
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

    char url[640];
    snprintf(url, sizeof(url),
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
        "&MediaSourceId=%s",
    g_server, item->id, req_w, req_h, item->id);

    char session_id[64] = "";
    if (jellyfin_get_play_session_id(item->id, session_id, sizeof(session_id))) {
        int ul = strlen(url);
        snprintf(url + ul, sizeof(url) - ul, "&PlaySessionId=%s", session_id);
    } else {
        plog("show_player: PlaybackInfo failed, streaming without PlaySessionId");
    }
    plog(url);

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
    bool          first_pkt  = true;
    int           frame_count = 0;
    int           rd          = 0;

    hud_init(0, NULL);
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
    DecodeCtx        dec_ctx = { &playing, &frame_count, sock };
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

        padInfo pi; padData pd;
        static u8 s_prev_l2 = 0, s_prev_r2 = 0;
        bool l2_pressed = false, r2_pressed = false;
        ioPadGetInfo(&pi);
        for (int i = 0; i < MAX_PADS; i++) {
            if (!pi.status[i]) continue;
            ioPadGetData(i, &pd); update_buttons(&pd);
            l2_pressed |= (!s_prev_l2 && pd.BTN_L2);
            r2_pressed |= (!s_prev_r2 && pd.BTN_R2);
            s_prev_l2 = pd.BTN_L2;
            s_prev_r2 = pd.BTN_R2;
            if (BTN_PRESSED(start)) {
                if (!playing) break;
                plog("playing=0 reason=user_stop");
                playing = false; break;
            }
        }
        if (!playing) break;

        {
            HudAction act = hud_handle_input(l2_pressed, r2_pressed, paused);
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
                char buf[64];
                snprintf(buf, sizeof(buf), "hud: seek %+d s", hud_seek_delta());
                plog(buf);
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
        }

        // Re-draw every vblank. s_vid_disp_idx advances on do_pop or silent_flip.
        if (frame_count > 0) {
            if (do_pop) rsxSync();
            u32 fw = jbuf_fw(), fh = jbuf_fh();
            vid_gpu_draw(render_blend, blend_factor, fw, fh);
        }

        {
            static int s_vb_log_n = 0;
            static int s_vb_log_start_fr = -1;
            // Skip first 60 frames (startup), then log 200 vblanks
            if (frame_count >= 60 && frame_count < 260 && s_timing_ready) {
                if (s_vb_log_start_fr < 0) s_vb_log_start_fr = frame_count;
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

        // HUD overlay — rsxSync() is called inside hud_draw() after the GPU dim quad.
        if (hud_is_visible() && frame_count > 0) {
            hud_draw(audio_get_clock_us(), paused);
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
