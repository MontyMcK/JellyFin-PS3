#pragma once
#include <ppu-types.h>

extern bool s_audio_ok;

void audio_open(void);
bool audio_write_pcm(void);  // returns true if a DMA event was consumed
void audio_close(void);

// Total audio DMA blocks consumed since audio_open().
// Each block = 256 samples at 48 kHz = 5.333 ms.
u64  audio_block_count(void);

// Microseconds of audio played since audio_open() (derived from DMA block count).
u64  audio_get_clock_us(void);
