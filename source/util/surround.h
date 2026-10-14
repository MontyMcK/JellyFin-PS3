#pragma once
#include <ppu-types.h>

// -------------------------------------------------------------------------
//  Audio Output setting
// -------------------------------------------------------------------------
//  What the HDMI audio output carries.  It gates every audio decision for
//  video -- device profile, stream URL, demux stream selection, decoder
//  choice and audio port width -- and is persisted as a digit in
//  jellyfin_surround.txt.  The menu reads
//
//      Stereo -> 5.1 -> [7.1] -> Dolby Digital -> Stereo
//
//    SURROUND_OFF       Stereo.  The server transcodes audio to MP3; every
//                       playback-path decision falls back to the stereo code.
//                       The default.
//    SURROUND_HD (5.1)  Decode multichannel locally to an 8-channel LPCM port.
//                       The source's own HD track is stream-copied by the
//                       server and decoded here when it has one:
//                         * DTS / DTS-HD MA / DTS:X -> libdca decodes the 5.1
//                           core, up to 1509 kbps (docs/dts-hd.md).
//                         * TrueHD / Dolby Atmos -> the vendored FFmpeg MLP
//                           decoder decodes the lossless 5.1 or 7.1 bed
//                           (docs/dolby-truehd.md).
//                       Any other track is requested as an AC-3 5.1 transcode
//                       at 640 kbps and decoded with liba52.
//    SURROUND_HD_71     The same with a 7.1 output; offered only where the
//                       chain takes 8 channels of LPCM.
//    SURROUND_BITSTREAM Dolby Digital.  The receiver decodes: a Dolby Digital
//                       track is stream-copied by the server and packed as
//                       IEC 61937 bursts, and any other track is transcoded to
//                       AC-3 by the server first.  Nothing is decoded here, so
//                       volume and Dialogue Boost do not apply.  For chains
//                       where multichannel LPCM loses the centre channel (plain
//                       ARC).  See audio_bitstream.cpp.
//
//  The music player ignores this setting: music is stereo by design.
//
//  5.1 needs "Linear PCM 5.1 Ch." ticked in XMB Settings > Sound Settings >
//  Audio Output Settings; without it the PS3 mixes the port down to stereo.
//
//  Persisted digits never change meaning: 1 was the AC-3-only 5.1 mode and
//  reads as 5.1; 4 is Dolby Digital.
//
//  SURROUND_AC3 is retired.  It forced an AC-3 transcode even when the source
//  had an HD track that could be copied untouched, and 5.1 already falls back
//  to that request for a source without one, so the separate choice could
//  only pick the worse path.
//
//  7.1 is shown only when audio_out_lpcm_max_channels() reports 8.  A chain
//  that caps at 6, which is most soundbars, must not be offered a setting it
//  cannot honour, and a 7.1 request there would give up the 5.1 routing
//  request that makes its centre channel work.

typedef enum {
    SURROUND_OFF = 0,       // "Stereo"
    SURROUND_AC3 = 1,       // retired; reads as 5.1
    SURROUND_HD = 2,        // "5.1"
    SURROUND_HD_71 = 3,     // "7.1"
    SURROUND_BITSTREAM = 4, // "Dolby Digital"
} surround_mode_t;

extern int surround_order_count(void);           // 3 or 4, decided by the chain

// Map a persisted value onto one that is still offered on THIS chain.
surround_mode_t surround_sanitize(int v);

void surround_load(void);                    // read the persisted value (once, at startup)
void surround_save(void);                    // persist the current value
surround_mode_t surround_get_mode(void);
void surround_set_mode(surround_mode_t m);   // set + persist immediately
void surround_cycle(void);                   // Stereo -> 5.1 -> [7.1] -> Dolby Digital -> Stereo
void surround_step(int dir);                 // the same walk, either way round (+1 / -1)
const char *surround_mode_label(void);       // "Stereo" / "5.1" / "7.1" / "Dolby Digital"

// True for any non-stereo mode: the one question the audio port, the demux
// and the device profile all ask.
static inline bool surround_enabled(void) { return surround_get_mode() != SURROUND_OFF; }

// True when the source's own HD audio track should be requested (copied) in
// preference to an AC-3 transcode.
// Session-scoped: the player clears it for the rest of the
// session if the copied track turns out to have no decodable core
// (adec_dts_no_core), so playback falls back to AC-3 instead of silence.
bool surround_hd_preferred(void);
void surround_hd_session_disable(void);   // this session only; not persisted
void surround_hd_session_reset(void);     // call when a new playback starts
