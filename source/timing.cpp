#include "timing.h"
#include "plog.h"
#include <ppu-asm.h>
#include <sys/systime.h>
#include <rsx/gcm_sys.h>

// Direct timebase register read — single instruction, no LV2 syscall.
// Cell PPU mftb fills a 64-bit GPR on a 64-bit core, so no upper/lower split needed.
static u64 s_tb_freq = 79800000ULL;  // overridden in timing_init

static inline u64 read_tb(void) { return __gettime(); }

u64 timing_get_us(void) {
    return read_tb() * 1000000ULL / s_tb_freq;
}

static u64 s_interval_us   = 33333;
static u64 s_last_shown_us = 0;

// Vsync-counted scheduling state
static u32 s_fps_num              = 30;
static u32 s_fps_den              = 1;
static volatile u64 s_vsync_count = 0;   // incremented by VBlank handler, read by display loop
static u64 s_last_shown_vsync     = 0;
static s64 s_vsync_err            = 0;   // Bresenham accumulator, units of fps_num
static u32 s_vsyncs_next          = 2;   // precomputed vsync hold for the next frame
static bool s_vsync_shown_once    = false;

// VBlank handler — called by RSX at 60Hz regardless of display loop speed.
// A PPC 64-bit store to an aligned address is a single instruction so the
// write is always coherent; the increment itself can race only against itself
// (there is one writer), which is acceptable for frame scheduling.
static void s_vblank_handler(const u32 head) {
    (void)head;
    s_vsync_count++;
}

void timing_register_vblank(void) {
    gcmSetVBlankHandler(s_vblank_handler);
    plog("timing: vsync handler registered");
}

void timing_init(u32 fps_num, u32 fps_den) {
    u64 f = sysGetTimebaseFrequency();
    if (f >= 1000000ULL && f <= 1000000000ULL) s_tb_freq = f;
    s_interval_us      = (u64)1000000 * fps_den / fps_num;
    s_last_shown_us    = 0;
    s_fps_num          = fps_num;
    s_fps_den          = fps_den;
    s_vsync_count      = 0;
    s_last_shown_vsync = 0;
    s_vsync_err        = 0;
    s_vsyncs_next      = 2;
    s_vsync_shown_once = false;
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

void timing_frame_shown(void) {
    s_last_shown_us    = timing_get_us();
    s_last_shown_vsync = s_vsync_count;
    s_vsync_shown_once = true;
    // Advance Bresenham accumulator to compute hold length for the next frame.
    s_vsync_err   += (s64)(60 * s_fps_den);
    s_vsyncs_next  = (u32)(s_vsync_err / (s64)s_fps_num);
    s_vsync_err   -= (s64)s_vsyncs_next * (s64)s_fps_num;
}
