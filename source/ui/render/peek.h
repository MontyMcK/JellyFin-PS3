// The Triangle quick-peek's motion, with no drawing in it (xmb/ui_peek.cpp
// draws it; tests/test_experience.c tests it).
//
// WHAT IT IS.  On a Movies or TV grid, Triangle turns the focused poster over:
// the card spins about its vertical axis while it lifts out of the grid and
// grows into a 4:3 panel, and its back is the synopsis and cast.  Triangle,
// Circle or any move turns it back and lays it down where it came from.
//
// A real 3D flip is not drawn: the card's width is scaled by |cos theta| about
// its centre, which on a flat quad IS the projection of a rotation seen
// head-on, and the card swaps faces at theta = 90 deg where its width is zero.
// A slight vertical swell at mid-turn (the near edge coming toward the eye)
// sells the depth.  Everything is a pure function of time, so an interrupted
// open reverses smoothly from wherever it had got to.

#ifndef JF_PEEK_H
#define JF_PEEK_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PEEK_OPEN_US   520000.0f
#define PEEK_CLOSE_US  380000.0f

enum { PEEK_OFF = 0, PEEK_OPENING, PEEK_OPEN, PEEK_CLOSING };

typedef struct {
    int      phase;
    uint64_t t0;        // phase began
    float    u0;        // progress when this phase began (0..1)
} peek_anim;

typedef struct { float x, y, w, h; } peek_rect;

typedef struct {
    peek_rect r;         // where to draw the card this frame
    int       back;      // 0 = poster (front), 1 = the panel (back)
    float     u;         // raw progress 0 (in the grid) .. 1 (open)
    float     lift;      // 0..1: how far out of the grid (the dim behind)
    float     content_a; // the back's text, once the turn has finished
} peek_frame;

static inline float peek_clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}
static inline float peek_ease(float t) {        // cubic in-out, no overshoot
    t = peek_clampf(t, 0.0f, 1.0f);
    return t < 0.5f ? 4.0f * t * t * t : 1.0f - 4.0f * (1.0f - t) * (1.0f - t) * (1.0f - t);
}

// Progress 0..1 now.
static inline float peek_progress(const peek_anim *a, uint64_t now) {
    const float dt = now > a->t0 ? (float)(now - a->t0) : 0.0f;
    switch (a->phase) {
    case PEEK_OPENING: return peek_clampf(a->u0 + dt / PEEK_OPEN_US, 0.0f, 1.0f);
    case PEEK_OPEN:    return 1.0f;
    case PEEK_CLOSING: return peek_clampf(a->u0 - dt / PEEK_CLOSE_US, 0.0f, 1.0f);
    default:           return 0.0f;
    }
}

static inline void peek_open(peek_anim *a, uint64_t now) {
    const float u = peek_progress(a, now);
    a->phase = PEEK_OPENING; a->t0 = now; a->u0 = u;
}
static inline void peek_close(peek_anim *a, uint64_t now) {
    if (a->phase == PEEK_OFF) return;
    const float u = peek_progress(a, now);
    a->phase = PEEK_CLOSING; a->t0 = now; a->u0 = u;
}
// Advance phase ends.  Returns the phase.
static inline int peek_tick(peek_anim *a, uint64_t now) {
    const float u = peek_progress(a, now);
    if (a->phase == PEEK_OPENING && u >= 1.0f) { a->phase = PEEK_OPEN; a->t0 = now; a->u0 = 1.0f; }
    if (a->phase == PEEK_CLOSING && u <= 0.0f) { a->phase = PEEK_OFF;  a->t0 = now; a->u0 = 0.0f; }
    return a->phase;
}

// |cos(pi e)| and sin(pi e) for e in [0,1], libm-free (Bhaskara I, < 0.2%).
static inline float peek_sinpi(float e) {
    e = peek_clampf(e, 0.0f, 1.0f);
    const float x = e * (1.0f - e);
    return 16.0f * x / (5.0f - 4.0f * x);
}
static inline float peek_abscospi(float e) {
    // |cos(pi e)| = sin(pi (e + 0.5)) folded into [0,1].
    float f = e + 0.5f;
    if (f > 1.0f) f -= 1.0f;
    return peek_sinpi(f);
}

// The card this frame: `from` is the poster's rect in the grid, `to` the 4:3
// panel's.  Both in screen px.
static inline peek_frame peek_eval(float u, peek_rect from, peek_rect to) {
    peek_frame f;
    const float e = peek_ease(u);
    const float cx = (from.x + from.w * 0.5f) + ((to.x + to.w * 0.5f) - (from.x + from.w * 0.5f)) * e;
    const float cy = (from.y + from.h * 0.5f) + ((to.y + to.h * 0.5f) - (from.y + from.h * 0.5f)) * e;
    const float w  = from.w + (to.w - from.w) * e;
    const float h  = from.h + (to.h - from.h) * e;
    const float turn  = peek_abscospi(e);            // width factor
    const float swell = 1.0f + 0.06f * peek_sinpi(e);
    f.r.w = w * turn;
    f.r.h = h * swell;
    f.r.x = cx - f.r.w * 0.5f;
    f.r.y = cy - f.r.h * 0.5f;
    f.back = e >= 0.5f;
    f.u = u;
    f.lift = e;
    {
        const float t = peek_clampf((u - 0.78f) / 0.22f, 0.0f, 1.0f);
        f.content_a = t * t * (3.0f - 2.0f * t);
    }
    return f;
}

// The 4:3 panel, centred on (cx, cy), h tall.
static inline peek_rect peek_panel(float cx, float cy, float h) {
    peek_rect r;
    r.h = h; r.w = h * 4.0f / 3.0f;
    r.x = cx - r.w * 0.5f; r.y = cy - r.h * 0.5f;
    return r;
}

#ifdef __cplusplus
}
#endif

#endif
