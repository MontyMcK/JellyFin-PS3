#pragma once
#include <ppu-types.h>

// AAC decode path — libfaad (portlibs).  Lives alongside the minimp3 / liba52 / libdca / TrueHD paths
// in adec.cpp, which selects between them at runtime from the PMT stream type (adec_set_codec) and
// owns the shared PES queue, PCM ring and PTS clock.  This module only turns ADTS frames into PS3-ordered
// float frames at 48 kHz (resampling 44.1 kHz and the like) and hands them to adec_push_frames().
//
// A file on a drive keeps the rate it was made at; resample.c does the conversion.

// Opens the decoder for a ring out_channels wide (6: a 5.1 program, FL FR FC LFE SL SR; 2: FL FR, a
// multichannel stream folded down).  False if libfaad could not be opened.
bool adec_aac_open(int out_channels);

void adec_aac_close(void);

// Seek / flush: drop the partial-frame carry, the decoder's overlap and the resampler's history.
void adec_aac_reset(void);

// Decodes one PES payload (PES header already stripped by adec.cpp).  ADTS frames may straddle PES
// boundaries, so a partial tail is carried to the next call.  Runs on the adec thread only.
void adec_aac_decode_payload(const u8 *es, int len);
