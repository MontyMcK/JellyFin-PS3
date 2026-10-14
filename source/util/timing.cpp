#include "timing.h"
#include "audio.h"
#include "plog.h"
#include "vblend.h"
#include "player_stats.h"
#include "../build_config.h"
#include <stdio.h>
#include <ppu-asm.h>
#include <sys/systime.h>
#include <rsx/gcm_sys.h>
#include <sysutil/video.h>

// Direct timebase register read — single instruction, no LV2 syscall.
// Cell PPU mftb fills a 64-bit GPR on a 64-bit core, so no upper/lower split needed.
static u64 s_tb_freq = 79800000ULL;  // overridden in timing_init

static inline u64 read_tb(void) { return __gettime(); }

u64 timing_get_us(void) {
    return read_tb() * 1000000ULL / s_tb_freq;
}

static u64 s_interval_us   = 33333;
static u64 s_last_shown_us = 0;

// Vsync-counted scheduling state.
// Variables marked volatile are read by the vblank handler (interrupt context)
// as well as written by the display thread; volatile prevents register caching.
static u32           s_fps_num          = 30;
static u32           s_fps_den          = 1;
static u32           s_display_num      = 60000;  // display refresh rate numerator
static bool          s_display_interlaced = false; // 480i/576i/1080i output
static u32           s_display_den      = 1001;   // display refresh rate denominator
static volatile u64  s_vsync_count      = 0;
static volatile u64  s_last_shown_vsync = 0;
static s64           s_vsync_err        = 0;   // Bresenham accumulator, units of fps_num
static volatile u32  s_vsyncs_next      = 2;   // precomputed vsync hold for the next frame
static volatile bool s_vsync_shown_once = false;

// Phase 2 — vblank-edge flip trigger.
// The vblank handler sets s_flip_trigger at the exact vsync edge where the
// Bresenham gate fires.  The display thread reads/clears it in timing_flip_due().
// s_flip_trigger_vc records which vsync set it, for slip detection.
static volatile u32 s_flip_trigger    = 0;
static volatile u64 s_flip_trigger_vc = 0;

// VBlank handler — called by RSX at the display refresh rate regardless of display loop speed.
// Now owns the Bresenham gate check: fires s_flip_trigger at the exact vsync
// edge where a new frame is due, so the display thread acts on a precise edge
// rather than a polled count.  Guard against overwrite: if the display thread
// hasn't consumed the previous trigger, don't stomp it.
static void s_vblank_handler(const u32 head) {
    (void)head;
    // Observe only — player_stats never feeds anything back into the gate
    // below.  Integer-only and allocation-free, safe at interrupt priority.
    player_stats_on_vblank();
    u64 vc = ++s_vsync_count;
    if (s_vsync_shown_once &&
        s_flip_trigger == 0 &&
        vc >= s_last_shown_vsync + (u64)s_vsyncs_next) {
        s_flip_trigger    = 1;
        s_flip_trigger_vc = vc;
    }
}

// Display-rate override, set by display_24p.cpp after it has switched the
// output AND measured the new vblank period.  The refresh bitmask cannot say
// 23.976 (its 24Hz-family bits are undocumented and would fall to the 59.94
// default below), so the measured rate is authoritative while it is set.
static u32 s_override_num = 0;
static u32 s_override_den = 0;

void timing_set_display_override(u32 num, u32 den) {
    s_override_num = num;
    s_override_den = den;
}

u64 timing_vsync_count(void) {
    return s_vsync_count;
}

void timing_register_vblank(void) {
    gcmSetVBlankHandler(s_vblank_handler);
    plog("timing: vsync handler registered");
}

void timing_init(u32 fps_num, u32 fps_den) {
    u64 f = sysGetTimebaseFrequency();
    if (f >= 1000000ULL && f <= 1000000000ULL) s_tb_freq = f;

    // Query actual display refresh rate; default 59.94 Hz if unavailable.
    s_display_num = 60000;
    s_display_den = 1001;
    {
        u16 rr_raw = 0;
        videoState vs;
        if (videoGetState(0, 0, &vs) == 0) {
            u16 rr = vs.displayMode.refreshRates;
            rr_raw = rr;
            s_display_interlaced =
                (vs.displayMode.scanMode == VIDEO_SCANMODE_INTERLACE);
            if      (rr & VIDEO_REFRESH_59_94HZ) { s_display_num = 60000; s_display_den = 1001; }
            else if (rr & VIDEO_REFRESH_50HZ)    { s_display_num = 50;    s_display_den = 1;    }
            else if (rr & VIDEO_REFRESH_60HZ)    { s_display_num = 60;    s_display_den = 1;    }
            else if (rr & VIDEO_REFRESH_30HZ)    { s_display_num = 30;    s_display_den = 1;    }
        }
        if (s_override_num && s_override_den) {
            s_display_num = s_override_num;
            s_display_den = s_override_den;
        }
        char buf[128];
        snprintf(buf, sizeof(buf), "timing: display=%u/%u%s (rr=0x%02x) fps=%u/%u scan=%s blend=%d",
                 s_display_num, s_display_den, s_override_num ? " MEASURED" : "",
                 (unsigned)rr_raw, fps_num, fps_den,
                 s_display_interlaced ? "interlaced" : "progressive",
                 vblend_enabled() ? 1 : 0);
        plog(buf);
    }

    s_interval_us      = (u64)1000000 * fps_den / fps_num;
    s_last_shown_us    = 0;
    s_fps_num          = fps_num;
    s_fps_den          = fps_den;
    s_vsync_count      = 0;
    s_last_shown_vsync = 0;
    s_vsync_err        = 0;
    s_vsyncs_next      = 2;
    s_vsync_shown_once = false;
    s_flip_trigger     = 0;
    s_flip_trigger_vc  = 0;
}

void timing_shutdown(void) {
    gcmSetVBlankHandler(NULL);
    plog("timing: vsync handler unregistered");
}

bool timing_frame_due(void) {
    if (s_last_shown_us == 0) return true;
    return timing_get_us() - s_last_shown_us >= s_interval_us;
}

void timing_vsync_tick(void) {
    // No-op: s_vsync_count is now maintained by the VBlank hardware callback.
}

bool timing_frame_due_vsync(void) {
    if (!s_vsync_shown_once) return true;
    return s_vsync_count >= s_last_shown_vsync + s_vsyncs_next;
}

// Phase 2: vblank-edge gate.  Returns true when the vblank handler has set the
// flip trigger at the exact vsync edge where the Bresenham accumulator fires.
// Clears the trigger on read (one-shot).  Logs a slip diagnostic if the display
// thread consumed the trigger more than one vsync after it was set, indicating
// a render overrun or thread scheduling stall.
bool timing_flip_due(void) {
    if (!s_vsync_shown_once) return true;
    if (!s_flip_trigger) return false;

    u64 now_vc       = s_vsync_count;
    u64 triggered_vc = s_flip_trigger_vc;
    s_flip_trigger = 0;

    if (now_vc > triggered_vc) {
        char buf[64];
        snprintf(buf, sizeof(buf), "flip_late: slipped %llu vsyncs",
                 (unsigned long long)(now_vc - triggered_vc));
        plog(buf);
    }
    return true;
}

void timing_frame_shown(void) {
    // Clear any trigger the handler may have re-armed between timing_flip_due()
    // and here; the handler will re-fire at the correct edge after this update.
    s_flip_trigger     = 0;
    s_last_shown_us    = timing_get_us();
    s_last_shown_vsync = s_vsync_count;
    s_vsync_shown_once = true;
    // Advance Bresenham accumulator using actual display refresh rate.
    s_vsync_err   += (s64)(s_display_num * s_fps_den);
    s_vsyncs_next  = (u32)(s_vsync_err / (s64)(s_fps_num * s_display_den));
    s_vsync_err   -= (s64)s_vsyncs_next * (s64)(s_fps_num * s_display_den);
}

// Nominal per-vblank duration (µs) for the detected display refresh, derived
// from the same s_display_num/den the Bresenham accumulator above uses.  The
// duration-consumption gate (player_display.cpp) drains this much content per
// real vblank, so it stays correct at ANY refresh: 16683 at 59.94Hz NTSC,
// 20000 at 50Hz PAL, 33333 at 30Hz.  Replaces a hardcoded 16683 that silently
// assumed a 60Hz display and undershot ~17% on a 50Hz CRT (judder).
// True when one content frame lasts exactly one vblank (24p output of 24p
// film, 2026-09-27): player_display.cpp then presents frame-locked.
bool timing_is_1to1(void) {
    return s_fps_num && s_display_num &&
           (u64)s_display_num * s_fps_den == (u64)s_fps_num * s_display_den;
}

bool timing_display_interlaced(void) { return s_display_interlaced; }

s64 timing_vblank_period_us(void) {
    if (s_display_num == 0) return 16683;   // defensive; never happens post-init
    return (s64)1000000 * (s64)s_display_den / (s64)s_display_num;
}

// ---- AV sync EMA ----

static s64  s_avsync_smooth_us    = 0;
static bool s_avsync_initialized  = false;

// Video-PTS base correction.
//
// The audio read cursor is the master clock and is always transcode-RELATIVE
// (starts ~0).  For A/V to compare correctly the video PTS must share that base.
// A normal (re)open returns video relative too (also ~0), so they already agree.
// A subtitle burn-in reopen (SubtitleMethod=Encode) instead returns the video
// PTS ABSOLUTE (~StartTimeTicks) while audio still resets to ~0 — e.g. audio
// 1.36s vs video 39.42s at a 37.87s seek target.  The ~seek-target gap makes
// avsync_is_locked() never trip and disables the drift bias.
//
// Fix: fold out only the LABEL part — the known play_base (== StartTimeTicks) —
// leaving the true, sub-second A/V content residual for the drift bias to
// converge.  We must NOT subtract the raw video-minus-audio gap (that would zero
// the real residual too and freeze the desync).  A per-stream latch decides
// absolute-vs-relative once, from the first frame, using the audio clock as the
// always-relative reference: if the first video PTS sits > 3s from the audio
// clock, the stream is absolute -> subtract play_base; else relative -> 0.
// Reset per stream via avsync_reset() (seek / track-change reopen, session
// start).  Normal (non-subtitle) playback keeps gap<3s so nothing is folded.
static s64  s_avsync_vbase_us  = 0;      // label offset folded out of video pts
static bool s_avsync_vbase_set = false;  // latched for the current stream?
static const s64 AVSYNC_ABS_THRESH_US = 3000000;   // >3s from audio clk => absolute video pts

s64 avsync_compute_diff(u64 video_pts_us, u64 play_base_us) {
    if (video_pts_us == 0) return 0;
    u64 audio_clk = audio_get_clock_us();
    if (audio_clk == 0) return 0;
    if (!s_avsync_vbase_set) {
        s64 gap = (s64)video_pts_us - (s64)audio_clk;
        s_avsync_vbase_us  = (gap > AVSYNC_ABS_THRESH_US || gap < -AVSYNC_ABS_THRESH_US)
                             ? (s64)play_base_us : 0;
        s_avsync_vbase_set = true;
        if (s_avsync_vbase_us != 0) {
            char b[112];
            snprintf(b, sizeof(b),
                "avsync_vbase: absolute video pts, folding out label=%llds "
                "(residual A/V content offset stays)",
                (long long)(s_avsync_vbase_us / 1000000));
            plog(b);
        }
    }
    if (s_avsync_vbase_us != 0)
        video_pts_us = (video_pts_us > (u64)s_avsync_vbase_us)
                       ? video_pts_us - (u64)s_avsync_vbase_us : 0;
    s64 raw_diff = (s64)video_pts_us - (s64)audio_clk;
    if (!s_avsync_initialized) {
        s_avsync_smooth_us   = raw_diff;
        s_avsync_initialized = true;
    } else {
        s_avsync_smooth_us = (s_avsync_smooth_us * 9 + raw_diff) / 10;
    }
    return raw_diff;
}

s64 avsync_get_smoothed_diff(void) {
    return s_avsync_smooth_us;
}

bool avsync_is_locked(void) {
    if (!s_avsync_initialized) return false;
    s64 abs_diff = s_avsync_smooth_us < 0 ? -s_avsync_smooth_us : s_avsync_smooth_us;
    return abs_diff < 41667;
}

s64 avsync_biased_period(s64 nominal_vblank_us) {
    // Video ahead (smooth > 0) → consume LESS per vblank → slow video down.
    // Video behind (smooth < 0) → consume MORE per vblank → speed video up.
    // Beyond a ±30 ms dead band (see below), quadratic bias:
    // delta = sign(smooth) * min(((|smooth|-30000)/1000)^2, 5000) us;
    // caps at ±5000µs around ±101ms.
    // Engage the correction for sub-second offsets too, not only once "locked"
    // (<41ms).  A subtitle burn-in reopen leaves the video/audio content ~0.2s
    // apart; with the label folded out (see avsync_compute_diff) that residual
    // is well under a second, and letting the (capped ±5ms/vblank) bias act on
    // it converges A/V in ~1s instead of leaving a permanent lip-sync gap.  A
    // gap larger than this is a mislatch, not real drift — leave it to lock.
    if (!s_avsync_initialized) return nominal_vblank_us;
    { s64 s = s_avsync_smooth_us; if (s > 1500000 || s < -1500000) return nominal_vblank_us; }
    s64 smooth   = avsync_get_smoothed_diff();
    // Dead band.  The measured offset is the FRONT frame's PTS against the audio
    // clock, so even a perfect sync reads ~half a frame (10-20 ms) behind, and
    // the old always-on quadratic term answered that with +50..+400 us on every
    // vblank, forever: video 1% fast, so a 24 fps frame (exactly 2.5 vblanks)
    // slid across the vblank grid, the blend weights swept 0..1 about once a
    // second and the 3-2 hold broke into 3-3 / 2-2 each time it wrapped -- a
    // hitch every ~1.5 s on a clean stream.  Inside +-30 ms the period is the
    // nominal one and the cadence stays rigid.
    s64 abs_us   = smooth < 0 ? -smooth : smooth;
    if (abs_us <= 30000) return nominal_vblank_us;
    s64 abs_ms   = (abs_us - 30000) / 1000;
    s64 delta_us = abs_ms * abs_ms;
    if (delta_us > 5000) delta_us = 5000;
    return nominal_vblank_us - (smooth > 0 ? delta_us : -delta_us);
}

void avsync_reset(void) {
    s_avsync_smooth_us   = 0;
    s_avsync_initialized = false;
    s_avsync_vbase_us  = 0;
    s_avsync_vbase_set = false;
}
