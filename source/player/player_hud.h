#pragma once
#include <ppu-types.h>

typedef enum {
    HUD_ACTION_NONE = 0,
    HUD_ACTION_SEEK,          // hud_seek_delta() gives signed seconds
    HUD_ACTION_TOGGLE_PAUSE,
    HUD_ACTION_AUDIO_TRACK,
    HUD_ACTION_SUBTITLE,
    HUD_ACTION_MENU_SELECT,   // hud_menu_choice() gives the chosen entry
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

// Update the audio-button label after a track change (e.g. "English - AC3").
void      hud_set_audio_label(const char *label);

// Mark the CC button active (subtitles on) — draws an accent underline.
void      hud_set_cc_active(bool active);

// Item title — shown top-left while playback is paused.
void      hud_set_title(const char *title);

// Open a popup menu above the control bar (e.g. track selection).  The HUD
// copies the item POINTERS only — the strings must outlive the menu.
// current marks the active entry (accent dot); it is also the initial cursor.
// While open, the HUD owns D-pad up/down + X/circle; X returns
// HUD_ACTION_MENU_SELECT from hud_handle_input, circle just closes.
void      hud_open_menu(const char *title, const char *const *items,
                        int n_items, int current);

// Entry chosen by the last HUD_ACTION_MENU_SELECT.
int       hud_menu_choice(void);
