#pragma once
// XMB tab-switch animation.
//
// Two movements, both eased out over TAB_ANIM_US and driven by the clock (so
// they take the same time at any frame rate, emulator or console):
//
//   * the tab wheel turns: the tab bar is a ring with the active tab always
//     at the centre, and a switch rotates it the short way round until the
//     new tab arrives there (ui_widgets.cpp draws it);
//   * the new tab's content rides in WITH the wheel: its offset each frame is
//     a fixed fraction of the new tab's own distance from the centre, so the
//     tab and its content move on the same curve and land together.
//
// The content offset is applied where each tab computes its left edge (grid
// x0, the Home rows' origin, the Settings list, the Search keyboard and
// results), never in the drawing primitives: those take unsigned
// coordinates, and the offset is kept small enough that no content edge
// goes negative.

// The wheel's geometry, shared by the tab bar and the content so the two
// cannot drift apart: the angle between neighbouring tabs, and the pixel
// offset from the screen centre of something `d` ring steps round the wheel.
#define TAB_WHEEL_RAD 0.40f
int   tab_wheel_x(float d);

// Start a turn of `delta` ring steps (signed, the short way round) from ring
// position `from_pos`.  from_pos is the position the wheel is SHOWING now, so
// a second press mid-turn carries on smoothly rather than snapping.
void  tab_anim_start(float from_pos, float delta);

// The ring position to draw this frame; `target` (the active tab's index) once
// the turn has finished.  Not wrapped -- the caller reduces it modulo the ring.
float tab_anim_ring_pos(float target);

// Horizontal offset, in pixels, to add to the active tab's content this
// frame.  0 once the animation has finished.
int   tab_anim_content_dx(void);

// --- d-pad focus glide ------------------------------------------------------
//
// The selection highlight (the focus ring round a card, the Settings row bar)
// eases from where it is drawn to where the selection now is, instead of
// jumping.  Call once per frame with the target rect; it is rewritten with
// the rect to draw.  `ctx` names the list being navigated: when it changes --
// a new tab, a drill-in, a sub-tab -- or the ring was not drawn last frame, or
// the tab wheel is turning, the highlight snaps rather than flying across from
// something unrelated.
void  focus_glide(int ctx, int *x, int *y, int *w, int *h);
