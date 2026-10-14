#pragma once
#include <ppu-types.h>

// FLAC decode path (audio/flac_dec.c) -- another codec of adec.cpp, chosen from the stream type 0xE1 that
// video/mkv_ts writes for a Matroska FLAC track (no real transport stream carries FLAC, so the type is
// this player's own).  The first PES of a stream starts with "fLaC" and the STREAMINFO block (mkv_ts
// builds it from the track's CodecPrivate), then every PES is one FLAC frame; a frame bigger than a
// queue slot arrives in pieces, and the decoder waits for the rest.  The output goes through adec_out
// (channel order, 48 kHz).

// Opens for a ring out_channels wide (6 or 2).
bool adec_flac_open(int out_channels);
void adec_flac_close(void);
// Seek / flush: drop the carry and expect the stream header again (a new stream always begins with it).
void adec_flac_reset(void);
// One PES payload, or a continuation chunk of one.  Runs on the adec thread only.
void adec_flac_decode_payload(const u8 *es, int len);
