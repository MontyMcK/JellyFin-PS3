// Artwork colour: one accent (plus a deep and a glow variant) derived from a
// poster or an album cover, and a small cache so it is derived ONCE per image.
//
// WHY IT IS A HEADER OF PURE C
//
// Same reason as spine.h and depth.h: it is arithmetic on pixels already in
// main memory, so it is host-tested (tests/test_experience.c) for the things
// that matter on the console -- the result is deterministic, an empty or
// broken image falls back to the theme accent, and a cached image is never
// analysed twice.  Nothing here touches the RSX, the theme or the heap.
//
// COST
//
// The image is never walked in full.  At most ART_GRID x ART_GRID (24 x 24 =
// 576) pixels are sampled on a regular grid inside an 8% border (posters carry
// studio logos and letterbox bars at their edges), and every sample is a few
// integer operations plus one float divide.  A 200x300 poster costs the same
// as a 2000x3000 one: well under 0.1 ms on the PPU, once, when the artwork
// first arrives -- never per frame.  The caller reads the pixels from the
// decoded main-memory bitmap, so there is no video-memory read at all.
//
// WHAT "DOMINANT" MEANS HERE
//
// Not the most common colour -- on most posters that is black.  Samples are
// binned by hue into ART_HUES buckets and each is weighted by its saturation
// and brightness, so the colour that carries the image (a red coat on a dark
// street, a blue sky over a grey city) wins over the large neutral areas
// around it.  Near-black and near-white samples do not vote.  If too little of
// the image is coloured at all (a black-and-white still, a grey album cover)
// the result is the fallback: the theme's own accent, which is also what the
// UI shows before the artwork has loaded.  The chosen hue is then pushed into
// a band of saturation and lightness that reads on this UI's dark background,
// so a muddy brown poster still yields a usable accent, not a muddy one.

#ifndef JF_ART_COLOUR_H
#define JF_ART_COLOUR_H

#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ART_GRID       24      // samples per axis, at most
#define ART_HUES       12      // 30-degree hue buckets
#define ART_CACHE_N    24      // images remembered

typedef struct {
    uint32_t accent;   // 0x00RRGGBB, readable on the dark UI (L ~0.62)
    uint32_t deep;     // the same hue, very dark (L ~0.16): backgrounds, veils
    uint32_t glow;     // the same hue, mid (L ~0.46): halos, rings
    int      valid;    // 1 = from the artwork, 0 = the fallback
} art_palette;

// --- colour arithmetic ---------------------------------------------------------

static inline float art_clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

// HSL in [0,1) x [0,1] x [0,1].  Plain textbook conversions; no libm.
static inline void art_rgb_to_hsl(uint32_t rgb, float *h, float *s, float *l) {
    const float r = (float)((rgb >> 16) & 0xFF) / 255.0f;
    const float g = (float)((rgb >> 8) & 0xFF) / 255.0f;
    const float b = (float)(rgb & 0xFF) / 255.0f;
    float mx = r > g ? r : g; if (b > mx) mx = b;
    float mn = r < g ? r : g; if (b < mn) mn = b;
    const float d = mx - mn;
    *l = (mx + mn) * 0.5f;
    if (d <= 1e-6f) { *h = 0.0f; *s = 0.0f; return; }
    *s = *l > 0.5f ? d / (2.0f - mx - mn) : d / (mx + mn);
    float hh;
    if (mx == r)      hh = (g - b) / d + (g < b ? 6.0f : 0.0f);
    else if (mx == g) hh = (b - r) / d + 2.0f;
    else              hh = (r - g) / d + 4.0f;
    *h = hh / 6.0f;
}

static inline float art_hue_ch(float p, float q, float t) {
    if (t < 0.0f) t += 1.0f;
    if (t > 1.0f) t -= 1.0f;
    if (t < 1.0f / 6.0f) return p + (q - p) * 6.0f * t;
    if (t < 0.5f)        return q;
    if (t < 2.0f / 3.0f) return p + (q - p) * (2.0f / 3.0f - t) * 6.0f;
    return p;
}

static inline uint32_t art_hsl_to_rgb(float h, float s, float l) {
    float r, g, b;
    if (s <= 1e-6f) {
        r = g = b = l;
    } else {
        const float q = l < 0.5f ? l * (1.0f + s) : l + s - l * s;
        const float p = 2.0f * l - q;
        r = art_hue_ch(p, q, h + 1.0f / 3.0f);
        g = art_hue_ch(p, q, h);
        b = art_hue_ch(p, q, h - 1.0f / 3.0f);
    }
    const uint32_t R = (uint32_t)(art_clampf(r, 0.0f, 1.0f) * 255.0f + 0.5f);
    const uint32_t G = (uint32_t)(art_clampf(g, 0.0f, 1.0f) * 255.0f + 0.5f);
    const uint32_t B = (uint32_t)(art_clampf(b, 0.0f, 1.0f) * 255.0f + 0.5f);
    return (R << 16) | (G << 8) | B;
}

// The three shades of one hue, tuned for the dark UI.
static inline art_palette art_palette_from_hs(float h, float s, int valid) {
    art_palette p;
    const float sa = art_clampf(s, 0.38f, 0.82f);
    p.accent = art_hsl_to_rgb(h, sa, 0.62f);
    p.glow   = art_hsl_to_rgb(h, sa, 0.46f);
    p.deep   = art_hsl_to_rgb(h, art_clampf(s, 0.30f, 0.60f), 0.16f);
    p.valid  = valid;
    return p;
}

// The fallback: the given UI accent, with its own deep and glow shades.
static inline art_palette art_palette_fallback(uint32_t ui_accent) {
    float h, s, l;
    art_rgb_to_hsl(ui_accent & 0x00FFFFFFu, &h, &s, &l);
    art_palette p = art_palette_from_hs(h, s, 0);
    p.accent = ui_accent & 0x00FFFFFFu;     // the theme's accent exactly
    return p;
}

// --- extraction ------------------------------------------------------------------

// px: w x h pixels, 0x??RRGGBB (the top byte is ignored), stride_px pixels per
// row.  Always fills *out; returns 1 when the artwork decided it, 0 when *out
// is the fallback (NULL / empty / degenerate / colourless image).
static inline int art_palette_extract(const uint32_t *px, int w, int h, int stride_px,
                                      uint32_t fallback, art_palette *out) {
    *out = art_palette_fallback(fallback);
    if (!px || w <= 0 || h <= 0 || stride_px < w) return 0;

    // Sample window: an 8% border off each edge, when the image is big
    // enough to have one.
    int x0 = w * 8 / 100, x1 = w - x0, y0 = h * 8 / 100, y1 = h - y0;
    if (x1 <= x0) { x0 = 0; x1 = w; }
    if (y1 <= y0) { y0 = 0; y1 = h; }
    const int nx = (x1 - x0) < ART_GRID ? (x1 - x0) : ART_GRID;
    const int ny = (y1 - y0) < ART_GRID ? (y1 - y0) : ART_GRID;

    float wsum[ART_HUES], rs[ART_HUES], gs[ART_HUES], bs[ART_HUES];
    memset(wsum, 0, sizeof wsum); memset(rs, 0, sizeof rs);
    memset(gs, 0, sizeof gs);     memset(bs, 0, sizeof bs);
    float total = 0.0f;
    int   n     = 0;

    for (int j = 0; j < ny; j++) {
        // Centre of each grid cell, in integer arithmetic (deterministic).
        const int y = y0 + (int)(((long long)(2 * j + 1) * (y1 - y0)) / (2 * ny));
        const uint32_t *row = px + (long long)y * stride_px;
        for (int i = 0; i < nx; i++) {
            const int x = x0 + (int)(((long long)(2 * i + 1) * (x1 - x0)) / (2 * nx));
            const uint32_t c = row[x] & 0x00FFFFFFu;
            n++;
            const int r = (int)((c >> 16) & 0xFF), g = (int)((c >> 8) & 0xFF), b = (int)(c & 0xFF);
            int mx = r > g ? r : g; if (b > mx) mx = b;
            int mn = r < g ? r : g; if (b < mn) mn = b;
            if (mx < 32) continue;                       // near black: no vote
            const float sat = (float)(mx - mn) / (float)mx;   // HSV saturation
            if (sat < 0.18f) continue;                   // neutral: no vote
            const float val = (float)mx / 255.0f;
            const float wt  = sat * sat * (0.35f + 0.65f * val);
            float hh, ss, ll;
            art_rgb_to_hsl(c, &hh, &ss, &ll);
            int bin = (int)(hh * (float)ART_HUES);
            if (bin >= ART_HUES) bin = ART_HUES - 1;
            if (bin < 0) bin = 0;
            wsum[bin] += wt;
            rs[bin] += wt * (float)r; gs[bin] += wt * (float)g; bs[bin] += wt * (float)b;
            total += wt;
        }
    }
    if (n == 0) return 0;
    // Too little colour in the whole image to call it anything: fall back.
    // (A mean weight of 0.02 is roughly 8% of the samples being clearly
    // coloured.)
    if (total / (float)n < 0.02f) return 0;

    // The winning bucket, smoothed with its neighbours so a hue sitting on a
    // bucket edge is not split in two.  Ties go to the lower index, so the
    // result never depends on anything but the pixels.
    int best = 0; float best_w = -1.0f;
    for (int k = 0; k < ART_HUES; k++) {
        const float s = wsum[k] + 0.5f * (wsum[(k + ART_HUES - 1) % ART_HUES] +
                                          wsum[(k + 1) % ART_HUES]);
        if (s > best_w) { best_w = s; best = k; }
    }
    if (wsum[best] <= 0.0f) return 0;
    const uint32_t mean =
        ((uint32_t)(rs[best] / wsum[best] + 0.5f) << 16) |
        ((uint32_t)(gs[best] / wsum[best] + 0.5f) << 8) |
         (uint32_t)(bs[best] / wsum[best] + 0.5f);
    float hh, ss, ll;
    art_rgb_to_hsl(mean, &hh, &ss, &ll);
    *out = art_palette_from_hs(hh, ss, 1);
    return 1;
}

// --- cache ------------------------------------------------------------------------
//
// Keyed by an FNV-1a hash of the image id (Jellyfin ids are 32 hex digits, so
// a 32-bit hash is ample for 24 entries) and checked against the stored id,
// so a collision is a miss, never a wrong colour.  Least-recently-used
// replacement.  Fixed storage: no allocation, ever.

typedef struct {
    char        id[40];
    uint32_t    hash;
    uint32_t    used;       // LRU clock; 0 = empty slot
    art_palette pal;
} art_cache_entry;

typedef struct {
    art_cache_entry e[ART_CACHE_N];
    uint32_t        clock;
    uint32_t        analyses;   // how many images were actually analysed
} art_cache;

static inline uint32_t art_hash(const char *s) {
    uint32_t h = 2166136261u;
    while (s && *s) { h ^= (uint8_t)*s++; h *= 16777619u; }
    return h;
}

static inline void art_cache_reset(art_cache *c) { memset(c, 0, sizeof *c); }

// 1 and *out filled when id is cached.
static inline int art_cache_get(art_cache *c, const char *id, art_palette *out) {
    if (!id || !id[0]) return 0;
    const uint32_t h = art_hash(id);
    for (int i = 0; i < ART_CACHE_N; i++) {
        art_cache_entry *e = &c->e[i];
        if (e->used && e->hash == h && strncmp(e->id, id, sizeof e->id - 1) == 0) {
            e->used = ++c->clock;
            if (out) *out = e->pal;
            return 1;
        }
    }
    return 0;
}

static inline void art_cache_put(art_cache *c, const char *id, const art_palette *pal) {
    if (!id || !id[0]) return;
    const uint32_t h = art_hash(id);
    int slot = -1;
    for (int i = 0; i < ART_CACHE_N; i++)
        if (c->e[i].used && c->e[i].hash == h &&
            strncmp(c->e[i].id, id, sizeof c->e[i].id - 1) == 0) { slot = i; break; }
    if (slot < 0) {
        uint32_t oldest = 0xFFFFFFFFu;
        for (int i = 0; i < ART_CACHE_N; i++) {
            if (!c->e[i].used) { slot = i; break; }
            if (c->e[i].used < oldest) { oldest = c->e[i].used; slot = i; }
        }
    }
    art_cache_entry *e = &c->e[slot];
    strncpy(e->id, id, sizeof e->id - 1);
    e->id[sizeof e->id - 1] = 0;
    e->hash = h;
    e->pal  = *pal;
    e->used = ++c->clock;
}

// The one call sites use: the cached palette for id, analysing px only on a
// miss.  A NULL / empty image is NOT cached (it may simply not have loaded
// yet), so the next call with real pixels still analyses it; a real image that
// turned out colourless IS cached, as the fallback, and never re-analysed.
static inline art_palette art_cache_lookup(art_cache *c, const char *id,
                                           const uint32_t *px, int w, int h,
                                           int stride_px, uint32_t fallback) {
    art_palette p;
    if (art_cache_get(c, id, &p)) return p;
    if (!px || w <= 0 || h <= 0) return art_palette_fallback(fallback);
    art_palette_extract(px, w, h, stride_px, fallback, &p);
    c->analyses++;
    art_cache_put(c, id, &p);
    return p;
}

// A colour 0..1 of the way from a to b, per channel.
static inline uint32_t art_mix(uint32_t a, uint32_t b, float t) {
    t = art_clampf(t, 0.0f, 1.0f);
    uint32_t out = 0;
    for (int sh = 0; sh <= 16; sh += 8) {
        const float ca = (float)((a >> sh) & 0xFF), cb = (float)((b >> sh) & 0xFF);
        out |= (uint32_t)(ca + (cb - ca) * t + 0.5f) << sh;
    }
    return out;
}

#ifdef __cplusplus
}
#endif

#endif
