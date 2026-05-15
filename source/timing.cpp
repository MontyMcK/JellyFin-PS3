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

static u64 s_interval_us   = 33333;
static u64 s_last_shown_us = 0;

void timing_init(u32 fps_num, u32 fps_den) {
    u64 f = sysGetTimebaseFrequency();
    if (f >= 1000000ULL && f <= 1000000000ULL) s_tb_freq = f;
    s_interval_us   = (u64)1000000 * fps_den / fps_num;
    s_last_shown_us = 0;
}

bool timing_frame_due(void) {
    if (s_last_shown_us == 0) return true;
    return timing_get_us() - s_last_shown_us >= s_interval_us;
}

void timing_frame_shown(void) {
    s_last_shown_us = timing_get_us();
}
