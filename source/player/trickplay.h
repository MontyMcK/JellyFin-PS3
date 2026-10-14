#pragma once
// Client side of Jellyfin's Trickplay feature: the scrub-preview sprite
// sheets, shown above the seek bar while holding L2/R2 to scrub (see
// player_seek.cpp's SEEK_SCRUB state, which today only moves the bar with
// nothing fetched -- this fills that in).
//
// API shape verified against Jellyfin's own TrickplayManager.cs rather than
// assumed from field names that turned out backwards on a first read:
// Width/Height on the manifest are a SINGLE thumbnail's pixel size;
// TileWidth/TileHeight are the tile GRID COUNT (thumbnails per row/column)
// in one sheet image. The tile-image URL's {index} addresses a SHEET FILE
// (index = floor(thumbnail_index / (TileWidth*TileHeight))), not a single
// thumbnail -- the server hands back a whole sheet and the client crops the
// tile it wants out of it. See source/api/trickplay_info.h for the manifest
// parser (host-tested) this builds on.
//
// Only one sheet is ever resident: a scrub session covers seconds of
// content, and one sheet already spans TileWidth*TileHeight thumbnails at
// Interval ms apart -- commonly ~16 minutes of runtime for a 10x10/10s
// manifest -- so a normal scrub fetches at most one or two sheets total.
// The fetch is deliberately SYNCHRONOUS: it only ever runs while the video
// is already paused for a scrub hold (see SEEK_SCRUB), so nothing else on
// the console needs the CPU or the network at that moment, unlike the
// thumbnail grid cache's always-be-fetching-while-scrolling case that
// justified a whole fetch thread there.

#include <ppu-types.h>

#ifdef __cplusplus
extern "C" {
#endif

// Drops any cached manifest/sheet. Call when a new title starts playing so
// a scrub early in the next title can never show a stale previous title's
// frame while its own manifest is (re)fetched.
void trickplay_reset(void);

// Frees the decoded sheet (the ~15+ MB part -- see trickplay.cpp's memory
// note) but keeps the manifest, so scrubbing again later in the SAME title
// skips straight to a sheet fetch instead of re-fetching the manifest too.
// Call when a scrub hold ends (player_seek.cpp SEEK_SCRUB -> SEEK_IDLE):
// the memory is not this feature's to hold onto once nobody is scrubbing.
void trickplay_release_sheet(void);

// Ensures the sheet covering time_ms is fetched and decoded, then updates
// what trickplay_current() returns. Cheap to call every frame while
// scrubbing: internally a no-op unless the sheet actually needs to change,
// and remembers "this title has no trickplay data" so an absent title costs
// one failed manifest fetch total, not one per scrub step.
void trickplay_scrub_update(const char *item_id, const char *media_source_id,
                            u32 time_ms);

// The sub-rectangle of the cached sheet covering the last time_ms passed to
// trickplay_scrub_update, or NULL if nothing is available right now (no
// manifest for this title, or the fetch/decode failed). Pixels are
// 0x00RRGGBB (framebuffer format, no alpha -- see ui.h's drawBitmapRect,
// which this is meant to be drawn with) and remain valid until the next
// trickplay_scrub_update call that changes sheets.
typedef struct {
    const u32 *sheet_rgb;      // full decoded sheet, sheet_w x sheet_h
    int        sheet_w, sheet_h;
    int        tile_x, tile_y; // top-left of this tile within the sheet
    int        tile_w, tile_h; // one thumbnail's pixel size
} TrickplayTile;

const TrickplayTile *trickplay_current(void);

#ifdef __cplusplus
}
#endif
