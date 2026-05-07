#pragma once
#include <ppu-types.h>

extern bool s_audio_ok;

void audio_open(void);
void audio_write_pcm(void);
void audio_close(void);

// Total audio DMA blocks consumed since audio_open().
// Each block = 256 samples at 48 kHz = 5.333 ms.
u64  audio_block_count(void);
