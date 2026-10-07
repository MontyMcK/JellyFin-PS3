#pragma once
#include <ppu-types.h>

// The playback buffering screen -- see render/buffer_anim.h for what it is
// and why it looks the way it does.  Used by show_player() when the spine
// gate is on; with the gate off the player keeps its old status lines.
//
// Everything is drawn from what is already in video memory: the detail page's
// backdrop or poster (ui_gpu_tex slots, reused only when they are tagged with
// THIS item), immediate-mode ramps, glows, the ring and the Jellyfin mark.
// No texture is decoded, nothing is allocated, no framebuffer is read.  One
// rsxSync per frame, for the text.
//
// Render thread only.  Frames pace themselves: a frame waits for its own
// previous flip, never for anything else.

// Start a presentation for this item (id selects the artwork and accent).
void buffering_begin(const char *item_id, const char *title);

// What is happening now.  `label` is the eyebrow ("PREPARING",
// "CONNECTING", "BUFFERING"); `show_pct` shows the percentage under it;
// `circle_hint` labels the O button in the hints bar (NULL = no hint).
void buffering_step(const char *label, bool show_pct, const char *circle_hint);

// Real progress 0..1 (monotonic; a lower value is ignored).
void buffering_progress(float p);

// Draw one frame now.
void buffering_frame(void);

// Draw one frame only if at least min_us have passed since the last.  For
// the pre-roll loop, which polls faster than it needs to redraw.
void buffering_frame_paced(u64 min_us);

// End it: ready = close the ring, pulse, fade; !ready = a quick fade.  Draws
// the outro frames itself and returns when the screen is black and the
// presentation is over, with the last flip still pending (the caller's next
// waitflip() consumes it).
void buffering_finish(bool ready);

// Whether a presentation is running (between begin and finish).
bool buffering_active(void);

// A short LOADING screen for a blocking load: `work(arg)` runs on a thread
// while the render thread draws the wave with the Jellyfin ring turning and
// the label.  Nothing is drawn for the first 120 ms, so a fast load shows no
// flash.  `flip_pending`: whether the caller has a flip queued (waitflip()
// would otherwise block forever).  Returns whether one is pending on exit.
bool loading_run(void (*work)(void *), void *arg, const char *label,
                 bool flip_pending);
