#pragma once
#include <ppu-types.h>

// GPU card images.
//
// Measured reason this exists (docs/spu-feasibility.md, "the same measurement
// with the library actually loaded"): on Home the XMB spends 6,446 us per
// frame -- 41% of all its work, on a frame that is already 93% full -- doing
// cpu_blit_bitmap() of every visible thumbnail from main memory into RSX
// video memory.  Every frame.  Whether or not anything about the card changed.
//
// The pixels have to reach video memory either way; what is wasteful is doing
// it again for a card that has not moved.  So each cache slot now keeps a VRAM
// mirror, uploaded once when the thumbnail decodes, and the RSX draws it as a
// textured quad.  Per-frame cost drops from a 455 KB memcpy per card to four
// vertices.
//
// The quad is streamed inline with rsxDrawVertexBegin/End rather than fetched
// from a vertex array.  At ~15 cards that is 60 vertices, and the measured
// cost of inline submission is 470 ns/vertex -- about 28 us, which is noise
// against the 6.4 ms it replaces.  Vertex arrays only start to matter at the
// particle counts in docs/wave-spec.md, and this path deliberately avoids the
// stale-binding hazard that retired HUD_DIM_GPU_ARRAY.
//
// ORDERING: these are RSX commands, so they must be submitted BEFORE the
// frame's rsxSync().  Anything the CPU writes afterwards (selection borders,
// progress strips, labels) lands on top, which is the z-order the CPU-only
// path already had.

// Upload the fragment program.  Call once, from ui_init(), after RSX is up.
void ui_card_gpu_init(void);

// True once init succeeded.  False means every caller keeps its CPU blit, so
// a failed upload costs appearance nothing.
bool ui_card_gpu_ready(void);

// Draw one card image at (x, y), size w x h, from a slot's VRAM mirror.
// Coordinates are in framebuffer pixels; the conversion to normalised device
// coordinates happens here so callers keep working in the same space the CPU
// path used.
// tex_pitch is BYTES per row (64-byte aligned; see thumb_gpu_texture).
void ui_card_gpu_draw(u32 tex_off, u32 tex_w, u32 tex_h, u32 tex_pitch,
                      int x, int y, int w, int h);

// Clip GPU card draws to a horizontal band, in framebuffer pixels.
//
// The Home shelves scroll by pixels, so a row leaving the band is drawn partly
// outside it; the CPU path handles that with g_cpu_clip_top/bot in
// cpu_row_clipped().  An RSX quad ignores those entirely and would bleed over
// the tab bar and the hints bar, so the GPU pass needs the hardware scissor to
// mean the same thing.  Pass bot = 0 to reset to the whole surface.
void ui_card_gpu_clip(int top, int bot);

// Restore the blend/program state the rest of the UI expects.  Call once
// after the last ui_card_gpu_draw() of a frame.
void ui_card_gpu_end(void);

// Blended solid-colour quad on the GPU.
//
// The card selection border is four 1 px lines drawn with drawRectBlend(),
// which is the VRAM read-modify-write path -- ~1,200 pixels per selected card
// at the measured ~700 ns each.  The RSX blends for free, and the wave's
// colour-passthrough programs already do exactly this, so the border costs
// four quads instead of 0.8 ms of uncached reads.
//
// colour is 0x00RRGGBB; alpha 255 is opaque.  Call between the card draws and
// ui_card_gpu_end(), i.e. still inside the frame's GPU phase.
void ui_rect_gpu_draw(int x, int y, int w, int h, u32 colour, u8 alpha);

// The full selection treatment for a card at (cx,cy,w,h): the same geometry
// xmb_draw_card draws on the CPU, so the two are interchangeable.
void ui_card_gpu_selection(int cx, int cy, int w, int h);
