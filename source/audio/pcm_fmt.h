#pragma once

// The header video/mkv_ts puts before every PES of an uncompressed-audio track, and adec_pcm.cpp reads: what the
// samples are, because a seek can begin a stream anywhere and a PES bigger than a queue slot arrives in pieces.
//
//   byte 0, 1   \x27P\x27 \x27C\x27
//   byte 2      channels (1..8), in the WAVE order (chan_map.h: chan_wave_positions)
//   byte 3      bits per sample: 8 (unsigned), 16, 24, 32 (signed integers)
//   byte 4      flags: PCM_FLAG_FLOAT (32-bit IEEE floats), PCM_FLAG_BIG (samples are big-endian)
//   byte 5..7   sample rate in Hz, big-endian
//
// followed by whole sample frames (a frame is one sample of every channel).  Pure header (no PS3 types) so the
// host tests see it.

#define PCM_HEADER_BYTES 8
#define PCM_FLAG_FLOAT   1
#define PCM_FLAG_BIG     2
