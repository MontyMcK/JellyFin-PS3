#pragma once
#include <ppu-types.h>

typedef enum {
    HUD_ACTION_NONE = 0,
    HUD_ACTION_SEEK,          // hud_seek_delta() gives signed seconds
    HUD_ACTION_TOGGLE_PAUSE,
    HUD_ACTION_AUDIO_TRACK,
    HUD_ACTION_SUBTITLE,
} HudAction;

// total_secs: item runtime in seconds (0 = unknown, hides progress)
// audio_label: current audio track description; NULL or "" defaults to "Audio"
void      hud_init(u32 total_secs, const char *audio_label);
void      hud_shutdown(void);
void      hud_gpu_init(void);
void      hud_gpu_shutdown(void);

// Process input for one frame.  l2/r2 are edge-detected presses from raw padData.
HudAction hud_handle_input(bool l2_pressed, bool r2_pressed, bool paused);

// Signed seek delta in seconds.  Valid only when last action == HUD_ACTION_SEEK.
int       hud_seek_delta(void);

// Draw the overlay onto the current framebuffer.  Must be called after rsxSync().
// elapsed_us: microseconds of playback elapsed (from audio_get_clock_us()).
void      hud_draw(u64 elapsed_us, bool paused);

// True when the overlay is currently visible.
bool      hud_is_visible(void);
