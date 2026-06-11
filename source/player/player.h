#pragma once
#include <ppu-types.h>
#include "jellyfin_api.h"

// Show the "now playing" screen for the given item.
// resume_secs > 0 starts playback at that position (Continue Watching).
// Blocks until the user presses START to go back.
void show_player(const JFItem *item, u32 resume_secs = 0);
