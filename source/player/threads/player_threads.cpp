// Decode, audio, and upload thread functions, plus the decode-thread
// spawn helper shared by the initial open and the post-seek respawn.

#include <stdio.h>
#include <stdlib.h>
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
#include "ts_demux.h"   // PesStats — PES truncation telemetry
#include "timing.h"
#include "player_internal.h"
#include "player_stats.h"
#include "jellyfin_api.h"

extern u32 running;

// -------------------------------------------------------
// Decode-thread spawn — initial open and post-seek respawn
// -------------------------------------------------------

// -------------------------------------------------------
// Compressed read-ahead ring
// -------------------------------------------------------
// The decode thread used to stop reading the socket entirely whenever the
// jitter buffer was full:
//
//     if (jbuf_count() >= jbuf_cap()) { usleep(1000); continue; }
//
// That made the decoded-frame jitter buffer the ONLY shock absorber, and at
// 1080p it holds sixteen frames — about half a second.  A server transcoding
// a dense Blu-ray delivers unevenly: fine through dialogue, under real time
// through a fast-cut action scene.  Half a second of runway does not survive
// that, which is why a high-bitrate 1080p source stutters while a calm one at
// the same resolution plays perfectly.  Audio had the same problem for the
// same reason: it arrives interleaved in this stream, so it could never read
// further ahead than the video buffer allowed.
//
// So the client now buffers COMPRESSED video here instead, and keeps reading
// until this ring is full rather than until the jitter buffer is.  Audio and
// PSI packets are fed the moment they arrive; video is queued in arrival
// order and fed to VDEC as the jitter buffer drains.  Nothing reaches VDEC
// while that buffer is full — no submits, no pulls — so the decoder cannot
// back up.
//
// ---- Sizing ----
//
// The decoded-frame jitter buffer is the wrong place to absorb a server that
// delivers unevenly, because a decoded 1080p frame is 3.13 MB: sixteen of them
// is 50 MB and barely half a SECOND of runway, and there is no room on the
// console for more.  The same memory spent on COMPRESSED data buys ~75x the
// time — 12 MB of transport stream is about ten seconds at 10 Mbps.  So this
// ring is the real shock absorber and the jitter buffer is left as the small
// decode-ahead it always was.
//
// Allocated per playback from whatever is free (player.cpp), between these
// bounds; below the floor it is not worth the complexity, above the ceiling
// it starves everything else.
#define RING_BYTES_MIN  (2u  * 1024u * 1024u)
#define RING_BYTES_MAX  (40u * 1024u * 1024u)
// Raised from 14 MB.  A direct-played stream is 3-4x the bitrate of a
// 10 Mbps transcode, so the old ceiling was barely two seconds of it.
// The allocator halves down from the ask, so a heap that cannot manage
// this simply gets a smaller ring rather than a failure.

static u8 *s_ring     = NULL;   // cap * TS_PACKET_SIZE bytes
static int s_ring_cap = 0;      // packets
static int s_ring_rd = 0, s_ring_wr = 0, s_ring_n = 0;

static void ring_reset(void) { s_ring_rd = s_ring_wr = s_ring_n = 0; }

bool decode_ring_alloc(u32 want_bytes) {
    decode_ring_free();
    if (want_bytes < RING_BYTES_MIN) want_bytes = RING_BYTES_MIN;
    if (want_bytes > RING_BYTES_MAX) want_bytes = RING_BYTES_MAX;
    int cap = (int)(want_bytes / TS_PACKET_SIZE);
    // Try the requested size, then halve down to the floor rather than fail
    // playback outright over a buffer that is only ever an optimisation.
    while (cap * TS_PACKET_SIZE >= (int)RING_BYTES_MIN) {
        s_ring = (u8 *)malloc((size_t)cap * TS_PACKET_SIZE);
        if (s_ring) {
            s_ring_cap = cap;
            ring_reset();
            char b[80];
            snprintf(b, sizeof(b), "ring: %d KB (%d packets)",
                     cap * TS_PACKET_SIZE / 1024, cap);
            plog(b);
            return true;
        }
        cap /= 2;
    }
    plog("ring: allocation FAILED - playing without read-ahead");
    s_ring_cap = 0;
    return false;
}

void decode_ring_free(void) {
    if (s_ring) { free(s_ring); s_ring = NULL; }
    s_ring_cap = 0;
    ring_reset();
}

int decode_ring_fill(void) { return s_ring_n; }
int decode_ring_cap(void)  { return s_ring_cap; }

static void ring_push(const u8 *pkt) {
    memcpy(s_ring + (size_t)s_ring_wr * TS_PACKET_SIZE, pkt, TS_PACKET_SIZE);
    s_ring_wr = (s_ring_wr + 1) % s_ring_cap;
    s_ring_n++;
}

bool player_spawn_decode(PlayerState *ps) {
    if (!ps->playing) return false;
    // A seek respawns this thread with a flushed demux, so anything still
    // buffered belongs to the old position and must not be fed.
    ring_reset();
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
        // Buffered video first, in arrival order, while there is room for it.
        while (s_ring_n > 0 && jbuf_count() < jbuf_cap()) {
            video_feed_ts(s_ring + (size_t)s_ring_rd * TS_PACKET_SIZE);
            s_ring_rd = (s_ring_rd + 1) % s_ring_cap;
            s_ring_n--;
        }

        // Keep reading until the RING is full, not just until the jitter
        // buffer is.  This is the whole point of the read-ahead: the server
        // delivers a transcode unevenly — fine through easy scenes, below
        // real time through hard ones — and a client that stops reading the
        // moment its half-second of decoded frames is full has nothing left
        // to play from when the next hard scene arrives.  Reading on fills
        // seconds of compressed video instead, which is what rides out the
        // dip.
        // The read-ahead is bounded by the AUDIO queue as well as the ring.
        // Video read far ahead drags the interleaved audio with it, and that
        // audio has to fit in the compressed-audio queue: overrun it and the
        // oldest PES is dropped, which is heard as a jump.  So the reserve is
        // whichever of the two runs out first.
        const bool jbuf_full = jbuf_count() >= jbuf_cap();
        const bool ring_full = (s_ring_cap == 0) || (s_ring_n >= s_ring_cap);
        if (jbuf_full && (ring_full || !adec_pes_queue_hungry())) {
            usleep(1000);
            continue;
        }

        for (int batch = 0; batch < 128; batch++) {
            // Stop the batch when neither the jitter buffer nor the ring can
            // take any more.
            if (jbuf_count() >= jbuf_cap() &&
                (s_ring_cap == 0 || s_ring_n >= s_ring_cap ||
                 !adec_pes_queue_hungry()))
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

            // Keep the buffered video moving as soon as room appears.  This
            // has to happen inside the batch, not just once per outer
            // iteration: the jitter buffer drains mid-batch.
            while (s_ring_n > 0 && jbuf_count() < jbuf_cap()) {
                video_feed_ts(s_ring + (size_t)s_ring_rd * TS_PACKET_SIZE);
                s_ring_rd = (s_ring_rd + 1) % s_ring_cap;
                s_ring_n--;
            }

            // Audio is fed the moment it arrives; video goes through the ring
            // whenever anything is already queued there.  Feeding a fresh
            // packet past queued ones would hand the demuxer packet N+50
            // before packet N, and a reordered video PID reassembles every
            // PES wrong — which looks like constant macroblock artifacts
            // rather than like a queueing bug.  A packet only goes straight
            // through when the ring is empty.
            if (s_ring_n == 0 && jbuf_count() < jbuf_cap()) {
                video_feed_ts(ts_pkt);          // normal path, unchanged
            } else if (!video_feed_ts_audio_only(ts_pkt)) {
                if (s_ring_cap > 0 && s_ring_n < s_ring_cap) ring_push(ts_pkt);
                else                                         break;
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
            char buf[352];
            long avg_ms = stall_ep_count ? stall_ep_dur_total_us / stall_ep_count / 1000 : 0;
            // Pulldown cadence comes along for the ride.  24fps film on a
            // 60Hz output is displayed 3,2,3,2 vblanks per frame; healthy
            // playback shows hold2 and hold3 roughly equal and "other" near
            // zero.  "other" dominating means the cadence is NOT holding,
            // which is visibly worse judder than correct 3:2 -- and it is
            // the only part of this the console lets us fix, since PSL1GHT
            // has no way to request a 24Hz output mode.
            PlayerStats ps_hb; player_stats_get(&ps_hb);
            // pes=<truncations>/<largest PES seen, KB>.  A non-zero count means
            // AUs are being clipped and handed to VDEC corrupt; the KB figure
            // is what TS_VPES_AU_MAX would have to be to hold this stream.
            PesStats pes_v; ts_pes_stats(&pes_v, NULL);
            // net=<Mbps pulled since the last heartbeat>, rxw=<% of that
            // interval spent blocked inside netRecv>.  Together they say
            // whether a starved ring is the network's fault or ours.
            u64 rxb = 0, rxw = 0; u32 rxc = 0;
            stream_rx_stats(&rxb, &rxw, &rxc);
            static u64 s_rxb_last = 0, s_rxw_last = 0; static u32 s_rxc_last = 0;
            static u64 s_rx_t_last = 0;
            u64 rx_now = timing_get_us();
            u64 rx_span = (s_rx_t_last && rx_now > s_rx_t_last) ? (rx_now - s_rx_t_last) : 1;
            double net_mbps = (double)(rxb - s_rxb_last) * 8.0 / (double)rx_span;
            int    rx_wpct  = (int)(((rxw - s_rxw_last) * 100ULL) / rx_span);
            unsigned rx_n   = rxc - s_rxc_last;
            s_rxb_last = rxb; s_rxw_last = rxw; s_rxc_last = rxc; s_rx_t_last = rx_now;
            // adt=<% of one PPU thread the audio decoder used this interval>.
            // The collapse always coincides with pcm= emptying, so this is the
            // number that says whether lossless HD audio simply costs more than
            // real time once the demux is also busy at 30-53 Mbps.
            u64 dbusy = 0; u32 dcnt = 0;
            adec_decode_stats(&dbusy, &dcnt);
            static u64 s_db_last = 0; static u32 s_dc_last = 0;
            int adt = (int)(((dbusy - s_db_last) * 100ULL) / rx_span);
            unsigned adn = dcnt - s_dc_last;
            s_db_last = dbusy; s_dc_last = dcnt;
            // lvl=<peak per port channel, 0-32768, in port order
            // FL FR FC LFE SL SR [BL BR]>.  This is the LAST point the app
            // can observe its own audio, so it separates the two things that
            // look identical from the sofa: a centre channel we never filled,
            // versus one we filled and the chain did not play.  The soundbar
            // dropping dialogue in multichannel LPCM is the whole reason the
            // bitstream work exists, and until now that evidence lived only
            // in the on-screen overlay, where no log could capture it.
            char lvl[96]; int lvn = 0;
            for (int c = 0; c < ps_hb.ch_count && c < 8 &&
                            lvn < (int)sizeof(lvl) - 8; c++)
                lvn += snprintf(lvl + lvn, sizeof(lvl) - lvn, "%s%u",
                                c ? "/" : "", (unsigned)ps_hb.ch_peak[c]);
            if (!lvn) { lvl[0] = 45; lvl[1] = 0; }   /* "-" */

            snprintf(buf, sizeof(buf),
                "hb: fr=%d q=%d au=%u ab=%llu stalls=%ld max=%ldms avg=%ldms fps=%.1f aumax=%d ring=%d/%d pcm=%d net=%.1fM rxw=%d%% adt=%d%% adn=%u pes=%u/%uk pd=%u/%u/%u lvl=%s",
                *frame_count, jbuf_count(), s_au_submitted,
                (unsigned long long)audio_block_count(),
                stall_ep_count, stall_ep_dur_max_us / 1000, avg_ms,
                display_fps, s_au_inflight_max, s_ring_n, s_ring_cap,
                adec_pcm_available(),
                net_mbps, rx_wpct, adt, adn,
                (unsigned)pes_v.trunc_count, (unsigned)(pes_v.max_want / 1024),
                (unsigned)ps_hb.hold2, (unsigned)ps_hb.hold3,
                (unsigned)ps_hb.hold_other, lvl);
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
        // Which frame this is, read under the same lock as the pointer so
        // the two cannot disagree.  The display needs it to distinguish a
        // new picture from a re-upload of the one already on screen.
        const u32 seq_a  = jbuf_peek_seq();
        sysMutexUnlock(s_jbuf_mtx);

        if (!slot_a) { usleep(1000); continue; }

        __asm__ volatile("sync" ::: "memory");
        int back = s_vid_disp_idx ^ 1;
        upload_yuv_planes(slot_a, s_yuvA_buf[back]);
        if (slot_b) { upload_yuv_planes(slot_b, s_yuvB_buf[back]); s_vid_b_present = true; }
        else        { s_vid_b_present = false; }
        s_vid_uploaded_seq = seq_a;
        __asm__ volatile("sync" ::: "memory");
        s_vid_frame_ready = true;
    }

    sysThreadExit(0);
}
