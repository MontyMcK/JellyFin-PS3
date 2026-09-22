#pragma once
#include <ppu-types.h>

void wave_init(void);
void wave_draw(void);
void wave_reset(void);

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

// Blended full-screen black quad at the given alpha, drawn on the GPU and
// fenced with rsxSync() so CPU pixel writes may follow immediately.  Used to
// dim the finished frame under a modal.
void wave_dim_screen(u8 alpha);
