#pragma once
#include <ppu-types.h>
#include "bitmap.h"

// Call once after http_init() and RSX is up.
void thumb_cache_init(void);

// Call once on shutdown.
void thumb_cache_shutdown(void);

// Which Jellyfin image to fetch for an item.
//
// Primary is the right default nearly everywhere: it's the portrait poster for
// movies/series and the (already 16:9) still for episodes, and it is the only
// image type guaranteed to exist.  THUMB is the wide banner art, and is only
// correct for landscape cards on items whose ImageTags actually list a "Thumb"
// — asking for one that doesn't exist just 404s and leaves a blank card, so
// callers must gate on XMBItem::has_thumb.
typedef enum { THUMB_IMG_PRIMARY = 0, THUMB_IMG_THUMB = 1 } ThumbImg;

// Non-blocking. Queues a fetch of item_id's image at exactly w x h pixels, if
// not already cached or in-flight at that size.  Safe to call every frame for
// every visible item.  The image kind is part of the cache key, so the same
// item can be held at both Primary and Thumb without either evicting the other.
void thumb_request(const char *item_id, int w, int h,
                   ThumbImg img = THUMB_IMG_PRIMARY);

// Returns a pointer to a ready w x h Bitmap, or NULL if not yet loaded.
// The returned pointer is valid until thumb_cache_shutdown().
const Bitmap *thumb_get(const char *item_id, int w, int h,
                        ThumbImg img = THUMB_IMG_PRIMARY);

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
