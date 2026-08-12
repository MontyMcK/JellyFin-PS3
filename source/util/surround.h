#pragma once
#include <ppu-types.h>

// -------------------------------------------------------------------------
//  Surround 5.1 (Alpha) toggle
// -------------------------------------------------------------------------
//  Experimental switch for 5.1 surround movie audio: an 8-channel CellAudio
//  port carrying 5.1 LPCM (decoded locally from AC-3), instead of the shipped
//  stereo MP3 path.  It gates every surround decision — device profile,
//  stream URL, demux stream selection, decoder choice and audio port width —
//  so the whole pipeline can be exercised WITHOUT changing the default
//  behaviour on real hardware.
//
//  Default is OFF.  When OFF every playback-path decision falls back to the
//  exact stereo code that shipped, so a build is untouched unless the user
//  deliberately flips this in Settings.  The music player ignores this flag
//  entirely — music is stereo by design.
//
//  Requires "Linear PCM 5.1 Ch." ticked in XMB Settings > Sound Settings >
//  Audio Output Settings; without it the PS3 mixes the port down to stereo.
//
//  Persisted as "0"/"1" in the app data dir next to the other settings files.

void surround_load(void);            // read the persisted value (once, at startup)
void surround_save(void);            // persist the current value
bool surround_enabled(void);         // true => request the 5.1/AC-3 path
void surround_set_enabled(bool on);  // set + persist immediately
