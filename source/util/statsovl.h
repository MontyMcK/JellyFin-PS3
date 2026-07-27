#pragma once
#include <ppu-types.h>

// -------------------------------------------------------------------------
//  Player Stats Overlay toggle
// -------------------------------------------------------------------------
//  Runtime on/off switch for the playback diagnostics overlay (player_stats).
//  It controls DISPLAY ONLY: when off, nothing is drawn and no text layout
//  runs, but the metric hooks keep updating their counters so the numbers are
//  already warm the moment it is switched on mid-movie.
//
//  Stripping the measurement code entirely is a separate, compile-time
//  decision — ENABLE_PLAYER_STATS in build_config.h.  This store deliberately
//  lives OUTSIDE player_stats.cpp so the settings screen still links when that
//  file is compiled out.
//
//  Default is OFF.  Persisted as "0"/"1" in the app data dir next to the
//  other settings files.

void statsovl_load(void);            // read the persisted value (once, at startup)
void statsovl_save(void);            // persist the current value
bool statsovl_enabled(void);         // true => draw the overlay during playback
void statsovl_set_enabled(bool on);  // set + persist immediately
