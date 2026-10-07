#pragma once
// Cold-boot animation -- the renderer and its integration points.
//
// The timeline itself (phases, easing, readiness, every number) is pure data
// in boot_seq.h, host-tested by tests/test_boot_seq.c.  This module turns a
// BootFrame into pixels and gives main.cpp and the XMB loop the few calls
// they need.  docs/boot-animation.md is the long form.
//
// WHO DRAWS WHAT
//
//   Before the XMB runs, main.cpp PUMPS frames between (and during) its init
//   steps: black, the centred mark, its halo, and eventually a status line.
//
//   Once the XMB runs, the XMB draws its own complete frame exactly as it
//   always does and this module adds an OVERLAY on top of it, after the text
//   flush: the black veil lifting off the XMB, and the mark while it is
//   large.  When the mark is small and the veil is gone, the lockup's own
//   code in ui_widgets.cpp draws it, posed -- so the final frames of the
//   animation run through the same draw calls as the static lockup and land
//   on its exact pixels.  There is ONE lockup, not a copy.
//
// OFF SWITCH: "0" in /dev_hdd0/tmp/jellyfin_bootanim.txt restores the old
// "Starting..." splash with no reflash.  Also off on the emulator build,
// whose CPU-composited background is incompatible with GPU overlay quads.

#include "boot_seq.h"

// The lockup's pose for the current XMB frame.  NULL = draw the static
// lockup (every frame after the boot, and every frame if the boot is off).
struct BootLockupPose {
    bool  show_mark;      // false: this frame's mark is drawn by the overlay
    bool  mark_posed;     // true: draw it at mark_x/y/bell, not the lockup's
    int   mark_x, mark_y, mark_bell;
    bool  show_word;
    bool  word_posed;     // true: letters at word_reveal/word_alpha
    float word_reveal[BOOT_WORD_LETTERS];
    float word_alpha[BOOT_WORD_LETTERS];
    float word_slide_px;  // longest slide into place (BOOT_WORD_SLIDE_EM)
};

// ---- main.cpp -------------------------------------------------------------

// Start the animation and put the first (black) frame up.  Call once, after
// the theme, the UI scale and wave_init() -- it reads all three.  Returns
// false if the animation is off, in which case the caller shows its old
// splash and every other call below is a harmless no-op.
bool boot_anim_begin(void);

// True from begin until the animation reaches DONE.
bool boot_anim_active(void);

// Draw one pre-XMB frame, paced to vsync.  Returns false (and does nothing)
// when the animation is not running, so callers waiting in a loop can fall
// back to their own sleep.  Call it between init steps and inside waits.
bool boot_anim_pump(void);

// Run `work` on a worker thread while pumping frames on this one, and return
// when it has finished.  The work must not touch the RSX; while it runs this
// thread only draws.  Falls back to calling it inline (no animation during
// it) if the thread cannot be created, or if the animation is off.
void boot_anim_run(void (*work)(void), const char *what);

// Startup is going somewhere other than the XMB (server URL, login, a fatal
// network error): fade the mark out and return once the screen is black.
void boot_anim_leave(void);

// Force the animation off and release its textures.  Safe to call any time,
// and idempotent -- main.cpp calls it whenever the XMB returns.
void boot_anim_finish(void);

// ---- the XMB loop (ui_xmb.cpp) --------------------------------------------

// Call once per frame after poll_buttons(), before the input handlers.
// Advances the animation and returns true while the XMB should IGNORE this
// frame's input: during the boot every press is taken as "skip to the end"
// rather than passed on to a UI the user can barely see yet.
bool boot_anim_xmb_frame(void);

// Call once per frame after ui_text_gpu_flush(), before flip(): the veil and
// the large mark, on top of everything the XMB drew.
void boot_anim_xmb_overlay(void);

// For xmb_draw_topbar().  See BootLockupPose.
const BootLockupPose *boot_anim_lockup_pose(void);
