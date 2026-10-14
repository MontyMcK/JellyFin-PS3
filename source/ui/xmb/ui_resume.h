#pragma once
#include <stdint.h>

// -------------------------------------------------------------------------
//  Where an item stopped, for lists fetched BEFORE it was played
// -------------------------------------------------------------------------
//  An XMBItem carries the resume position the server reported when the list
//  was fetched.  A details page opened from that list keeps the item it was
//  given, so after watching half a film and backing out the page still said
//  "no position" and played from the start.  Every playback therefore notes
//  where it ended, and anything about to start an item asks here first.
//
//  The rule is the server's own (Jellyfin's default MinResumePct 5, MaxResumePct
//  90): near the start or near the end the item has no resume point.  Pure (no
//  PS3 headers) so tests/test_ui_resume.cpp compiles the code the console runs.
// -------------------------------------------------------------------------

// What the player's last stop means as a resume position: 0 when the item is
// to start over (under 10 s in, under 5 %, or 90 % or more through).  total 0 =
// runtime unknown: only the 10 s rule applies.
uint32_t resume_secs_after(uint32_t pos, uint32_t total);

// Called after every playback of `id` (player_last_position_secs / _total_secs).
void xmb_resume_note(const char *id, uint32_t pos, uint32_t total);

// The position to start `id` at: what the last playback this session left, else
// `listed` (the position the list was fetched with).
uint32_t xmb_resume_for(const char *id, uint32_t listed);
