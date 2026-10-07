// Artwork colour: the app-wide cache.  See ui_art.h and art_colour.h.

#include "ui_art.h"
#include "ui_visuals.h"   // XMB_ACCENT

static art_cache s_cache;          // ~2 KB, static: no allocation, ever
static bool      s_inited = false;

static void art_init_once(void) {
    if (s_inited) return;
    art_cache_reset(&s_cache);
    s_inited = true;
}

art_palette ui_art_fallback(void) { return art_palette_fallback(XMB_ACCENT); }

art_palette ui_art_palette(const char *id, const Bitmap *bm) {
    art_init_once();
    if (!bm || !bm->pixels || !bm->width || !bm->height)
        return art_cache_lookup(&s_cache, id, NULL, 0, 0, 0, XMB_ACCENT);
    return art_cache_lookup(&s_cache, id, (const uint32_t *)bm->pixels,
                            (int)bm->width, (int)bm->height, (int)bm->width,
                            XMB_ACCENT);
}

bool ui_art_peek(const char *id, art_palette *out) {
    art_init_once();
    if (art_cache_get(&s_cache, id, out)) return true;
    if (out) *out = ui_art_fallback();
    return false;
}

unsigned ui_art_analyses(void) { return s_cache.analyses; }
