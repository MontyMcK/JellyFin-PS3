#pragma once
#include <ppu-types.h>

void wave_init(void);
void wave_draw(void);

// Call just before wave_draw() on a menu screen: with Settings > Menu
// Particles on, the snow field drifts there too (it fades out on any screen
// that does not ask -- playback, the item details page).  One frame only.
void wave_snow_ambient(void);

// The screen's solid boxes for this frame, in pixels ({x, y, w, h} x n, n <=
// 6): near particles bounce off them, far ones pass behind (wave_snow.h).
// Call before wave_draw(); applies to that frame only.
void wave_snow_obstacles(const int *xywh, int n);

// Draw the NEAR JellyWave ribbon again, clipped to (x, y, w, h) px, over
// whatever was drawn since wave_draw() this frame -- so the ribbon passes in
// front of the album cover where it rises across it.  Same geometry, same
// buffer, same blends as wave_draw()'s own pass; nothing is rebuilt or
// uploaded.  GPU phase only (before the frame's rsxSync), after wave_draw().
void wave_draw_front(int x, int y, int w, int h);

// The music screen's album colour: a gentle per-channel pull of the ribbons
// toward the cover's accent (0x00RRGGBB), kept inside the purple/blue family.
// strength ~0.3 is a hint; 0 (or rgb 0) clears it.
void wave_set_album_tint(unsigned int rgb, float strength);

// A details page's poster colour, the same kind of pull but steady rather
// than breathing: it eases in when set and out when cleared (rgb 0 or
// strength 0).  Render thread.
void wave_set_art_tint(unsigned int rgb, float strength);

// One navigation step, from btn_nav_repeat(): dx -1 left / +1 right, dy -1 up
// / +1 down.  The wave dips and the particles blow the way the menu content
// moves; a held scroll shakes it (wave_nav.h).  Render thread.
void wave_nav_event(int dx, int dy);
void wave_reset(void);

// JellyDrop (wave_drop.h): the three ribbons closed into the Jellyfin bell,
// floating behind everything.  Toggled by L1 + R1 together (ui_input.cpp),
// morphs over ~2.4 s, and the choice survives a relaunch
// (jellyfin_jellydrop.txt).  Render thread.
void wave_drop_toggle(void);
bool wave_drop_on(void);

// The visualiser (2026-09-27): SQUARE cycles JellyWave -> JellyDrop -> Off
// (the background gradient alone) -> JellyWave, everywhere the wave is drawn.
// Default JellyWave; kept in jellyfin_visualiser.txt (0/1/2).  Render thread.
enum { WAVE_VIS_JELLYWAVE = 0, WAVE_VIS_JELLYDROP = 1, WAVE_VIS_OFF = 2 };
int         wave_vis_mode(void);
void        wave_vis_cycle(void);
void        wave_vis_set(int mode);          // WAVE_VIS_*, persisted
const char *wave_vis_next_label(void);   // what Square switches TO: the hint
// Run the Off -> JellyWave entrance (the ribbons sweep in from the left over
// ~1.3 s) now -- the cold boot fires it as its veil starts to lift.  Only
// when the mode is JellyWave.  Render thread.
void        wave_reveal_start(void);

// True when CPU framebuffer writes are cheap (emulator): the whole XMB
// background is composited on the CPU so the flip presents CPU-drawn content.
// False on real hardware, where the background stays on the GPU.  Set once by
// wave_init().  clearScreen() branches on this too.
bool ui_cpu_bg(void);

// True when the wave is running mode 2 -- vertex arrays with GPU alpha
// blending proven live.  Callers use it to choose a blended-quad path over a
// CPU read-modify-write one.
bool wave_gpu_blend_ready(void);

// The tab-bar hairline as a blended quad: six vertices, no VRAM reads.
// MUST be called in the GPU phase, BEFORE the frame's rsxSync() -- issuing it
// after the fence would put it in the FIFO behind CPU pixel writes that have
// already landed, and it would draw over them.  See ui_wave.cpp.
void wave_draw_divider_gpu(int y_px, u8 r, u8 g, u8 b, u8 peak_alpha);

// Focus glow for the XMB category strip.  This is a small blended fan emitted
// in the pre-fence GPU phase, so it never composites through the framebuffer.
void wave_draw_glow_gpu(int cx_px, int cy_px, int radius_px,
                        u8 r, u8 g, u8 b, u8 peak_alpha);

// Bind the wave's passthrough programs + standard alpha blend for immediate-
// mode geometry drawn elsewhere.  No vertex buffers, no fence.  GPU phase.
bool wave_imm_bind(void);

// --- spine panels (ui_wave_panels.cpp) -------------------------------------
// All immediate-mode on the programs above; GPU phase only, before the
// frame's rsxSync().

// Elliptical glow: peak alpha at the centre, falling linearly to zero at
// (rx, ry).  The three-radius form above is the round case.
void wave_draw_glow_gpu(int cx_px, int cy_px, int rx_px, int ry_px,
                        u8 r, u8 g, u8 b, u8 peak_alpha);

// Rounded rectangle (r = h/2 gives a pill), filled with a horizontal colour
// ramp from rgb_left to rgb_right at one alpha.
void wave_draw_rrect_gpu(int x, int y, int w, int h, int r,
                         u32 rgb_left, u32 rgb_right, u8 alpha);
// The same shape with a t-pixel border: the outer shape in the line colour,
// the inset one in the fill.
void wave_draw_rrect_outline_gpu(int x, int y, int w, int h, int r, int t,
                                 u32 line_rgb, u8 line_a,
                                 u32 fill_left, u32 fill_right, u8 fill_a);
// A one-colour alpha ramp over a rectangle -- a CSS linear-gradient of n
// stops at fractions pos[] (0..1 along x, or along y when vertical).
void wave_draw_ramp_gpu(int x, int y, int w, int h, u32 rgb, bool vertical,
                        int n, const float *pos, const u8 *alpha);

// --- experience panels (ui_wave_panels.cpp) --------------------------------
// A thin ring arc of thickness t_px centred on radius r_px: from angle a0 (in
// turns, 0 = 12 o'clock, clockwise) for `arc` turns, one triangle strip of
// `segs` segments.  Colour and alpha run from the TAIL (a0) to the HEAD
// (a0 + arc), so a short bright head over a fading tail reads as a gradient
// ring turning.  arc >= 1 draws the closed circle.
void wave_draw_ring_arc_gpu(int cx, int cy, float r_px, float t_px,
                            float a0, float arc, int segs,
                            u32 rgb_tail, u8 a_tail, u32 rgb_head, u8 a_head);
// The Jellyfin mark (render/jf_logo_geom.h), half_w_px from its centre to the
// arch's side, coloured with a diagonal ramp rgb_a -> rgb_b.  244 vertices.
void wave_draw_jf_logo_gpu(int cx, int cy, float half_w_px,
                           u32 rgb_a, u32 rgb_b, u8 alpha);

// Blended full-screen black quad at the given alpha, drawn on the GPU and
// fenced with rsxSync() so CPU pixel writes may follow immediately.  Used to
// dim the finished frame under a modal.
void wave_dim_screen(u8 alpha);

// Darken the background gradient alone (not the ribbons or particles):
// 0 = as themed, 1 = black.  The music screen's focus mode sets it each
// frame and puts it back to 0 on exit.
void wave_set_bg_dim(float d);
