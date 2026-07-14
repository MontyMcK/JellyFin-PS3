#pragma once
#include <ppu-types.h>
#include "bitmap.h"

// Call once after http_init() and RSX is up.
void thumb_cache_init(void);

// Call once on shutdown.
void thumb_cache_shutdown(void);

// Non-blocking. Queues a fetch of item_id's Primary image at exactly w x h
// pixels, if not already cached or in-flight at that size.  Safe to call
// every frame for every visible item.
void thumb_request(const char *item_id, int w, int h);

// Returns a pointer to a ready w x h Bitmap, or NULL if not yet loaded.
// The returned pointer is valid until thumb_cache_shutdown().
const Bitmap *thumb_get(const char *item_id, int w, int h);

// Advance the cache clock and unload slots nothing has requested/drawn for
// a few seconds.  Call once per frame from the browsing UI loop.
void thumb_cache_tick(void);

// Call on tab switch: unloads the old tab's thumbs on the next tick and
// pauses fetches briefly so rapid tab flipping causes no decode churn.
void thumb_cache_retarget(void);

// Largest square edge a slot can hold (slots are sized for grid cards at
// init).  thumb_request silently drops anything bigger, so callers wanting
// larger on-screen art must request at this cap and upscale when blitting.
int thumb_max_square(void);
