#pragma once
// Parser for Jellyfin's Trickplay manifest -- pure C, no PS3 dependencies,
// host-tested (tests/test_trickplay_info.c) the same way media_sources.cpp
// is. See PROVENANCE note in trickplay.h for how this was verified against
// Jellyfin's own TrickplayManager.cs rather than a secondary writeup.
//
// Shape, straight from BaseItemDto: Trickplay is a TWO-level dictionary,
// Dictionary<MediaSourceId, Dictionary<Width, TrickplayInfo>>. A title can
// carry trickplay data for more than one media source (multi-version) and,
// within a source, more than one tile pixel width (rare -- most servers
// generate exactly one). This picks the media source the caller names and
// then whichever width appears first, which is enough for a player that
// only ever plays one source at a time and does not offer a quality choice
// for trickplay specifically.
//
// Field names/meaning (confirmed against @jellyfin/sdk's TrickplayInfoDto,
// NOT assumed from the similar-sounding names -- Width/Height are the pixel
// size of ONE thumbnail; TileWidth/TileHeight are the tile GRID COUNT per
// sheet image, the opposite of what the names suggest at a glance):
//   Width / Height         -- pixel size of one thumbnail
//   TileWidth / TileHeight -- thumbnails per row / per column in one sheet
//   ThumbnailCount         -- total thumbnails across the whole title
//   Interval               -- milliseconds between thumbnails

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int  width, height;         // one thumbnail's pixel size
    int  tile_cols, tile_rows;  // thumbnails per sheet row / column
    int  thumbnail_count;
    int  interval_ms;
    char width_key[8];          // the manifest's width key, as text --
                                 // this is the {width} path segment the
                                 // tile-image URL needs, not necessarily
                                 // printf("%d", width) if a server ever
                                 // rounds/labels it differently
} TrickplayInfo;

// Parses an Items/{id}?Fields=Trickplay response body for media_source_id.
// Returns false if the title has no trickplay data at all (not generated,
// or no task/plugin has produced it yet) -- the same "silently absent" shape
// callers already handle for a missing poster image.
bool trickplay_parse_info(const char *json, const char *media_source_id,
                          TrickplayInfo *out);

#ifdef __cplusplus
}
#endif
