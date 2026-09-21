#pragma once
#include <ppu-types.h>

// TrueHD (Dolby) decode path — vendored FFmpeg MLP decoder (source/audio/mlp/,
// see PROVENANCE.md there).  Lives alongside the minimp3, liba52 and libdca
// paths in adec.cpp, which selects between them at runtime from the PMT
// stream type (adec_set_codec) and owns the shared PES queue, PCM ring and
// PTS clock.  This module only turns ES bytes into PS3-ordered float frames
// and hands them to adec_push_frames().
//
// SCOPE — this is the Dolby counterpart of the DTS path, and it reaches
// further than that one does.  TrueHD decoding here is **lossless**: what
// comes out is bit-for-bit the studio master's 5.1 or 7.1 bed (verified
// against FFmpeg's decoder in tests/test_truehd_decode.c), not a lossy core.
// A **Dolby Atmos** track is a TrueHD track plus object metadata in an
// extension; the objects are not rendered (no free renderer exists, and the
// PS3 has no way to be told where the speakers are), so an Atmos track plays
// as its lossless 7.1 bed.  Dolby Digital Plus (E-AC-3) is a different codec
// and is NOT handled here — those sources still take the AC-3 transcode path.
//
// The one cost: a TrueHD track is stream-copied at its original bitrate,
// which on a Blu-ray remux can be several Mbps on top of the video.

// Allocate the decoder and set the requested output width: 8 → a 7.1 program
// when the stream has one, 6 → fold to 5.1, 2 → stereo downmix.  Returns
// false if the decoder could not be initialised.
bool adec_truehd_open(int out_channels);

// Free the decoder.  Safe to call when not open.
void adec_truehd_close(void);

// Seek/flush: drop the partial access unit and require a new major sync.
void adec_truehd_reset(void);

// Decode one PES payload (PES header already stripped by adec.cpp).  Access
// units may straddle PES boundaries, so a partial tail is carried over to the
// next call.  Runs on the adec thread only.
void adec_truehd_decode_payload(const u8 *es, int len);

// True when a lot of bytes have gone in and NOT ONE access unit has decoded —
// a copied track this decoder cannot make sense of.  The player uses it the
// same way it uses adec_dts_no_core(): drop back to the AC-3 transcode for
// the rest of the session rather than play silence.
bool adec_truehd_no_audio(void);

// Program channels the decoder is currently producing: 8 for a 7.1 stream,
// 6 for 5.1, 2 for stereo — 0 before the first access unit decodes.  The
// stats overlay reports it, and it is what makes a 7.1 track visible.
int  adec_truehd_program_channels(void);
