// JellyWave: the wave answers the pad -- the idea behind the console's own
// PARTICLES_UI.mnu, where the D-pad turns the field, moving through the icon
// column blows the sparkles along and a fast scroll shakes it.  Only the
// RELATIONSHIPS are taken (dpad x rotates, icon wind is vertical, brownian
// 0.60 in UI mode against 0.225 ambient, a shake past a threshold); the
// numbers here are this renderer's own.
//
// THREE RESPONSES, all neutral at rest, all decaying back to exactly nothing:
//
//   push    each LEFT / RIGHT step sends a soft dip along the band in the
//           direction the menu content moves (RIGHT moves the categories
//           left, so the dip runs left).  A dip, never a crest: displacement
//           is only ever NEGATIVE (down, away from the card grid), so this can
//           never spend the framing budget the audio already uses up.
//   bob     each UP / DOWN step presses the band down a little; a damped
//           spring brings it back.  Clamped at 0 -- it never rises.
//   gust    wind for the particles, the way the content moves (UP blows them
//           down, DOWN blows them up, RIGHT blows them left), plus extra
//           jostle while navigating (the stock UI-mode brownian ratio, 2.67x).
//
// and the SHAKE: a held scroll (auto-repeat, ~7 steps/s) is detected from the
// step rate and raises the solver's perturbation, so the whole band shivers
// while the list races, and settles when it stops.
//
// House rules: header-only, pure C, no libm, no PS3 headers, caller-owned
// state.  tests/test_wave_nav.c.

#ifndef WAVE_NAV_H
#define WAVE_NAV_H

#define WNV_PUSHES      3
#define WNV_PUSH_A      0.045f    // dip depth, solver units (a beat warp is 0.050, upward)
#define WNV_PUSH_LIFE   1.40f     // s
#define WNV_PUSH_SPEED  0.45f     // band fractions per second
#define WNV_PUSH_W0     0.09f     // half-width at birth, band fractions ...
#define WNV_PUSH_WG     0.10f     // ... growing this much per second
#define WNV_LAYER_LAG   0.09f     // s, near -> mid -> far
#define WNV_DIP_MAX     0.060f    // deepest total dip, x layer weight (== WDF_NEG_MAX)

#define WNV_BOB_KICK    0.55f     // world units/s of downward velocity per step
#define WNV_BOB_K       40.0f     // spring, 1/s^2 (~1 Hz)
#define WNV_BOB_C       9.0f      // damping, 1/s (~0.7 critical)
#define WNV_BOB_MAX     0.10f     // deepest press, world units (~27 px at 1080p)

#define WNV_GUST_TAU    0.45f     // s
#define WNV_GUST_MAX    1.5f      // stacked steps
#define WNV_WIND_X      0.10f     // clip units/s at one fresh step
#define WNV_WIND_Y      0.08f
#define WNV_JOSTLE      1.67f     // extra brownian at full activity (2.67x the ambient)

#define WNV_RATE_TAU    0.60f     // s, the step-rate estimate's memory
#define WNV_SHAKE_ON    4.5f      // steps/s where the shake begins ...
#define WNV_SHAKE_FULL  6.5f      // ... and where it is full (auto-repeat is ~7.1)
#define WNV_SHAKE_UP    0.15f     // s
#define WNV_SHAKE_DOWN  0.50f     // s
#define WNV_SHAKE_PERT  2.0f      // perturbation x (1 + this) at full shake
#define WNV_SHIVER_A    0.014f    // the shiver's depth at full shake, solver units
#define WNV_SHIVER_K    9.0f      // its cycles along the band
#define WNV_SHIVER_HZ   2.6f      // and how fast it runs

#define WNV_DT_MAX      0.10f

static const float WNV_LAYER_W[3] = { 1.00f, 0.80f, 0.60f };   // near, mid, far
static const float WNV_BOB_W[3]   = { 1.00f, 0.75f, 0.50f };

typedef struct {
    float push_x[WNV_PUSHES], push_age[WNV_PUSHES], push_dir[WNV_PUSHES], push_a[WNV_PUSHES];
    float bob, bob_v;           // world units (<= 0) and world units/s
    float gust_x, gust_y;       // in wind directions, decaying
    float energy;               // decaying step count: rate = energy / WNV_RATE_TAU
    float shake;                // 0..1
    float t;                    // running time, s (the shiver's phase)
} wnv_state;

// What the loft reads, as a value: copied into the generation worker's job.
typedef struct {
    float push_x[WNV_PUSHES], push_age[WNV_PUSHES], push_dir[WNV_PUSHES], push_a[WNV_PUSHES];
    float bob[3];               // per solver layer, world units, <= 0
    float shake, t;             // the shiver
    int   live;                 // 0 = add nothing
} wnv_look;

// What the particles and the solver read.
typedef struct {
    float wind_x, wind_y;       // clip units/s
    float jostle;               // extra brownian multiplier (0 at rest)
    float perturb;              // wf_step perturbation multiplier (1 at rest)
} wnv_fx;

static inline float wnv_clampf(float v, float lo, float hi)
{
    if (v != v) return lo;
    return v < lo ? lo : (v > hi ? hi : v);
}

static inline float wnv_smooth01(float x)
{
    x = wnv_clampf(x, 0.0f, 1.0f);
    return x * x * (3.0f - 2.0f * x);
}

static inline void wnv_init(wnv_state *st)
{
    int i;
    if (!st) return;
    for (i = 0; i < WNV_PUSHES; i++) {
        st->push_x[i] = 0.5f; st->push_age[i] = 9.0f;
        st->push_dir[i] = 1.0f; st->push_a[i] = 0.0f;
    }
    st->bob = st->bob_v = 0.0f;
    st->gust_x = st->gust_y = 0.0f;
    st->energy = 0.0f;
    st->shake = 0.0f;
    st->t = 0.0f;
}

// One navigation step.  dx: -1 left, +1 right.  dy: -1 up, +1 down.
static inline void wnv_event(wnv_state *st, int dx, int dy)
{
    if (!st || (!dx && !dy)) return;
    st->energy += 1.0f;
    if (dx) {
        // The content moves opposite to the press; the dip goes with it,
        // entering from the side the content is coming from.
        const float dir = dx > 0 ? -1.0f : 1.0f;
        int j, slot = 0;
        for (j = 1; j < WNV_PUSHES; j++)          // the oldest (or empty) slot
            if (st->push_age[j] > st->push_age[slot]) slot = j;
        st->push_x[slot]   = dir < 0.0f ? 0.80f : 0.20f;
        st->push_dir[slot] = dir;
        st->push_age[slot] = 0.0f;
        st->push_a[slot]   = WNV_PUSH_A;
        st->gust_x = wnv_clampf(st->gust_x + dir, -WNV_GUST_MAX, WNV_GUST_MAX);
    }
    if (dy) {
        st->bob_v -= WNV_BOB_KICK;
        // DOWN scrolls the list up, and the particles go with it
        st->gust_y = wnv_clampf(st->gust_y + (dy > 0 ? 1.0f : -1.0f), -WNV_GUST_MAX, WNV_GUST_MAX);
    }
}

static inline void wnv_step(wnv_state *st, float dt, wnv_fx *fx)
{
    int j;
    if (!st) return;
    dt = wnv_clampf(dt, 0.0f, WNV_DT_MAX);
    st->t += dt;
    if (st->t > 1000.0f) st->t -= 1000.0f;

    for (j = 0; j < WNV_PUSHES; j++) {
        st->push_age[j] += dt;
        if (st->push_age[j] > 9.0f) st->push_age[j] = 9.0f;
        if (st->push_age[j] >= WNV_PUSH_LIFE + 2.0f * WNV_LAYER_LAG) st->push_a[j] = 0.0f;
    }

    // the bob: semi-implicit spring toward 0, never above it
    st->bob_v += (-WNV_BOB_K * st->bob - WNV_BOB_C * st->bob_v) * dt;
    st->bob   += st->bob_v * dt;
    if (st->bob > 0.0f) { st->bob = 0.0f; if (st->bob_v > 0.0f) st->bob_v = 0.0f; }
    if (st->bob < -WNV_BOB_MAX) { st->bob = -WNV_BOB_MAX; if (st->bob_v < 0.0f) st->bob_v = 0.0f; }
    if (st->bob > -1e-5f && st->bob_v > -1e-4f && st->bob_v < 1e-4f) { st->bob = 0.0f; st->bob_v = 0.0f; }

    {
        const float k = 1.0f / (1.0f + dt / WNV_GUST_TAU);
        st->gust_x *= k; st->gust_y *= k;
        if (st->gust_x > -1e-4f && st->gust_x < 1e-4f) st->gust_x = 0.0f;
        if (st->gust_y > -1e-4f && st->gust_y < 1e-4f) st->gust_y = 0.0f;
    }
    st->energy *= 1.0f / (1.0f + dt / WNV_RATE_TAU);
    if (st->energy < 1e-4f) st->energy = 0.0f;
    {
        const float rate = st->energy / WNV_RATE_TAU;
        const float want = wnv_smooth01((rate - WNV_SHAKE_ON) / (WNV_SHAKE_FULL - WNV_SHAKE_ON));
        const float tau  = want > st->shake ? WNV_SHAKE_UP : WNV_SHAKE_DOWN;
        st->shake += (want - st->shake) * (dt / (tau + dt));
        if (want == 0.0f && st->shake < 0.005f) st->shake = 0.0f;   // rest is exactly rest
    }

    if (fx) {
        const float act = wnv_clampf(st->energy, 0.0f, 1.0f);
        fx->wind_x  = WNV_WIND_X * st->gust_x;
        fx->wind_y  = WNV_WIND_Y * st->gust_y;
        fx->jostle  = WNV_JOSTLE * (act > st->shake ? act : st->shake);
        fx->perturb = 1.0f + WNV_SHAKE_PERT * st->shake;
    }
}

static inline void wnv_snapshot(const wnv_state *st, wnv_look *o)
{
    int j, li;
    if (!o) return;
    o->live = 0;
    for (j = 0; j < WNV_PUSHES; j++) {
        o->push_x[j]   = st ? st->push_x[j]   : 0.5f;
        o->push_age[j] = st ? st->push_age[j] : 9.0f;
        o->push_dir[j] = st ? st->push_dir[j] : 1.0f;
        o->push_a[j]   = st ? st->push_a[j]   : 0.0f;
        if (o->push_a[j] > 0.0f) o->live = 1;
    }
    for (li = 0; li < 3; li++) o->bob[li] = st ? st->bob * WNV_BOB_W[li] : 0.0f;
    o->shake = st ? st->shake : 0.0f;
    o->t     = st ? st->t : 0.0f;
    if (st && (st->bob < 0.0f || st->shake > 0.0f)) o->live = 1;
}

static inline void wnv_rest(wnv_look *o) { wnv_snapshot(0, o); }

// A push's envelope at age a: eases in over 0.12 s, fades over the last 0.5 s.
static inline float wnv_env(float a)
{
    if (a <= 0.0f || a >= WNV_PUSH_LIFE) return 0.0f;
    return wnv_smooth01(a / 0.12f) * (1.0f - wnv_smooth01((a - (WNV_PUSH_LIFE - 0.5f)) / 0.5f));
}

// sin(2 pi t), libm-free (Bhaskara on the folded phase, < 0.2%)
static inline float wnv_sin2pi(float t)
{
    float f = t - (float)(int)t;
    if (f < 0.0f) f += 1.0f;
    float s = 1.0f;
    if (f >= 0.5f) { f -= 0.5f; s = -1.0f; }
    const float x = f * (0.5f - f);
    return s * 16.0f * x / (1.25f - 4.0f * x);
}

// A smooth bump, 1 at q = 0, 0 from |q| = 2 out.
static inline float wnv_bump(float q)
{
    float t = 1.0f - q * q * 0.25f;
    if (t <= 0.0f) return 0.0f;
    return t * t * t;
}

// Add the pushes to one layer's copy of the curve (u in [0,1] over n
// samples, as wave_field.h's).  Only ever moves it DOWN.
static inline void wnv_apply(const wnv_look *d, int layer, float *disp, int n)
{
    int k, j;
    float du, wl, lim;
    if (!d || !d->live || !disp || n < 2) return;
    if (layer < 0) layer = 0;
    if (layer > 2) layer = 2;
    wl  = WNV_LAYER_W[layer];
    lim = WNV_DIP_MAX * wl;
    du  = 1.0f / (float)(n - 1);
    for (k = 0; k < n; k++) {
        const float u = (float)k * du;
        float x = 0.0f;
        for (j = 0; j < WNV_PUSHES; j++) {
            const float a   = d->push_age[j] - (float)layer * WNV_LAYER_LAG;
            const float env = d->push_a[j] > 0.0f ? wnv_env(a) : 0.0f;
            if (env <= 0.0f) continue;
            {
                const float xc = d->push_x[j] + d->push_dir[j] * WNV_PUSH_SPEED * a;
                const float w  = WNV_PUSH_W0 + WNV_PUSH_WG * a;
                x += d->push_a[j] * env * wnv_bump((u - xc) / w);
            }
        }
        // the shiver: a fine ripple running along the band while a scroll
        // races -- 0..1 rather than -1..1, so it too only ever dips
        if (d->shake > 0.0f)
            x += d->shake * WNV_SHIVER_A
               * (0.5f + 0.5f * wnv_sin2pi(WNV_SHIVER_K * u - WNV_SHIVER_HZ * d->t + 0.2f * (float)layer));
        if (x <= 0.0f) continue;
        x *= wl;
        // soft limit: untouched up to 70% of lim (one push), then a knee with
        // a continuous slope that never passes lim however many stack
        {
            const float knee = 0.7f * lim, room = lim - knee;
            if (x > knee) { const float e = x - knee; x = knee + room * e / (e + room); }
        }
        disp[k] -= x;
    }
}

#endif // WAVE_NAV_H
