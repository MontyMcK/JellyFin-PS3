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
#include "ui.h"
#include "jellyfin_api.h"
#include "rsxutil.h"
#include "video_shaders.h"

// -------------------------------------------------------
// Video GPU blit state (allocated per playback session)
// -------------------------------------------------------

// Double-buffered textures: upload thread writes to tex_buf[disp_idx^1],
// display thread binds tex_buf[disp_idx] for RSX draw.
static u32 *s_vid_tex_buf[2]    = {NULL, NULL};
static u32  s_vid_tex_off[2]    = {0, 0};
static u32 *s_vid_fp_buf        = NULL;  // RSX-local FP ucode copy
static u32  s_vid_fp_off        = 0;
static u8  *s_vid_vbuf          = NULL;  // RSX-local 4-vertex quad buffer
static u32  s_vid_vbuf_off      = 0;
static volatile int  s_vid_disp_idx    = 0;     // index RSX is reading from
static volatile bool s_vid_frame_ready = false;  // upload → display handoff

// -------------------------------------------------------
// Async log — ring buffer drained by a dedicated thread
// -------------------------------------------------------

#define PLOG_RING  256
#define PLOG_LEN   128

static char              s_plog_ring[PLOG_RING][PLOG_LEN];
static volatile int      s_plog_wr      = 0;
static volatile int      s_plog_rd      = 0;
static volatile int      s_plog_lock    = 0;
static volatile bool     s_plog_running = false;
static FILE             *s_plog_file    = NULL;
static sys_ppu_thread_t  s_plog_tid     = 0;

void plog(const char *msg) {
    while (!__sync_bool_compare_and_swap(&s_plog_lock, 0, 1))
        ;
    int next = (s_plog_wr + 1) % PLOG_RING;
    if (next != s_plog_rd) {
        strncpy(s_plog_ring[s_plog_wr], msg, PLOG_LEN - 1);
        s_plog_ring[s_plog_wr][PLOG_LEN - 1] = '\0';
        s_plog_wr = next;
    }
    __sync_bool_compare_and_swap(&s_plog_lock, 1, 0);
}

static void plog_drain(void) {
    char local[PLOG_LEN];
    while (true) {
        while (!__sync_bool_compare_and_swap(&s_plog_lock, 0, 1))
            ;
        bool empty = (s_plog_rd == s_plog_wr);
        if (!empty) {
            memcpy(local, s_plog_ring[s_plog_rd], PLOG_LEN);
            s_plog_rd = (s_plog_rd + 1) % PLOG_RING;
        }
        __sync_bool_compare_and_swap(&s_plog_lock, 1, 0);
        if (empty) break;
        if (s_plog_file) {
            time_t t = time(NULL);
            struct tm *tm = localtime(&t);
            fprintf(s_plog_file, "[%04d-%02d-%02d %02d:%02d:%02d] %s\n",
                    tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
                    tm->tm_hour, tm->tm_min, tm->tm_sec, local);
            fflush(s_plog_file);
        }
    }
}

static void plog_thread_fn(void *arg) {
    (void)arg;
    while (s_plog_running) {
        plog_drain();
        usleep(5000);
    }
    plog_drain();
    sysThreadExit(0);
}

void plog_start(void) {
    s_plog_file    = fopen("/dev_hdd0/tmp/player_log.txt", "w");
    s_plog_running = true;
    sysThreadCreate(&s_plog_tid, plog_thread_fn, NULL,
                    1200, 32 * 1024, 0, "jf_plog");
}

void plog_stop(void) {
    s_plog_running = false;
    if (s_plog_tid) {
        u64 tret;
        sysThreadJoin(s_plog_tid, &tret);
        s_plog_tid = 0;
    }
    if (s_plog_file) {
        fclose(s_plog_file);
        s_plog_file = NULL;
    }
}

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
// Decode thread  (Steps 2, 5c, 8b)
// -------------------------------------------------------

struct DecodeCtx {
    volatile bool *playing;
    int           *frame_count;  // read-only for heartbeat (benign race)
    int            sock;
};

static void decode_thread_fn(void *arg) {
    DecodeCtx     *ctx         = (DecodeCtx*)arg;
    volatile bool *playing     = ctx->playing;
    int           *frame_count = ctx->frame_count;

    u8   ts_pkt[TS_PACKET_SIZE];
    bool in_stall              = false;
    u64  stall_ep_start_us     = 0;
    long stall_ep_count        = 0;
    long stall_ep_dur_max_us   = 0;
    long stall_ep_dur_total_us = 0;
    u64  hb_last_us            = timing_get_us();
    int  hb_fr_last            = 0;

    while (running && *playing && !s_vdec_error) {
        if (jbuf_count() >= JBUF_SIZE) {
            usleep(1000);
            continue;
        }

        for (int batch = 0; batch < 128 && jbuf_count() < JBUF_SIZE; batch++) {
            int rd = stream_read(ctx->sock, ts_pkt, TS_PACKET_SIZE);
            if (rd < 0) {
                plog("playing=0 reason=stream_eof");
                *ctx->playing = false;
                break;
            }
            if (rd == 0) { usleep(1000); continue; }

            if (in_stall) {
                in_stall = false;
                long dur = (long)(timing_get_us() - stall_ep_start_us);
                stall_ep_dur_total_us += dur;
                if (dur > stall_ep_dur_max_us) stall_ep_dur_max_us = dur;
                stall_ep_count++;
            }

            video_feed_ts(ts_pkt);
        }

        // Drain all decoded frames from VDEC into the jitter buffer
        while (s_frames_ready > 0 && jbuf_count() < JBUF_SIZE) {
            if (!vdec_pull_frame()) break;
        }

        {
            int q = jbuf_count();
            if (q < 4) {
                char buf[32];
                snprintf(buf, sizeof(buf), "jbuf_low: q=%d", q);
                plog(buf);
            }
        }

        // Heartbeat every 2.5 s (wall-clock)  (Step 8b: add fps= field)
        u64 hb_now = timing_get_us();
        if (hb_now - hb_last_us >= 2500000ULL) {
            float display_fps = (*frame_count - hb_fr_last) * 1000000.0f
                                / (float)(hb_now - hb_last_us);
            hb_fr_last = *frame_count;
            hb_last_us = hb_now;
            char buf[160];
            long avg_ms = stall_ep_count ? stall_ep_dur_total_us / stall_ep_count / 1000 : 0;
            snprintf(buf, sizeof(buf),
                "hb: fr=%d q=%d au=%u ab=%llu stalls=%ld max=%ldms avg=%ldms fps=%.1f netq=%d",
                *frame_count, jbuf_count(), s_au_submitted,
                (unsigned long long)audio_block_count(),
                stall_ep_count, stall_ep_dur_max_us / 1000, avg_ms,
                display_fps, 0);
            plog(buf);
            stall_ep_count = stall_ep_dur_max_us = stall_ep_dur_total_us = 0;
        }
    }

    sysThreadExit(0);
}

// -------------------------------------------------------
// Audio thread  (Step 6a)
// -------------------------------------------------------

struct AudioCtx {
    volatile bool *playing;
};

static void audio_thread_fn(void *arg) {
    AudioCtx      *ctx     = (AudioCtx*)arg;
    volatile bool *playing = ctx->playing;

    while (running && *playing) {
        if (!audio_write_pcm())
            usleep(1000);
    }

    plog("audio_thread: exit");
    sysThreadExit(0);
}

// -------------------------------------------------------
// Upload thread — memcpy jbuf front slot → RSX-local back texture
// -------------------------------------------------------

struct UploadCtx {
    volatile bool *playing;
    u32 fw, fh;
};

static void upload_thread_fn(void *arg) {
    UploadCtx     *ctx     = (UploadCtx*)arg;
    volatile bool *playing = ctx->playing;
    u32 nbytes = ctx->fw * ctx->fh * 4;

    while (running && *playing && !s_vdec_error) {
        if (s_vid_frame_ready) {
            // Display thread hasn't consumed the last upload yet.
            usleep(500);
            continue;
        }

        sysMutexLock(s_jbuf_mtx, 0);
        const u8 *slot = jbuf_peek();
        sysMutexUnlock(s_jbuf_mtx);

        if (!slot) {
            usleep(1000);
            continue;
        }

        // Upload to back buffer (the one RSX is NOT currently reading).
        // Acquire barrier: ensure we see the latest s_vid_disp_idx after any
        // pending display-thread flip before snapshotting the back index.
        __asm__ volatile("sync" ::: "memory");
        int back = s_vid_disp_idx ^ 1;
        memcpy(s_vid_tex_buf[back], slot, nbytes);
        // Ensure all PPU stores to GDDR3 are visible before signalling.
        __asm__ volatile("sync" ::: "memory");
        s_vid_frame_ready = true;
    }

    sysThreadExit(0);
}

// -------------------------------------------------------
// show_player — public entry point
// -------------------------------------------------------

void show_player(const JFItem *item) {
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
    u32 req_w = display_width  < 960 ? display_width  : 960;
    u32 req_h = display_height < 540 ? display_height : 540;

    char url[640];
    snprintf(url, sizeof(url),
        "%s/Videos/%s/stream.ts"
        "?VideoCodec=h264"
        "&VideoProfile=baseline"
        "&VideoLevel=30"
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

    plog("show_player: vdec_open");
    if (!vdec_open()) {
        plog("show_player: vdec_open FAILED");
        vdec_close();
        show_error("VDEC init failed.", "See /dev_hdd0/tmp/player_log.txt");
        ui_restore_rsx_state();
        return;
    }
    plog("show_player: vdec_open OK");

    plog("show_player: audio_open");
    audio_open();
    adec_init();
    plog("show_player: audio_open done");

    drawHeader();
    drawTextf(40, 100, "%.70s", item->name);
    drawText(40, 130, "Connecting to stream...");
    flip();

    plog("show_player: stream_open");
    int sock = stream_open(url);
    if (sock < 0) {
        plog("show_player: stream_open FAILED");
        audio_close();
        vdec_close();
        show_error("Stream connection failed.", url);
        ui_restore_rsx_state();
        return;
    }
    plog("show_player: stream_open OK");

    drawHeader();
    drawTextf(40, 100, "%.70s", item->name);
    drawText(40, 130, "Streaming... START=stop");
    flip();

    video_reset();

    if (!jbuf_alloc(req_w, req_h)) {
        plog("show_player: jbuf_alloc FAILED");
        netClose(sock);
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

    // Video GPU blit init — allocate RSX buffers once per session
    {
        u32 fw = jbuf_fw(), fh = jbuf_fh();

        s_vid_tex_buf[0] = (u32*)rsxMemalign(64, fw * fh * 4);
        rsxAddressToOffset(s_vid_tex_buf[0], &s_vid_tex_off[0]);
        s_vid_tex_buf[1] = (u32*)rsxMemalign(64, fw * fh * 4);
        rsxAddressToOffset(s_vid_tex_buf[1], &s_vid_tex_off[1]);
        s_vid_disp_idx    = 0;
        s_vid_frame_ready = false;

        rsxFragmentProgram *vid_fpo = (rsxFragmentProgram*)video_fp_data;
        void *fp_ucode; u32 fp_size;
        rsxFragmentProgramGetUCode(vid_fpo, &fp_ucode, &fp_size);
        s_vid_fp_buf = (u32*)rsxMemalign(256, fp_size);
        memcpy(s_vid_fp_buf, fp_ucode, fp_size);
        rsxAddressToOffset(s_vid_fp_buf, &s_vid_fp_off);

        // 4 vertices: float4 pos + float2 uv = 24 bytes each
        s_vid_vbuf = (u8*)rsxMemalign(128, 4 * 24);

        // Aspect-ratio fit — same formula as display loop
        u32 sw, sh;
        if ((u64)fw * display_height > (u64)fh * display_width) {
            sw = display_width;
            sh = (u32)((u64)fh * display_width / fw);
        } else {
            sh = display_height;
            sw = (u32)((u64)fw * display_height / fh);
        }
        u32 ox0v = (display_width  - sw) / 2;
        u32 oy0v = (display_height - sh) / 2;
        float cx0 = (float)ox0v / display_width  * 2.0f - 1.0f;
        float cx1 = (float)(ox0v + sw) / display_width  * 2.0f - 1.0f;
        float cy0 = 1.0f - (float)oy0v / display_height * 2.0f;
        float cy1 = 1.0f - (float)(oy0v + sh) / display_height * 2.0f;

        float *v = (float*)s_vid_vbuf;
        v[0]=cx0; v[1]=cy0; v[2]=0.f; v[3]=1.f; v[4]=0.f; v[5]=0.f;   // TL
        v[6]=cx1; v[7]=cy0; v[8]=0.f; v[9]=1.f; v[10]=1.f; v[11]=0.f; // TR
        v[12]=cx0; v[13]=cy1; v[14]=0.f; v[15]=1.f; v[16]=0.f; v[17]=1.f; // BL
        v[18]=cx1; v[19]=cy1; v[20]=0.f; v[21]=1.f; v[22]=1.f; v[23]=1.f; // BR
        rsxAddressToOffset(s_vid_vbuf, &s_vid_vbuf_off);

        plog("vid_gpu: init done");
    }

    // 5 ms socket receive timeout keeps the network thread responsive
    { struct { u32 sec; u32 usec; } tv = { 0, 5000 };
      setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)); }

    u8            ts_pkt[TS_PACKET_SIZE];
    volatile bool playing    = true;
    bool          first_pkt  = true;
    int           frame_count = 0;
    int           rd          = 0;
    u64           s_ref_pts_us  = 0;
    u64           s_ref_wall_us = 0;

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
        if (s_vid_tex_buf[0]) { rsxFree(s_vid_tex_buf[0]); s_vid_tex_buf[0] = NULL; }
        if (s_vid_tex_buf[1]) { rsxFree(s_vid_tex_buf[1]); s_vid_tex_buf[1] = NULL; }
        if (s_vid_fp_buf)     { rsxFree(s_vid_fp_buf);     s_vid_fp_buf     = NULL; }
        if (s_vid_vbuf)       { rsxFree(s_vid_vbuf);       s_vid_vbuf       = NULL; }
        jbuf_free();
        netClose(sock);
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
    AudioCtx         aud_ctx = { &playing };
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

    // Detection timeout tracking for Bresenham fallback initialisation
    u64 det_timeout_start = 0;

    // ---- Main (display) loop ----
    while (running && playing && !s_vdec_error) {
        waitflip();
        sysUtilCheckCallback();

        padInfo pi; padData pd;
        ioPadGetInfo(&pi);
        for (int i = 0; i < MAX_PADS; i++) {
            if (!pi.status[i]) continue;
            ioPadGetData(i, &pd); update_buttons(&pd);
            if (BTN_PRESSED(start)) {
                if (!playing) break;
                plog("playing=0 reason=user_stop");
                playing = false; break;
            }
        }
        if (!playing) break;

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
        // (~5 s at 59.94 Hz). Logs whenever the refreshRates bitmask changes and
        // emits periodic stats so a stable run shows a clean zero-change baseline.
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

        u64  audio_clk      = audio_get_clock_us();
        bool do_pop         = false;
        bool used_bresenham = false;

        if (audio_clk == 0 || !s_timing_ready) {
            // Bresenham fallback: audio not yet started, or fps not yet detected
            if (s_vid_frame_ready && s_timing_ready && timing_flip_due()) {
                do_pop = true;
                used_bresenham = true;
            }
        } else if (s_ref_pts_us == 0) {
            // Anchor reference pair on first frame with a valid PTS
            sysMutexLock(s_jbuf_mtx, 0);
            u64 fpts = (jbuf_count() > 0) ? jbuf_peek_pts() : 0;
            sysMutexUnlock(s_jbuf_mtx);
            if (fpts != 0 && s_vid_frame_ready) {
                s_ref_pts_us  = fpts;
                s_ref_wall_us = audio_clk;
                do_pop = true;
            } else if (fpts == 0 && s_vid_frame_ready && timing_flip_due()) {
                // No PTS yet: advance sequentially via Bresenham until we get one
                do_pop = true;
                used_bresenham = true;
            }
        } else {
            // Audio-clock-driven selection: show frame whose adjusted PTS <= audio offset
            u64 audio_offset = audio_clk - s_ref_wall_us;
            if (s_vid_frame_ready && jbuf_count() > 0) {
                sysMutexLock(s_jbuf_mtx, 0);
                u64 fpts = jbuf_peek_pts();
                sysMutexUnlock(s_jbuf_mtx);
                if (fpts == 0) {
                    do_pop = true;
                } else {
                    s64 adj = (s64)fpts - (s64)s_ref_pts_us;
                    do_pop = (adj <= (s64)audio_offset);
                }
            }
        }

        if (do_pop) {
            sysMutexLock(s_jbuf_mtx, 0);
            jbuf_pop();
            sysMutexUnlock(s_jbuf_mtx);
            frame_count++;
            if (used_bresenham) timing_frame_shown();
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
                    char buf[128];
                    snprintf(buf, sizeof(buf),
                        "frame_interval: fr=%d gap1=%lluus gap2=%lluus audio=%lluus",
                        frame_count,
                        (unsigned long long)s_fi_gaps[0],
                        (unsigned long long)s_fi_gaps[1],
                        (unsigned long long)audio_clk);
                    plog(buf);
                }
            }
        }

        // Re-draw the current frame into the RSX back color buffer every vblank.
        // s_vid_disp_idx advances only on do_pop; hold vblanks re-blit the same texture.
        if (frame_count > 0) {
            if (do_pop) rsxSync();
            u32 fw = jbuf_fw(), fh = jbuf_fh();

            // Clear framebuffer to black (covers bars outside the quad)
            rsxSetClearColor(context, 0x00000000);
            rsxSetClearDepthStencil(context, 0xffff);
            rsxClearSurface(context,
                GCM_CLEAR_R | GCM_CLEAR_G | GCM_CLEAR_B | GCM_CLEAR_A);

            // Bind texture from RSX-local display buffer
            {
                gcmTexture tex;
                memset(&tex, 0, sizeof(tex));
                tex.format    = GCM_TEXTURE_FORMAT_A8R8G8B8 | GCM_TEXTURE_FORMAT_LIN;
                tex.mipmap    = 1;
                tex.dimension = GCM_TEXTURE_DIMS_2D;
                tex.cubemap   = GCM_FALSE;
                tex.remap     =
                    ((u32)GCM_TEXTURE_REMAP_TYPE_REMAP << GCM_TEXTURE_REMAP_TYPE_A_SHIFT)
                  | ((u32)GCM_TEXTURE_REMAP_TYPE_REMAP << GCM_TEXTURE_REMAP_TYPE_R_SHIFT)
                  | ((u32)GCM_TEXTURE_REMAP_COLOR_R    << GCM_TEXTURE_REMAP_COLOR_R_SHIFT)
                  | ((u32)GCM_TEXTURE_REMAP_TYPE_REMAP << GCM_TEXTURE_REMAP_TYPE_G_SHIFT)
                  | ((u32)GCM_TEXTURE_REMAP_COLOR_G    << GCM_TEXTURE_REMAP_COLOR_G_SHIFT)
                  | ((u32)GCM_TEXTURE_REMAP_TYPE_REMAP << GCM_TEXTURE_REMAP_TYPE_B_SHIFT)
                  | ((u32)GCM_TEXTURE_REMAP_COLOR_B    << GCM_TEXTURE_REMAP_COLOR_B_SHIFT);
                tex.width     = (u16)fw;
                tex.height    = (u16)fh;
                tex.depth     = 1;
                tex.location  = GCM_LOCATION_RSX;
                tex.pitch     = fw * 4;
                tex.offset    = s_vid_tex_off[s_vid_disp_idx];
                rsxInvalidateTextureCache(context, GCM_INVALIDATE_TEXTURE);
                rsxLoadTexture(context, 0, &tex);
                rsxTextureControl(context, 0, GCM_TRUE, 0, 12 << 8,
                    GCM_TEXTURE_MAX_ANISO_1);
                rsxTextureFilter(context, 0, 0,
                    GCM_TEXTURE_LINEAR, GCM_TEXTURE_LINEAR,
                    GCM_TEXTURE_CONVOLUTION_QUINCUNX);
                rsxTextureWrapMode(context, 0,
                    GCM_TEXTURE_CLAMP_TO_EDGE, GCM_TEXTURE_CLAMP_TO_EDGE,
                    GCM_TEXTURE_CLAMP_TO_EDGE, 0, GCM_TEXTURE_ZFUNC_LESS, 0);
            }

            // Render state
            rsxSetDepthTestEnable(context, GCM_FALSE);
            rsxSetBlendEnable(context, GCM_FALSE);
            rsxSetColorMask(context,
                GCM_COLOR_MASK_R | GCM_COLOR_MASK_G |
                GCM_COLOR_MASK_B | GCM_COLOR_MASK_A);
            {
                float vp_sc[4]  = { display_width  *  0.5f,
                                   -(float)display_height * 0.5f, 0.5f, 0.0f };
                float vp_off[4] = { display_width  *  0.5f,
                                    (float)display_height * 0.5f, 0.5f, 0.0f };
                rsxSetViewport(context, 0, 0,
                    (u16)display_width, (u16)display_height,
                    0.0f, 1.0f, vp_sc, vp_off);
            }

            // Load shaders
            {
                rsxVertexProgram  *vid_vpo = (rsxVertexProgram*)  video_vp_data;
                rsxFragmentProgram *vid_fpo = (rsxFragmentProgram*) video_fp_data;
                void *vp_ucode; u32 vp_size;
                rsxVertexProgramGetUCode(vid_vpo, &vp_ucode, &vp_size);
                rsxLoadVertexProgram(context, vid_vpo, vp_ucode);
                rsxSetVertexAttribOutputMask(context, vid_vpo->output_mask);
                rsxLoadFragmentProgramLocation(context, vid_fpo,
                    s_vid_fp_off, GCM_LOCATION_RSX);
            }

            // Draw fullscreen quad (triangle strip, 4 verts)
            // Stride 24: float4 pos at offset 0, float2 uv at offset 16
            rsxBindVertexArrayAttrib(context, GCM_VERTEX_ATTRIB_POS, 0,
                s_vid_vbuf_off, 24, 4,
                GCM_VERTEX_DATA_TYPE_F32, GCM_LOCATION_RSX);
            rsxBindVertexArrayAttrib(context, GCM_VERTEX_ATTRIB_TEX0, 0,
                s_vid_vbuf_off + 16, 24, 2,
                GCM_VERTEX_DATA_TYPE_F32, GCM_LOCATION_RSX);
            rsxInvalidateVertexCache(context);
            rsxDrawVertexArray(context, GCM_TYPE_TRIANGLE_STRIP, 0, 4);
        }

        flip();
    }

    {
        char buf[96];
        snprintf(buf, sizeof(buf),
            "show_player: loop exit running=%u playing=%d vdec_err=%d fr=%d",
            running, (int)playing, (int)s_vdec_error, frame_count);
        plog(buf);
    }

    timing_shutdown();

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

    // Free video GPU blit resources before releasing the jitter buffer
    if (s_vid_tex_buf[0]) { rsxFree(s_vid_tex_buf[0]); s_vid_tex_buf[0] = NULL; }
    if (s_vid_tex_buf[1]) { rsxFree(s_vid_tex_buf[1]); s_vid_tex_buf[1] = NULL; }
    if (s_vid_fp_buf)     { rsxFree(s_vid_fp_buf);     s_vid_fp_buf     = NULL; }
    if (s_vid_vbuf)       { rsxFree(s_vid_vbuf);       s_vid_vbuf       = NULL; }

    jbuf_free();
    netClose(sock);
    audio_close();
    vdec_close();

    ui_restore_rsx_state();
    plog("show_player: done");
}
