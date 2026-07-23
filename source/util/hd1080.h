#pragma once
#include <ppu-types.h>

// -------------------------------------------------------------------------
//  1080p playback (Alpha) toggle
// -------------------------------------------------------------------------
//  Experimental switch for requesting a full 1920x1080 High-profile transcode
//  instead of the shipped 1280x720 baseline path.  It exists so the whole
//  1080p pipeline (device profile, stream URL, VDEC level, jitter-buffer /
//  arena sizing) can be exercised from the emulator WITHOUT changing the
//  default behaviour on real hardware.
//
//  Default is OFF.  When OFF every playback-path decision falls back to the
//  exact 720p code that shipped, so a real-hardware build is untouched unless
//  the user deliberately flips this in Settings.  Promotion to the main build
//  is gated on the owner's sign-off once playback is proven decent.
//
//  Persisted as "0"/"1" in the app data dir next to the other settings files.

void hd1080_load(void);            // read the persisted value (once, at startup)
void hd1080_save(void);            // persist the current value
bool hd1080_enabled(void);         // true => request the 1080p High-profile path
void hd1080_set_enabled(bool on);  // set + persist immediately
