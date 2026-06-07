// Decode, audio, and upload thread functions.

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <ppu-types.h>
#include <sys/thread.h>
#include <sys/mutex.h>

#include "plog.h"
#include "stream.h"
#include "audio.h"
#include "adec.h"
#include "video.h"
#include "timing.h"
#include "player_internal.h"

extern u32 running;

// -------------------------------------------------------
// Decode thread  (Steps 2, 5c, 8b)
// -------------------------------------------------------

void decode_thread_fn(void *arg) {
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

    while (running && *playing && *ctx->dec_run && !s_vdec_error) {
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

void audio_thread_fn(void *arg) {
    AudioCtx      *ctx     = (AudioCtx*)arg;
    volatile bool *playing = ctx->playing;
    volatile bool *paused  = ctx->paused;

    while (running && *playing) {
        if (*paused || !audio_write_pcm())
            usleep(1000);
    }

    plog("audio_thread: exit");
    sysThreadExit(0);
}

// -------------------------------------------------------
// Upload thread — memcpy jbuf front slot → RSX-local back texture
// -------------------------------------------------------

void upload_thread_fn(void *arg) {
    UploadCtx     *ctx     = (UploadCtx*)arg;
    volatile bool *playing = ctx->playing;
    u32 nbytes = ctx->fw * ctx->fh * 4;

    while (running && *playing && !s_vdec_error) {
        if (s_vid_frame_ready) {
            usleep(500);
            continue;
        }

        sysMutexLock(s_jbuf_mtx, 0);
        const u8 *slot_a = jbuf_peek();
        const u8 *slot_b = jbuf_peek_next();
        sysMutexUnlock(s_jbuf_mtx);

        if (!slot_a) { usleep(1000); continue; }

        __asm__ volatile("sync" ::: "memory");
        int back = s_vid_disp_idx ^ 1;
        memcpy((void*)(u32*)s_vid_tex_buf[back], (const void*)slot_a, nbytes);
        if (slot_b) {
            memcpy((void*)(u32*)s_vid_tex_buf_b[back], (const void*)slot_b, nbytes);
            s_vid_b_present = true;
        } else {
            s_vid_b_present = false;
        }
        __asm__ volatile("sync" ::: "memory");
        s_vid_frame_ready = true;
    }

    sysThreadExit(0);
}
