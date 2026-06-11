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
