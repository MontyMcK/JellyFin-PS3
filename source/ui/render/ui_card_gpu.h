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
// As ui_card_gpu_draw, but for a texture drawn at a different size (the spine
// column shows a grid-sized thumbnail larger than the grid does).  Filters
// LINEAR when the size differs, NEAREST when it does not.
void ui_card_gpu_draw_scaled(u32 tex_off, u32 tex_w, u32 tex_h, u32 tex_pitch,
                             int x, int y, int w, int h);
void ui_card_gpu_clip(int top, int bot);

// The general form: texture rows v_top..v_bot (0..1; pass them reversed for a
// mirror image) drawn at (x, y, w, h) at a constant opacity, alpha 255 being
// opaque.  Linear filtering unless it is a plain 1:1 draw.  Opacity uses the
// RSX's constant blend colour, so the image's own alpha channel is ignored,
// as it is for opaque cards.  The Home queue's receding items and their
// reflections use this.
void ui_card_gpu_draw_ex(u32 tex_off, u32 tex_w, u32 tex_h, u32 tex_pitch,
                         int x, int y, int w, int h,
                         float v_top, float v_bot, u8 alpha);

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

// Persistent VRAM textures for art too big for the thumbnail cache (the
// detail page's backdrop and poster).  upload copies a main-memory Bitmap in
// once; draw scales it (LINEAR) every frame in a GPU phase.  Slot 0 backdrop,
// slot 1 poster.  upload returns false when the RSX path is not up.
#include "bitmap.h"
#define GPU_TEX_BACKDROP 0
#define GPU_TEX_POSTER   1
bool ui_gpu_tex_upload(int slot, const Bitmap *bm);
void ui_gpu_tex_clear(int slot);
bool ui_gpu_tex_draw(int slot, int x, int y, int w, int h);
// The same at a constant opacity (255 = ui_gpu_tex_draw).  Item detail
// fades its backdrop in as the spine arrives at L3.
bool ui_gpu_tex_draw_a(int slot, int x, int y, int w, int h, u8 alpha);

// Which item's art a slot holds, so a screen other than detail (the player's
// buffering screen) can reuse it without guessing.  Set by whoever uploads;
// cleared by ui_gpu_tex_clear().  "" = unknown / empty.
void        ui_gpu_tex_set_tag(int slot, const char *item_id);
const char *ui_gpu_tex_tag(int slot);
// The slot's texture drawn filling (x, y, w, h) with the rows v_top..v_bot
// only -- a portrait poster cropped to a wide band.  Opaque (no constant
// blend colour), LINEAR.
bool ui_gpu_tex_draw_crop(int slot, int x, int y, int w, int h,
                          float v_top, float v_bot);
