#pragma once
#include <ppu-types.h>

typedef enum {
    HUD_ACTION_NONE = 0,
    HUD_ACTION_SEEK,          // hud_seek_delta() gives signed seconds
    HUD_ACTION_TOGGLE_PAUSE,
    HUD_ACTION_AUDIO_TRACK,
    HUD_ACTION_SUBTITLE,
    HUD_ACTION_MENU_SELECT,   // hud_menu_choice() gives the chosen entry
    HUD_ACTION_STOP,          // O on the redesigned HUD (spine gate on)
    HUD_ACTION_SKIP_SEGMENT,  // X on the "Skip Intro" badge (segments.h)
    HUD_ACTION_CHANNEL_NEXT,  // Live TV: Up / NEXT -- never seen outside live mode
    HUD_ACTION_CHANNEL_PREV,  //         Down / PREV
} HudAction;

// A skippable segment is on screen.  While it is and the bar is hidden, X
// returns HUD_ACTION_SKIP_SEGMENT instead of revealing the bar.
void      hud_set_skip_offered(bool offered);

// total_secs: item runtime in seconds (0 = unknown, hides progress)
// audio_label: current audio track description; NULL or "" defaults to "Audio"
void      hud_init(u32 total_secs, const char *audio_label);
void      hud_shutdown(void);
void      hud_gpu_init(void);
void      hud_gpu_shutdown(void);

// Allocate (or reuse) the cached display-sized overlay buffers.  Call this
// BEFORE vdec_open/jbuf_alloc on the first session so the cached buffers
// land LOW in the heap: allocated late they sit where the next session's
// jitter buffer needs to go, and that session dies with jbuf_alloc FAILED.
// Idempotent — later calls just clear the buffers.
void      hud_overlay_alloc(void);

// Process input for one frame.  l2/r2 are edge-detected presses from raw padData.
HudAction hud_handle_input(bool l2_pressed, bool r2_pressed, bool paused);

// Signed seek delta in seconds.  Valid only when last action == HUD_ACTION_SEEK.
int       hud_seek_delta(void);

// Draw the overlay onto the current framebuffer.  Must be called after rsxSync().
// elapsed_us: microseconds of playback elapsed (from audio_get_clock_us()).
// scrubbing: true only during an active L2/R2 hold-scrub (player_seek.cpp's
// SEEK_SCRUB state) -- draws the trickplay preview card above the seek bar
// when a tile is available (trickplay.h). Never true for a quick tap, so a
// ±10s skip never shows the card.
void      hud_draw(u64 elapsed_us, bool paused, bool scrubbing);

// True when the overlay is currently visible.
bool      hud_is_visible(void);

// Update the audio-button label after a track change (e.g. "English - AC3").
void      hud_set_audio_label(const char *label);

// Mark the CC button active (subtitles on) — draws an accent underline.
void      hud_set_cc_active(bool active);

// Item title — shown top-left while playback is paused.
void      hud_set_title(const char *title);

// Live TV.  A live HUD has no seek bar and no seek keys: the bar shows how far
// through the current programme it is, Up/Down and NEXT/PREV change channel.
// `times` is "19:30 - 20:30"; permille < 0 = no programme data (no bar fill).
void      hud_set_live(bool live);
void      hud_set_live_programme(const char *title, const char *times, int permille);
// The channel banner: two lines top-left, until `until_us` (timing_get_us()).
// Shows the HUD.
void      hud_set_banner(const char *line1, const char *line2, u64 until_us);

// Put the bar away at once (the auto-hide does it after a few seconds).
void      hud_hide(void);

// The picture-size readout: the banner box alone, with no bar, until
// hud_clear_picture_banner().  It needs the redesigned HUD (spine gate).
void      hud_set_picture_banner(const char *line1, const char *line2);
void      hud_clear_picture_banner(void);

// Open a popup menu above the control bar (e.g. track selection).  The HUD
// copies the item POINTERS only — the strings must outlive the menu.
// current marks the active entry (accent dot); it is also the initial cursor.
// While open, the HUD owns D-pad up/down + X/circle; X returns
// HUD_ACTION_MENU_SELECT from hud_handle_input, circle just closes.
void      hud_open_menu(const char *title, const char *const *items,
                        int n_items, int current);

// Entry chosen by the last HUD_ACTION_MENU_SELECT.
int       hud_menu_choice(void);
