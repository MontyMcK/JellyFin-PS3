#pragma once
#include "bitmap.h"

// -------------------------------------------------------
// Detail-page hero art (poster + backdrop)
// -------------------------------------------------------
// The grid thumbnail cache sizes every slot for a small library card
// (~110x165) and silently DROPS anything bigger, so a detail-page poster
// blown up from a card looks pixelated and there is nowhere to hold a wide
// backdrop at all.  This loads ONE image at an arbitrary (larger) size into
// its own main-memory Bitmap, synchronously.
//
// Blocking network + decode — call it once when the detail overlay opens
// (off the render loop), not every frame.  On success out->pixels is a
// malloc'd w*h ARGB buffer owned by the caller; free it with
// detail_media_free().  On any failure out->pixels is NULL and the caller
// falls back (cached card thumb for the poster, no backdrop otherwise).
//
// image_type is a Jellyfin image name: "Primary" (poster) or "Backdrop".
// The art is "cover"-fit into w×h (scaled to fill, overflow cropped — never
// stretched); vbias chooses the vertical crop window (0=top .. 1=bottom, 0.5
// centre) for the axis that gets cropped.
bool detail_media_load(const char *item_id, const char *image_type,
                       int w, int h, float vbias, Bitmap *out);

void detail_media_free(Bitmap *out);
