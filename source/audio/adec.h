#pragma once
#include <ppu-types.h>

// Initialize minimp3 state and PCM ring buffer.  Call once before playback.
void adec_init(void);

// Spawn the dedicated audio decode thread.  Call after adec_init().
void adec_start(void);

// Signal the audio decode thread to stop and join it.  Call before audio_close().
void adec_stop(void);

// Drain PES queue and PCM ring without stopping the decode thread.  Call on seek.
void adec_flush(void);

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

// PTS (stream microseconds) of the next sample adec_read_pcm() would return.
// Returns 0 until the first PES with a PTS has been decoded.
u64  adec_get_read_pts_us(void);
