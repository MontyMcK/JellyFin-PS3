#pragma once
// Helpers shared between the render/ source files only.

#include "ui_visuals.h"

// CPU-blit one cached thumbnail scaled to w x h at (x, y); dim placeholder
// while the cache fills.  Call after rsxSync (ui_lists.cpp).
void xmb_cpu_blit_thumb_scaled(const char *item_id, int x, int y, int w, int h);

// Draw one card (cached image or placeholder + progress strip + selection
// frame) at (cx,cy) of size card_w x card_h.  Call after rsxSync.
// Shared by the grid (ui_lists.cpp) and the Home shelf (ui_home.cpp).
void xmb_draw_card(const char *item_id, int cx, int cy, int card_w, int card_h,
                   u8 progress_pct, bool selected);
