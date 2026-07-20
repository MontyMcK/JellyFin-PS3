#define MINIMP3_IMPLEMENTATION
#include "minimp3.h"
#include "adec.h"
#include "plog.h"
#include "../build_config.h"   // relative: source/ is not on the -I path

#include <stdio.h>
#include <string.h>
#include <sys/mutex.h>
#include <sys/cond.h>
#include <sys/thread.h>

extern void crash_log(const char *msg);

// ~683 ms of stereo audio at 48 kHz.  Power of two for cheap modulo.
#define PCM_RING_CAP 32768

// PES queue: raw PES packets queued by demux thread, consumed by adec thread.
#define PES_QUEUE_SLOTS 32
#define PES_SLOT_BYTES  8192

// ---- MP3 decoder + PCM ring ----
static mp3dec_t         s_dec;
static float            s_ring[PCM_RING_CAP * 2];  // interleaved L/R float32
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
static int              s_pes_q_rd  = 0;
static int              s_pes_q_wr  = 0;
static volatile int     s_pes_q_n   = 0;
static sys_mutex_t      s_pes_mtx;
static sys_cond_t       s_pes_cond;
static volatile bool    s_adec_run  = false;
static sys_ppu_thread_t s_adec_thread = 0;

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

static void push_samples(const short *pcm, int n, int channels) {
    sysMutexLock(s_pcm_mtx, 0);
    for (int i = 0; i < n; i++) {
        if (s_n >= PCM_RING_CAP) break;
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

static void adec_decode_pes(const u8 *pes, int pes_len) {
    // Seed the write PTS from this packet's header.  On the very first valid PTS,
    // also initialise the read cursor so both cursors start from a coherent origin.
    u64 pes_pts_us;
    if (parse_pes_pts(pes, pes_len, &pes_pts_us)) {
        sysMutexLock(s_pcm_mtx, 0);
        s_next_pcm_pts_us = pes_pts_us;
        if (!s_pts_valid) {
            s_read_pts_us = pes_pts_us;
            s_pts_valid   = true;
        }
        sysMutexUnlock(s_pcm_mtx);
    }
    if (pes_len < 9) return;
    if (pes[0] || pes[1] || pes[2] != 0x01) return;
    int hdr  = 9 + pes[8];
    if (hdr >= pes_len) return;
    const u8 *es   = pes + hdr;
    int       left = pes_len - hdr;
    while (left > 0) {
        mp3dec_frame_info_t info;
        short pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
        int samples = mp3dec_decode_frame(&s_dec, es, left, pcm, &info);
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
        es   += info.frame_bytes;
        left -= info.frame_bytes;
    }
}

void adec_push_pes(const u8 *pes, int pes_len) {
    if (pes_len <= 0 || pes_len > PES_SLOT_BYTES) return;
    sysMutexLock(s_pes_mtx, 0);
    if (s_pes_q_n >= PES_QUEUE_SLOTS) {
        // queue full — drop oldest to avoid back-pressuring the demux thread
        s_pes_q_rd = (s_pes_q_rd + 1) % PES_QUEUE_SLOTS;
        s_pes_q_n--;
    }
    memcpy(s_pes_q[s_pes_q_wr], pes, pes_len);
    s_pes_q_len[s_pes_q_wr] = pes_len;
    s_pes_q_wr = (s_pes_q_wr + 1) % PES_QUEUE_SLOTS;
    s_pes_q_n++;
    sysCondSignal(s_pes_cond);
    sysMutexUnlock(s_pes_mtx);
}

static void adec_thread_fn(void *arg) {
    (void)arg;
    u8  local_pes[PES_SLOT_BYTES];
    int local_len;
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
        local_len = s_pes_q_len[s_pes_q_rd];
        s_pes_q_rd = (s_pes_q_rd + 1) % PES_QUEUE_SLOTS;
        s_pes_q_n--;
        sysMutexUnlock(s_pes_mtx);

        adec_decode_pes(local_pes, local_len);
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
    s_adec_run = true;
    crash_log("ad3 sysThreadCreate");
    sysThreadCreate(&s_adec_thread, adec_thread_fn, NULL,
                    750, 0x10000, THREAD_JOINABLE, (char*)"jf_adec");
    crash_log("ad4 adec_start done");
}

void adec_flush(void) {
    sysMutexLock(s_pes_mtx, 0);
    s_pes_q_rd = s_pes_q_wr = s_pes_q_n = 0;
    sysMutexUnlock(s_pes_mtx);
    sysMutexLock(s_pcm_mtx, 0);
    mp3dec_init(&s_dec);
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
    crash_log("adx5 adec_stop done");
}

int adec_pcm_available(void) { return s_n; }

int adec_read_pcm(float *buf, int n_pairs) {
    sysMutexLock(s_pcm_mtx, 0);
    int got = 0;
    while (got < n_pairs && s_n > 0) {
        buf[got * 2    ] = s_ring[s_rd * 2    ];
        buf[got * 2 + 1] = s_ring[s_rd * 2 + 1];
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
