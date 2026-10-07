#pragma once
// Now Playing screen for the Music tab.  Each entry point builds a queue,
// starts the playback engine, and blocks in its own frame loop until the
// user backs out (or the queue finishes).

#include "ui.h"

// Album (from the Albums grid or an artist/genre sub-screen).  parent is
// the breadcrumb segment before the album name — "Albums", or the artist/
// genre the user drilled through.
void music_screen_open_album(const XMBItem *album, const char *parent);

// Call just before music_screen_open_album() when the album was opened from a
// card whose rect was noted with depth_note_focus_rect(): the cover then
// flies out of that card instead of rising out of depth.
void music_screen_origin_from_focus(void);

// Songs sub-tab: queue the loaded song list, starting at start_idx.
void music_screen_open_songs(const XMBItem *items, int count, int start_idx);

// Playlist: queue its tracks in playlist order.
void music_screen_open_playlist(const XMBItem *playlist);
