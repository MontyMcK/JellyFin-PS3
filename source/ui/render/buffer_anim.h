// The playback buffering presentation's motion, with no drawing in it.
//
//   buffer_anim.h     state machine + every animated value   (this file)
//   ui_buffering.cpp  draws what it decides (artwork, veil, ring, mark, text)
//
// WHAT IT SHOWS
//
// Between Play and the first picture the player opens the decoder, connects,
// pre-fills and fills its read-ahead ring -- seconds, sometimes tens of them.
// That wait is presented as: the item's artwork, darkened; the Jellyfin mark
// in the centre, breathing slightly; a thin gradient ring around it turning
// slowly; and BUFFERING / 47% beneath, the percentage secondary.  When the
// ring is full the ring closes, the mark gives one small pulse, and the whole
// thing fades before the first frame.
//
// WHY IT IS PURE C
//
// Every value is a function of timestamps the caller passes in, so the whole
// sequence is host-tested (tests/test_experience.c): 0 -> loading -> ready,
// an immediate ready, a long load, cancellation, and that the same inputs
// give the same frame.  Nothing here allocates, sleeps or reads a clock.
//
// NO FIXED WAITS
//
// The loading phase lasts exactly as long as the player is loading.  Only two
// short times are added, both presentation rather than delay:
//   * BUF_MIN_SHOW_US: if the stream was ready almost at once, the screen is
//     still held long enough to read as intended instead of a one-frame
//     flash.  It is measured from when the screen first appeared, so any
//     real loading time counts toward it; a slow start adds nothing.
//   * the outro (close + pulse + fade), ~0.45 s.
// Cancelling skips the close and pulse and just fades, quicker.

#ifndef JF_BUFFER_ANIM_H
#define JF_BUFFER_ANIM_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BUF_ROT_US       2600000.0f  // one ring revolution while loading
#define BUF_PULSE_US     2200000.0f  // one breath of the mark
#define BUF_PCT_TAU_US    220000.0f  // displayed % eases toward the real one
#define BUF_MIN_SHOW_US   450000ull  // shortest presentation (no flash)
#define BUF_CLOSE_US      220000ull  // ready: the arc closes to a full ring
#define BUF_SETTLE_US     420000ull  // ready: the rotation decelerates to rest
#define BUF_PULSE1_US     320000ull  // ready: the mark's one final pulse
#define BUF_FADE_AT_US    200000ull  // ready: the fade starts this far in
#define BUF_FADE_US       250000ull  // ready: the fade itself
#define BUF_CANCEL_US     180000ull  // cancelled: just fade, quickly
#define BUF_INTRO_US      260000ull  // first appearance fades in

enum { BUF_IDLE = 0, BUF_LOADING, BUF_READY, BUF_CANCELLED };

typedef struct {
    int      phase;
    uint64_t t0;          // presentation began
    uint64_t t_end;       // ready / cancel was signalled
    uint64_t t_out;       // the outro's start (t_end, held for BUF_MIN_SHOW_US)
    uint64_t t_last;      // last eval, for the displayed-% easing
    float    target;      // real progress, 0..1, never decreases
    float    shown;       // displayed progress, eases toward target
    float    angle_end;   // ring angle when loading stopped (turns)
} buf_anim;

// Everything a frame needs.  Angles in turns (0..1 = one revolution).
typedef struct {
    float ring_angle;   // where the arc's bright head is
    float arc;          // arc length, turns: grows with progress, 1 = closed
    float ring_a;       // ring opacity
    float track_a;      // the faint full-circle track under the arc
    float mark_scale;   // 1 +/- a few percent
    float mark_a;       // the mark's opacity
    float glow_a;       // soft glow behind the mark
    float ui_a;         // everything (veil, art, text) -- the fade
    int   pct;          // the number to print, 0..100
    int   done;         // the presentation has finished; start playback
} buf_frame;

static inline float buf_clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}
static inline float buf_smooth(float t) {       // smoothstep, no overshoot
    t = buf_clampf(t, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}
// A triangle wave 0..1..0 over one period -- the breath, without libm.
static inline float buf_tri(float phase01) {
    phase01 -= (float)(int)phase01;
    return phase01 < 0.5f ? phase01 * 2.0f : 2.0f - phase01 * 2.0f;
}
static inline float buf_frac(float v) { return v - (float)(int)v; }

static inline void buf_anim_start(buf_anim *b, uint64_t now) {
    b->phase = BUF_LOADING;
    b->t0 = now; b->t_end = 0; b->t_out = 0; b->t_last = now;
    b->target = 0.0f; b->shown = 0.0f; b->angle_end = 0.0f;
}

// p in 0..1.  Progress only moves forward: the ring never runs backwards when
// the player's own estimate wobbles.
static inline void buf_anim_progress(buf_anim *b, float p) {
    if (b->phase != BUF_LOADING) return;
    p = buf_clampf(p, 0.0f, 1.0f);
    if (p > b->target) b->target = p;
}

static inline float buf_loading_angle(const buf_anim *b, uint64_t now) {
    return buf_frac((float)(now - b->t0) / BUF_ROT_US);
}

static inline void buf_anim_ready(buf_anim *b, uint64_t now) {
    if (b->phase != BUF_LOADING) return;
    b->phase = BUF_READY;
    b->angle_end = buf_loading_angle(b, now);
    b->t_end = now;
    const uint64_t min_end = b->t0 + BUF_MIN_SHOW_US;
    b->t_out = now > min_end ? now : min_end;
    b->target = 1.0f;
}

static inline void buf_anim_cancel(buf_anim *b, uint64_t now) {
    if (b->phase != BUF_LOADING && b->phase != BUF_READY) return;
    if (b->phase == BUF_LOADING) b->angle_end = buf_loading_angle(b, now);
    b->phase = BUF_CANCELLED;
    b->t_end = now;
    b->t_out = now;
}

static inline uint64_t buf_anim_outro_us(int phase) {
    return phase == BUF_CANCELLED ? BUF_CANCEL_US : BUF_FADE_AT_US + BUF_FADE_US;
}

static inline int buf_anim_done(const buf_anim *b, uint64_t now) {
    if (b->phase == BUF_IDLE) return 1;
    if (b->phase == BUF_LOADING) return 0;
    return now >= b->t_out + buf_anim_outro_us(b->phase);
}

static inline void buf_anim_eval(buf_anim *b, uint64_t now, buf_frame *f) {
    if (now < b->t0) now = b->t0;
    // Displayed progress: an exponential approach, stepped by the elapsed
    // time (a rational approximation of 1 - e^-x, exact enough and libm-free).
    {
        const float dt = now > b->t_last ? (float)(now - b->t_last) : 0.0f;
        const float x  = dt / BUF_PCT_TAU_US;
        const float q  = x + 0.5f * x * x;          // 1 - e^-x ~ q / (1 + q)
        const float k  = q / (1.0f + q);
        b->shown += (b->target - b->shown) * buf_clampf(k, 0.0f, 1.0f);
        if (b->shown > b->target) b->shown = b->target;
        b->t_last = now;
    }

    const float t_since = (float)(now - b->t0);
    const float intro   = buf_smooth(t_since / (float)BUF_INTRO_US);
    const float breath  = buf_tri(t_since / BUF_PULSE_US);   // 0..1..0

    f->pct   = (int)(b->shown * 100.0f + 0.5f);
    f->ui_a  = intro;
    f->done  = buf_anim_done(b, now);
    f->track_a = 0.18f;
    f->glow_a  = 0.22f + 0.10f * breath;
    f->mark_a  = 0.86f + 0.14f * breath;
    f->mark_scale = 1.0f + 0.025f * (breath - 0.5f);
    f->ring_a  = 1.0f;

    if (b->phase == BUF_LOADING || b->phase == BUF_IDLE) {
        f->ring_angle = buf_loading_angle(b, now);
        f->arc        = 0.16f + 0.60f * b->shown;
        return;
    }

    // The ring's settle: the rotation decelerates linearly to rest over
    // BUF_SETTLE_US, from the loading speed (so there is no jolt).
    {
        const float u  = buf_clampf((float)(now - b->t_end), 0.0f, (float)BUF_SETTLE_US);
        const float D  = (float)BUF_SETTLE_US;
        const float v  = 1.0f / BUF_ROT_US;                   // turns per us
        f->ring_angle  = buf_frac(b->angle_end + v * (u - u * u / (2.0f * D)));
    }
    const float arc_now = 0.16f + 0.60f * b->shown;

    if (b->phase == BUF_CANCELLED) {
        const float a = 1.0f - buf_smooth((float)(now - b->t_out) / (float)BUF_CANCEL_US);
        f->arc  = arc_now;
        f->ui_a = intro * a;
        return;
    }

    // READY.  Hold the loading look until the minimum presentation has passed
    // (an instant ready), then: close the arc, one pulse, fade.
    if (now < b->t_out) {
        f->arc = arc_now;
        return;
    }
    const float o = (float)(now - b->t_out);
    const float close = buf_smooth(o / (float)BUF_CLOSE_US);
    f->arc     = arc_now + (1.0f - arc_now) * close;
    f->track_a = 0.18f * (1.0f - close);
    f->pct     = (int)((b->shown + (1.0f - b->shown) * close) * 100.0f + 0.5f);
    const float p1 = buf_clampf(o / (float)BUF_PULSE1_US, 0.0f, 1.0f);
    const float bump = p1 < 1.0f ? buf_tri(p1) : 0.0f;          // 0..1..0, once
    f->mark_scale = 1.0f + 0.07f * bump;
    f->mark_a     = 1.0f;
    f->glow_a     = 0.28f + 0.22f * bump;
    const float fade = buf_smooth((o - (float)BUF_FADE_AT_US) / (float)BUF_FADE_US);
    f->ui_a = intro * (1.0f - fade);
}

#ifdef __cplusplus
}
#endif

#endif
