#pragma once
// Helpers shared between the render/ source files only.

#include "ui_visuals.h"
#include "thumbnail_cache.h"

// CPU-blit one cached thumbnail scaled to w x h at (x, y); dim placeholder
// while the cache fills.  Call after rsxSync (ui_lists.cpp).
void xmb_cpu_blit_thumb_scaled(const char *item_id, int x, int y, int w, int h);

// Draw one card (cached image or placeholder + progress strip + selection
// frame) at (cx,cy) of size card_w x card_h.  Call after rsxSync.
// Shared by the grid (ui_lists.cpp) and the Home shelf (ui_home.cpp).
// tile_name != NULL swaps the loading placeholder for a colored letter
// tile (music cards, per the album-grid design).
// img selects which Jellyfin image to draw — landscape cards pass
// THUMB_IMG_THUMB for items that have wide art (see ui_home.cpp).
// GPU phase (BEFORE rsxSync) counterparts to the CPU card draws.  See
// ui_card_gpu.h: these submit RSX textured quads for card images whose VRAM
// mirror is ready, and the matching CPU call then skips its blit.
bool xmb_card_gpu_one(const char *item_id, int cx, int cy,
                      int card_w, int card_h, ThumbImg img = THUMB_IMG_PRIMARY);
// The card focus ring, drawn after the cards so a gliding ring is never
// painted over by a neighbour (ui_tab_anim.h's focus_glide).  The CPU one is
// only for when the GPU card path is off; the GPU one is ui_card_gpu_selection.
void xmb_focus_ring_cpu(int cx, int cy, int w, int h);
// Which list the d-pad is moving in -- tab, drill depth, music sub-tab -- so
// the ring snaps rather than glides when that changes.
int  xmb_focus_ctx(void);
void xmb_draw_card(const char *item_id, int cx, int cy, int card_w, int card_h,
                   u8 progress_pct, bool selected,
                   const char *tile_name = NULL,
                   ThumbImg img = THUMB_IMG_PRIMARY);
