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

// Nominal per-vblank duration in microseconds for the DETECTED display refresh
// (1e6 * display_den / display_num): 59.94Hz->16683, 50Hz->20000, 60Hz->16666,
// 30Hz->33333.  The duration-consumption gate must drain exactly this much
// content per real vblank; using a fixed 16683 assumes a 60Hz-family display and
// mis-paces playback on a 50Hz (PAL) one.  Valid after timing_init().
s64  timing_vblank_period_us(void);

// Real vblanks elapsed since the previous call, clamped to [1, 4].
//
// The duration-consumption gate must drain one vblank of content per REAL
// vblank, not per loop iteration.  Those are the same thing only while the
// display loop keeps up: flips are vsync-locked with two framebuffers, so a
// loop body that overruns its refresh by any margin lands on the NEXT one and
// an entire vblank passes with no display step.  Consuming a single period per
// call then silently loses that time, video falls permanently behind real time,
// the jitter buffer saturates, and the decode thread stops reading the muxed
// socket -- which starves audio (see decode_thread_fn).  This slips at ANY
// content rate, not just at the refresh rate: a host simulation of this gate
// slips 3.4s per 70s and saturates the 24-slot jbuf even at 29.97fps once
// roughly one step in twenty overruns.  What makes a 59.94fps bob-deinterlaced
// transcode on a 59.94Hz display so much worse is that the retire ceiling is
// one frame per display step, so demand sits exactly ON the ceiling and every
// iteration is a candidate to overrun.  Returning the true elapsed count lets
// the gate retire the backlog and hold real time by dropping frames instead;
// with it, the same simulation holds zero slip and never reaches the cap.
//
// Clamped at 4 so a long stall (seek, unpause, HDD hitch) cannot dump the whole
// jitter buffer in one step.
u32  timing_vsyncs_elapsed(void);

// Forget the elapsed-vblank cursor: the next timing_vsyncs_elapsed() returns 1.
// Call when the display loop deliberately skips display steps (paused idle) so
// the backlog drain does not count that gap as frames owed.
void timing_gate_reset(void);

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
s64  avsync_compute_diff(u64 video_pts_us, u64 play_base_us);

// Smoothed AV diff via exponential moving average.  Updated by avsync_compute_diff.
s64  avsync_get_smoothed_diff(void);

// True once EMA has been seeded and |smooth_diff| < 41 667 µs (~1 frame at 24 fps).
bool avsync_is_locked(void);

// Returns the per-vblank duration to consume in the gate, biased
// by the smoothed AV diff. Pass the nominal vblank period; the
// returned value will be within ±5000us of it.
s64 avsync_biased_period(s64 nominal_vblank_us);

// Reset AV sync EMA state.  Call on seek so post-seek drift is measured fresh.
void avsync_reset(void);
