#pragma once
#include "jellyfin_api.h"

// Show the "now playing" screen for the given item.
// Blocks until the user presses O to go back.
void show_player(const JFItem *item);
