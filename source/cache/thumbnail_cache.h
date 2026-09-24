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

// GPU path: resolve a ready thumbnail to a VRAM texture the RSX can sample.
//
// Returns false when there is no ready slot, or the slot has no VRAM mirror
// (allocation failed at init) -- callers then fall back to cpu_blit_bitmap()
// and nothing about the UI changes.
//
// Call ONLY from the render thread, and only in the frame's GPU phase: on a
// miss it performs the one main->VRAM copy that keeps the mirror in step with
// the decoded bitmap, and the RSX must not be sampling that texture while it
// happens.  The copy is a plain memcpy in the write direction, which this
// hardware does at ~767 MB/s, and it runs once per decode rather than once
// per frame per card.
// out_pitch is the mirror's ROW PITCH IN BYTES, which is not width*4: the RSX
// requires a linear texture's pitch to be 64-byte aligned and a card is rarely
// that wide, so rows are padded.  Pass it to rsxLoadTexture as tex.pitch.
bool thumb_gpu_texture(const char *item_id, int w, int h,
                       u32 *out_offset, u32 *out_pitch,
                       ThumbImg img = THUMB_IMG_PRIMARY);

// Largest square edge a slot can hold (slots are sized for grid cards at
// init).  thumb_request silently drops anything bigger, so callers wanting
// larger on-screen art must request at this cap and upscale when blitting.
int thumb_max_square(void);
