// player.cpp — Jellyfin PS3 media player orchestrator

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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
#include "ui.h"
#include "jellyfin_api.h"
#include "rsxutil.h"

// -------------------------------------------------------
// Debug log — survives crashes, written before each step
// -------------------------------------------------------

static FILE *s_plog = NULL;
void plog(const char *msg) {
    if (!s_plog) s_plog = fopen("/dev_hdd0/tmp/player_log.txt", "w");
    if  (s_plog) {
        time_t t = time(NULL);
        struct tm *tm = localtime(&t);
        fprintf(s_plog, "[%04d-%02d-%02d %02d:%02d:%02d] %s\n",
                tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
                tm->tm_hour, tm->tm_min, tm->tm_sec, msg);
        fflush(s_plog);
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
// Network ring buffer  (Step 5a)
// 256 KB / TS_PACKET_SIZE (188) = 1394 packet slots
// -------------------------------------------------------

#define NET_RING_PKTS  1394
static u8           s_netbuf[NET_RING_PKTS][TS_PACKET_SIZE];
static int          s_net_wr = 0, s_net_rd = 0;
static volatile int s_net_n  = 0;
static sys_mutex_t  s_net_mtx;

// -------------------------------------------------------
// Network thread  (Step 5b)
// -------------------------------------------------------

struct NetworkCtx {
    int           sock;
    volatile bool *playing;
};

static void network_thread_fn(void *arg) {
    NetworkCtx    *ctx     = (NetworkCtx*)arg;
    int            sock    = ctx->sock;
    volatile bool *playing = ctx->playing;

    u8 pkt[TS_PACKET_SIZE];

    while (running && *playing && !s_vdec_error) {
        int rd = stream_read(sock, pkt, TS_PACKET_SIZE);
        if (rd < 0) {
            plog("playing=0 reason=stream_eof");
            *playing = false;
            break;
        }
        if (rd == 0) continue;

        sysMutexLock(s_net_mtx, 0);
        if (s_net_n >= NET_RING_PKTS) {
            sysMutexUnlock(s_net_mtx);
            usleep(1000);
            continue;
        }
        memcpy(s_netbuf[s_net_wr], pkt, TS_PACKET_SIZE);
        s_net_wr = (s_net_wr + 1) % NET_RING_PKTS;
        s_net_n++;
        sysMutexUnlock(s_net_mtx);
    }

    plog("network_thread: exit");
    sysThreadExit(0);
}

// -------------------------------------------------------
// Decode thread  (Steps 2, 5c, 8b)
// -------------------------------------------------------

struct DecodeCtx {
    volatile bool *playing;
    int           *frame_count;  // read-only for heartbeat (benign race)
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
            u64 t0 = timing_get_us();

            // Read one packet from the network ring buffer (Step 5c)
            sysMutexLock(s_net_mtx, 0);
            bool have_pkt = (s_net_n > 0);
            if (have_pkt) {
                memcpy(ts_pkt, s_netbuf[s_net_rd], TS_PACKET_SIZE);
                s_net_rd = (s_net_rd + 1) % NET_RING_PKTS;
                s_net_n--;
            }
            sysMutexUnlock(s_net_mtx);

            if (!have_pkt) {
                if (!in_stall) { in_stall = true; stall_ep_start_us = t0; }
                usleep(1000);
                break;
            }

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
                display_fps, s_net_n);
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
        audio_write_pcm();
    }

    plog("audio_thread: exit");
    sysThreadExit(0);
}

// -------------------------------------------------------
// show_player — public entry point
// -------------------------------------------------------

void show_player(const JFItem *item) {
    plog("show_player: enter");

    char session_id[32];
    snprintf(session_id, sizeof(session_id), "ps3-%u", (unsigned)time(NULL));

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
        "&MediaSourceId=%s&PlaySessionId=%s",
    g_server, item->id, req_w, req_h, item->id, session_id);
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
        return;
    }
    {
        char buf[72];
        snprintf(buf, sizeof(buf), "jbuf: %d slots %ux%u ~%u KB each",
                 JBUF_SIZE, req_w, req_h, req_w * req_h * 4 / 1024);
        plog(buf);
    }

    // 5 ms socket receive timeout keeps the network thread responsive
    { struct { u32 sec; u32 usec; } tv = { 0, 5000 };
      setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)); }

    // Initialise network ring buffer mutex
    s_net_wr = s_net_rd = 0; s_net_n = 0;
    {
        sys_mutex_attr_t mattr;
        memset(&mattr, 0, sizeof(mattr));
        mattr.attr_protocol  = SYS_LWMUTEX_ATTR_PROTOCOL;
        mattr.attr_recursive = SYS_MUTEX_ATTR_RECURSIVE;
        sysMutexCreate(&s_net_mtx, &mattr);
    }

    u8            ts_pkt[TS_PACKET_SIZE];
    volatile bool playing    = true;
    bool          first_pkt  = true;
    int           frame_count = 0;
    int           rd          = 0;

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
        sysMutexDestroy(s_net_mtx);
        jbuf_free();
        netClose(sock);
        audio_close();
        vdec_close();
        return;
    }

    plog("jbuf: pre-fill done — starting threads");
    timing_init(24, 1);   // placeholder; overwritten by fps detection (Step 1/7)

    // ---- Spawn network thread (Step 5d) ----
    NetworkCtx       net_ctx = { sock, &playing };
    sys_ppu_thread_t net_tid = 0;
    {
        int trc = sysThreadCreate(&net_tid, network_thread_fn,
                                  (void *)&net_ctx,
                                  600, 64 * 1024,
                                  0, "jf_network");
        if (trc != 0) {
            char buf[64];
            snprintf(buf, sizeof(buf), "show_player: net thread_create FAILED rc=%d", trc);
            plog(buf);
            playing = false;
        }
    }

    // ---- Spawn decode thread (Step 2) ----
    DecodeCtx        dec_ctx = { &playing, &frame_count };
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
                                  900, 32 * 1024,
                                  0, "jf_audio");
        if (trc != 0) {
            char buf[64];
            snprintf(buf, sizeof(buf), "show_player: aud thread_create FAILED rc=%d", trc);
            plog(buf);
            playing = false;
        }
    }

    // Detection timeout tracking for Step 7e fallback
    u64 det_timeout_start = 0;

    // ---- Main (display) loop  (Steps 1, 3, 7d) ----
    while (running && playing && !s_vdec_error) {
        waitflip();
        sysUtilCheckCallback();

        padInfo pi; padData pd;
        ioPadGetInfo(&pi);
        for (int i = 0; i < MAX_PADS; i++) {
            if (!pi.status[i]) continue;
            ioPadGetData(i, &pd); update_buttons(&pd);
            if (BTN_PRESSED(start)) {
                plog("playing=0 reason=user_stop");
                playing = false; break;
            }
        }
        if (!playing) break;

        if (!s_timing_ready) {
            // Let frames accumulate until fps detection completes (Step 7d)
            if (det_timeout_start == 0) det_timeout_start = timing_get_us();
            if (timing_get_us() - det_timeout_start >= 5000000ULL) {
                plog("fps_detect: timeout, fallback to 24fps");
                timing_init(24, 1);
                s_timing_ready = true;
            }
        } else if (timing_frame_due()) {
            sysMutexLock(s_jbuf_mtx, 0);
            const u8 *rslot = jbuf_peek();
            sysMutexUnlock(s_jbuf_mtx);

            if (rslot) {
                u32 fw = jbuf_fw(), fh = jbuf_fh();

                // Fit into display preserving aspect ratio, center with black bars.
                u32 sw, sh;
                if ((u64)fw * display_height > (u64)fh * display_width) {
                    // wider than display → letterbox (black top/bottom)
                    sw = display_width;
                    sh = (u32)((u64)fh * display_width / fw);
                } else {
                    // narrower or equal → pillarbox (black left/right)
                    sh = display_height;
                    sw = (u32)((u64)fw * display_height / fh);
                }
                u32 ox0 = (display_width  - sw) / 2;
                u32 oy0 = (display_height - sh) / 2;

                const u32 *src = (const u32*)rslot;
                u32       *dst = color_buffer[curr_fb];

                // Precompute source-X and source-Y lookup tables.
                // One 64-bit divide per output column/row instead of per pixel —
                // eliminates all software-emulated divides from the inner blit loop.
                static u32 sx[2048], sy[1200];
                for (u32 ox = 0; ox < sw && ox < 2048; ox++)
                    sx[ox] = (u32)((u64)ox * fw / sw);
                for (u32 oy = 0; oy < sh && oy < 1200; oy++)
                    sy[oy] = (u32)((u64)oy * fh / sh);

                // Black top bar
                if (oy0 > 0) memset(dst, 0, oy0 * display_width * 4);

                // Video rows — inner loop is pure indexed load + store, no division
                for (u32 oy = 0; oy < sh; oy++) {
                    const u32 *srow = src + sy[oy] * fw;
                    u32       *drow = dst + (oy0 + oy) * display_width;
                    if (ox0 > 0) {
                        memset(drow,                0, ox0 * 4);
                        memset(drow + ox0 + sw,     0, ox0 * 4);
                    }
                    for (u32 ox = 0; ox < sw; ox++)
                        drow[ox0 + ox] = srow[sx[ox]];
                }

                // Black bottom bar
                if (oy0 > 0) memset(dst + (oy0 + sh) * display_width, 0, oy0 * display_width * 4);

                sysMutexLock(s_jbuf_mtx, 0);
                jbuf_pop();
                sysMutexUnlock(s_jbuf_mtx);
                frame_count++;
                timing_frame_shown();
            }
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

    // Signal all threads to stop, join in order: network → decode → audio (Steps 5e, 6d)
    playing = false;

    if (net_tid) {
        u64 tret;
        sysThreadJoin(net_tid, &tret);
        plog("show_player: network thread joined");
    }
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

    sysMutexDestroy(s_net_mtx);
    jbuf_free();
    netClose(sock);
    audio_close();
    vdec_close();

    setRenderTarget(curr_fb);
    init_btns();
    plog("show_player: done");
}
