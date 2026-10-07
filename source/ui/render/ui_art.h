#pragma once
#include <ppu-types.h>
#include "art_colour.h"
#include "bitmap.h"

// The console side of render/art_colour.h: ONE cache for the whole app, fed
// from bitmaps already decoded into main memory (the detail page's poster, a
// thumbnail the cache has finished) and read by any screen that wants the
// artwork's accent -- the buffering ring, the music screen's halo, the
// screensaver.  Render thread only.
//
// Analysis happens once per image id (a 24x24 sample, well under 0.1 ms);
// every later call is a lookup.  Without artwork it is the theme's accent.

// Cached palette for id, analysing bm on a miss.  bm may be NULL (not
// loaded yet): the fallback is returned and nothing is cached.
art_palette ui_art_palette(const char *id, const Bitmap *bm);

// Cached palette for id without analysing anything.  False (and the theme's
// fallback in *out) when id has not been seen with pixels yet.
bool ui_art_peek(const char *id, art_palette *out);

// The theme-accent fallback palette.
art_palette ui_art_fallback(void);

// Counters for the log: images analysed so far.
unsigned ui_art_analyses(void);
