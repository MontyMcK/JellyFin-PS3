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

// Phase 2: vblank-edge gate.  The vblank handler sets a one-shot trigger at the
// exact vsync edge where the Bresenham accumulator fires; this function reads and
// clears it.  Preferred over timing_frame_due_vsync() for the display loop —
// eliminates polling jitter and adds slip detection via "flip_late" log entries.
bool timing_flip_due(void);

// Call immediately after each frame is consumed from the jitter buffer.
// Records the current vsync count and advances the Bresenham accumulator.
void timing_frame_shown(void);

// Unregister the VBlank handler.  Call once at session teardown.
void timing_shutdown(void);

// Audio-video difference in microseconds.  Positive = video PTS is ahead of
// audio PTS.  Returns 0 if either clock is invalid (no PTS yet, empty jbuf).
s64  avsync_compute_diff(u64 video_pts_us);

// Smoothed AV diff via exponential moving average.  Updated by avsync_compute_diff.
s64  avsync_get_smoothed_diff(void);

// True once EMA has been seeded and |smooth_diff| < 41 667 µs (~1 frame at 24 fps).
bool avsync_is_locked(void);
