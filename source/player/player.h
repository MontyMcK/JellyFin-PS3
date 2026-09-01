#pragma once
#include <ppu-types.h>
#include "jellyfin_api.h"

// Show the "now playing" screen for the given item.
// resume_secs > 0 starts playback at that position (Continue Watching).
// Blocks until the user presses START to go back.
// media_source_id selects a version before the stream opens; NULL/empty uses
// Jellyfin's default.  Sources are intentionally not switchable mid-playback.
void show_player(const JFItem *item, u32 resume_secs = 0,
                 const char *media_source_id = NULL);

// End-of-item auto-advance.  Arm before show_player() when the item has a
// follower: during the last 90 s of playback the player shows a popup badge
// reading `label` (with the instruction line `hint` drawn separately below
// it), and SELECT ends playback with the next-request flag set.  Episodes
// additionally auto-advance — the last 30 s show a countdown that sets the
// flag when it reaches zero (or when the stream EOFs inside that window).
// The armed state is consumed by the next show_player() call;
// player_take_next_request() returns and clears the flag.
void player_arm_next(const char *label, const char *hint);
bool player_take_next_request(void);
