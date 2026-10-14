#pragma once

// Changing a user's data on an item: favourite and played.  Blocking calls
// (one request each); they run on the UI thread from a button press, as the
// details page's other actions do.  The caller refreshes whatever depends on
// them (Home's rows follow g_play_gen).

// Mark an item a favourite (true) or not (false).  True on a 2xx reply.
bool jf_set_favourite(const char *item_id, bool favourite);

// Mark an item played (true) or unplayed (false).  True on a 2xx reply.
bool jf_set_played(const char *item_id, bool played);
