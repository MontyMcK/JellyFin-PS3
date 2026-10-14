// The spine, drawn and driven.  Geometry lives in spine.h; this is the
// XMB-side glue.  On by default; jellyfin_spine.txt = 0 makes all of it inert.
//
// Two levels today (SPINE-PLAN.md S1 + the base layer of S4):
//
//   base  (L1)  the spine IS the content.  Left/Right walks the tabs, and the
//               active tab's items hang in a column under its icon.  Down or
//               X enters the tab.
//   tab   (L2)  the tab's own screen, as it has always been, under a smaller
//               spine.  Up from its top row, or O at its root, returns to base.
//
// That is the PS3 XMB's shape -- and 1etu/XMP's (MIT), whose navigation model
// this follows: a root row you can always get back to, entered downward.
#ifndef JF_UI_SPINE_H
#define JF_UI_SPINE_H

#include "ui_visuals.h"   // GridGeom

// Read jellyfin_spine.txt into g_spine_on.  Call from main.cpp AFTER
// plog_load_setting(), so which path is live reaches the log.  Starts at base.
void spine_load(void);

// Sample the animated depth for this frame.  Call once, at the top of the
// frame loop; every query below reads that one sample.
void spine_frame_begin(void);

// This frame's depth: 0 = base, 1 = inside a tab.  1 when the gate is off.
float spine_depth(void);

// True while the base layer owns the screen (the gate is on and the depth is
// on the base side of halfway).  The frame loop draws the column instead of
// the tab's content, and routes input to spine_input_base().
bool spine_at_base(void);

// Header divider alpha at the current depth, 0..1 (0 at base, where the
// design draws none).  1 when the gate is off.
float spine_divider_alpha(void);

// Move the spine to a level (SPINE_L1..SPINE_L3) with the usual approach.  The
// item-detail screen sets L3 as it opens and L2 as it closes, so the spine
// rises to y=74 over the detail page and settles back on the way out.
void spine_set_level(int level);

// The level the spine is going to (SPINE_L1..L3); SPINE_L2 with the gate off.
// The detail screen reads it on the way in, to hand the same level back.
int spine_target_level(void);

// Base-layer input.  Returns true to exit the XMB (never, today).
bool spine_input_base(void);

// Inside a tab: if this frame's Up / O means "back to base" for the screen the
// tab is on, start the move and return true -- the caller must then skip the
// tab's own handler for this frame.  False leaves input to the tab.
bool spine_try_back(int tab);

// Glow band + bloom behind the active slot.  Blended fans: GPU phase only,
// before the frame's rsxSync().
void spine_draw_gpu(void);

// Icons, active label and underline.  Text phase, where the tab strip went.
void spine_draw(void);

// Where the active tab's column hangs this frame: its centre x (following
// the row as it glides) and its nearness to the focus (1 on it, 0 a slot
// away).  The depth engine's stages (Home, the libraries) hang from it.
void spine_column_anchor(int tab, int *cx, float *near);

// Each category remembers its focus (render/depth.h depth_focus_mem).
// xmb_switch_tab() calls leave() for the tab it leaves -- window_dropped when
// that tab's paged window or filter is being thrown away, which makes the old
// position meaningless -- and enter() once the new tab is current.  Grid tabs
// only; no-ops with the gate off.
void spine_focus_leave(int tab, bool window_dropped);
void spine_focus_enter(int tab);

// Forget the frame clock, so the next spine_frame_begin() steps by 0.  For a
// screen that blocked (item detail's first fetch) and wants its move to start
// visibly on its first frame rather than land in one step.
void spine_clock_reset(void);

// --- motion shared with the screens under the spine ---------------------------
//
// Everything below is INERT with the gate off: values snap to their targets,
// so every screen draws exactly as it did before the spine existed.

// A number that changes once per frame (spine_frame_begin), and the real time
// since the previous frame.  Screens that ease their own values step them once
// per frame id, by this dt, with spine.h's approach.
unsigned           spine_frame_id(void);
unsigned long long spine_frame_dt_us(void);

// Pixels the tab's content sits below its rest position while a level move is
// in flight (it glides up into place as a tab is entered).  0 at rest, 0 with
// the gate off, and 0 whenever the depth is on the base side, so screens drawn
// outside the XMB (login) never see it.  Also declared in ui_visuals.h, where
// XMB_CONTENT_Y uses it.
int spine_content_dy(void);

// The card focus ring, gliding.  Draws the ring where it currently IS, easing
// toward (x, y, w, h).  It snaps instead when the screen changed (tab, sub-
// screen) or no ring was drawn last frame.  GPU phase only.
void spine_focus_ring_gpu(int x, int y, int w, int h);

// Grid scroll easing.  Given the grid's logical `scroll` (first visible
// index), returns the window to DRAW: rows relative to scroll's row (row0 may
// be -1), how many rows (one more than rest while moving), and the pixel
// offset for every cell.  At rest: 0, XMB_GRID_ROWS, 0.
void spine_grid_motion(const GridGeom *gg, int scroll,
                       int *row0, int *rows, int *dy);

#endif
