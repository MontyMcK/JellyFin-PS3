#pragma once
// Circular image blit and flat disc — header-only scalar rasterisers.
//
// v1.0 makes the cast headshots round (64px with a one-pixel rim). The images
// themselves predate this fork; only the shape changed. This lives in a header
// rather than inside ui_lists.cpp for the same reason wave_ribbon.h does: it is
// pixel arithmetic with clipping on four edges, which is easy to get subtly
// wrong and impossible to debug on a TV. tests/test_circle_blit.c drives the
// SAME code the console runs.
//
// ---------------------------------------------------------------------------
// WHY THE ANTI-ALIASING IS AGAINST THE IMAGE AND NOT THE FRAMEBUFFER
// ---------------------------------------------------------------------------
// The obvious way to round a rectangle is to draw it square and blend a soft
// rim over the top, which is what music_screen.cpp's ring_aa() does. That reads
// video memory for every rim pixel. UI-BRIEF.md measured a blended pixel
// composited against VRAM at ~700 ns; a 64px circle has ~200 pixels of
// circumference and a 2px blend band, so ~600 reads per headshot and ~2.5 ms
// for six. The whole frame's CPU budget is 1,147 us. It would also push `bpx`
// off zero, which is the one number the renderer may not regress.
//
// So every pixel here is a pure WRITE:
//
//   inside r-1    the source pixel, unchanged
//   the rim band  source blended toward the rim colour by coverage, where BOTH
//                 operands are already in hand — nothing is read back
//   outside r     not written at all, so the animated background shows through,
//                 which is what "circular" has to mean over a moving wave
//
// The outer edge is therefore hard rather than feathered, and the rim is what
// hides it: a one-pixel ring reads as a deliberate edge where a bare stair-step
// reads as a bug. THE INVARIANT THE TEST ENFORCES IS THAT NOTHING OUTSIDE THE
// CIRCLE IS EVER WRITTEN — that is what makes it composable over the wave, and
// an off-by-one there corrupts whatever was already on screen.

#include <math.h>

#ifdef __cplusplus
extern "C" {
#endif

// The destination. `pitch` is in PIXELS, not bytes. Rows outside
// [clip_top, clip_bot) are skipped; clip_bot == 0 means "to the bottom edge",
// matching cpu_row_clipped()'s convention on the PS3 side.
typedef struct {
    unsigned int *px;
    int w, h;
    int pitch;
    int clip_top, clip_bot;
} CbSurface;

static inline int cb_row_clipped(const CbSurface *s, int y)
{
    if (y < 0 || y >= s->h) return 1;
    if (y < s->clip_top) return 1;
    if (s->clip_bot && y >= s->clip_bot) return 1;
    return 0;
}

// Blend two known colours; no operand comes from the destination.
static inline unsigned int cb_mix(unsigned int p, unsigned int q, unsigned int a)
{
    unsigned int pr = (p >> 16) & 0xFF, pg = (p >> 8) & 0xFF, pb = p & 0xFF;
    unsigned int qr = (q >> 16) & 0xFF, qg = (q >> 8) & 0xFF, qb = q & 0xFF;
    return (((pr * a + qr * (255 - a)) / 255) << 16)
         | (((pg * a + qg * (255 - a)) / 255) <<  8)
         |  ((pb * a + qb * (255 - a)) / 255);
}

// Nearest-neighbour scale of a sw x sh image into a circle of diameter d whose
// bounding box starts at (dx, dy).
static inline void cb_blit_circle(const CbSurface *s,
                                  const unsigned int *src, int sw, int sh,
                                  int dx, int dy, int d, unsigned int rim)
{
    if (d <= 0 || !src || sw <= 0 || sh <= 0) return;

    const float r    = (float)d * 0.5f;
    const float cxf  = (float)dx + r;
    const float cyf  = (float)dy + r;
    const float r2   = r * r;
    const float r_in = r - 1.0f;
    const float ri2  = r_in > 0.0f ? r_in * r_in : 0.0f;

    const unsigned int qx = (unsigned)sw / (unsigned)d, rx = (unsigned)sw % (unsigned)d;
    const unsigned int qy = (unsigned)sh / (unsigned)d, ry = (unsigned)sh % (unsigned)d;

    unsigned int src_y = 0, accy = 0;
    for (int row = 0; row < d; row++) {
        int sy = dy + row;
        if (!cb_row_clipped(s, sy)) {
            // Solve the circle for this row rather than testing every pixel in
            // the bounding box: above and below the circle there is nothing.
            const float yy  = (float)sy + 0.5f - cyf;
            const float sp2 = r2 - yy * yy;
            if (sp2 > 0.0f) {
                const float span = sqrtf(sp2);
                int c0 = (int)(cxf - span);
                int c1 = (int)(cxf + span) + 1;
                if (c0 < dx)     c0 = dx;
                if (c1 > dx + d) c1 = dx + d;
                if (c0 < 0)      c0 = 0;
                if (c1 > s->w)   c1 = s->w;

                const unsigned int *srow = src + (size_t)src_y * (size_t)sw;
                unsigned int *dst = s->px + (size_t)sy * (size_t)s->pitch;

                // Step the source column up to c0 the way the rectangular blit
                // does, so a clipped circle samples the same pixels an
                // unclipped one would.
                unsigned int sx   = (unsigned)(((long long)(c0 - dx) * sw) / d);
                unsigned int accx = (unsigned)(((long long)(c0 - dx) * (long long)rx) % d);

                for (int col = c0; col < c1; col++) {
                    const float xx = (float)col + 0.5f - cxf;
                    const float dd = xx * xx + yy * yy;
                    if (dd <= r2) {
                        unsigned int p = srow[sx < (unsigned)sw ? sx : (unsigned)sw - 1];
                        if (dd > ri2) {
                            float cov = r - sqrtf(dd);      // 1 -> 0 across 1px
                            if (cov < 0.0f) cov = 0.0f;
                            if (cov > 1.0f) cov = 1.0f;
                            p = cb_mix(p, rim, (unsigned)(cov * 255.0f + 0.5f));
                        }
                        dst[col] = p;
                    }
                    sx += qx;
                    accx += rx;
                    if (accx >= (unsigned)d) { accx -= (unsigned)d; sx++; }
                }
            }
        }
        src_y += qy;
        accy  += ry;
        if (accy >= (unsigned)d) { accy -= (unsigned)d; src_y++; }
    }
}

// Flat disc with the same rim, for the placeholder while a headshot is still
// being fetched. Same write-only contract.
static inline void cb_fill_circle(const CbSurface *s, int dx, int dy, int d,
                                  unsigned int fill, unsigned int rim)
{
    if (d <= 0) return;
    const float r    = (float)d * 0.5f;
    const float cxf  = (float)dx + r, cyf = (float)dy + r;
    const float r2   = r * r;
    const float r_in = r - 1.0f;
    const float ri2  = r_in > 0.0f ? r_in * r_in : 0.0f;

    for (int row = 0; row < d; row++) {
        int sy = dy + row;
        if (cb_row_clipped(s, sy)) continue;
        const float yy  = (float)sy + 0.5f - cyf;
        const float sp2 = r2 - yy * yy;
        if (sp2 <= 0.0f) continue;
        const float span = sqrtf(sp2);
        int c0 = (int)(cxf - span), c1 = (int)(cxf + span) + 1;
        if (c0 < dx)     c0 = dx;
        if (c1 > dx + d) c1 = dx + d;
        if (c0 < 0)      c0 = 0;
        if (c1 > s->w)   c1 = s->w;
        unsigned int *dst = s->px + (size_t)sy * (size_t)s->pitch;
        for (int col = c0; col < c1; col++) {
            const float xx = (float)col + 0.5f - cxf;
            const float dd = xx * xx + yy * yy;
            if (dd <= r2) dst[col] = (dd > ri2) ? rim : fill;
        }
    }
}

#ifdef __cplusplus
}
#endif
