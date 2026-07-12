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

// Blended full-screen black quad at the given alpha, drawn on the GPU and
// fenced with rsxSync() so CPU pixel writes may follow immediately.  Used to
// dim the finished frame under a modal.
void wave_dim_screen(u8 alpha);
