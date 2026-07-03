#pragma once

#include <ppu-types.h>

void rsx_draw_frame(bool render_blend, float blend_factor, u32 fw, u32 fh);

// Draw the HUD overlay texture as one full-screen alpha-blended quad,
// vertices submitted inline (no vertex-array fetch — same freeze-proof
// technique as the hud_dim quad).  Must be queued after rsx_draw_frame()
// in the same frame so shaders/viewport are current.
void rsx_draw_hud_overlay(u32 tex_off, u32 tw, u32 th);
