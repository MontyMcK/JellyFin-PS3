// JellyWave: the snow field -- screen-wide drifting particles while music plays.
//
// Replaces the stage 9 motes (wave_motes.h, kept, unwired) as what the music
// screen draws.  Hardware verdict on the motes: "need to be much better and
// present throughout the whole screen, kinda like a snowing vibe".
//
// WHAT IT IS.  Original work; the reference is only the RELATIONSHIPS in the
// console's own ambient field (lines.qrc, read as parameters, never copied):
//   * a screen-filling population in the low thousands, not a cloud around one
//     attractor -- here ~700, sized for this renderer's upload budget;
//   * near-weightless drift: a slow fall, a light wind, Brownian jostle and a
//     friction that makes every push decay, so nothing ever moves fast;
//   * lifetimes of seconds with wide variance, so births are spread out;
//   * DEPTH OF FIELD doing most of the look: near particles are large, soft and
//     faint (bokeh), a middle band is small, sharp and bright, far ones are
//     tiny and dim; parallax makes the near ones fall and sway faster;
//   * a "shake" response -- an impulse that jolts the whole field -- which is
//     what the sub-bass hits drive here.
//
// Space: x and y in clip units (the screen is [-1,1] both ways, +y up), with a
// margin so particles enter and leave off-screen; z is a depth fraction,
// 0 = nearest .. 1 = furthest.
//
// THE AUDIO (all through ws_ctl, all neutral at 0):
//   sway     lows        wider, slower side-to-side drift
//   twinkle  highs       per-particle sparkle
//   kick     sub-bass    a faint glint only -- it moves NOTHING (2026-09-25
//                        v2: throwing the field up on every hit kept pushing
//                        the particles off the top of the screen)
//   bright   loudness    the whole field a little brighter
//
// House rules: header-only, pure C, no libm, no PS3 headers, caller-owned
// state, struct of arrays.  tests/test_wave_snow.c.

#ifndef WAVE_SNOW_H
#define WAVE_SNOW_H

#include <stdint.h>

#define WS_MAX        900
#define WS_COUNT_DEF  450         // 700 before: "a bit less overwhelming, more spread out"
#define WS_GRID_X     30          // stratified spawn: one jittered cell per particle
#define WS_GRID_Y     15

#define WS_X_EDGE     1.12f       // spawn/wrap margin past the screen, clip units
#define WS_Y_EDGE     1.12f
#define WS_FALL_NEAR  0.090f      // clip units/s at z = 0 (parallax)
#define WS_FALL_FAR   0.022f      // at z = 1
#define WS_WIND       0.012f      // steady drift to the right
#define WS_SWAY       0.030f      // side-to-side amplitude of the drift velocity
#define WS_SWAY_HZ    0.11f
#define WS_BROWN      0.060f      // jostle, clip units/s^2 per sqrt-frame
#define WS_FRICTION   1.6f        // 1/s -- how fast a push dies away
#define WS_KICK_FLASH 0.30f       // glint at a full kick, of the full flash
#define WS_FLASH_TAU  0.22f       // s
#define WS_LIFE_MIN   5.0f
#define WS_LIFE_MAX  12.0f
#define WS_FADE       1.0f        // s, in and out
#define WS_DT_MAX     0.10f

// depth of field, in pixels at 1080p (the renderer scales by display height)
#define WS_FOCUS_Z    0.45f
#define WS_R_SHARP    1.3f        // in-focus core radius
#define WS_R_NEAR    11.0f        // extra radius at the nearest (bokeh); 16 before
#define WS_R_FAR      0.9f

#define WS_WV         16          // wave velocity samples across the screen

// OBSTACLES.  The screen's solid things -- the cover, the text block, the
// Up Next panel, the controls -- as rectangles in clip space (y up).  A
// particle NEARER than WS_COLLIDE_Z cannot enter one: it is pushed back to
// the surface it crossed, the part of its motion heading in is reflected
// (WS_RESTITUTION) and the flow then carries it along the edge; each hit
// leaves a brief glint.  Further particles pass behind, so the boxes sit IN
// the space rather than on top of it.  ~4 rectangle tests per near particle.
#define WS_OBST_MAX     12        // the cover, each text line, each control, the panel
#define WS_COLLIDE_Z    0.62f
#define WS_RESTITUTION  0.55f
#define WS_SPARK_TAU    0.25f

typedef struct { float x0, y0, x1, y1; } ws_rect;

typedef struct {
    float sway;     // 0..1 lows: stronger, faster swirl
    float twinkle;  // 0..1 highs
    float kick;     // 0..1, non-zero on the frame a sub-bass hit lands
    float bright;   // 0..1 loudness
    const float *wv;  // WS_WV vertical velocities of the wave (clip units/s,
                      // +up) at evenly spaced x over [-1,1]; NULL = none
    float band_y;     // the wave's resting centre, clip y
    const ws_rect *obst;  // obstacles, clip space; NULL = none
    int   n_obst;
    // a beat's shockwave: a ring expanding from (ring_x, ring_y) in clip
    // space, radius ring_r (measured in screen-x units), strength ring_a (0 =
    // none).  Particles it passes glint, and are nudged outward VERY slightly.
    float ring_x, ring_y, ring_r, ring_a;
    float calm;       // 0..1: a vocal-led passage -- slower, stiller particles
    // The pad (wave_nav.h): wind the way the menu content moves, clip
    // units/s at the middle depth (near ones go further -- parallax), and
    // extra Brownian jostle while navigating (0 = none).
    float wind_x, wind_y;
    float jostle;
} ws_ctl;

#define WS_RING_W     0.09f       // the ring's half-thickness
#define WS_RING_PUSH  0.035f      // outward nudge at full strength, clip units/s
#define WS_ASPECT     1.7777778f

// 2026-09-25 v3: "less like snow and more like swirling floatiness that
// reacts to the wave".  No fall any more: every particle rides a slowly
// turning, divergence-free flow (the curl of a sum of drifting sine cells),
// so the field swirls in eddies and fills the whole screen evenly; the lows
// stir it harder, and near the band the WAVE'S OWN vertical motion carries
// the particles -- a crest rising lifts what floats above it, a ripple runs
// through them as it runs along the band.
#define WS_SWIRL      0.055f      // flow speed, clip units/s
#define WS_WAVE_K     0.55f       // share of the wave's velocity passed on
#define WS_WAVE_REACH 0.75f       // clip units above/below the band it reaches

typedef struct {
    int      n;
    float    t;                     // running time, s
    float    flash;                 // 0..1, decays
    float    x[WS_MAX], y[WS_MAX], z[WS_MAX];
    float    vx[WS_MAX], vy[WS_MAX];
    float    ph[WS_MAX];            // sway / twinkle phase
    float    hue[WS_MAX];           // 0 violet .. 1 blue
    float    age[WS_MAX], life[WS_MAX];
    float    spark[WS_MAX];         // impact glint, 0..1, decays
    uint32_t rng;
} ws_state;

typedef struct {
    float         x, y;             // clip centre
    float         r_px;             // radius, pixels at 1080p
    unsigned char r, g, b, a;       // a == 0: do not draw
} ws_sprite;

static inline float ws_rand(uint32_t *s)
{
    *s = *s * 1664525u + 1013904223u;
    return (float)(*s >> 8) * (1.0f / 16777216.0f);
}

static inline float ws_clampf(float v, float lo, float hi)
{
    if (v != v) return lo;
    return v < lo ? lo : (v > hi ? hi : v);
}

// sin(2 pi t) for any t, libm-free (Bhaskara on the folded phase, < 0.2%)
static inline float ws_sin2pi(float t)
{
    float f = t - (float)(int)t;
    if (f < 0.0f) f += 1.0f;
    float s = 1.0f;
    if (f >= 0.5f) { f -= 0.5f; s = -1.0f; }
    const float x = f * (0.5f - f);            // f in [0, 0.5): sin(2 pi f)
    return s * 16.0f * x / (1.25f - 4.0f * x);
}

// Depth distribution: most particles mid-to-far, a few near (the bokeh).
static inline float ws_pick_z(uint32_t *s)
{
    const float u = ws_rand(s);
    return u < 0.08f ? 0.02f + 0.20f * (u / 0.08f)             // ~8% near: the bokeh
                     : 0.22f + 0.78f * ((u - 0.08f) / 0.92f);  // the rest, mid to far
}

static inline void ws_spawn(ws_state *st, int i, int anywhere)
{
    uint32_t *r = &st->rng;
    // Stratified: particle i always respawns somewhere in ITS cell of a
    // jittered grid, so the field stays evenly spread instead of clumping.
    {
        const int   cx = i % WS_GRID_X, cy = (i / WS_GRID_X) % WS_GRID_Y;
        const float cw = 2.0f * WS_X_EDGE / (float)WS_GRID_X;
        const float ch = 2.0f * WS_Y_EDGE / (float)WS_GRID_Y;
        st->x[i] = -WS_X_EDGE + cw * ((float)cx + ws_rand(r));
        st->y[i] = anywhere ? (-WS_Y_EDGE + ch * ((float)cy + ws_rand(r)))
                            : WS_Y_EDGE - 0.05f * ws_rand(r);
    }
    st->spark[i] = 0.0f;
    st->z[i]  = ws_pick_z(r);
    st->vx[i] = 0.0f;
    st->vy[i] = 0.0f;
    st->ph[i] = ws_rand(r);
    st->hue[i] = ws_rand(r);
    st->life[i] = WS_LIFE_MIN + (WS_LIFE_MAX - WS_LIFE_MIN) * ws_rand(r);
    st->age[i]  = anywhere ? st->life[i] * ws_rand(r) : 0.0f;
}

static inline int ws_init(ws_state *st, int count, uint32_t seed)
{
    int i;
    if (!st) return 0;
    if (count < 0) count = 0;
    if (count > WS_MAX) count = WS_MAX;
    st->n = count;
    st->t = 0.0f;
    st->flash = 0.0f;
    st->rng = seed ? seed : 0x5EEDu;
    for (i = 0; i < count; i++) ws_spawn(st, i, 1);
    return 1;
}

static inline void ws_step(ws_state *st, const ws_ctl *c, float dt)
{
    int i;
    ws_ctl z0 = { 0.0f, 0.0f, 0.0f, 0.0f, 0, 0.0f };
    if (!st) return;
    if (!c) c = &z0;
    dt = ws_clampf(dt, 0.0f, WS_DT_MAX);
    st->t += dt;

    const float kick  = ws_clampf(c->kick, 0.0f, 1.0f);
    const float calmk = 1.0f - 0.45f * ws_clampf(c->calm, 0.0f, 1.0f);   // vocal calm: stiller
    const float stir  = calmk * 1.0f + 1.3f * calmk * ws_clampf(c->sway, 0.0f, 1.0f)
                             + 0.5f * ws_clampf(c->bright, 0.0f, 1.0f);
    const float sway  = WS_SWAY * stir;
    const float T     = st->t * 0.045f;
    const float fric  = 1.0f / (1.0f + WS_FRICTION * dt);
    const float brown = WS_BROWN * dt * calmk * (1.0f + ws_clampf(c->jostle, 0.0f, 3.0f));
    const float wnd_x = ws_clampf(c->wind_x, -0.5f, 0.5f);
    const float wnd_y = ws_clampf(c->wind_y, -0.5f, 0.5f);
    st->flash = st->flash * (1.0f / (1.0f + dt / WS_FLASH_TAU));
    if (kick * WS_KICK_FLASH > st->flash) st->flash = kick * WS_KICK_FLASH;

    for (i = 0; i < st->n; i++) {
        const float z    = st->z[i];
        const float near = 1.0f - z;
        // the swirl: velocity = curl of psi, psi = sum of drifting cells
        //   psi_k = A_k sin(2pi(fx_k x + a_k)) sin(2pi(fy_k y + b_k))
        const float x = st->x[i], y = st->y[i];
        float fu = 0.0f, fv = 0.0f;
        {
            static const float FX[3] = { 0.55f, 0.95f, 1.60f };
            static const float FY[3] = { 0.60f, 1.05f, 1.45f };
            static const float AK[3] = { 1.00f, 0.55f, 0.28f };
            static const float WK[3] = { 1.00f, -1.40f, 2.10f };
            int k;
            for (k = 0; k < 3; k++) {
                const float px = FX[k] * x + T * WK[k] + 0.13f * k;
                const float py = FY[k] * y - T * WK[k] * 0.8f + 0.29f * k;
                const float sx = ws_sin2pi(px), cx = ws_sin2pi(px + 0.25f);
                const float sy = ws_sin2pi(py), cy = ws_sin2pi(py + 0.25f);
                // dpsi/dy and -dpsi/dx (the 2pi f folds into the gain)
                fu +=  AK[k] * FY[k] * sx * cy;
                fv += -AK[k] * FX[k] * cx * sy;
            }
        }
        const float par  = 0.45f + 0.75f * near;           // parallax
        const float fall = 0.0f;
        const float sw   = WS_SWIRL * stir * par * fu
                         + sway * 0.3f * ws_sin2pi(st->ph[i] + st->t * WS_SWAY_HZ);
        const float swv  = WS_SWIRL * stir * par * fv;
        // the wave: its vertical motion carries what floats near it
        if (c->wv) {
            const float dy = y - c->band_y;
            float w = 1.0f - (dy < 0.0f ? -dy : dy) / WS_WAVE_REACH;
            if (w > 0.0f) {
                float s = (x + 1.0f) * 0.5f * (float)(WS_WV - 1);
                int   k = (int)s;
                if (s < 0.0f) { s = 0.0f; k = 0; }
                if (k > WS_WV - 2) { k = WS_WV - 2; s = (float)(WS_WV - 1); }
                const float fr = s - (float)k;
                const float v  = c->wv[k] + (c->wv[k + 1] - c->wv[k]) * fr;
                st->vy[i] += WS_WAVE_K * v * w * w * par * WS_FRICTION * dt;
            }
        }
        // the push: Brownian jostle, damped
        st->vx[i] = st->vx[i] * fric + brown * (ws_rand(&st->rng) - 0.5f);
        st->vy[i] = st->vy[i] * fric + brown * (ws_rand(&st->rng) - 0.5f);
        {
            const float fu = WS_WIND * 0.3f * (0.5f + 0.5f * near) + sw     // the flow, x
                           + wnd_x * par;                                  // + the pad's gust
            const float fv = swv - fall + wnd_y * par;                     // and y
            st->x[i] += (fu + st->vx[i]) * dt;
            st->y[i] += (st->vy[i] + fv) * dt;
            st->spark[i] *= 1.0f / (1.0f + dt / WS_SPARK_TAU);
            if (c->ring_a > 0.0f) {
                const float dx = (st->x[i] - c->ring_x) * WS_ASPECT, dy = st->y[i] - c->ring_y;
                const float d2 = dx * dx + dy * dy;
                // |d - r| < w, tested on squares to keep the sqrt off the
                // common path; the few inside the band take one
                const float lo = c->ring_r - WS_RING_W, hi = c->ring_r + WS_RING_W;
                if (d2 < hi * hi && (lo <= 0.0f || d2 > lo * lo)) {
                    float d = d2, gx, gy, w;
                    {   // sqrt: bit estimate + two Newton steps
                        union { float f; uint32_t u; } v; v.f = d2 > 1e-12f ? d2 : 1e-12f;
                        v.u = (v.u >> 1) + 0x1FBD1DF5u;
                        d = v.f; d = 0.5f * (d + d2 / d); d = 0.5f * (d + d2 / d);
                    }
                    w = 1.0f - (d > c->ring_r ? d - c->ring_r : c->ring_r - d) / WS_RING_W;
                    if (w > 0.0f) {
                        const float gl = c->ring_a * w;
                        if (gl > st->spark[i]) st->spark[i] = gl > 1.0f ? 1.0f : gl;
                        gx = d > 1e-4f ? dx / d : 0.0f; gy = d > 1e-4f ? dy / d : 0.0f;
                        st->vx[i] += gx * WS_RING_PUSH * gl * (1.0f / WS_ASPECT) * par * 4.0f * dt;
                        st->vy[i] += gy * WS_RING_PUSH * gl * par * 4.0f * dt;
                    }
                }
            }
            if (c->obst && c->n_obst > 0 && z < WS_COLLIDE_Z) {
                // the particle's own size, so its edge (not its centre) touches
                const float rpx = WS_R_SHARP + 1.2f * near
                                + WS_R_NEAR * (z < 0.22f ? ((0.22f - z) / 0.22f) * ((0.22f - z) / 0.22f) : 0.0f);
                const float ry = rpx * (1.0f / 540.0f), rx = rpx * (1.0f / 960.0f);
                int k;
                for (k = 0; k < c->n_obst && k < WS_OBST_MAX; k++) {
                    const ws_rect *o = &c->obst[k];
                    const float x0 = o->x0 - rx, x1 = o->x1 + rx, y0 = o->y0 - ry, y1 = o->y1 + ry;
                    const float px = st->x[i], py = st->y[i];
                    if (px <= x0 || px >= x1 || py <= y0 || py >= y1) continue;
                    {
                        const float dl = px - x0, dr = x1 - px, db = py - y0, dtp = y1 - py;
                        const float nx = fu + st->vx[i], ny = fv + st->vy[i];
                        float m = dl; int side = 0;
                        if (dr < m) { m = dr; side = 1; }
                        if (db < m) { m = db; side = 2; }
                        if (dtp < m) { m = dtp; side = 3; }
                        if (side == 0) { st->x[i] = x0; if (nx > 0.0f) st->vx[i] = -nx * WS_RESTITUTION - fu; }
                        else if (side == 1) { st->x[i] = x1; if (nx < 0.0f) st->vx[i] = -nx * WS_RESTITUTION - fu; }
                        else if (side == 2) { st->y[i] = y0; if (ny > 0.0f) st->vy[i] = -ny * WS_RESTITUTION - fv; }
                        else { st->y[i] = y1; if (ny < 0.0f) st->vy[i] = -ny * WS_RESTITUTION - fv; }
                        {
                            const float sp = (side < 2 ? (nx < 0.0f ? -nx : nx) : (ny < 0.0f ? -ny : ny)) * 8.0f;
                            if (sp > st->spark[i]) st->spark[i] = sp > 1.0f ? 1.0f : sp;
                        }
                    }
                }
            }
        }
        st->age[i] += dt;

        // run out of life: reborn anywhere (fading in); every edge wraps
        if (st->age[i] >= st->life[i]) { ws_spawn(st, i, 1); st->age[i] = 0.0f; }
        if (st->y[i] >  WS_Y_EDGE) st->y[i] -= 2.0f * WS_Y_EDGE;
        if (st->y[i] < -WS_Y_EDGE) st->y[i] += 2.0f * WS_Y_EDGE;
        if (st->x[i] >  WS_X_EDGE) st->x[i] -= 2.0f * WS_X_EDGE;
        if (st->x[i] < -WS_X_EDGE) st->x[i] += 2.0f * WS_X_EDGE;
    }
}

// Shade: one sprite per particle, in state order.  Returns how many are drawn.
// `alpha` is the presence fade (0..1); colours are Jellyfin violet -> blue,
// lifted toward white in focus.  The fade is STAGGERED: each particle has its
// own threshold, so as alpha rises they appear one by one, each on its own
// smooth curve, instead of the whole field brightening as one sheet.
static inline int ws_shade(const ws_state *st, const ws_ctl *c, float alpha,
                           ws_sprite *out, int cap)
{
    int i, n, vis = 0;
    ws_ctl z0 = { 0.0f, 0.0f, 0.0f, 0.0f, 0, 0.0f };
    if (!st || !out || cap <= 0) return 0;
    if (!c) c = &z0;
    n = st->n < cap ? st->n : cap;
    alpha = ws_clampf(alpha, 0.0f, 1.0f);
    const float tw  = ws_clampf(c->twinkle, 0.0f, 1.0f);
    const float brt = 1.0f + 0.25f * ws_clampf(c->bright, 0.0f, 1.0f) + 0.55f * st->flash;

    for (i = 0; i < n; i++) {
        const float z     = st->z[i];
        const float nearb = ws_clampf((0.22f - z) / 0.22f, 0.0f, 1.0f);   // bokeh 0..1
        const float farf  = ws_clampf((z - WS_FOCUS_Z) / (1.0f - WS_FOCUS_Z), 0.0f, 1.0f);
        const float focus = 1.0f - ws_clampf((z < WS_FOCUS_Z ? WS_FOCUS_Z - z : z - WS_FOCUS_Z) / 0.35f, 0.0f, 1.0f);
        float r = WS_R_SHARP + 1.2f * (1.0f - z) + WS_R_NEAR * nearb * nearb - (WS_R_SHARP - WS_R_FAR) * farf;
        if (r < 0.8f) r = 0.8f;

        // life fade in and out
        float e = st->age[i] < st->life[i] - st->age[i] ? st->age[i] : st->life[i] - st->age[i];
        e = ws_clampf(e / WS_FADE, 0.0f, 1.0f);
        e = e * e * (3.0f - 2.0f * e);

        // a big soft disc spreads its light: dim it by its area, or the near
        // ones would read as fog
        float a = 0.80f * (0.30f + 0.62f * focus) * (1.0f - 0.82f * nearb) * (1.0f - 0.55f * farf)
                + 0.45f * st->spark[i];   // the glint of a hit
        // highs: each particle twinkles on its own phase
        const float sp = 0.5f + 0.5f * ws_sin2pi(st->ph[i] * 3.7f + st->t * (1.3f + 1.7f * st->hue[i]));
        a *= 1.0f + tw * (0.9f * sp * sp - 0.2f);
        {
            const float thr = st->ph[i] * 7.31f - (float)(int)(st->ph[i] * 7.31f);   // 0..1 per particle
            const float ai  = ws_clampf(alpha * 1.6f - 0.6f * thr, 0.0f, 1.0f);
            a *= e * (ai * ai * (3.0f - 2.0f * ai)) * brt;
        }
        a = ws_clampf(a, 0.0f, 1.0f);

        // colour (2026-09-27): JellyDrop's orbit palette -- violet -> Jellyfin
        // blue -> the cyan rim, lifted toward white so a speck reads as light
        // (wave_drop.h jd_motes_sprite; the user wanted it field-wide) --
        // and toward white again in focus.
        const float h = st->hue[i];
        float cr, cg, cb;
        if (h < 0.5f) {
            const float u = h * 2.0f;
            cr = 170.0f + (0.0f   - 170.0f) * u; cg = 92.0f + (164.0f - 92.0f) * u; cb = 195.0f + (220.0f - 195.0f) * u;
        } else {
            const float u = (h - 0.5f) * 2.0f;
            cr = 0.0f + (46.0f - 0.0f) * u; cg = 164.0f + (214.0f - 164.0f) * u; cb = 220.0f + (255.0f - 220.0f) * u;
        }
        cr = cr * 0.7f + 76.5f; cg = cg * 0.7f + 76.5f; cb = cb * 0.7f + 76.5f;
        const float wt = 0.55f * focus;
        cr += (255.0f - cr) * wt; cg += (255.0f - cg) * wt; cb += (255.0f - cb) * wt;

        out[i].x = st->x[i];
        out[i].y = st->y[i];
        out[i].r_px = r;
        out[i].r = (unsigned char)cr;
        out[i].g = (unsigned char)cg;
        out[i].b = (unsigned char)cb;
        out[i].a = (unsigned char)(a * 255.0f + 0.5f);
        vis += out[i].a != 0;
    }
    return vis;
}

#endif // WAVE_SNOW_H
