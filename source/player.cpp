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

#include "plog.h"
#include "stream.h"
#include "audio.h"
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
// show_player — public entry point
// -------------------------------------------------------

void show_player(const JFItem *item) {
    plog("show_player: enter");

    char session_id[32];
    snprintf(session_id, sizeof(session_id), "ps3-%u", (unsigned)time(NULL));

    // H.264 level 3.1 caps at 1280×720 @ 30fps
    u32 req_w = display_width  < 1280 ? display_width  : 1280;
    u32 req_h = display_height < 720  ? display_height : 720;

    char url[640];
    snprintf(url, sizeof(url),
        "%s/Videos/%s/stream.ts"
        "?VideoCodec=h264"
        "&VideoProfile=baseline"
        "&VideoLevel=30"
        "&MaxWidth=%u&MaxHeight=%u"
        "&VideoBitrate=4000000"
        "&AudioCodec=aac&AudioBitrate=192000"
        "&MaxAudioChannels=2"
        "&MaxFramerate=24"
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

    // Shorten socket timeout so timing_frame_due() fires within 5 ms of deadline
    { struct { u32 sec; u32 usec; } tv = { 0, 5000 };
      setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)); }

    u8   ts_pkt[TS_PACKET_SIZE];
    bool playing      = true;
    bool first_pkt    = true;
    int  frame_count  = 0;
    u64  hb_last_us   = 0;
    int  rd                    = 0;
    bool in_stall              = false;
    u64  stall_ep_start_us     = 0;
    long stall_ep_count        = 0;
    long stall_ep_dur_max_us   = 0;
    long stall_ep_dur_total_us = 0;

    // ---- Pre-fill: decode JBUF_PREFILL frames before display starts ----
    plog("jbuf: pre-fill start");
    while (jbuf_count() < JBUF_PREFILL && running && !s_vdec_error) {
        sysUtilCheckCallback();
        rd = stream_read(sock, ts_pkt, TS_PACKET_SIZE);
        if (rd < 0) { plog("jbuf prefill: stream disconnected"); playing = false; break; }
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
    if (playing) {
        plog("jbuf: pre-fill done — starting display");
        timing_init(24, 1);
        hb_last_us = timing_get_us();
    }

    // ---- Main playback loop ----
    while (running && playing && !s_vdec_error) {
        sysUtilCheckCallback();

        padInfo pi; padData pd;
        ioPadGetInfo(&pi);
        for (int i = 0; i < MAX_PADS; i++) {
            if (!pi.status[i]) continue;
            ioPadGetData(i, &pd); update_buttons(&pd);
            if (BTN_PRESSED(start)) { playing = false; break; }
        }
        if (!playing) break;

        // === DECODE SIDE — drain socket greedily to fill jitter buffer ===
        // Break early if a display deadline arrives mid-batch.
        {
            bool stream_done = false;
            for (int batch = 0; batch < 64 && jbuf_count() < JBUF_SIZE; batch++) {
                if (timing_frame_due()) break;
                u64 t0 = timing_get_us();
                rd = stream_read(sock, ts_pkt, TS_PACKET_SIZE);
                if (rd == 0) {
                    if (!in_stall) { in_stall = true; stall_ep_start_us = t0; }
                } else {
                    if (in_stall) {
                        in_stall = false;
                        long dur = (long)(timing_get_us() - stall_ep_start_us);
                        stall_ep_dur_total_us += dur;
                        if (dur > stall_ep_dur_max_us) stall_ep_dur_max_us = dur;
                        stall_ep_count++;
                    }
                }
                if (rd < 0) { stream_done = true; break; }
                if (rd == 0) break;
                video_feed_ts(ts_pkt);
            }
            if (stream_done) { plog("show_player: stream disconnected"); break; }
        }

        // === DISPLAY SIDE — drain at fixed 33 ms intervals ===
        if (timing_frame_due()) {
            const u8 *rslot = jbuf_peek();
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
                u32       *dst = color_buffer[curr_fb ^ 1];

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

                flip();
                timing_frame_shown();
                audio_silence();
                jbuf_pop();
                frame_count++;
            }
        }

        // Heartbeat every 2.5 s (wall-clock)
        {
            u64 hb_now = timing_get_us();
            if (hb_now - hb_last_us >= 2500000ULL) {
                hb_last_us = hb_now;
                char buf[128];
                long avg_ms = stall_ep_count ? stall_ep_dur_total_us / stall_ep_count / 1000 : 0;
                // ab: audio blocks consumed (each = 5.333 ms); expected ~469/hb at 30fps
                snprintf(buf, sizeof(buf), "hb: fr=%d q=%d au=%u ab=%llu stalls=%ld max=%ldms avg=%ldms",
                    frame_count, jbuf_count(), s_au_submitted,
                    (unsigned long long)audio_block_count(),
                    stall_ep_count, stall_ep_dur_max_us / 1000, avg_ms);
                plog(buf);
                stall_ep_count = stall_ep_dur_max_us = stall_ep_dur_total_us = 0;
            }
        }
    }

    {
        char buf[96];
        snprintf(buf, sizeof(buf),
            "show_player: loop exit running=%u playing=%d vdec_err=%d fr=%d",
            running, (int)playing, (int)s_vdec_error, frame_count);
        plog(buf);
    }
    jbuf_free();
    netClose(sock);
    audio_close();
    vdec_close();

    setRenderTarget(curr_fb);
    init_btns();
    plog("show_player: done");
}
