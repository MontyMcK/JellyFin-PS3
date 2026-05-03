#include "timing.h"
#include <ppu-asm.h>
#include <sys/systime.h>

// Direct timebase register read — single instruction, no LV2 syscall.
// Cell PPU mftb fills a 64-bit GPR on a 64-bit core, so no upper/lower split needed.
static u64 s_tb_freq = 79800000ULL;  // overridden in timing_init

static inline u64 read_tb(void) { return __gettime(); }

u64 timing_get_us(void) {
    return read_tb() * 1000000ULL / s_tb_freq;
}

static u64 s_interval_us = 33333;
static u64 s_next_us     = 0;

void timing_init(u32 fps_num, u32 fps_den) {
    u64 f = sysGetTimebaseFrequency();
    if (f >= 1000000ULL && f <= 1000000000ULL) s_tb_freq = f;
    s_interval_us = (u64)fps_den * 1000000ULL / fps_num;
    s_next_us     = timing_get_us() + s_interval_us;
}

bool timing_frame_due(void) {
    return timing_get_us() >= s_next_us;
}

void timing_frame_shown(void) {
    s_next_us += s_interval_us;
    // If the new deadline is already in the past we've stalled by at least
    // one full frame.  Don't attempt catch-up — just reschedule one interval
    // from now so the next frame is shown as soon as possible without
    // causing a burst of back-to-back flips.
    u64 now = timing_get_us();
    if (s_next_us < now)
        s_next_us = now + s_interval_us;
}
