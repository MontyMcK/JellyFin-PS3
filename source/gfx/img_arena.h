#pragma once
#include <ppu-types.h>
#include <stddef.h>

// Boot-reserved scratch for stb_image decodes.
//
// WHY THIS EXISTS: thumbnail slots are reserved at boot, but the DECODE that
// fills them was still calling plain malloc — stbi_load_from_memory allocates
// its own output buffer (~455KB for the biggest card) plus a working set, per
// image.  thumb_cache_init() allocates slots until the heap runs dry (that is
// what "slotsFailed=N" in GUIlog means), so by the time the fetch thread tried
// to decode there was nothing left: on hardware every decode failed with
// reason=outofmem / probe512=FAIL while RPCS3's roomier memory container hid
// it completely.  A transient allocation in the fetch path is exactly the dice
// roll the boot-reservation architecture exists to eliminate — so the decoder
// gets its own reserved home too.
//
// SCOPE: the arena is armed by begin() for the CALLING thread only and is
// inert everywhere else, so unrelated stb users (the 1536x1536 sprite sheet on
// the UI thread) keep using the real heap and are unaffected.  Requests that
// do not fit fall back to malloc, so nothing that works today can start
// failing because of this.
//
// LIFETIME: begin() reclaims the whole arena in one go, so buffers returned by
// a decode stay valid only until the NEXT begin() on that thread.  Both call
// sites consume the pixels and free them before looping.
bool   img_arena_reserve(size_t bytes);   // boot; false if the heap can't spare it
bool   img_arena_ready(void);
void   img_arena_begin(void);             // arm + reclaim, for this thread
void   img_arena_end(void);               // disarm
size_t img_arena_size(void);
size_t img_arena_peak(void);              // high-water mark, for tuning the size

// stb_image plumbing (see the STBI_MALLOC defines in thumbnail_cache.cpp).
void  *img_arena_malloc(size_t sz);
void  *img_arena_realloc(void *p, size_t oldsz, size_t newsz);
void   img_arena_free(void *p);
