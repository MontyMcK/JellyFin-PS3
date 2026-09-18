#define MINIMP3_IMPLEMENTATION
#include "minimp3.h"
#include "adec.h"
#include "adec_ac3.h"
#include "adec_dts.h"
#include "adec_truehd.h"
#include "audio.h"           // audio_output_channels() — port width drives ring width
#include "plog.h"
#include "timing.h"      // timing_get_us() — decode-cost telemetry
#include "../build_config.h"   // relative: source/ is not on the -I path

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/mutex.h>
#include <sys/cond.h>
#include <sys/thread.h>

extern void crash_log(const char *msg);

// PCM ring (decoded audio).  Power of two for cheap modulo.
//
// The sub-burn desync: a subtitle burn-in reopen (SubtitleMethod=Encode)
// delivers its audio in a large FRONT-LOADED burst — the server ships ~10s of
// audio while the subtitle-burning video encoder warms up, so the muxed stream
// is audio-heavy at the start.  The shared decode thread back-pressures only on
// the VIDEO jitter buffer (decode_thread_fn), so to collect its 12 video frames
// it must read through the whole audio burst, flooding the audio path.  The
// audio must therefore BUFFER that burst; the first audio/video PES start
// ALIGNED (~1s apart), so as long as nothing is dropped the audio plays FIFO in
// sync.  We buffer it COMPRESSED, in the PES queue below (~KB), and keep this
// decoded-PCM ring small by back-pressuring the decoder to consumption
// (adec_thread_fn) — decoding the whole burst to float PCM would cost ~8 MB.
// ~1.37s @ 48 kHz stereo float = 512 KB.
// 65536 frames.  Must stay a power of two: the read/write
// cursors wrap with & (PCM_RING_CAP - 1).  At 8 channels of float32 that is
// 2 MB.  It was briefly 4 MB to chase an audio-decode theory that the
// adt= telemetry then disproved (3% AC-3, 14-16% lossless TrueHD), so the
// memory is better spent on the compressed ring -- see adec.h.
#define PCM_RING_CAP        (1 << 16)
// PCM_RING_HIGHWATER (~1.0s: decoder idles above this) now lives in adec.h so
// the stats overlay can scale its ring-fill readout against the same level.

// PES queue: raw (compressed) PES packets queued by the demux/decode thread,
// consumed by the adec thread.  This is where the sub-burn burst is HELD: with
// the decoder back-pressured, ~10s of compressed audio waits here as it drains
// to the small PCM ring in sync with playback.  Undersized, it drops the oldest
// PES (adec_push_pes) and audio skips ~10s ahead — the original bug (was 32
// slots).  Measured worst case for the ~10s 2F2F burst: depth 80 PES, max PES
// 2894 bytes (see adec_pes_hwm telemetry).  256 slots gives >3x depth margin
// (~30s of burst) and 8192-byte slots >2.5x the largest PES; 256*8192 = 2 MB,
// vs the ~8 MB a PCM-side buffer of the same duration would cost.
// A PES that does NOT fit a slot (only DTS gets near it — a stream-copied
// DTS-HD MA frame at Blu-ray bitrates) is split across consecutive slots by
// adec_push_pes() rather than dropped, so the slot size stays a memory
// decision instead of a correctness one.
// 512 slots since the decode thread gained a compressed read-ahead ring: it
// now buffers seconds of video ahead of playback, and the audio interleaved
// with those seconds lands here.  At AC-3's 640 kbps the old 256 slots were
// ~25 s and ample; a stream-copied TrueHD track at ~4 Mbps filled them in
// about four, and a full queue DROPS THE OLDEST PES, which is an audible
// jump, not a stall.  512 slots is 4 MB and about eight seconds of HD audio.
// 768, was 512.  This queue -- not the ring -- is what ended preroll early:
// the log's `preroll: audio queue full, starting` fires once it is 75% full,
// and at TrueHD's ~4.6 Mbps that capped buffering at roughly 5.5 seconds no
// matter how big the ring was.  768 slots takes that to ~8 s.
//
// The 75% hungry threshold is deliberately NOT raised with it.  That margin
// is what stops pes_enqueue() reaching the full-queue path below, which
// DROPS the oldest PES -- audible as skipped audio.  Growing the queue buys
// preroll depth without spending that safety margin; raising the threshold
// would have bought the same depth by spending it.
#define PES_QUEUE_SLOTS 768
#define PES_SLOT_BYTES  8192

// ---- Decoders + PCM ring ----
static mp3dec_t         s_dec;
// Ring width follows the selected codec: 2 floats/frame for MP3 (the shipped
// layout, byte-identical indexing), 6 for AC-3 or DTS 5.1, 8 for TrueHD 7.1.
// Storage is sized for the widest case: 65536 frames * 8 ch * 4 B = 2 MB
// static (was 512 KB stereo, then 1.5 MB for 5.1 — the last +512 KB is what
// carrying a 7.1 program at the same 1.37 s ring depth costs).
static float            s_ring[PCM_RING_CAP * 8];  // interleaved float32 frames
static int              s_ring_ch = 2;             // 2, 6 (5.1) or 8 (7.1)
static adec_codec_t     s_codec   = ADEC_CODEC_MP3;
static int              s_wr = 0;
static int              s_rd = 0;
static volatile int     s_n  = 0;
static sys_mutex_t      s_pcm_mtx;

// ---- PTS tracking (guarded by s_pcm_mtx) ----
// s_next_pcm_pts_us: stream PTS (us) of the write cursor — set from each PES,
//   then advanced by N*1000000/48000 per decoded frame batch.
// s_read_pts_us: stream PTS (us) of the read cursor — initialised to the first
//   valid PES PTS, then advanced inside adec_read_pcm() as samples are consumed.
static u64  s_next_pcm_pts_us = 0;
static u64  s_read_pts_us     = 0;
static bool s_pts_valid       = false;

// ---- PES queue ----
static u8               s_pes_q[PES_QUEUE_SLOTS][PES_SLOT_BYTES];
static int              s_pes_q_len[PES_QUEUE_SLOTS] = {};
// 1 = this slot is a CONTINUATION chunk: raw elementary-stream bytes with no
// PES header of their own, carrying the tail of the PES in the preceding
// slot(s).  A PES larger than one slot is split across slots rather than
// dropped — see adec_push_pes().
static u8               s_pes_q_cont[PES_QUEUE_SLOTS] = {};
static int              s_pes_q_rd  = 0;
static int              s_pes_q_wr  = 0;
static volatile int     s_pes_q_n   = 0;
static sys_mutex_t      s_pes_mtx;

// Decode-cost telemetry (see adec_decode_stats below).  Declared here rather
// than beside the accessor because adec_thread_fn() updates them above it.
static volatile u64     s_dec_us    = 0;
static volatile u32     s_dec_count = 0;
static sys_cond_t       s_pes_cond;
static volatile bool    s_adec_run  = false;
static sys_ppu_thread_t s_adec_thread = 0;

// Flush generation — the seek barrier for audio.
//
// adec_flush() empties the PES queue and the PCM ring, but the decode thread
// runs CONCURRENTLY and has usually already popped a PES by then.  Worse, it
// then sleeps in the PCM high-water back-pressure loop still holding that
// packet, for up to a second.  A seek landing anywhere in that window used to
// end with pre-seek audio being decoded and pushed into the freshly cleared
// ring AFTER the flush — which also set the post-seek PTS baseline from stale
// data.  Heard as audio drifting out of sync with the picture after scrubbing,
// getting worse the more you scrub, and worst on TrueHD because its frames
// take the longest to decode.
//
// So every flush bumps the generation, the thread stamps each packet it pops,
// and anything decoded from an older generation is dropped instead of played.
static volatile u32 s_flush_gen  = 0;   // bumped by adec_flush()
static volatile u32 s_decode_gen = 0;   // generation of the packet in flight

// True while the packet being decoded still belongs to the current position.
static inline bool adec_gen_current(void) { return s_decode_gen == s_flush_gen; }

// Returns true and fills *pts_us (microseconds) if the PES header contains a PTS.
static bool parse_pes_pts(const u8 *pes, int pes_len, u64 *pts_us) {
    if (pes_len < 14) return false;
    if (pes[0] || pes[1] || pes[2] != 0x01) return false;
    u8 pts_dts_flags = (pes[7] >> 6) & 0x3;
    if (!(pts_dts_flags & 0x2)) return false;  // no PTS present
    // PTS is 33 bits packed into bytes 9-13 with marker bits between segments.
    u64 pts90 =
          ((u64)(pes[9]  & 0x0E) << 29)
        | ((u64)(pes[10] & 0xFF) << 22)
        | ((u64)(pes[11] & 0xFE) << 14)
        | ((u64)(pes[12] & 0xFF) <<  7)
        | ((u64)(pes[13] & 0xFE) >>  1);
    *pts_us = (pts90 * 100ULL) / 9ULL;  // 90kHz → microseconds
    return true;
}

void adec_init(void) {
    mp3dec_init(&s_dec);
    s_wr = s_rd = s_n = 0;
    s_next_pcm_pts_us = s_read_pts_us = 0;
    s_pts_valid = false;
    sys_mutex_attr_t mattr;
    sysMutexAttrInitialize(mattr);
    sysMutexCreate(&s_pcm_mtx, &mattr);
}

// MP3 path only — writes stereo frames.  The hardcoded 2-wide indexing is
// deliberate: with s_ring_ch == 2 (always true for MP3) it is byte-identical
// to the shipped stereo code.
static void push_samples(const short *pcm, int n, int channels) {
    sysMutexLock(s_pcm_mtx, 0);
    if (!adec_gen_current()) {          // stale MP3 frame from before a seek
        sysMutexUnlock(s_pcm_mtx);
        return;
    }
    for (int i = 0; i < n; i++) {
        if (s_n >= PCM_RING_CAP) {
#if BUILD_FOR_RPCS3
            // Ring overflow => audio dropped => playback skips ahead.  With the
            // enlarged ring this should never fire on a sub-burn burst; log it
            // (throttled) so a regression is visible.
            static u64 s_drop_samples = 0;
            if ((s_drop_samples++ % 48000) == 0) {
                char b[64];
                snprintf(b, sizeof(b), "adec_drop: ring full, dropped ~%llus of audio",
                         (unsigned long long)(s_drop_samples / 48000));
                plog(b);
            }
#endif
            break;
        }
        float l = pcm[i * channels    ] * (1.0f / 32768.0f);
        float r = (channels >= 2) ? pcm[i * channels + 1] * (1.0f / 32768.0f) : l;
        s_ring[s_wr * 2    ] = l;
        s_ring[s_wr * 2 + 1] = r;
        s_wr = (s_wr + 1) & (PCM_RING_CAP - 1);
        s_n++;
    }
    // Advance write PTS by the full decoded count (stream time always moves forward,
    // even if the ring dropped some samples due to overflow).
    s_next_pcm_pts_us += ((u64)n * 1000000ULL) / 48000ULL;
    sysMutexUnlock(s_pcm_mtx);
}

// Surround path (AC-3, DTS) — s_ring_ch-wide float frames, already in PS3
// channel order (adec_ac3.cpp/ac3_map.c, adec_dts.cpp/dts_map.c).  Same
// overflow policy and PTS advance as push_samples(); the two differ only in
// sample format and width.
void adec_push_frames(const float *frames, int n) {
    sysMutexLock(s_pcm_mtx, 0);
    if (!adec_gen_current()) {
        // A flush landed while this frame was being decoded.  Dropping it here
        // matters as much as the check in the thread loop: a codec frame can
        // take long enough that the seek happens mid-decode, and pushing it
        // would both play old audio and set the new PTS baseline from it.
        sysMutexUnlock(s_pcm_mtx);
        return;
    }
    int ch = s_ring_ch;
    for (int i = 0; i < n; i++) {
        if (s_n >= PCM_RING_CAP) {
#if BUILD_FOR_RPCS3
            static u64 s_drop_frames = 0;
            if ((s_drop_frames++ % 48000) == 0) {
                char b[64];
                snprintf(b, sizeof(b), "adec_drop: ring full, dropped ~%llus of audio",
                         (unsigned long long)(s_drop_frames / 48000));
                plog(b);
            }
#endif
            break;
        }
        for (int c = 0; c < ch; c++)
            s_ring[s_wr * ch + c] = frames[i * ch + c];
        s_wr = (s_wr + 1) & (PCM_RING_CAP - 1);
        s_n++;
    }
    s_next_pcm_pts_us += ((u64)n * 1000000ULL) / 48000ULL;
    sysMutexUnlock(s_pcm_mtx);
}

// Hand elementary-stream bytes to whichever decoder currently owns the ring.
static void adec_decode_es(const u8 *es, int len) {
    if (len <= 0) return;
    if (s_codec == ADEC_CODEC_AC3) { adec_ac3_decode_payload(es, len); return; }
    if (s_codec == ADEC_CODEC_DTS) { adec_dts_decode_payload(es, len); return; }
    if (s_codec == ADEC_CODEC_TRUEHD) {
        adec_truehd_decode_payload(es, len);
        return;
    }
    while (len > 0) {
        mp3dec_frame_info_t info;
        short pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
        int samples = mp3dec_decode_frame(&s_dec, es, len, pcm, &info);
        if (info.frame_bytes <= 0) break;
        if (samples > 0) {
            static bool s_logged_frame = false;
            if (!s_logged_frame) {
                s_logged_frame = true;
                char fbuf[64];
                snprintf(fbuf, sizeof(fbuf), "adec_frame: hz=%d ch=%d samples=%d",
                         info.hz, info.channels, samples);
                plog(fbuf);
            }
            push_samples(pcm, samples, info.channels);
        }
        es  += info.frame_bytes;
        len -= info.frame_bytes;
    }
}

// cont = this buffer is a continuation chunk of the previous PES: raw ES
// bytes, no header to strip and no PTS to read (adec_push_pes).
static void adec_decode_pes(const u8 *pes, int pes_len, bool cont) {
    if (cont) {
        adec_decode_es(pes, pes_len);
        return;
    }
    // Seed the write PTS from this packet's header.  On the very first valid PTS,
    // also initialise the read cursor so both cursors start from a coherent origin.
    u64 pes_pts_us;
    if (parse_pes_pts(pes, pes_len, &pes_pts_us)) {
        sysMutexLock(s_pcm_mtx, 0);
        s_next_pcm_pts_us = pes_pts_us;
        if (!s_pts_valid) {
            s_read_pts_us = pes_pts_us;
            s_pts_valid   = true;
#if BUILD_FOR_RPCS3
            { char b[64];
              snprintf(b, sizeof(b), "adec_first_pes: pts=%lluus",
                       (unsigned long long)pes_pts_us);
              plog(b); }
#endif
        }
        sysMutexUnlock(s_pcm_mtx);
    }
    if (pes_len < 9) return;
    if (pes[0] || pes[1] || pes[2] != 0x01) return;
    int hdr  = 9 + pes[8];
    if (hdr >= pes_len) return;
    adec_decode_es(pes + hdr, pes_len - hdr);
}

// Enqueue one chunk.  Caller holds s_pes_mtx.
static void pes_enqueue(const u8 *buf, int len, bool cont) {
    if (s_pes_q_n >= PES_QUEUE_SLOTS) {
        // queue full — drop oldest to avoid back-pressuring the demux thread
        s_pes_q_rd = (s_pes_q_rd + 1) % PES_QUEUE_SLOTS;
        s_pes_q_n--;
#if BUILD_FOR_RPCS3
        // A dropped PES = skipped audio = playback jumps ahead.  Count it.
        static u64 s_pes_drops = 0;
        if ((s_pes_drops++ % 32) == 0) {
            char b[64];
            snprintf(b, sizeof(b), "adec_pes_drop: queue full, drops=%llu",
                     (unsigned long long)s_pes_drops);
            plog(b);
        }
#endif
    }
    memcpy(s_pes_q[s_pes_q_wr], buf, len);
    s_pes_q_len[s_pes_q_wr]  = len;
    s_pes_q_cont[s_pes_q_wr] = cont ? 1 : 0;
    s_pes_q_wr = (s_pes_q_wr + 1) % PES_QUEUE_SLOTS;
    s_pes_q_n++;
#if BUILD_FOR_RPCS3
    // Sizing telemetry: worst-case PES length (-> PES_SLOT_BYTES) and worst-case
    // queue depth (-> PES_QUEUE_SLOTS) so the buffers are sized to the real burst.
    { static int s_max_len = 0, s_max_depth = 0;
      bool chg = false;
      if (len > s_max_len)       { s_max_len = len;         chg = true; }
      if (s_pes_q_n > s_max_depth){ s_max_depth = s_pes_q_n; chg = true; }
      if (chg) {
        char b[80];
        snprintf(b, sizeof(b), "adec_pes_hwm: max_len=%d max_depth=%d/%d",
                 s_max_len, s_max_depth, PES_QUEUE_SLOTS);
        plog(b);
      } }
#endif
    sysCondSignal(s_pes_cond);
}

void adec_push_pes(const u8 *pes, int pes_len) {
    if (pes_len <= 0) return;
    // A PES bigger than one queue slot is SPLIT, not dropped.  MP3 and AC-3
    // PES never reach 8 KB, so this loop runs exactly once for them and the
    // shipped path is byte-identical.  A stream-copied DTS-HD MA / DTS:X
    // track is the case that needs it: its PES carry a core frame plus the
    // extension substream and can exceed the slot at Blu-ray bitrates, and
    // the old `pes_len > PES_SLOT_BYTES → return` silently dropped exactly
    // those packets, i.e. all of the audio.  Splitting here rather than
    // enlarging the slots keeps the queue at 2 MB instead of 8 MB.
    // The whole PES is enqueued under one lock so its chunks stay adjacent.
    sysMutexLock(s_pes_mtx, 0);
    bool cont = false;
    while (pes_len > 0) {
        int take = (pes_len < PES_SLOT_BYTES) ? pes_len : PES_SLOT_BYTES;
        pes_enqueue(pes, take, cont);
        pes     += take;
        pes_len -= take;
        cont     = true;
    }
    sysMutexUnlock(s_pes_mtx);
}

static void adec_thread_fn(void *arg) {
    (void)arg;
    u8   local_pes[PES_SLOT_BYTES];
    int  local_len;
    bool local_cont;
    while (s_adec_run) {
        sysMutexLock(s_pes_mtx, 0);
        while (s_adec_run && s_pes_q_n == 0) {
            sysCondWait(s_pes_cond, 100000);  // 100ms timeout
        }
        if (!s_adec_run) {
            sysMutexUnlock(s_pes_mtx);
            break;
        }
        memcpy(local_pes, s_pes_q[s_pes_q_rd], s_pes_q_len[s_pes_q_rd]);
        local_len  = s_pes_q_len[s_pes_q_rd];
        local_cont = s_pes_q_cont[s_pes_q_rd] != 0;
        s_pes_q_rd = (s_pes_q_rd + 1) % PES_QUEUE_SLOTS;
        s_pes_q_n--;
        // Stamp the packet with the position it belongs to, while still under
        // the lock adec_flush() takes — so the stamp cannot straddle a flush.
        s_decode_gen = s_flush_gen;
        sysMutexUnlock(s_pes_mtx);

        // Back-pressure: don't decode further ahead than the PCM ring high-water.
        // This keeps the decoded-PCM ring small while the compressed burst waits
        // in the (cheap) PES queue, draining in sync with playback.  The popped
        // PES is held in local_pes meanwhile; the demux keeps filling the queue.
        while (s_adec_run && s_n >= PCM_RING_HIGHWATER && adec_gen_current())
            usleep(2000);

        // A seek while we were queued or waiting above: this packet is from
        // the OLD position.  Drop it rather than decode it.
        if (!adec_gen_current())
            continue;

        {
            u64 dt0 = timing_get_us();
            adec_decode_pes(local_pes, local_len, local_cont);
            s_dec_us += timing_get_us() - dt0;
            s_dec_count++;
        }
    }
#if BUILD_FOR_RPCS3
    // Exit freeze fix: this thread otherwise just falls off the end and returns.
    // On RPCS3 a PPU thread that merely returns is parked in state 0x40[ret] and
    // never marked exited ("Returning from the thread entry function!" in
    // RPCS3.log), so the sys_ppu_thread_join() in adec_stop() blocks forever —
    // the movie hangs on exit.  sysThreadExit(0) makes the thread formally exit
    // so the join completes.  NB: every other thread in this codebase already
    // ends this way (player_threads.cpp, thumbnail_cache, music_player, plog,
    // update_check) — adec_thread_fn is the lone omission, so hardware would be
    // equally correct with this call; it's gated only to honour the byte-for-
    // byte hardware rule in build_config.h.
    sysThreadExit(0);
#endif
}

void adec_start(void) {
    crash_log("ad1 adec_start enter");
    sys_mutex_attr_t mattr;
    crash_log("ad2 mutex/cond create");
    sysMutexAttrInitialize(mattr);
    sysMutexCreate(&s_pes_mtx, &mattr);
    sys_cond_attr_t cattr;
    sysCondAttrInitialize(cattr);
    sysCondCreate(&s_pes_cond, s_pes_mtx, &cattr);
    s_pes_q_rd = s_pes_q_wr = s_pes_q_n = 0;
    memset(s_pes_q_cont, 0, sizeof(s_pes_q_cont));
    s_adec_run = true;
    crash_log("ad3 sysThreadCreate");
    sysThreadCreate(&s_adec_thread, adec_thread_fn, NULL,
                    750, 0x10000, THREAD_JOINABLE, (char*)"jf_adec");
    crash_log("ad4 adec_start done");
}

void adec_flush(void) {
    sysMutexLock(s_pes_mtx, 0);
    // Invalidate anything the decode thread already popped. Must happen under
    // the same lock the thread stamps under, or a packet could be stamped with
    // the old generation just after we bump it.
    s_flush_gen++;
    s_pes_q_rd = s_pes_q_wr = s_pes_q_n = 0;
    // Dropping the queue can strand a continuation chunk whose head is gone;
    // clearing the flags means nothing left behind is ever read as one.
    memset(s_pes_q_cont, 0, sizeof(s_pes_q_cont));
    sysMutexUnlock(s_pes_mtx);
    sysMutexLock(s_pcm_mtx, 0);
    mp3dec_init(&s_dec);
    adec_ac3_reset();   // drop the partial-frame carry; codec choice survives
    adec_dts_reset();   // ditto for DTS (also clears a pending ext-substream skip)
    adec_truehd_reset();// ditto for TrueHD (also drops the major-sync lock)
    s_wr = s_rd = s_n = 0;
    s_next_pcm_pts_us = s_read_pts_us = 0;
    s_pts_valid = false;
    sysMutexUnlock(s_pcm_mtx);
}

void adec_stop(void) {
    crash_log("adx1 adec_stop enter");
    sysMutexLock(s_pes_mtx, 0);
    s_adec_run = false;
    crash_log("adx2 signal stop");
    sysCondSignal(s_pes_cond);
    sysMutexUnlock(s_pes_mtx);
    crash_log("adx3 sysThreadJoin");
    u64 retval;
    sysThreadJoin(s_adec_thread, &retval);
    crash_log("adx4 mutex/cond destroy");
    sysCondDestroy(s_pes_cond);
    sysMutexDestroy(s_pes_mtx);
    sysMutexDestroy(s_pcm_mtx);
    // Session over: free the liba52/libdca state and return to the shipped stereo
    // defaults so the next owner of the port (music player, next movie with
    // surround off) starts from the exact shipped configuration.
    adec_ac3_close();
    adec_dts_close();
    adec_truehd_close();
    s_codec   = ADEC_CODEC_MP3;
    s_ring_ch = 2;
    crash_log("adx5 adec_stop done");
}

// Decode-cost telemetry.  The heartbeat showed playback collapsing exactly
// when the PCM buffer emptied, while the network was still delivering 20+
// Mbps -- so the suspect is the cost of decoding TrueHD/DTS-HD MA on the
// PPU, not delivery.  This measures it directly instead of inferring it:
// microseconds spent inside adec_decode_pes(), and how many PES it covered.
// Reported as adt=<% of one thread> in the heartbeat.  Near 100% means the
// decoder is saturated and lossless HD audio cannot hold real time here.
void adec_decode_stats(u64 *busy_us, u32 *count) {
    if (busy_us) *busy_us = s_dec_us;
    if (count)   *count   = s_dec_count;
}

int adec_pcm_available(void) { return s_n; }

// Room left for more compressed audio.  The decode thread reads video far
// ahead of playback now, and the audio interleaved with it has to fit HERE —
// a full queue drops the oldest PES, which is an audible jump.  So the
// read-ahead asks this before pulling more from the socket.
bool adec_pes_queue_hungry(void) {
    return s_pes_q_n < (PES_QUEUE_SLOTS * 3) / 4;
}

int adec_output_channels(void) { return s_ring_ch; }

adec_codec_t adec_get_codec(void) { return s_codec; }

static const char *adec_codec_name(adec_codec_t c) {
    switch (c) {
    case ADEC_CODEC_AC3: return "ac3";
    case ADEC_CODEC_DTS:    return "dts";
    case ADEC_CODEC_TRUEHD: return "truehd";
    default:                return "mp3";
    }
}

void adec_set_codec(adec_codec_t codec) {
    // Ring width for the surround codecs follows the port that is actually
    // open: a surround stream feeding a stereo port (8ch open failed) is
    // downmixed at decode time, so the ring stays 2-wide there.  TrueHD is
    // the one codec that can fill all eight slots (7.1); AC-3 and DTS top
    // out at a 5.1 program.
    const int  port_ch   = audio_output_channels();
    const bool wide_port = port_ch >= 6;
    int want_ch = 2;
    if (codec == ADEC_CODEC_AC3 || codec == ADEC_CODEC_DTS)
        want_ch = wide_port ? 6 : 2;
    else if (codec == ADEC_CODEC_TRUEHD)
        // Never decode WIDER than the port actually is.  The port is now
        // opened 6-wide when it can be (see audio_open), and a 7.1 ring
        // feeding a 6-wide port would be wider than its destination — which
        // the output stage can only resolve by dropping back to FL/FR.
        // truehd_map folds the rears into the surrounds at -3 dB for a
        // six-slot program, so nothing in the mix is lost by asking for 6.
        want_ch = wide_port ? (port_ch >= 8 ? 8 : 6) : 2;

    if (codec == s_codec && want_ch == s_ring_ch) return;

    // Only one surround decoder is ever open: close the other one before
    // opening this one, so a PMT that changes codec mid-session (or a track
    // change between a DTS and an AC-3 track) cannot leave both allocated.
    if (codec != ADEC_CODEC_AC3)    adec_ac3_close();
    if (codec != ADEC_CODEC_DTS)    adec_dts_close();
    if (codec != ADEC_CODEC_TRUEHD) adec_truehd_close();

    if (codec == ADEC_CODEC_AC3) {
        if (!adec_ac3_open(want_ch)) {
            // liba52 unavailable — stay on MP3 so a wrong PMT degrades to
            // silence-on-AC3-PES rather than noise; loudly logged.
            plog("adec_set_codec: AC-3 open failed, staying on MP3");
            codec   = ADEC_CODEC_MP3;
            want_ch = 2;
        }
    } else if (codec == ADEC_CODEC_DTS) {
        if (!adec_dts_open(want_ch)) {
            plog("adec_set_codec: DTS open failed, staying on MP3");
            codec   = ADEC_CODEC_MP3;
            want_ch = 2;
        }
    } else if (codec == ADEC_CODEC_TRUEHD) {
        if (!adec_truehd_open(want_ch)) {
            plog("adec_set_codec: TrueHD open failed, staying on MP3");
            codec   = ADEC_CODEC_MP3;
            want_ch = 2;
        }
    }

    // Width/codec change invalidates whatever PCM is queued: drop it and
    // restart PTS tracking from the next PES (the demux switches codec at
    // stream (re)open, when the ring is empty anyway).
    sysMutexLock(s_pcm_mtx, 0);
    s_codec   = codec;
    s_ring_ch = want_ch;
    mp3dec_init(&s_dec);
    s_wr = s_rd = s_n = 0;
    s_next_pcm_pts_us = s_read_pts_us = 0;
    s_pts_valid = false;
    sysMutexUnlock(s_pcm_mtx);

    char b[64];
    snprintf(b, sizeof(b), "adec_set_codec: codec=%s ch=%d",
             adec_codec_name(codec), want_ch);
    plog(b);
}

int adec_read_pcm(float *buf, int n_frames) {
    sysMutexLock(s_pcm_mtx, 0);
    int ch  = s_ring_ch;   // 2 on the shipped path — identical copy pattern
    int got = 0;
    while (got < n_frames && s_n > 0) {
        for (int c = 0; c < ch; c++)
            buf[got * ch + c] = s_ring[s_rd * ch + c];
        s_rd = (s_rd + 1) & (PCM_RING_CAP - 1);
        s_n--;
        got++;
    }
    if (got > 0)
        s_read_pts_us += ((u64)got * 1000000ULL) / 48000ULL;
    sysMutexUnlock(s_pcm_mtx);
    return got;
}

u64 adec_get_read_pts_us(void) {
    sysMutexLock(s_pcm_mtx, 0);
    u64 rpts = s_pts_valid ? s_read_pts_us : 0;
    u64 wpts = s_next_pcm_pts_us;
    sysMutexUnlock(s_pcm_mtx);
    static int s_dbg_n = 0;
    if (++s_dbg_n % 500 == 0) {
        char buf[128];
        snprintf(buf, sizeof(buf),
            "adec_pts: read=%lluus write=%lluus delta=%lldus",
            (unsigned long long)rpts,
            (unsigned long long)wpts,
            (long long)(wpts - rpts));
        plog(buf);
    }
    return rpts;
}
