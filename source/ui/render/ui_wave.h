#pragma once
#include <ppu-types.h>

void wave_init(void);
void wave_draw(void);
void wave_reset(void);

// Blended full-screen black quad at the given alpha, drawn on the GPU and
// fenced with rsxSync() so CPU pixel writes may follow immediately.  Used to
// dim the finished frame under a modal.
void wave_dim_screen(u8 alpha);
