#pragma once
#include <ppu-types.h>

// -------------------------------------------------------------------------
//  Menu Particles toggle
// -------------------------------------------------------------------------
//  Settings > Menu Particles: when on, the snow field (wave_snow.h) that the
//  music screen shows also drifts behind the XMB menus -- every menu screen
//  that calls wave_snow_ambient() before wave_draw().  Playback and the item
//  details page never do, so it fades out there.
//
//  Default is OFF.  Persisted as "0"/"1" in the app data dir next to the
//  other settings files.

void menusnow_load(void);            // read the persisted value (once, at startup)
bool menusnow_enabled(void);
void menusnow_set_enabled(bool on);  // set + persist immediately
