#pragma once
// Helpers shared between the render/ source files only.

#include "ui_visuals.h"

// Blit one cached thumbnail (XMB_THUMB_W x XMB_THUMB_H) into the framebuffer
// at (x, y), requesting it first; falls back to a dim placeholder rectangle
// while the cache fills.  CPU draw — call after rsxSync().
void xmb_blit_thumb(const char *item_id, int x, int y);
