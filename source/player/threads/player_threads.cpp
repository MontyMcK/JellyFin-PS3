// Decode, audio, and upload thread functions, plus the decode-thread
// spawn helper shared by the initial open and the post-seek respawn.

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
#include "hd1080.h"
#include "video.h"
#include "timing.h"
#include "player_internal.h"
#include "jellyfin_api.h"

extern u32 running;

// -------------------------------------------------------
// Decode-thread spawn — initial open and post-seek respawn
// -------------------------------------------------------

// -------------------------------------------------------
// Video hold-back ring
// -------------------------------------------------------
// The decode thread used to stop reading the socket entirely whenever the
// jitter buffer was full:
//
//     if (jbuf_count() >= jbuf_cap()) { usleep(1000); continue; }
//
// Audio arrives interleaved in that same transport stream, so this capped the
// audio read-ahead at whatever was interleaved among the buffered video
// frames — about half a second — no matter how much room the PCM ring had.
// Measured on hardware: the PCM ring sat at 1% of its one-second target for
// the whole run and the audio thread starved 175-359 times, on BOTH DTS and
// TrueHD, i.e. the decoder was never behind on CPU, it was simply never given
// the bytes.  HD audio made it worse for the obvious reason: a stream-copied
// TrueHD track is several Mbps rather than AC-3's 640 kbps, so the same
// half-second of runway carries far more data and any dip empties it.
//
// The fix keeps reading while the audio still wants data, feeds the audio
// half immediately, and PARKS the video packets here in arrival order until
// the jitter buffer drains.  Nothing reaches VDEC while the jitter buffer is
// full — no submits, no pulls — so the decoder cannot back up; the only cost
// is this buffer.  ~1 MB is roughly 0.8 s of 10 Mbps video, and reading stops
// again once either it fills or the PCM ring reaches its high-water mark, so
// the read-ahead stays bounded in both directions.
#define VHOLD_PKTS 5576                      // * 188 B = ~1.0 MB

static u8  s_vhold[VHOLD_PKTS][TS_PACKET_SIZE];
static int s_vhold_rd = 0, s_vhold_wr = 0, s_vhold_n = 0;

static void vhold_reset(void) { s_vhold_rd = s_vhold_wr = s_vhold_n = 0; }

static void vhold_push(const u8 *pkt) {
    memcpy(s_vhold[s_vhold_wr], pkt, TS_PACKET_SIZE);
    s_vhold_wr = (s_vhold_wr + 1) % VHOLD_PKTS;
    s_vhold_n++;
}

bool player_spawn_decode(PlayerState *ps) {
    if (!ps->playing) return false;
    // A seek respawns this thread with a flushed demux, so anything still
    // parked belongs to the old position and must not be fed.
    vhold_reset();
    ps->dec_ctx.playing     = &ps->playing;
    ps->dec_ctx.frame_count = &ps->frame_count;
    ps->dec_ctx.sock        = ps->sock;
    ps->dec_ctx.dec_run     = &ps->dec_run;
    ps->dec_run = true;
    int trc = sysThreadCreate(&ps->dec_tid, decode_thread_fn,
                              (void *)&ps->dec_ctx,
                              800, 128 * 1024,
                              0, "jf_decode");
    if (trc != 0) {
        char buf[64];
        snprintf(buf, sizeof(buf), "player: dec thread_create FAILED rc=%d", trc);
        plog(buf);
        ps->playing = false;
        return false;
    }
    return true;
}

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
        // Parked video first, in arrival order, while there is room for it.
        while (s_vhold_n > 0 && jbuf_count() < jbuf_cap()) {
            video_feed_ts(s_vhold[s_vhold_rd]);
            s_vhold_rd = (s_vhold_rd + 1) % VHOLD_PKTS;
            s_vhold_n--;
        }

        // Read ahead past a full jitter buffer only while the audio ring is
        // still below the level the decoder targets, and only while there is
        // somewhere to park the video that comes with it.  When neither holds,
        // this is the original "stop reading" behaviour.
        const bool jbuf_full  = jbuf_count() >= jbuf_cap();
        const bool audio_wants = adec_pcm_available() < PCM_RING_HIGHWATER;
        if (jbuf_full && (!audio_wants || s_vhold_n >= VHOLD_PKTS)) {
            usleep(1000);
            continue;
        }

        for (int batch = 0; batch < 128; batch++) {
            // Stop the batch when neither the jitter buffer nor the hold ring
            // can take any more.
            if (jbuf_count() >= jbuf_cap() &&
                (s_vhold_n >= VHOLD_PKTS ||
                 adec_pcm_available() >= PCM_RING_HIGHWATER))
                break;

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

            if (jbuf_count() < jbuf_cap()) {
                video_feed_ts(ts_pkt);          // normal path, unchanged
            } else if (!video_feed_ts_audio_only(ts_pkt)) {
                // A video packet with nowhere to go yet: park it.  Checked
                // above, but the jitter buffer can fill mid-batch.
                if (s_vhold_n < VHOLD_PKTS) vhold_push(ts_pkt);
                else                        break;
            }
        }

        // Drain all decoded frames from VDEC into the jitter buffer
        while (s_frames_ready > 0 && jbuf_count() < jbuf_cap()) {
            if (!vdec_pull_frame()) break;
        }

        {
            // Throttled: this loop spins every millisecond, so an unthrottled
            // line here wrote thousands of log entries per stall — through the
            // async log ring, on the thread that is trying to catch up, which
            // made the stall it was reporting worse.  Once a second is plenty
            // to see that the buffer is running dry.
            static u64 jlow_last_us = 0;
            int q = jbuf_count();
            if (q < 4) {
                u64 now_us = timing_get_us();
                if (now_us - jlow_last_us >= 1000000ULL) {
                    jlow_last_us = now_us;
                    char buf[32];
                    snprintf(buf, sizeof(buf), "jbuf_low: q=%d", q);
                    plog(buf);
                }
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
                "hb: fr=%d q=%d au=%u ab=%llu stalls=%ld max=%ldms avg=%ldms fps=%.1f aumax=%d vhold=%d pcm=%d",
                *frame_count, jbuf_count(), s_au_submitted,
                (unsigned long long)audio_block_count(),
                stall_ep_count, stall_ep_dur_max_us / 1000, avg_ms,
                display_fps, s_au_inflight_max, s_vhold_n, adec_pcm_available());
            plog(buf);
            s_au_inflight_max = 0;
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
// Progress reporter — POST position to Jellyfin every ~10 s
// -------------------------------------------------------

void progress_thread_fn(void *arg) {
    PlayerState *ps = (PlayerState*)arg;

    int tick = 0;
    while (running && ps->playing) {
        usleep(250000);              // 250 ms granularity for a quick exit
        if (++tick < 40) continue;   // report every ~10 s
        tick = 0;
        if (!ps->playing) break;
        if (!ps->dec_tid) continue;  // mid-seek flush: position unstable
        u64 pos_ticks = (ps->play_base_us + audio_get_clock_us()) * 10ULL;
        jellyfin_report_progress(ps->item->id, ps->session_id,
                                 pos_ticks, ps->paused);
    }

    sysThreadExit(0);
}

// -------------------------------------------------------
// Upload thread — memcpy jbuf front slot → RSX-local back texture
// -------------------------------------------------------

// Copy one planar YUV420P frame (Y|Cb|Cr, tight) from a jbuf slot into three
// separate RSX plane textures.  Uses the decoder's ACTUAL dims (jbuf_fw/fh).
static void upload_yuv_planes(const u8 *slot, volatile u8 *dst[3]) {
    u32 fw = jbuf_fw(), fh = jbuf_fh();
    u32 cw = fw / 2, ch = fh / 2;
    u32 ysz = fw * fh, csz = cw * ch;
    memcpy((void*)dst[0], (const void*)slot,             ysz);   // Y
    memcpy((void*)dst[1], (const void*)(slot + ysz),      csz);   // Cb
    memcpy((void*)dst[2], (const void*)(slot + ysz + csz), csz);  // Cr
}

void upload_thread_fn(void *arg) {
    UploadCtx     *ctx     = (UploadCtx*)arg;
    volatile bool *playing = ctx->playing;

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
        upload_yuv_planes(slot_a, s_yuvA_buf[back]);
        if (slot_b) { upload_yuv_planes(slot_b, s_yuvB_buf[back]); s_vid_b_present = true; }
        else        { s_vid_b_present = false; }
        __asm__ volatile("sync" ::: "memory");
        s_vid_frame_ready = true;
    }

    sysThreadExit(0);
}
