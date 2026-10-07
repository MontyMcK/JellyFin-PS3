#pragma once
// -------------------------------------------------------------------------
//  Auto Skip toggle
// -------------------------------------------------------------------------
//  Settings > Auto Skip: when on, intros and recaps are skipped the moment
//  playback reaches them, without pressing X on the Skip badge.  Credits are
//  NOT skipped: with a next episode queued they start its 25 s countdown
//  instead (player.cpp).  Default OFF.  Persisted as "0"/"1".

void autoskip_load(void);            // read the persisted value (once, at startup)
bool autoskip_enabled(void);
void autoskip_set_enabled(bool on);  // set + persist immediately
