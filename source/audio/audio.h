#pragma once
#include <ppu-types.h>

extern bool s_audio_ok;

void audio_open(void);
bool audio_write_pcm(void);  // returns true if a DMA event was consumed
void audio_close(void);

// Pluggable PCM source for audio_write_pcm().  Defaults to the video
// pipeline's decoder (adec_pcm_available / adec_read_pcm); the music player
// swaps in its own ring while it owns the port.  avail returns the number of
// interleaved stereo sample pairs ready; read fills buf with n pairs of
// float32 L/R and returns the count actually written.  Pass NULL/NULL to
// restore the adec default.
typedef int (*audio_avail_fn)(void);
typedef int (*audio_read_fn)(float *buf, int n_pairs);
void audio_set_source(audio_avail_fn avail, audio_read_fn read);

// Total audio DMA blocks consumed since audio_open().
// Each block = 256 samples at 48 kHz = 5.333 ms.
u64  audio_block_count(void);

// Microseconds of audio played since audio_open().
// Uses PTS-based clock once the audio decoder has seen a valid PTS;
// falls back to DMA block count at startup before the first PTS arrives.
u64  audio_get_clock_us(void);

// True once PTS tracking has started (first decoded PES with a valid PTS).
bool audio_clock_valid(void);
