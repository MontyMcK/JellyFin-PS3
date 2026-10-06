#pragma once
#include <stdbool.h>
#include "picture_size.h"

// Picture size (picture_size.h): the saved width/height percentages and the
// L1 adjust mode that edits them while a video plays.  The sizes persist
// across videos and restarts (jellyfin_picsize.txt) -- they correct for the
// screen, not for the film.

void    picture_load(void);              // once at startup; missing file = 100 x 100
PicSize picture_get(void);               // what the video quad should use

// One call per player-loop frame.  L1 enters the mode; inside it the d-pad
// resizes (Left/Right = width, Up/Down = height, held = repeat), Triangle
// resets, L1 / X / O leave.  Returns true while the mode owns the input, in
// which case the HUD must not see this frame's buttons.
bool    picture_mode_input(void);

// Playback is over: leave the mode if it was open and save what changed.
void    picture_session_end(void);
