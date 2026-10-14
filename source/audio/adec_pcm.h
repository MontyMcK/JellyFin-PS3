#pragma once
#include <ppu-types.h>

#include "pcm_fmt.h"

// Uncompressed audio from a Matroska file (A_PCM/INT/LIT, A_PCM/INT/BIG, A_PCM/FLOAT/IEEE) -- another
// codec of adec.cpp, chosen from the stream type 0xE2 that video/mkv_ts writes for it (a private type of
// this player's own).  Every PES starts with an 8-byte header that says what the samples are, because a
// seek can begin a stream anywhere and a PES bigger than a queue slot arrives in pieces:
//
//   'P' 'C', channels, bits (8, 16, 24, 32), flags (1 = IEEE float, 2 = big-endian), sample rate (24 bits, big-endian)
//
// then whole sample frames.  Channels are in the WAVE order (chan_map.h's chan_wave_positions); the
// output goes through adec_out (channel order, 48 kHz).

bool adec_pcm_open(int out_channels);
void adec_pcm_close(void);
void adec_pcm_reset(void);
// One chunk of a PES: `head` for the first (it carries the header), false for a continuation.  Runs on the adec thread only.
void adec_pcm_decode_payload(const u8 *es, int len, bool head);
