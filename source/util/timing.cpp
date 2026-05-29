#include "timing.h"
#include "audio.h"
#include "plog.h"
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
    u64 vc = ++s_vsync_count;
    if (s_vsync_shown_once &&
        s_flip_trigger == 0 &&
        vc >= s_last_shown_vsync + (u64)s_vsyncs_next) {
        s_flip_trigger    = 1;
        s_flip_trigger_vc = vc;
    }
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
        videoState vs;
        if (videoGetState(0, 0, &vs) == 0) {
            u16 rr = vs.displayMode.refreshRates;
            if      (rr & VIDEO_REFRESH_59_94HZ) { s_display_num = 60000; s_display_den = 1001; }
            else if (rr & VIDEO_REFRESH_50HZ)    { s_display_num = 50;    s_display_den = 1;    }
            else if (rr & VIDEO_REFRESH_60HZ)    { s_display_num = 60;    s_display_den = 1;    }
            else if (rr & VIDEO_REFRESH_30HZ)    { s_display_num = 30;    s_display_den = 1;    }
        }
        char buf[80];
        snprintf(buf, sizeof(buf), "timing: display=%u/%u fps=%u/%u",
                 s_display_num, s_display_den, fps_num, fps_den);
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

// ---- AV sync EMA ----

static s64  s_avsync_smooth_us    = 0;
static bool s_avsync_initialized  = false;

s64 avsync_compute_diff(u64 video_pts_us) {
    if (video_pts_us == 0) return 0;
    u64 audio_clk = audio_get_clock_us();
    if (audio_clk == 0) return 0;
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
    // Quadratic bias: delta = sign(smooth) * min((|smooth|/1000)^2, 5000).
    // At ±10ms gives ±100µs bias; caps at ±5000µs around ±71ms.
    if (!avsync_is_locked()) return nominal_vblank_us;
    s64 smooth   = avsync_get_smoothed_diff();
    s64 abs_ms   = (smooth < 0 ? -smooth : smooth) / 1000;
    s64 delta_us = abs_ms * abs_ms;
    if (delta_us > 5000) delta_us = 5000;
    return nominal_vblank_us - (smooth > 0 ? delta_us : -delta_us);
}
