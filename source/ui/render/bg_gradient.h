#pragma once
#include <ppu-types.h>
#include <math.h>

/*
 * Stable four-corner background helper for the XMB renderer.
 *
 * A two-stop theme background is represented as:
 *   TL = TR = top
 *   BL = BR = bottom
 *
 * The helper is deliberately pure: it reads no clock and has no mutable
 * process-global state. This is important because wave_draw() is called from
 * several XMB screens, while the rest of the application has worker threads.
 */

enum {
    BG_TL = 0,
    BG_TR = 1,
    BG_BL = 2,
    BG_BR = 3
};

typedef struct {
    u32 c[4];
} bg_quad;

static inline bg_quad bg_from_two(u32 top, u32 bot)
{
    bg_quad q = { { top, top, bot, bot } };
    return q;
}

static inline float bg_chan(u32 c, int shift)
{
    return (float)((c >> shift) & 0xFFu);
}

/* Bilinear sample in 0..255 float channel space. */
static inline void bg_sample_f(const bg_quad *q, float u, float v,
                               float *r, float *g, float *b)
{
    if (u < 0.0f) u = 0.0f;
    if (u > 1.0f) u = 1.0f;
    if (v < 0.0f) v = 0.0f;
    if (v > 1.0f) v = 1.0f;

    const float tlr = bg_chan(q->c[BG_TL], 16);
    const float tlg = bg_chan(q->c[BG_TL], 8);
    const float tlb = bg_chan(q->c[BG_TL], 0);
    const float trr = bg_chan(q->c[BG_TR], 16);
    const float trg = bg_chan(q->c[BG_TR], 8);
    const float trb = bg_chan(q->c[BG_TR], 0);
    const float blr = bg_chan(q->c[BG_BL], 16);
    const float blg = bg_chan(q->c[BG_BL], 8);
    const float blb = bg_chan(q->c[BG_BL], 0);
    const float brr = bg_chan(q->c[BG_BR], 16);
    const float brg = bg_chan(q->c[BG_BR], 8);
    const float brb = bg_chan(q->c[BG_BR], 0);

    const float tr = tlr + (trr - tlr) * u;
    const float tg = tlg + (trg - tlg) * u;
    const float tb = tlb + (trb - tlb) * u;
    const float br = blr + (brr - blr) * u;
    const float bg = blg + (brg - blg) * u;
    const float bb = blb + (brb - blb) * u;

    *r = tr + (br - tr) * v;
    *g = tg + (bg - tg) * v;
    *b = tb + (bb - tb) * v;
}

static inline u8 bg_u8(float x)
{
    if (x <= 0.0f) return 0;
    if (x >= 255.0f) return 255;
    return (u8)(x + 0.5f);
}

static inline void bg_sample(const bg_quad *q, float u, float v,
                             u8 *r, u8 *g, u8 *b)
{
    float fr, fg, fb;
    bg_sample_f(q, u, v, &fr, &fg, &fb);
    *r = bg_u8(fr);
    *g = bg_u8(fg);
    *b = bg_u8(fb);
}

/*
 * Small ordered dither for the CPU/emulator raster path. The RSX path uses
 * the same four corner colours and its own interpolation, so this is only
 * a final-pixel operation and never feeds back into the ribbon compositor.
 */
static inline u8 bg_dither_channel(float value, int x, int y)
{
    static const u8 bayer4[16] = {
         0,  8,  2, 10,
        12,  4, 14,  6,
         3, 11,  1,  9,
        15,  7, 13,  5
    };
    const float n = ((float)bayer4[((y & 3) << 2) | (x & 3)] - 7.5f) / 16.0f;
    return bg_u8(value + n);
}
