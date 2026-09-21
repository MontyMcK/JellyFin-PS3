#pragma once
#include <ppu-types.h>

// AC-3 (Dolby Digital) decode path — vendored liba52 (source/audio/a52/,
// see PROVENANCE.md there).  Lives alongside the minimp3 path in adec.cpp,
// which selects between them at runtime from the PMT stream type
// (adec_set_codec) and owns the shared PES queue, PCM ring and PTS clock.
// This module only turns ES bytes into PS3-ordered float frames and hands
// them to adec_push_frames().

// Allocate the liba52 state and set the requested output configuration:
// out_channels 6 → A52_3F2R|A52_LFE (full 5.1), 2 → A52_STEREO (liba52 does
// the downmix).  Returns false if liba52 could not be initialised.
bool adec_ac3_open(int out_channels);

// Free the liba52 state.  Safe to call when not open.
void adec_ac3_close(void);

// Seek/flush: drop the partial-frame carry buffer and resync.  The liba52
// state itself carries nothing across syncframes that a flush must clear.
void adec_ac3_reset(void);

// Decode one PES payload (PES header already stripped by adec.cpp).  AC-3
// syncframes may straddle PES boundaries, so a partial tail is carried over
// to the next call.  Runs on the adec thread only.
void adec_ac3_decode_payload(const u8 *es, int len);
