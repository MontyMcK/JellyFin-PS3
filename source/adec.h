#pragma once
#include <ppu-types.h>

// Initialize minimp3 state and PCM ring buffer.  Call once before playback.
void adec_init(void);

// Feed a complete audio PES packet (PES header included).
// Strips the header, runs mp3dec_decode_frame() on every frame found in the
// payload, and pushes the resulting stereo PCM into the ring buffer.
void adec_push_pes(const u8 *pes, int pes_len);

// Stereo sample pairs currently available in the ring.
int  adec_pcm_available(void);

// Copy up to n_pairs stereo float32 pairs into buf[] (interleaved L/R).
// Returns the number of pairs written — may be less than n_pairs if the
// ring is empty.
int  adec_read_pcm(float *buf, int n_pairs);
