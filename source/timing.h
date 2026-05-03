#pragma once
#include <ppu-types.h>

// Returns current monotonic time in microseconds.
u64  timing_get_us(void);

// Call once before the decode loop.  fps_num/fps_den = e.g. 30/1 or 24000/1001.
void timing_init(u32 fps_num, u32 fps_den);

// Non-blocking: returns true when it is time to display the next frame.
bool timing_frame_due(void);

// Call immediately after each flip().  Advances the deadline by one frame interval.
// If we are more than 3 frames behind, resyncs to avoid a catch-up spiral.
void timing_frame_shown(void);
