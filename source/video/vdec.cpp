#include "video.h"
#include "video_internal.h"
#include "plog.h"
#include "timing.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <malloc.h>
#include <ppu-types.h>
#include <ppu-asm.h>
#include <sysmodule/sysmodule.h>
#include <codec/vdec.h>

extern void crash_log(const char *msg);

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

// In-flight AU tracking (diagnostic): AUs actually handed to VDEC minus
// AUDONE callbacks received.  If this reaches AU_BUF_COUNT at buffer-reuse
// time, vdec_submit is overwriting a buffer the decoder may still be
// reading — bitstream corruption that smears until the next IDR.
static int   s_au_sent         = 0;
volatile int s_au_inflight_max = 0;

// Set true once fps detection completes and timing_init has been called
// with the detected rate; display loop must not pop frames until then.
volatile bool s_timing_ready = false;

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
    crash_log("v1 vdec_open enter");
    crash_log("v2 sysModuleLoad VDEC");
    plog("vdec_open: load VDEC base");
    s32 r1 = sysModuleLoad(SYSMODULE_VDEC);
    crash_log("v3 sysModuleLoad H264");
    plog("vdec_open: load VDEC_H264");
    s32 r2 = sysModuleLoad(SYSMODULE_VDEC_H264);
    { char buf[64]; snprintf(buf,sizeof(buf),"vdec_open: mod_ret base=%d h264=%d",(int)r1,(int)r2); plog(buf); }

    vdecType codec;
    codec.codec_type    = VDEC_CODEC_TYPE_H264;
    codec.profile_level = 31;

    crash_log("v4 queryAttr");
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
    crash_log("v5 memalign vdec_mem");
    plog("vdec_open: memalign vdec_mem");
    s_vdec_mem = (u8*)memalign(1024*1024, mem_size_aligned);
    if (!s_vdec_mem) { plog("vdec_open: vdec_mem alloc FAILED (4x SPU size)"); return false; }

    crash_log("v6 memalign au_bufs");
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
    crash_log("v7 vdecOpen");
    plog("vdec_open: vdecOpen");
    s32 oret = vdecOpen(&codec, &cfg, &closure, &s_vdec);
    if (oret != 0) {
        char buf[64]; snprintf(buf, sizeof(buf), "vdec_open: vdecOpen FAILED ret=%d", (int)oret);
        plog(buf); return false;
    }

    crash_log("v8 startSequence");
    plog("vdec_open: startSequence");
    s32 sret = vdecStartSequence(s_vdec);
    { char buf[64]; snprintf(buf, sizeof(buf), "vdec_open: startSequence ret=%d", (int)sret); plog(buf); }
    if (sret != 0) { plog("vdec_open: startSequence FAILED"); return false; }
    crash_log("v9 vdec_open done");
    plog("vdec_open: done");
    return true;
}

void vdec_close(void) {
    crash_log("c1 vdec_close enter");
    if (s_vdec) {
        crash_log("c2 vdecEndSequence");
        vdecEndSequence(s_vdec);
        crash_log("c3 vdecClose");
        vdecClose(s_vdec);
        s_vdec = 0;
    }
    crash_log("c4 free vdec_mem");
    if (s_vdec_mem)  { free(s_vdec_mem);  s_vdec_mem  = NULL; }
    crash_log("c5 free au_bufs");
    for (int i = 0; i < AU_BUF_COUNT; i++) {
        if (s_au_bufs[i]) { free(s_au_bufs[i]); s_au_bufs[i] = NULL; }
    }
    crash_log("c6 unload H264");
    sysModuleUnload(SYSMODULE_VDEC_H264);
    crash_log("c7 unload VDEC");
    sysModuleUnload(SYSMODULE_VDEC);
    crash_log("c8 vdec_close done");
}

void vdec_reset_counters(void) {
    s_au_submitted = 0;
    s_got_sps      = false;
    s_au_buf_idx   = 0;
    s_frames_ready = 0;
    s_au_done      = 0;
    s_au_sent      = 0;
    s_vdec_error   = false;
}

void vdec_flush(void) {
    if (!s_vdec) return;
    vdecEndSequence(s_vdec);
    vdec_reset_counters();
    vdecStartSequence(s_vdec);
}

void vdec_submit(const u8 *data, int len, u64 pts) {
    if (len > AU_BUF_SIZE) {
        // Dropped AU = missing reference frame = corruption until next IDR.
        static int s_drop_log = 0;
        if (s_drop_log < 20) {
            s_drop_log++;
            char buf[80];
            snprintf(buf, sizeof(buf),
                "AU_DROP: len=%d > buf=%d au#%d (corrupt until IDR)",
                len, AU_BUF_SIZE, s_au_submitted);
            plog(buf);
        }
        return;
    }
    if (len <= 0) return;
    {
        static int s_in_pts_log = 0;
        if (s_in_pts_log < 10) {
            s_in_pts_log++;
            char buf[96];
            snprintf(buf, sizeof(buf),
                "vdec_in_pts: n=%d pts=%llu (=0x%016llx)",
                s_in_pts_log,
                (unsigned long long)pts,
                (unsigned long long)pts);
            plog(buf);
        }
    }

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

    // VDEC reads the submitted AU buffer asynchronously and signals AUDONE
    // when finished.  Movian blocks on that callback before releasing the
    // buffer; do the same here — wait until the slot we are about to reuse
    // has been consumed, or its old bitstream gets overwritten mid-read
    // (= corrupted macroblocks smearing until the next IDR).
    {
        int inflight = s_au_sent - s_au_done;
        if (inflight > s_au_inflight_max) s_au_inflight_max = inflight;
        int waits = 0;
        while (s_au_sent - s_au_done >= AU_BUF_COUNT && waits < 500) {
            usleep(1000);
            waits++;
        }
        if (waits > 0) {
            static int s_wait_log = 0;
            if (s_wait_log < 20) {
                s_wait_log++;
                char buf[80];
                snprintf(buf, sizeof(buf), "au_wait: %dms inflight=%d au#%d",
                         waits, inflight, s_au_submitted);
                plog(buf);
            }
        }
        if (s_au_sent - s_au_done >= AU_BUF_COUNT) {
            // AUDONE never came (decoder wedged?) — overwrite anyway rather
            // than hang playback, but make it loud in the log.
            static int s_ovw_log = 0;
            if (s_ovw_log < 40) {
                s_ovw_log++;
                char buf[96];
                snprintf(buf, sizeof(buf),
                    "AU_OVERWRITE: inflight=%d >= bufs=%d au#%d (after wait)",
                    s_au_sent - s_au_done, AU_BUF_COUNT, s_au_submitted);
                plog(buf);
            }
        }
    }

    u8 *au_buf = s_au_bufs[s_au_buf_idx % AU_BUF_COUNT];
    s_au_buf_idx++;
    memcpy(au_buf, data, len);

    vdecAU au;
    memset(&au, 0, sizeof(au));
    au.packet_addr = (u32)(uintptr_t)au_buf;
    au.packet_size = (u32)len;
    // Convert 90kHz PTS to microseconds before passing to VDEC.
    // Hypothesis: VDEC rejects PTS values below some threshold and
    // returns VDEC_TS_INVALID. Movian passes microsecond-scale
    // values (millions) and VDEC accepts them.
    u64 pts_us = (pts * 100ULL) / 9ULL;
    // Round to millisecond precision (Movian: required for some streams)
    pts_us = (pts_us / 1000ULL) * 1000ULL;
    au.pts.low = (u32)(pts_us & 0xFFFFFFFFUL);
    au.pts.hi  = (u32)(pts_us >> 32);
    au.dts.low = (u32)(pts_us & 0xFFFFFFFFUL);
    au.dts.hi  = (u32)(pts_us >> 32);

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
    s_au_sent++;
}

// -------------------------------------------------------
// Frame-rate detection
// -------------------------------------------------------

// MPEG-2/H264 frame rate codes reported by the PS3 VDEC hardware.
// Values 1-8 follow the ISO 13818-2 frame_rate_code table.
static void fps_from_frc(int frc, int *num, int *den) {
    switch (frc) {
        case 1: *num = 24000; *den = 1001; break;  /* 23.976fps */
        case 2: *num = 24;    *den = 1;    break;  /* 24fps     */
        case 3: *num = 25;    *den = 1;    break;  /* 25fps     */
        case 4: *num = 30000; *den = 1001; break;  /* 29.97fps  */
        case 5: *num = 30;    *den = 1;    break;  /* 30fps     */
        case 6: *num = 50;    *den = 1;    break;  /* 50fps     */
        case 7: *num = 60000; *den = 1001; break;  /* 59.94fps  */
        case 8: *num = 60;    *den = 1;    break;  /* 60fps     */
        default:
            plog("fps_detect: unknown frc, defaulting to 30fps");
            *num = 30; *den = 1; break;
    }
}

// Frame durations in microseconds from ISO 13818-2 frame_rate_code (Movian mpeg_durations table).
static s64 dur_from_frc(int frc) {
    switch (frc) {
        case 1: return 41708;   /* 23.976 fps */
        case 2: return 41667;   /* 24 fps     */
        case 3: return 40000;   /* 25 fps     */
        case 4: return 33367;   /* 29.97 fps  */
        case 5: return 33333;   /* 30 fps     */
        case 6: return 20000;   /* 50 fps     */
        case 7: return 16683;   /* 59.94 fps  */
        case 8: return 16667;   /* 60 fps     */
        default: return 40000;
    }
}

// Pull one decoded frame into the next free jitter buffer slot.
// Called only from the decode thread (the single jbuf producer).
bool vdec_pull_frame(void) {
    if (jbuf_full()) return false;
    if (s_frames_ready <= 0) return false;

    u32 pic_addr = 0;
    if (vdecGetPicItem(s_vdec, &pic_addr) != 0 || pic_addr == 0)
        return false;

    const vdecPicture *pic = (const vdecPicture*)(uintptr_t)pic_addr;
    // Read PTS before vdecGetPicture — GetPicture consumes the picture
    // and may invalidate the pic_addr metadata (mirrors Movian order).
    u64 pts_us = 0;
    if (pic->pts[0].low != (u32)VDEC_TS_INVALID) {
        // VDEC now outputs PTS in microseconds (matches input format).
        pts_us = ((u64)pic->pts[0].hi << 32) | (u64)pic->pts[0].low;
    }
    {
        static int s_vdec_pts_log = 0;
        if (s_vdec_pts_log < 30) {
            s_vdec_pts_log++;
            char buf[160];
            snprintf(buf, sizeof(buf),
                "vdec_pts: n=%d raw_lo=0x%08x raw_hi=0x%08x pts_us=%llu",
                s_vdec_pts_log,
                (unsigned)pic->pts[0].low,
                (unsigned)pic->pts[0].hi,
                (unsigned long long)pts_us);
            plog(buf);
        }
    }
    u8 frc = 0;
    if (pic->codec_specific_addr) {
        const vdecH264Info *h =
            (const vdecH264Info*)(uintptr_t)pic->codec_specific_addr;
        frc = h->frame_rate;
        if (h->width > 0 && h->height > 0 &&
            (h->width != jbuf_fw() || h->height != jbuf_fh())) {
            char buf[80];
            snprintf(buf, sizeof(buf), "vdec: actual %ux%u (alloc %ux%u)",
                     h->width, h->height, jbuf_fw(), jbuf_fh());
            plog(buf);
            jbuf_set_dims(h->width, h->height);
        }
    }

    vdecPictureFormat vfmt;
    vfmt.format_type  = VDEC_PICFMT_ARGB32;
    vfmt.color_matrix = VDEC_COLOR_MATRIX_BT709;
    vfmt.alpha        = 0xFF;
    s32 gpret = vdecGetPicture(s_vdec, &vfmt, jbuf_write_ptr());
    if (gpret != 0) {
        char buf[128];
        snprintf(buf, sizeof(buf), "CORRUPT: vdecGetPicture failed rc=0x%x slot=%d",
            (unsigned)gpret, jbuf_write_idx());
        plog(buf);
        return false;
    }
    {
        static int s_sw_count = 0;
        if (s_sw_count < 60) {
            u32 centre_px = ((u32*)jbuf_write_ptr())[(jbuf_fh() / 2) * jbuf_fw() + (jbuf_fw() / 2)];
            char buf[64];
            snprintf(buf, sizeof(buf), "slot_write: slot=%d px=0x%08x",
                jbuf_write_idx(), centre_px);
            plog(buf);
            s_sw_count++;
        }
    }
    s_frames_ready--;

    s64 dur_us = dur_from_frc(frc);
    {
        static u64 s_last_push_pts = 0;
        static int s_push_count    = 0;
        static int s_nonmono_count = 0;

        if (pts_us != 0 && pts_us != (u64)VDEC_TS_INVALID) {
            if (s_last_push_pts != 0 && pts_us < s_last_push_pts) {
                s_nonmono_count++;
                if (s_nonmono_count <= 20) {
                    char buf[128];
                    snprintf(buf, sizeof(buf),
                        "pts_nonmono: push#%d prev=%llu new=%llu delta=%lld",
                        s_push_count,
                        (unsigned long long)s_last_push_pts,
                        (unsigned long long)pts_us,
                        (long long)((s64)pts_us - (s64)s_last_push_pts));
                    plog(buf);
                }
            }
            s_last_push_pts = pts_us;
        }
        s_push_count++;

        if (s_push_count % 300 == 0) {
            char buf[128];
            snprintf(buf, sizeof(buf),
                "pts_stats: pushes=%d nonmono=%d dur=%lldus",
                s_push_count, s_nonmono_count,
                (long long)dur_us);
            plog(buf);
        }
    }
    jbuf_push(pts_us, dur_us);

    if (!s_timing_ready) {
        int fps_num, fps_den;
        fps_from_frc(frc, &fps_num, &fps_den);
        timing_init(fps_num, fps_den);
        s_timing_ready = true;
        char buf[64];
        snprintf(buf, sizeof(buf), "fps_detect: frc=%d -> %d/%d", (int)frc, fps_num, fps_den);
        plog(buf);
    }

    return true;
}
