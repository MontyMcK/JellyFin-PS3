#pragma once
#include <ppu-types.h>
#include "display_mode.h"

// Write the per-playback 24p decision record to player_log.txt:
//
//   24p: DISPLAY  what the output is doing and what the TV advertises
//   24p: CONTENT  what the stream is and how sure we are
//   24p: DECISION would 24Hz output help, and is it supported
//   24p: RESULT   whether a switch will be attempted (display_24p.cpp does it)
//
// Called once per stream, at the moment the frame rate is settled and
// timing_init() has run.  Read-only: queries the firmware, changes nothing.
void display_diag_session(u32 fps_num, u32 fps_den, dm_fps_source src,
                          u32 width, u32 height);

// The facts from the last display_diag_session() of this playback, if any.
bool display_diag_last(u32 *num, u32 *den, dm_fps_source *src, u32 *w, u32 *h);
// Forget them (called at the start of every playback session).
void display_diag_reset(void);
