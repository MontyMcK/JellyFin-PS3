#pragma once
#include <ppu-types.h>

// -------------------------------------------------------------------------
//  Surround 5.1 (Alpha) setting
// -------------------------------------------------------------------------
//  Experimental switch for 5.1 surround movie audio: an 8-channel CellAudio
//  port carrying 5.1 LPCM (decoded locally), instead of the shipped stereo
//  MP3 path.  It gates every surround decision — device profile, stream URL,
//  demux stream selection, decoder choice and audio port width — so the whole
//  pipeline can be exercised WITHOUT changing the default behaviour on real
//  hardware.
//
//  Three states, cycled by the Settings row:
//
//    SURROUND_OFF  Stereo MP3.  Every playback-path decision falls back to
//                  the exact stereo code that shipped.  The default.
//    SURROUND_AC3  Ask the server to transcode audio to AC-3 5.1 at 640 kbps
//                  and decode it with liba52.  Works with any source.
//    SURROUND_HD   Prefer the source's OWN HD audio track, stream-copied by
//                  the server (no audio transcode), and decode it here:
//                    * DTS / DTS-HD MA / DTS:X → libdca decodes the 5.1 core,
//                      up to 1509 kbps (docs/dts-hd.md).
//                    * TrueHD / Dolby Atmos → the vendored FFmpeg MLP decoder
//                      decodes the LOSSLESS 5.1 or 7.1 bed
//                      (docs/dolby-truehd.md).
//                  Falls back to the AC-3 request for any other track, so it
//                  is a superset of SURROUND_AC3 and never does worse.
//
//  The music player ignores this setting entirely — music is stereo by design.
//
//  Requires "Linear PCM 5.1 Ch." ticked in XMB Settings > Sound Settings >
//  Audio Output Settings; without it the PS3 mixes the port down to stereo.
//
//  Persisted as "0"/"1"/"2" in the app data dir next to the other settings
//  files.  "1" is what the AC-3-only version of this feature wrote, so an
//  existing settings file keeps meaning exactly what it meant before.

typedef enum {
    SURROUND_OFF = 0,    // "Stereo"
    SURROUND_AC3 = 1,    // RETIRED -- see below
    SURROUND_HD = 2,     // "5.1"
    SURROUND_HD_71 = 3,  // "7.1" -- only offered where the chain takes 8ch LPCM
} surround_mode_t;

// -------------------------------------------------------------------------
//  What the menu offers, and why AC-3 is not in it
// -------------------------------------------------------------------------
//  The row reads Stereo / 5.1 / 7.1, because that is the question a listener
//  actually has. It used to read Off / AC-3 / HD, which mixed up "how many
//  speakers" with "which track the server sends" and left people picking the
//  worse of two paths for no gain.
//
//  SURROUND_AC3 is RETIRED. It forced the server to transcode to AC-3 5.1
//  even when the source had an HD track that could be copied untouched, and
//  SURROUND_HD already falls back to exactly that request when a source has
//  no HD track -- it is a superset and, as this file has said since it was
//  written, "never does worse". The only thing the separate option could do
//  was pick the worse path on purpose. It keeps its digit so saved settings
//  do not shift, and maps to 5.1.
//
//  7.1 is shown only when audio_out_lpcm_max_channels() reports 8. A chain
//  that caps at 6 -- which is most soundbars -- must not be offered a setting
//  it cannot honour, and a 7.1 request there would also give up the 5.1
//  routing fix that makes its centre channel work at all.
extern const surround_mode_t SURROUND_ORDER[];   // NULL-free, see _N
extern int surround_order_count(void);           // 2 or 3, decided by the chain

// Map a persisted value onto one that is still offered on THIS chain.
surround_mode_t surround_sanitize(int v);

void surround_load(void);                    // read the persisted value (once, at startup)
void surround_save(void);                    // persist the current value
surround_mode_t surround_get_mode(void);
void surround_set_mode(surround_mode_t m);   // set + persist immediately
void surround_cycle(void);                   // Stereo -> 5.1 -> [7.1] -> Stereo
const char *surround_mode_label(void);       // "Stereo" / "5.1" / "7.1"

// True for any surround mode: the one question the audio port, the demux and
// the device profile all ask.  Every existing call site keeps its meaning.
static inline bool surround_enabled(void) { return surround_get_mode() != SURROUND_OFF; }

// True when the source's own HD audio track should be requested (copied) in
// preference to an AC-3 transcode.
// Session-scoped: the player clears it for the rest of the
// session if the copied track turns out to have no decodable core
// (adec_dts_no_core), so playback falls back to AC-3 instead of silence.
bool surround_hd_preferred(void);
void surround_hd_session_disable(void);   // this session only; not persisted
void surround_hd_session_reset(void);     // call when a new playback starts
