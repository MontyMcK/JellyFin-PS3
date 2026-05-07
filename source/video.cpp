#include "video.h"
#include "adec.h"
#include "plog.h"
#include "timing.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <malloc.h>
#include <ppu-types.h>
#include <ppu-asm.h>
#include <sysmodule/sysmodule.h>
#include <codec/vdec.h>
#include <sys/mutex.h>

// -------------------------------------------------------
// MPEG-TS demuxer
// -------------------------------------------------------

#define TS_SYNC_BYTE     0x47
#define TS_PID_PAT       0x0000
#define TS_STREAM_H264   0x1B
#define TS_STREAM_MP3    0x03   // MPEG-1 audio (Layer 1/2/3)
#define TS_STREAM_MP3_2  0x04   // MPEG-2 audio

typedef struct {
    u16  pmt_pid;
    u16  video_pid;
    u16  audio_pid;
    // Video PES reassembly
    u8   pes_buf[512 * 1024];
    int  pes_len;
    bool pes_started;
    // Audio PES reassembly (MP3 frames are small; 32 KB is ample)
    u8   a_pes_buf[32 * 1024];
    int  a_pes_len;
    bool a_pes_started;
} TSState;

static void ts_parse_pat(TSState *ts, const u8 *data, int len) {
    if (len < 12 || data[0] != 0x00) return;
    u16 sec_len = ((u16)(data[1] & 0x0F) << 8) | data[2];
    int end = 3 + (int)sec_len - 4;
    if (end > len) end = len;
    int pos = 8;
    while (pos + 3 < end) {
        u16 prog = ((u16)data[pos] << 8) | data[pos+1];
        u16 pid  = ((u16)(data[pos+2] & 0x1F) << 8) | data[pos+3];
        if (prog != 0) { ts->pmt_pid = pid; return; }
        pos += 4;
    }
}

static void ts_parse_pmt(TSState *ts, const u8 *data, int len) {
    if (len < 16 || data[0] != 0x02) return;
    u16 sec_len   = ((u16)(data[1] & 0x0F) << 8) | data[2];
    int end       = 3 + (int)sec_len - 4;
    if (end > len) end = len;
    u16 prog_info = ((u16)(data[10] & 0x0F) << 8) | data[11];
    int pos       = 12 + prog_info;
    while (pos + 4 < end) {
        u8  stype  = data[pos];
        u16 epid   = ((u16)(data[pos+1] & 0x1F) << 8) | data[pos+2];
        u16 esinfo = ((u16)(data[pos+3] & 0x0F) << 8) | data[pos+4];
        if (stype == TS_STREAM_H264 && !ts->video_pid)
            ts->video_pid = epid;
        if ((stype == TS_STREAM_MP3 || stype == TS_STREAM_MP3_2) && !ts->audio_pid)
            ts->audio_pid = epid;
        pos += 5 + esinfo;
    }
}

// Returns bitmask: bit 0 = video PES ready, bit 1 = audio PES ready.
static int ts_process(TSState *ts, const u8 *pkt,
                      u8 *out_vpes, int *out_vlen,
                      u8 *out_apes, int *out_alen) {
    *out_vlen = 0;
    *out_alen = 0;
    if (pkt[0] != TS_SYNC_BYTE) return 0;

    bool pusi    = (pkt[1] & 0x40) != 0;
    u16  pid     = ((u16)(pkt[1] & 0x1F) << 8) | pkt[2];
    u8   afl     = (pkt[3] >> 4) & 3;
    bool has_pay = (afl & 1) != 0;
    if (!has_pay) return 0;

    int  off = 4;
    if (afl & 2) off += pkt[4] + 1;
    if (off >= TS_PACKET_SIZE) return 0;

    const u8 *pay  = pkt + off;
    int       plen = TS_PACKET_SIZE - off;

    if (pid == TS_PID_PAT && pusi && !ts->pmt_pid) {
        int ptr = pay[0];
        if (ptr + 1 < plen) ts_parse_pat(ts, pay + 1 + ptr, plen - 1 - ptr);
        return 0;
    }
    // Keep parsing PMT until both video and audio PIDs are known.
    if (ts->pmt_pid && pid == ts->pmt_pid && pusi &&
        (!ts->video_pid || !ts->audio_pid)) {
        int ptr = pay[0];
        if (ptr + 1 < plen) ts_parse_pmt(ts, pay + 1 + ptr, plen - 1 - ptr);
        return 0;
    }

    int result = 0;

    if (ts->video_pid && pid == ts->video_pid) {
        if (pusi && ts->pes_started && ts->pes_len > 0) {
            int copy = ts->pes_len < (int)sizeof(ts->pes_buf)
                     ? ts->pes_len : (int)sizeof(ts->pes_buf);
            memcpy(out_vpes, ts->pes_buf, copy);
            *out_vlen  = copy;
            result    |= 1;
            ts->pes_len = 0;
        }
        if (pusi) { ts->pes_started = true; ts->pes_len = 0; }
        if (ts->pes_started && plen > 0) {
            int room = (int)sizeof(ts->pes_buf) - ts->pes_len;
            int copy = plen < room ? plen : room;
            memcpy(ts->pes_buf + ts->pes_len, pay, copy);
            ts->pes_len += copy;
        }
    }

    if (ts->audio_pid && pid == ts->audio_pid) {
        if (pusi && ts->a_pes_started && ts->a_pes_len > 0) {
            int copy = ts->a_pes_len < (int)sizeof(ts->a_pes_buf)
                     ? ts->a_pes_len : (int)sizeof(ts->a_pes_buf);
            memcpy(out_apes, ts->a_pes_buf, copy);
            *out_alen    = copy;
            result      |= 2;
            ts->a_pes_len = 0;
        }
        if (pusi) { ts->a_pes_started = true; ts->a_pes_len = 0; }
        if (ts->a_pes_started && plen > 0) {
            int room = (int)sizeof(ts->a_pes_buf) - ts->a_pes_len;
            int copy = plen < room ? plen : room;
            memcpy(ts->a_pes_buf + ts->a_pes_len, pay, copy);
            ts->a_pes_len += copy;
        }
    }

    return result;
}

// Strip PES header and return pointer into the H.264 payload.
// Sets *pts_out to the 90 kHz PTS when pes[7]&0x80, else VDEC_TS_INVALID.
static bool pes_payload(const u8 *pes, int pes_len, const u8 **h264, int *h264_len, u64 *pts_out) {
    if (pes_len < 9) return false;
    if (pes[0] != 0x00 || pes[1] != 0x00 || pes[2] != 0x01) return false;
    int hdr = 9 + pes[8];
    if (hdr >= pes_len) return false;
    *h264     = pes + hdr;
    *h264_len = pes_len - hdr;
    if ((pes[7] & 0x80) && pes_len >= 14) {
        *pts_out = ((u64)((pes[9]  & 0x0E) >> 1) << 30) |
                   ((u64)(pes[10])               << 22) |
                   ((u64)((pes[11] & 0xFE) >> 1) << 15) |
                   ((u64)(pes[12])               <<  7) |
                   ((u64)((pes[13] & 0xFE) >> 1));
    } else {
        *pts_out = (u64)VDEC_TS_INVALID;
    }
    return true;
}

// -------------------------------------------------------
// VDEC — video decoder
// -------------------------------------------------------

#define AU_BUF_SIZE  (512 * 1024)
#define AU_BUF_COUNT 4

static u32  s_vdec     = 0;
static u8  *s_vdec_mem = NULL;
static u8  *s_au_bufs[AU_BUF_COUNT] = {};
static int  s_au_buf_idx = 0;

static opd32 s_vdec_cb_opd32;

volatile bool s_vdec_error   = false;
volatile int  s_frames_ready = 0;
volatile int  s_au_done      = 0;
int           s_au_submitted = 0;
static bool   s_got_sps      = false;

static u32 vdec_cb(u32 handle, u32 msgtype, u32 msgdata, u32 arg) {
    (void)handle; (void)msgdata; (void)arg;
    switch (msgtype) {
    case VDEC_CALLBACK_PICOUT:  s_frames_ready++; break;
    case VDEC_CALLBACK_AUDONE:  s_au_done++;      break;
    case VDEC_CALLBACK_SEQDONE: break;
    case VDEC_CALLBACK_ERROR:   s_vdec_error = true; break;
    }
    return 0;
}

bool vdec_open(void) {
    plog("vdec_open: load VDEC base");
    s32 r1 = sysModuleLoad(SYSMODULE_VDEC);
    plog("vdec_open: load VDEC_H264");
    s32 r2 = sysModuleLoad(SYSMODULE_VDEC_H264);
    { char buf[64]; snprintf(buf,sizeof(buf),"vdec_open: mod_ret base=%d h264=%d",(int)r1,(int)r2); plog(buf); }

    vdecType codec;
    codec.codec_type    = VDEC_CODEC_TYPE_H264;
    codec.profile_level = 31;

    plog("vdec_open: queryAttr");
    vdecAttr attr;
    s32 qret = vdecQueryAttr(&codec, &attr);
    if (qret != 0) {
        char buf[64]; snprintf(buf, sizeof(buf), "vdec_open: queryAttr FAILED ret=%d", (int)qret);
        plog(buf); return false;
    }
    { char buf[64]; snprintf(buf, sizeof(buf), "vdec_open: mem_size=%u", attr.mem_size); plog(buf); }

    const u32 NUM_SPUS = 3; //was 4
    u32 mem_size_aligned = ((attr.mem_size * NUM_SPUS) + (1024*1024-1))
                           & ~(u32)(1024*1024-1);
    plog("vdec_open: memalign vdec_mem");
    s_vdec_mem = (u8*)memalign(1024*1024, mem_size_aligned);
    if (!s_vdec_mem) { plog("vdec_open: vdec_mem alloc FAILED (4x SPU size)"); return false; }

    plog("vdec_open: memalign au_bufs");
    for (int i = 0; i < AU_BUF_COUNT; i++) {
        s_au_bufs[i] = (u8*)memalign(128, AU_BUF_SIZE);
        if (!s_au_bufs[i]) { plog("vdec_open: au_buf alloc FAILED"); return false; }
    }
    s_au_buf_idx = 0;

    vdecConfig cfg;
    cfg.mem_addr              = (u32)(uintptr_t)s_vdec_mem;
    cfg.mem_size              = mem_size_aligned;
    cfg.ppu_thread_prio       = 500;
    cfg.ppu_thread_stack_size = 0x40000;
    cfg.spu_thread_prio       = 250;
    cfg.num_spus              = NUM_SPUS;

    vdecClosure closure;
    closure.fn  = __build_opd32((opd64*)(uintptr_t)(vdecCallback)vdec_cb, &s_vdec_cb_opd32);
    closure.arg = 0;

    {
        char buf[128];
        snprintf(buf, sizeof(buf),
            "cfg: addr=0x%08x size=%u(%u) prio=%u stack=0x%x spu_prio=%u spus=%u",
            cfg.mem_addr, cfg.mem_size, attr.mem_size,
            cfg.ppu_thread_prio, cfg.ppu_thread_stack_size,
            cfg.spu_thread_prio, cfg.num_spus);
        plog(buf);
    }
    { char buf[80]; snprintf(buf, sizeof(buf), "vdec_cb_opd32: code=0x%08x toc=0x%08x fn=0x%08x",
        (unsigned)s_vdec_cb_opd32.func, (unsigned)s_vdec_cb_opd32.rtoc, (unsigned)closure.fn); plog(buf); }
    plog("vdec_open: vdecOpen");
    s32 oret = vdecOpen(&codec, &cfg, &closure, &s_vdec);
    if (oret != 0) {
        char buf[64]; snprintf(buf, sizeof(buf), "vdec_open: vdecOpen FAILED ret=%d", (int)oret);
        plog(buf); return false;
    }

    plog("vdec_open: startSequence");
    s32 sret = vdecStartSequence(s_vdec);
    { char buf[64]; snprintf(buf, sizeof(buf), "vdec_open: startSequence ret=%d", (int)sret); plog(buf); }
    if (sret != 0) { plog("vdec_open: startSequence FAILED"); return false; }
    plog("vdec_open: done");
    return true;
}

void vdec_close(void) {
    if (s_vdec) {
        vdecEndSequence(s_vdec);
        vdecClose(s_vdec);
        s_vdec = 0;
    }
    if (s_vdec_mem)  { free(s_vdec_mem);  s_vdec_mem  = NULL; }
    for (int i = 0; i < AU_BUF_COUNT; i++) {
        if (s_au_bufs[i]) { free(s_au_bufs[i]); s_au_bufs[i] = NULL; }
    }
    sysModuleUnload(SYSMODULE_VDEC_H264);
    sysModuleUnload(SYSMODULE_VDEC);
}

static void vdec_submit(const u8 *data, int len, u64 pts) {
    if (len <= 0 || len > AU_BUF_SIZE) return;

    u8   nal_first = 0;
    bool has_sps   = false;
    for (int i = 0; i + 3 < len; i++) {
        if (data[i] != 0x00 || data[i+1] != 0x00) continue;
        u8 nal = 0;
        if (                   data[i+2] == 0x01 && i+3 < len) nal = data[i+3] & 0x1f;
        else if (data[i+2]==0 && data[i+3]==0x01 && i+4 < len) nal = data[i+4] & 0x1f;
        else continue;
        if (!nal_first) nal_first = nal;
        if (nal == 7) has_sps = true;
    }
    if (has_sps) s_got_sps = true;

    // Log first 5 AUs (startup) and every 300th thereafter (~10 s at 30 fps).
    // IDR events are NOT logged here — each plog() flushes to the PS3 HDD
    // (5-15 ms) and IDRs arrive every ~120 AUs (every 4 s), so logging them
    // caused a freeze at identical timestamps on every playback attempt.
    s_au_submitted++;

    if (!s_got_sps) return;

    u8 *au_buf = s_au_bufs[s_au_buf_idx % AU_BUF_COUNT];
    s_au_buf_idx++;
    memcpy(au_buf, data, len);

    vdecAU au;
    memset(&au, 0, sizeof(au));
    au.packet_addr = (u32)(uintptr_t)au_buf;
    au.packet_size = (u32)len;
    au.pts.low     = (u32)(pts & 0xFFFFFFFFUL);
    au.pts.hi      = (u32)(pts >> 32);
    au.dts.low     = VDEC_TS_INVALID;
    au.dts.hi      = 0;

    int retries = 0;
    s32 dret;
    do {
        dret = vdecDecodeAu(s_vdec, VDEC_DECODER_MODE_NORMAL, &au);
        if (dret == (s32)VDEC_ERROR_BUSY) {
            usleep(1000);
            retries++;
        }
    } while (dret == (s32)VDEC_ERROR_BUSY && retries < 200);

    if (dret != 0) {
        char buf[64];
        snprintf(buf, sizeof(buf), "AU#%d FAIL ret=0x%08x retries=%d",
            s_au_submitted-1, (unsigned)dret, retries);
        plog(buf);
        return;
    }
}

// -------------------------------------------------------
// Frame-rate detection state  (Step 7)
// -------------------------------------------------------

volatile bool s_timing_ready = false;

static u64  s_det_ts[8];
static int  s_det_count = 0;
static bool s_det_done  = false;

// -------------------------------------------------------
// Jitter buffer
// -------------------------------------------------------

sys_mutex_t s_jbuf_mtx;

static u8  *s_jbuf_data[JBUF_SIZE] = {};
static u32  s_jbuf_fw = 0, s_jbuf_fh = 0;
static int  s_jb_wr = 0, s_jb_rd = 0;
static volatile int s_jb_n = 0;

bool jbuf_alloc(u32 fw, u32 fh) {
    s_jbuf_fw = fw; s_jbuf_fh = fh;
    for (int i = 0; i < JBUF_SIZE; i++) {
        s_jbuf_data[i] = (u8*)memalign(128, fw * fh * 4);
        if (!s_jbuf_data[i]) return false;
    }
    s_jb_wr = s_jb_rd = s_jb_n = 0;
    sys_mutex_attr_t attr;
    memset(&attr, 0, sizeof(attr));
    attr.attr_protocol  = SYS_LWMUTEX_ATTR_PROTOCOL;
    attr.attr_recursive = SYS_MUTEX_ATTR_RECURSIVE;
    sysMutexCreate(&s_jbuf_mtx, &attr);
    return true;
}

void jbuf_free(void) {
    for (int i = 0; i < JBUF_SIZE; i++) {
        if (s_jbuf_data[i]) { free(s_jbuf_data[i]); s_jbuf_data[i] = NULL; }
    }
    s_jb_wr = s_jb_rd = s_jb_n = 0;
    sysMutexDestroy(s_jbuf_mtx);
}

const u8 *jbuf_peek(void)  { return (s_jb_n > 0) ? s_jbuf_data[s_jb_rd] : NULL; }
void      jbuf_pop(void)   { s_jb_rd = (s_jb_rd + 1) % JBUF_SIZE; s_jb_n--; }
u32       jbuf_fw(void)    { return s_jbuf_fw; }
u32       jbuf_fh(void)    { return s_jbuf_fh; }
int       jbuf_count(void) { return s_jb_n; }

// Pull one decoded frame into the next free jitter buffer slot.
// Called only from the decode thread; s_jbuf_mtx guards s_jb_n.
bool vdec_pull_frame(void) {
    sysMutexLock(s_jbuf_mtx, 0);
    bool full = (s_jb_n >= JBUF_SIZE);
    sysMutexUnlock(s_jbuf_mtx);
    if (full) return false;
    if (s_frames_ready <= 0) return false;

    // Peek at actual decoded frame dimensions before consuming.
    // VDEC may write fewer rows than s_jbuf_fh (e.g. 1280×534 for 2.35:1 content);
    // using the wrong height makes the centering treat black rows as picture rows.
    u32 pic_addr = 0;
    if (vdecGetPicItem(s_vdec, &pic_addr) == 0 && pic_addr != 0) {
        const vdecPicture  *pic = (const vdecPicture*)(uintptr_t)pic_addr;
        if (pic->codec_specific_addr) {
            const vdecH264Info *h = (const vdecH264Info*)(uintptr_t)pic->codec_specific_addr;
            if (h->width > 0 && h->height > 0 &&
                (h->width != s_jbuf_fw || h->height != s_jbuf_fh)) {
                char buf[80];
                snprintf(buf, sizeof(buf), "vdec: actual %ux%u (alloc %ux%u)",
                         h->width, h->height, s_jbuf_fw, s_jbuf_fh);
                plog(buf);
                s_jbuf_fw = h->width;
                s_jbuf_fh = h->height;
            }
        }
    }

    vdecPictureFormat vfmt;
    vfmt.format_type  = VDEC_PICFMT_ARGB32;
    vfmt.color_matrix = VDEC_COLOR_MATRIX_BT709;
    vfmt.alpha        = 0xFF;
    if (vdecGetPicture(s_vdec, &vfmt, s_jbuf_data[s_jb_wr]) != 0) return false;
    s_frames_ready--;
    sysMutexLock(s_jbuf_mtx, 0);
    s_jb_wr = (s_jb_wr + 1) % JBUF_SIZE;
    s_jb_n++;
    sysMutexUnlock(s_jbuf_mtx);

    // Step 7a-7c: record timestamps for the first 8 frames, then detect fps.
    if (!s_det_done) {
        s_det_ts[s_det_count] = timing_get_us();
        s_det_count++;
        if (s_det_count >= 8) {
            s_det_done = true;
            u64 avg_us = (s_det_ts[7] - s_det_ts[0]) / 7;
            u32 detected = 24;
            if (avg_us > 0) {
                u32 raw = (u32)(1000000ULL / avg_us);
                static const u32 std_fps[] = {24, 25, 30, 50, 60};
                u32 best_diff = (raw > 24) ? (raw - 24) : (24 - raw);
                for (int i = 1; i < 5; i++) {
                    u32 d = (raw > std_fps[i]) ? (raw - std_fps[i]) : (std_fps[i] - raw);
                    if (d < best_diff) { best_diff = d; detected = std_fps[i]; }
                }
                char buf[96];
                snprintf(buf, sizeof(buf),
                    "fps_detect: avg_us=%llu raw=%u -> %u fps",
                    (unsigned long long)avg_us, raw, detected);
                plog(buf);
            } else {
                plog("fps_detect: avg_us=0, fallback 24fps");
            }
            timing_init(detected, 1);
            s_timing_ready = true;
        }
    }

    return true;
}

// -------------------------------------------------------
// Per-session state shared by video_feed_ts
// -------------------------------------------------------

static TSState s_ts;
static u8      s_pes_out[512 * 1024];
static u8      s_audio_pes_out[32 * 1024];

void video_reset(void) {
    memset(&s_ts, 0, sizeof(s_ts));
    s_au_submitted = 0;
    s_got_sps      = false;
    s_au_buf_idx   = 0;
    s_frames_ready = 0;
    s_au_done      = 0;
    s_vdec_error   = false;
    s_timing_ready = false;
    s_det_count    = 0;
    s_det_done     = false;
}

bool video_feed_ts(const u8 *pkt) {
    int vlen = 0, alen = 0;
    int ready = ts_process(&s_ts, pkt,
                           s_pes_out,       &vlen,
                           s_audio_pes_out, &alen);
    if (ready & 1) {
        const u8 *h264; int h264_len; u64 pts;
        if (pes_payload(s_pes_out, vlen, &h264, &h264_len, &pts))
            vdec_submit(h264, h264_len, pts);
    }
    if (ready & 2)
        adec_push_pes(s_audio_pes_out, alen);

    if (s_frames_ready > 0)
        return vdec_pull_frame();
    return false;
}
