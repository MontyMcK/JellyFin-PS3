#pragma once
// Helpers shared between the render/ source files only.

#include "ui_visuals.h"

// CPU-blit one cached thumbnail scaled to w x h at (x, y); dim placeholder
// while the cache fills.  Call after rsxSync (ui_lists.cpp).
void xmb_cpu_blit_thumb_scaled(const char *item_id, int x, int y, int w, int h);
