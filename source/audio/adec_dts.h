#pragma once
#include <ppu-types.h>

// DTS decode path — vendored libdca (source/audio/dca/, see PROVENANCE.md
// there).  Lives alongside the minimp3 and liba52 paths in adec.cpp, which
// selects between them at runtime from the PMT stream type (adec_set_codec)
// and owns the shared PES queue, PCM ring and PTS clock.  This module only
// turns ES bytes into PS3-ordered float frames and hands them to
// adec_push_frames().
//
// SCOPE — what "DTS-HD / DTS:X support" means here.  There is no bitstream
// passthrough on this platform (docs/dts-hd.md §2), so every format must be
// decoded to LPCM on the PPU, and no GPL-compatible decoder exists for the
// DTS-HD extension substreams.  A DTS-HD MA, DTS-HD HRA, DTS-ES or DTS:X
// track therefore plays through its **backward-compatible core substream**
// (up to 5.1, up to 1509 kbps); the extension substreams that carry the
// lossless residual, the extra channels and the DTS:X objects are located,
// counted and skipped here.  That is the whole of the format's reach on a
// PS3, and it is still a step up from the 640 kbps AC-3 transcode the
// surround path otherwise asks the server for.

// Allocate the libdca state and set the requested output configuration:
// out_channels 6 → DCA_3F2R|DCA_LFE (full 5.1), 2 → DCA_STEREO (libdca does
// the downmix).  Returns false if libdca could not be initialised.
bool adec_dts_open(int out_channels);

// Free the libdca state.  Safe to call when not open.
void adec_dts_close(void);

// Seek/flush: drop the partial-frame carry and any pending extension-substream
// skip, and resync.  The libdca state itself carries nothing across core
// frames that a flush must clear.
void adec_dts_reset(void);

// Decode one PES payload (PES header already stripped by adec.cpp).  Core
// frames and extension substreams may straddle PES boundaries, so a partial
// tail is carried over to the next call.  Runs on the adec thread only.
void adec_dts_decode_payload(const u8 *es, int len);

// True once an extension substream (DTS-HD / DTS:X) has been seen in this
// stream — i.e. the track is an HD track being played from its core.
// Drives the "dts-hd" tag in the stats overlay.
bool adec_dts_saw_extension(void);

// True when this stream has delivered a lot of bytes, all of them extension
// substream, and NOT ONE decodable core frame — a coreless DTS-HD MA track
// (rare: Blu-ray always carries a core, but re-encodes need not).  Nothing
// here can decode that, so the player uses this to drop back to the AC-3
// transcode for the rest of the session instead of playing silence
// (player.cpp reopens the stream; surround.h: surround_dts_session_disable).
bool adec_dts_no_core(void);
