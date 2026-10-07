// JellyWave look pass (2026-09-26): the pure parts of five of the six
// changes, so they can be tested on the host.  ui_wave.cpp does the emitting.
//
//   haze      the further layers take on the background's colour -- aerial
//             perspective -- instead of only turning more transparent
//   fringe    a thin strip along each layer's top and bottom silhouette that
//             fades to transparent: the edge anti-aliasing the RSX path never
//             had (no MSAA surface, and none is going to be allocated for a
//             background)
//   glow      a soft additive band behind the near ribbon, the light it gives
//             off falling on the sky around it
//   interp    the 59.94 Hz in-between frames: which fraction to draw, and
//             whether two builds can be blended at all
//
// (The sixth, the drifting key light, lives with the light: wave_light.h's
// jw_key_drift.)
//
// House rules: header-only, pure C, no libm, no PS3 headers.
// tests/test_wave_look.c.

#ifndef WAVE_LOOK_H
#define WAVE_LOOK_H

#include <stdint.h>
#include "wave_gel.h"

// --- haze -------------------------------------------------------------------
// Per SOLVER layer (0 = near .. 2 = far): how far the body colour moves to the
// sky behind it.  The rim (the lit edge) keeps more of itself, as a distant
// highlight does.
static const float JWL_HAZE[3] = { 0.00f, 0.20f, 0.38f };
#define JWL_HAZE_RIM   0.55f          // the rim fades by haze x this

static inline unsigned char jwl_mix8(unsigned char a, float b, float t)
{
    float v = (float)a + (b - (float)a) * t;
    if (!(v > 0.0f)) return 0;
    if (v >= 255.0f) return 255;
    return (unsigned char)(v + 0.5f);
}

// Hazes one finished vertex in place toward the sky colour (sr, sg, sb) that
// sits behind it.  h = 0 leaves it bit for bit.
static inline void jwl_haze(jw_vert *v, float sr, float sg, float sb, float h)
{
    if (!v || !(h > 0.0f)) return;
    if (h > 1.0f) h = 1.0f;
    v->r = jwl_mix8(v->r, sr, h);
    v->g = jwl_mix8(v->g, sg, h);
    v->b = jwl_mix8(v->b, sb, h);
    {
        const float k = 1.0f - JWL_HAZE_RIM * h;
        v->rr = jwl_mix8(v->rr, 0.0f, 1.0f - k);
        v->rg = jwl_mix8(v->rg, 0.0f, 1.0f - k);
        v->rb = jwl_mix8(v->rb, 0.0f, 1.0f - k);
    }
}

// --- silhouette ---------------------------------------------------------------
// At station i: the section points with the highest and lowest screen y among
// the drawable ones -- the band's top and bottom outline there.  The index
// may change from station to station as the ribbon rolls; the outline is
// still the outline.  Returns 0 if the station has nothing drawable.
static inline int jwl_silhouette(const jw_vert *v, int i, int *top, int *bot)
{
    int j, t = -1, b = -1;
    if (!v || i < 0 || i >= JW_STATIONS) return 0;
    for (j = 0; j < JW_SECTION; j++) {
        const jw_vert *p = &v[i * JW_SECTION + j];
        if (!p->ok) continue;
        if (t < 0 || p->y > v[i * JW_SECTION + t].y) t = j;
        if (b < 0 || p->y < v[i * JW_SECTION + b].y) b = j;
    }
    if (t < 0) return 0;
    if (top) *top = t;
    if (bot) *bot = b;
    return 1;
}

// The fringe's width, in pixels: wide enough to cover a stair-step, narrow
// enough to read as an edge rather than a glow.
#define JWL_FRINGE_PX  1.6f

// --- the glow -----------------------------------------------------------------
#define JWL_GLOW_UP    0.18f          // clip units above the band's centre line
#define JWL_GLOW_DOWN  0.40f          // ... and below (light falls downward)
#define JWL_GLOW_A     60             // alpha at the centre line, additive

// --- vertex counts --------------------------------------------------------------
// Two fringe strips per pass per layer, each with a degenerate join in front;
// the glow is two strips (above, below) with one join between them.
#define JWL_FRINGE_VERTS  (2 * (2 * JW_STATIONS + 2))
#define JWL_GLOW_VERTS    (2 * (2 * JW_STATIONS) + 2)

// --- 59.94 Hz in-between frames -----------------------------------------------
//
// The geometry is rebuilt every `every` wave_draw() calls.  Without blending,
// each build is shown for `every` frames and the motion steps at 29.97 Hz (at
// every = 2) -- the choppiness.  With it, the frame `k` calls after a rebuild
// (k = 0 .. every-1) shows the PREVIOUS build blended (k + 1) / every of the
// way to the NEWEST, so every flip moves by the same amount and the last frame
// of the interval lands exactly on the newest build.  The cost is a delay of
// (every - 1) / every of an interval -- 16.7 ms at every = 2.
//
// COUNTED IN FRAMES, NOT MILLISECONDS, on purpose.  The console scans out at
// 59.94 Hz (60000/1001), not 60, and the solver already steps once per call
// rather than per second -- so a fraction taken from a clock would beat
// against the display.  A fraction taken from the call count cannot: it runs
// at whatever rate the flips come, 59.94 here.
static inline float jwl_interp_t(int k, int every)
{
    if (every <= 1) return 1.0f;
    if (k < 0) k = 0;
    if (k > every - 1) k = every - 1;
    return (float)(k + 1) / (float)every;
}

// Two builds can be blended vertex by vertex only if vertex i is the same
// point of the same strip in both: the same strip order in every layer and
// the same counts in every range.  This is a signature of exactly that.
static inline uint32_t jwl_sig_mix(uint32_t h, uint32_t v)
{
    h ^= v + 0x9E3779B9u + (h << 6) + (h >> 2);
    return h;
}

#endif // WAVE_LOOK_H
