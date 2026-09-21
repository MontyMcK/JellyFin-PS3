#pragma once
#include <ppu-types.h>

extern bool s_audio_ok;

// channels selects the CellAudio port width: 2 (stereo, the shipped path) or
// 8 (5.1 surround carried in an 8-channel port — PSL1GHT has no 6-channel
// port).  An 8-channel open that fails falls back to 2 internally; check
// audio_output_channels() for what was actually opened.
void audio_open(int channels);
bool audio_write_pcm(void);  // returns true if a DMA event was consumed
void audio_close(void);

// Widest program the port that is actually open can carry: 2 (stereo) or 8.
// A 5.1 program uses six of those eight slots and the output stage zeroes the
// two rear ones every block; a TrueHD 7.1 program uses all eight.  The
// decoder asks this to decide how wide to make the PCM ring, so it reports
// capacity, not what is currently playing (adec_output_channels() is that).
int  audio_output_channels(void);

// Pluggable PCM source for audio_write_pcm().  Defaults to the video
// pipeline's decoder (adec_pcm_available / adec_read_pcm / adec_output_channels);
// the music player swaps in its own ring while it owns the port.  avail
// returns the number of PCM FRAMES ready; read fills buf with n_frames
// interleaved frames of `channels()` float32 samples each and returns the
// frame count actually written; channels reports the source's interleave
// width (2 or 6 — for 6 the order is the PS3 one: FL FR FC LFE SL SR).
// Pass NULL/NULL/NULL to restore the adec defaults.
typedef int (*audio_avail_fn)(void);
typedef int (*audio_read_fn)(float *buf, int n_frames);
typedef int (*audio_channels_fn)(void);
void audio_set_source(audio_avail_fn avail, audio_read_fn read,
                      audio_channels_fn channels);

// Total audio DMA blocks consumed since audio_open().
// Each block = 256 sample FRAMES regardless of channel count;
// at 48 kHz that is 5.333 ms per block.
u64  audio_block_count(void);

// Microseconds of audio played since audio_open().
// Uses PTS-based clock once the audio decoder has seen a valid PTS;
// falls back to DMA block count at startup before the first PTS arrives.
u64  audio_get_clock_us(void);

// True once PTS tracking has started (first decoded PES with a valid PTS).
bool audio_clock_valid(void);

// Master output volume, 0..100 % (100 = unity, bit-exact).  Applied as a linear
// gain to every PCM block.  audio_set_volume() clamps and persists the value;
// audio_volume_load() restores it at startup.  Driven by the player HUD's
// volume slider (d-pad up/down on the speaker control).
int  audio_get_volume(void);
void audio_set_volume(int pct);
void audio_volume_load(void);
