#pragma once
#include <ppu-types.h>

// Returns current monotonic time in microseconds.
u64  timing_get_us(void);

// Register the VBlank callback.  Call once at session init (before any threads start).
// Separate from timing_init() so fps detection can call timing_init() without re-registering.
void timing_register_vblank(void);

// Reset fps parameters and Bresenham accumulator.  Does NOT touch the vblank handler.
// Call once on fps detection (from video.cpp) and once on timeout fallback.
// fps_num/fps_den = e.g. 30/1 or 24000/1001.
void timing_init(u32 fps_num, u32 fps_den);

// Non-blocking: returns true when it is time to display the next frame.
bool timing_frame_due(void);

// No-op — vsync counting is now driven by the gcmSetVBlankHandler callback.
void timing_vsync_tick(void);

// Vsync-counted frame gate using a Bresenham accumulator — no wall-clock dependency.
// Returns true when enough vsyncs have elapsed for the next frame.
bool timing_frame_due_vsync(void);

// Call immediately after each frame is consumed from the jitter buffer.
// Records the current vsync count and advances the Bresenham accumulator.
void timing_frame_shown(void);

// Unregister the VBlank handler.  Call once at session teardown.
void timing_shutdown(void);
