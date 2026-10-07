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
 * Day / night, on the XMB's own schedule (lines.qrc BACKGROUND.mnu):
 * NIGHT2DAY 04:00 -> 06:00, DAY2NIGHT 18:00 -> 20:00.  Returns how much "day"
 * to apply at a minute of the local day: 1 by day, 0 by night, smoothstepped
 * across each two-hour ramp.  Pure, like the rest of this file.
 *
 * NIGHT IS THE THEME, BIT FOR BIT.  The palette was tuned after dark, and
 * that is when this client is mostly used, so the look nobody asked to change
 * stays exactly as it is; the DAY look is the new one -- brighter, a flatter
 * sky -- and the two ramps carry a brief warm glow in the top-right corner,
 * the theme's own accent, the way the XMB colours its dusk.
 */
static inline float bg_day_blend(int minute_of_day)
{
    int m = minute_of_day % 1440;
    if (m < 0) m += 1440;
    float t;
    if (m < 4 * 60)        return 0.0f;
    else if (m < 6 * 60)   t = (float)(m - 4 * 60) / 120.0f;
    else if (m < 18 * 60)  return 1.0f;
    else if (m < 20 * 60)  t = 1.0f - (float)(m - 18 * 60) / 120.0f;
    else                   return 0.0f;
    return t * t * (3.0f - 2.0f * t);
}

/* The dawn / dusk glow: 0 by day and by night, 1 half way through a ramp. */
static inline float bg_dusk_glow(float day)
{
    if (!(day > 0.0f) || !(day < 1.0f)) return 0.0f;
    return 4.0f * day * (1.0f - day);
}

#define BG_DAY_TOP_GAIN 1.45f   /* the top of the sky, brighter          */
#define BG_DAY_BOT_LIFT 0.40f   /* the bottom pulled this far to the top  */
#define BG_DAY_BOT_GAIN 1.20f   /* ... and brightened                     */
#define BG_DUSK_MIX     0.35f   /* the glow's share of the top-right corner */
#define BG_DUSK_LUM     0.40f   /* the accent, darkened to sit in the sky  */

static inline u32 bg_mix_u32(u32 a, u32 b, float t)
{
    return (a & 0xFF000000u) |
           ((u32)bg_u8(bg_chan(a, 16) + (bg_chan(b, 16) - bg_chan(a, 16)) * t) << 16) |
           ((u32)bg_u8(bg_chan(a, 8)  + (bg_chan(b, 8)  - bg_chan(a, 8))  * t) << 8) |
            (u32)bg_u8(bg_chan(a, 0)  + (bg_chan(b, 0)  - bg_chan(a, 0))  * t);
}

static inline u32 bg_scale_u32(u32 c, float g)
{
    return (c & 0xFF000000u) |
           ((u32)bg_u8(bg_chan(c, 16) * g) << 16) |
           ((u32)bg_u8(bg_chan(c, 8)  * g) << 8) |
            (u32)bg_u8(bg_chan(c, 0)  * g);
}

/*
 * The day look for a theme background.  day = 0 returns the quad unchanged,
 * bit for bit.  Hue is kept (every channel scales together), so a warm theme
 * stays warm; `accent` is the theme accent, used only for the dusk glow.
 */
static inline bg_quad bg_apply_day(bg_quad q, float day, u32 accent)
{
    if (!(day > 0.0f)) return q;
    if (day > 1.0f) day = 1.0f;
    bg_quad d;
    d.c[BG_TL] = bg_scale_u32(q.c[BG_TL], BG_DAY_TOP_GAIN);
    d.c[BG_TR] = bg_scale_u32(q.c[BG_TR], BG_DAY_TOP_GAIN);
    d.c[BG_BL] = bg_scale_u32(bg_mix_u32(q.c[BG_BL], q.c[BG_TL], BG_DAY_BOT_LIFT), BG_DAY_BOT_GAIN);
    d.c[BG_BR] = bg_scale_u32(bg_mix_u32(q.c[BG_BR], q.c[BG_TR], BG_DAY_BOT_LIFT), BG_DAY_BOT_GAIN);
    for (int i = 0; i < 4; i++) q.c[i] = bg_mix_u32(q.c[i], d.c[i], day);
    {
        const float g = bg_dusk_glow(day);
        if (g > 0.0f)
            q.c[BG_TR] = bg_mix_u32(q.c[BG_TR], bg_scale_u32(accent, BG_DUSK_LUM), BG_DUSK_MIX * g);
    }
    return q;
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
