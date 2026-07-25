// Detail-page hero art loader — see detail_media.h.
//
// This TU carries its OWN private copy of stb_image (STB_IMAGE_STATIC), on the
// real heap.  The shared decoder in thumbnail_cache.cpp routes STBI_MALLOC
// through img_arena, which is armed on the fetch THREAD; reusing it from the
// main thread (where the detail overlay runs) would fight that arena.  A
// file-local static copy sidesteps both the arena and any duplicate-symbol
// clash with the shared implementation.

#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#ifdef __GNUC__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wtype-limits"
#endif
#include "stb_image.h"
#ifdef __GNUC__
#pragma GCC diagnostic pop
#endif

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <malloc.h>

#include "detail_media.h"
#include "http.h"
#include "jellyfin_api.h"   // g_server, g_token
#include "plog.h"

// A poster JPEG at ~90 quality is tens of KB; a 1280-wide backdrop lands
// around 150-250KB.  512KB covers both with headroom.
#define HERO_FETCH_MAX (512 * 1024)

bool detail_media_load(const char *item_id, const char *image_type,
                       int w, int h, float vbias, Bitmap *out) {
    out->width = out->height = 0;
    out->pixels = NULL;
    out->offset = 0;
    if (!item_id || !item_id[0] || !image_type || w <= 0 || h <= 0) return false;
    if (!g_server[0]) return false;

    uint8_t *buf = (uint8_t*)malloc(HERO_FETCH_MAX);
    if (!buf) { plog("detail_media: no fetch buf"); return false; }

    // Cap the SERVER-side resolution so the JPEG we decode stays small: a
    // 1080p display asks for a 1920-wide backdrop, but the decode buffer for a
    // full-size image (~8MB) is exactly the transient that starves the heap on
    // real hardware.  Fetch at most FETCH_CAP_W wide (aspect-preserved) and
    // nearest-scale UP to the on-screen w×h below — a darkened backdrop hides
    // the softness, and a poster is never over the cap anyway.
    const int FETCH_CAP_W = 960;
    int fw = w, fh = h;
    if (fw > FETCH_CAP_W) { fh = (int)((int64_t)fh * FETCH_CAP_W / fw); fw = FETCH_CAP_W; }

    // fillWidth/fillHeight = scale + centre-crop to that box server-side, so a
    // poster stays 2:3 and a backdrop is cropped to the strip we ask for.
    char url[512];
    snprintf(url, sizeof(url),
        "%s/Items/%s/Images/%s?fillWidth=%d&fillHeight=%d&quality=90&format=Jpeg",
        g_server, item_id, image_type, fw, fh);

    int bytes = http_fetch_binary(url, g_token, buf, HERO_FETCH_MAX);
    if (bytes <= 0) {
        char m[96];
        snprintf(m, sizeof(m), "detail_media: fetch %s fail (%d)", image_type, bytes);
        plog(m);
        free(buf);
        return false;
    }

    int dw, dh, ch;
    unsigned char *px = stbi_load_from_memory(buf, bytes, &dw, &dh, &ch, 4);
    free(buf);
    if (!px) {
        char m[96];
        snprintf(m, sizeof(m), "detail_media: decode %s fail", image_type);
        plog(m);
        return false;
    }

    u32 *dst = (u32*)memalign(16, (size_t)w * h * 4);
    if (!dst) {
        plog("detail_media: no pixel buf");
        stbi_image_free(px);
        return false;
    }

    // Aspect-preserving "cover" crop (like CSS object-fit: cover): scale the
    // decoded image to fill w×h and centre-crop the overflow, so a 16:9
    // backdrop forced into a wide banner box is never STRETCHED — only cropped.
    // vbias picks the vertical crop window: 0=top, 0.5=centre, 1=bottom (a
    // backdrop reads better biased toward the top, a poster centred).
    int cw = dw, chh = dh, ox = 0, oy = 0;   // source crop rect
    // Compare aspects with integer cross-multiplication (dw/dh vs w/h).
    if ((int64_t)dw * h > (int64_t)w * dh) {
        // Source is wider than target: crop width, full height.
        cw = (int)((int64_t)dh * w / h);
        ox = (dw - cw) / 2;
    } else {
        // Source is taller than target: crop height, full width.
        chh = (int)((int64_t)dw * h / w);
        oy = (int)((dh - chh) * vbias);
    }
    if (cw  < 1) cw  = 1;
    if (chh < 1) chh = 1;

    // Nearest-neighbour sample from the crop rect into the exact w×h output.
    for (int y = 0; y < h; y++) {
        int sy = oy + (int)((int64_t)y * chh / h);
        const unsigned char *srow = px + (size_t)sy * dw * 4;
        u32 *drow = dst + (size_t)y * w;
        for (int x = 0; x < w; x++) {
            int sx = ox + (int)((int64_t)x * cw / w);
            const unsigned char *s = srow + (size_t)sx * 4;
            drow[x] = ((u32)s[0] << 16) | ((u32)s[1] << 8) | s[2];
        }
    }
    stbi_image_free(px);

    out->width  = (u32)w;
    out->height = (u32)h;
    out->pixels = dst;
    out->offset = 0;
    char m[96];
    snprintf(m, sizeof(m), "detail_media: %s ready %dx%d (src %dx%d)",
             image_type, w, h, dw, dh);
    plog(m);
    return true;
}

void detail_media_free(Bitmap *out) {
    if (out && out->pixels) { free(out->pixels); out->pixels = NULL; }
    if (out) { out->width = out->height = 0; }
}
