// The spine: the XMB depth model's geometry, with no drawing in it.
//
//   spine.h        per-level constants, interpolation, slot layout  (this file)
//   ui_spine.cpp   the draw code that consumes it                   (stage S1)
//
// WHAT THE SPINE IS
//
// README section 2.9 of the 2026-09-21 design export (design-import-v2/)
// replaces the tab strip with a "spine": one row of category icons that is the
// axis the whole UI hangs off.  Navigation is one composition seen at three
// depths -- L1 Home names the thing, L2 Category describes it, L3 Item states
// every fact -- and the camera pushing forward is drawn as the spine sliding
// up and shrinking:
//
//     icon centre y   232 -> 141 -> 74
//
// The design calls that row "the single most important value in the revamp".
// The geometry never re-lays-out.  Every depth-dependent value is a TRIPLE,
// one per level, and a transition animates a single float 0 -> 1 -> 2 and
// interpolates all of them.  That is the whole contract this file implements:
// give it a float depth and it returns every value at that depth.
//
// WHY IT IS A HEADER OF PURE C
//
// Same reason as wave_gel.h.  Everything here is arithmetic on authored
// numbers, so it can be host-tested against the design table exactly
// (tests/test_spine.c) before any of it reaches a TV.  Nothing here touches
// display_width, the theme or the RSX.  Values are in the design's 1280x720
// authoring units, and the caller applies UIS_W / UIS_H / UIS_TF and the
// overscan inset, like every other XMB constant.
//
// WHERE THE NUMBERS CAME FROM
//
// README section 2.9 wherever it gives a value.  It is the export's
// self-declared source of truth, and the size question (below) was decided in
// its favour on 2026-09-23.  The few values the README does not state were
// MEASURED by script from uploads/dirD.html (the L2 mockup) at 1280x720, and
// are marked "dirD" where they are defined.
//
// THE SIZE CONFLICT, RECORDED SO IT IS NOT REDISCOVERED
//
// The two spine mockups (dirE = L1, dirD = L2) agree with the README on icon
// y (232 / 141) and on the idle falloff, but not on size:
//
//                     mockups     README
//     stride            128        200
//     active x          152        230
//     idle icon          26         44
//     active icon      36 / 32    64 / 56
//
// That is 1.5-1.8x, not uniform.  The README won: it is the later document,
// and "too small to read on the TV" is this UI's recurring complaint.  To go
// back to the mockups, change the four SPINE_* values above the level table
// and the active_px column; test_spine.c checks fit for whatever is here.

#ifndef JF_SPINE_H
#define JF_SPINE_H

#ifdef __cplusplus
extern "C" {
#endif

// --- level indices ---------------------------------------------------------
#define SPINE_L1        0       // Home: the spine IS the content
#define SPINE_L2        1       // Category
#define SPINE_L3        2       // Item (the existing item-detail screen)
#define SPINE_LEVELS    3

// --- constant at every level (README 2.9) ----------------------------------
#define SPINE_STRIDE        200.0f  // slot pitch
#define SPINE_ACTIVE_X      230.0f  // active slot centre; left-biased on purpose
#define SPINE_IDLE_PX        44.0f  // idle icon box
#define SPINE_IDLE_STROKE     1.4f  // idle icon stroke -- advisory, glyph art
#define SPINE_RULE_W         26.0f  // accent underline under the active label
#define SPINE_RULE_H          2.0f
#define SPINE_LABEL_TRACK_EM  0.04f // uppercase Satoshi, 0.04em

// Offsets below the active icon, measured on dirD (32px icon, 11.5px label).
// The label's top sits 11 px below the icon's bottom edge; the underline's top
// sits 20 px below the label's top, which is 1.74 label-heights.  Both are kept
// RELATIVE to the icon and label sizes so the stack stays proportioned as they
// shrink between levels -- a fixed pixel offset would crowd L3 and float at L1.
#define SPINE_LABEL_GAP      11.0f  // dirD
#define SPINE_RULE_GAP_EM     1.74f // dirD: (188 - 168) / 11.5

// The glow band behind the active slot: an ellipse, 28% of the screen width by
// half of a 112 px band, centred on the active icon, falling to zero at 70% of
// its radius.  dirD: radial-gradient(ellipse 28% 50% at 152px 56px,
// rgba(170,92,195,.09) 0%, ... 0 70%) in a 1280x112 band.  The peak alpha is
// the level table's glow_a, which is what the README varies per level.
#define SPINE_GLOW_RX       358.4f  // 0.28 * 1280
#define SPINE_GLOW_RY        56.0f  // 0.50 * 112
#define SPINE_GLOW_FADE       0.70f // alpha reaches 0 at this fraction of R

// Idle falloff by distance from the active slot (README 2.9, and measured
// identically on dirD and dirE).  Index 0 is the active slot itself.  Beyond
// the table a slot is not drawn at all.
#define SPINE_FALLOFF_N 6
static const float SPINE_FALLOFF[SPINE_FALLOFF_N] = {
    1.00f, 0.62f, 0.46f, 0.33f, 0.22f, 0.14f
};

// Transition: slow-in / slow-out, no spring, no overshoot (README 2.9).
#define SPINE_MOTION_US  280000ULL

// --- the per-level triples (README 2.9 table) -------------------------------
//
// divider_y at L1: the README says "not drawn".  It is carried as the L2 value
// with alpha 0, so an L1 -> L2 transition FADES the divider in where it will
// rest instead of sweeping it down from some invented L1 position.  That keeps
// "not drawn" exact at L1 without a special case in the interpolation.
typedef struct {
    float icon_y;       // spine icon CENTRE
    float active_px;    // active icon box (the README's "1.16x" is baked in)
    float label_px;     // active label size
    float glow_a;       // glow band peak alpha, 0..1
    float divider_y;    // header divider
    float divider_a;    // 0 = not drawn
    float art_w;        // focused artwork
    float art_h;
    float title_px;
} spine_level;

static const spine_level SPINE_LEVEL[SPINE_LEVELS] = {
    /* L1 */ { 232.0f, 64.0f, 15.0f, 0.12f, 214.0f, 0.0f, 128.0f, 192.0f, 30.0f },
    /* L2 */ { 141.0f, 56.0f, 13.5f, 0.09f, 214.0f, 1.0f, 200.0f, 300.0f, 36.0f },
    /* L3 */ {  74.0f, 48.0f, 13.0f, 0.06f, 144.0f, 1.0f, 216.0f, 324.0f, 25.0f },
};

// The active label stack under the icon, at any level (settled or evaluated).
static inline float spine_label_top(const spine_level *L)
{
    return L->icon_y + L->active_px * 0.5f + SPINE_LABEL_GAP;
}
static inline float spine_rule_top(const spine_level *L)
{
    return spine_label_top(L) + L->label_px * SPINE_RULE_GAP_EM;
}
static inline float spine_rule_bottom(const spine_level *L)
{
    return spine_rule_top(L) + SPINE_RULE_H;
}

// Clearance between the underline's bottom and the divider at which the
// divider reaches full alpha.  See spine_eval().
#define SPINE_DIVIDER_CLEAR   8.0f

// --- interpolation ---------------------------------------------------------

static inline float spine_lerp(float a, float b, float t) { return a + (b - a) * t; }

static inline float spine_clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

// Smoothstep: zero slope at both ends (slow-in / slow-out), monotonic on [0,1],
// and it never leaves [0,1], which is what "no overshoot" means for a value
// that drives positions.
static inline float spine_ease(float t)
{
    t = spine_clampf(t, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

// Every triple at a fractional depth in [0, 2].  Piecewise-linear between
// adjacent levels.  The easing is applied to TIME (spine_motion below), not
// here, so a depth that is sitting still at 1.0 is exactly the L2 row.
static inline spine_level spine_eval(float depth)
{
    spine_level o;
    int   k;
    float f;
    const spine_level *a, *b;

    depth = spine_clampf(depth, 0.0f, (float)(SPINE_LEVELS - 1));
    k = (int)depth;
    if (k >= SPINE_LEVELS - 1) return SPINE_LEVEL[SPINE_LEVELS - 1];
    f = depth - (float)k;
    a = &SPINE_LEVEL[k];
    b = &SPINE_LEVEL[k + 1];

    o.icon_y    = spine_lerp(a->icon_y,    b->icon_y,    f);
    o.active_px = spine_lerp(a->active_px, b->active_px, f);
    o.label_px  = spine_lerp(a->label_px,  b->label_px,  f);
    o.glow_a    = spine_lerp(a->glow_a,    b->glow_a,    f);
    o.divider_y = spine_lerp(a->divider_y, b->divider_y, f);
    o.divider_a = spine_lerp(a->divider_a, b->divider_a, f);
    o.art_w     = spine_lerp(a->art_w,     b->art_w,     f);
    o.art_h     = spine_lerp(a->art_h,     b->art_h,     f);
    o.title_px  = spine_lerp(a->title_px,  b->title_px,  f);

    // The divider must never be drawn THROUGH the label stack.  On the way
    // L1 -> L2 it fades in at y=214 while the label and underline are still
    // sliding up past that line -- at depth 0.9 the underline's bottom is still
    // about a pixel below it -- so a plain lerp of its alpha would draw a
    // half-visible hairline through the underline for part of every
    // transition.  Scale the alpha by how far the underline has cleared it,
    // reaching full strength at SPINE_DIVIDER_CLEAR px of clearance.  At rest
    // this is exactly 1 (L2 clears by 8.5 px, L3 by 10.4), so the table's
    // values are still what a settled screen draws; with the slow-out easing
    // the fade takes the last ~55 ms of the move.
    {
        float gap = o.divider_y - spine_rule_bottom(&o);
        o.divider_a *= spine_clampf(gap / SPINE_DIVIDER_CLEAR, 0.0f, 1.0f);
    }
    return o;
}

// --- slot layout ------------------------------------------------------------

// Centre x of slot i when slot `active` is focused.  Slots left of the active
// one may be negative: the row is cropped at the left edge by design, and it
// trails off to the right into empty air.  Nothing re-flows when the focus
// moves -- the row translates.
static inline float spine_slot_x(int i, int active)
{
    return SPINE_ACTIVE_X + (float)(i - active) * SPINE_STRIDE;
}

// Opacity of slot i: 1 for the active slot, the falloff table by distance, and
// 0 (do not draw) past the end of the table.
static inline float spine_slot_alpha(int i, int active)
{
    int d = i - active;
    if (d < 0) d = -d;
    return d < SPINE_FALLOFF_N ? SPINE_FALLOFF[d] : 0.0f;
}

// --- motion ------------------------------------------------------------------
//
// A transition is a pure function of time: where it started, where it is
// going, and when.  That makes it testable without a frame loop and immune to
// a slow frame -- a 40 ms frame lands the value where it should be at 40 ms,
// instead of 1/60th further along.
//
// Retargeting mid-flight (Down pressed again before the slide finished) starts
// a new 280 ms leg FROM THE CURRENT VALUE.  That is continuous in position,
// which is the property that matters; a velocity kink at the join is invisible
// at this duration and is what "no spring" asks for anyway.  A 0 -> 2 jump
// (the X-opens-detail route from L1) takes the same 280 ms as a single step.
typedef struct {
    float              from;
    float              to;
    unsigned long long t0_us;
} spine_motion;

static inline void spine_motion_init(spine_motion *m, float depth)
{
    m->from  = depth;
    m->to    = depth;
    m->t0_us = 0;
}

static inline float spine_motion_value(const spine_motion *m,
                                       unsigned long long now_us)
{
    unsigned long long dt;
    if (m->from == m->to) return m->to;
    dt = now_us > m->t0_us ? now_us - m->t0_us : 0;
    if (dt >= SPINE_MOTION_US) return m->to;
    return spine_lerp(m->from, m->to,
                      spine_ease((float)dt / (float)SPINE_MOTION_US));
}

static inline int spine_motion_busy(const spine_motion *m,
                                    unsigned long long now_us)
{
    return m->from != m->to && now_us < m->t0_us + SPINE_MOTION_US;
}

static inline void spine_motion_go(spine_motion *m, int level,
                                   unsigned long long now_us)
{
    float target = (float)level;
    float cur    = spine_motion_value(m, now_us);
    if (target == m->to && spine_motion_busy(m, now_us)) return;
    m->from  = cur;
    m->to    = target;
    m->t0_us = now_us;
}

// --- the XMB's own motion: APPROACH -------------------------------------------
//
// The first hardware pass drove everything off the timed 280 ms smoothstep
// above and tab changes were instant.  On a TV that read as "way too static":
// the row jumped a whole stride in one frame and nothing drifted.  The real
// XMB does not use a timed curve for navigation at all.  It uses APPROACH: every
// frame the value covers a fixed fraction of the remaining distance, so a move
// starts quick and settles long and soft -- the "floaty" feel -- and a new
// target mid-move is simply chased from wherever the value is, with no seam.
//
// The gain table is 1etu/XMP's (MIT), packages/paf/src/anim/approach.ts, where
// it is marked as verified against the console: frames = trunc(ms * 60 / 1000),
// the five short durations are tabulated, and longer ones follow
// 1 / ((frames - 5) * 0.333333 + 2.2).  XMP's CATEGORY_MOVE and ITEM_MOVE are
// 200 ms.
//
// FRAME-RATE INDEPENDENT.  That gain is per 60 Hz frame, and this client does
// not always hold 60 (JellyWave mode 3 runs ~36 fps today).  Stepped once per
// rendered frame, a slow frame would make the motion slower, not just
// choppier.  So a step covers the number of 60 Hz frames that ELAPSED: the
// remaining gap is multiplied by (1 - g) once per whole frame and linearly for
// the fraction.  No libm, so it stays callable from anywhere.
#define SPINE_CATEGORY_MS   200   // XMP CATEGORY_MOVE (verified)
// Decided here, not measured: XMP pushes a level in 220 ms, but that is a
// timed decelerate, which ends dead.  As an approach, 320 ms keeps the long
// soft tail the rest of the motion has, so entering a tab drifts in rather
// than landing.
#define SPINE_LEVEL_MS      320
#define SPINE_APPROACH_SNAP 0.0015f   // closer than this and it is there

static inline float spine_approach_gain(int ms)
{
    static const float SHORT[5] = {
        1.0f, 0.96153849f, 0.78125f, 0.63694263f, 0.52910054f
    };
    int frames = (int)((float)ms * 0.001f * 60.0f);
    if (frames < 0) frames = 0;
    if (frames < 5) return SHORT[frames];
    return 1.0f / ((float)(frames - 5) * 0.33333299f + 2.2f);
}

typedef struct {
    float value;
    float target;
    float gain;     // per 60 Hz frame
} spine_approach;

static inline void spine_approach_init(spine_approach *a, float v, int ms)
{
    a->value  = v;
    a->target = v;
    a->gain   = spine_approach_gain(ms);
}

static inline void spine_approach_set(spine_approach *a, float target)
{
    a->target = target;
}

static inline int spine_approach_busy(const spine_approach *a)
{
    return a->value != a->target;
}

static inline float spine_approach_step(spine_approach *a,
                                        unsigned long long dt_us)
{
    float frames = (float)dt_us * (60.0f / 1000000.0f);
    const float q = 1.0f - a->gain;
    float keep = 1.0f, gap;
    int   n, i;

    if (frames > 30.0f) frames = 30.0f;          // a stall lands, not spins
    n = (int)frames;
    for (i = 0; i < n; i++) keep *= q;
    keep *= 1.0f - (frames - (float)n) * a->gain;

    a->value = a->target + (a->value - a->target) * keep;
    gap = a->value - a->target;
    if (gap < 0.0f) gap = -gap;
    if (gap < SPINE_APPROACH_SNAP) a->value = a->target;
    return a->value;
}

// --- slots at a FRACTIONAL distance from the focus --------------------------
//
// With the row's position animated, a slot sits at a fractional distance d
// from the focus, and everything about it is continuous in d: position,
// size and opacity all glide instead of stepping when the focus moves.
// Integer d gives exactly the table values above.

// 1 on the focus, falling linearly to 0 one slot away.  XMP's nearness().
static inline float spine_near(float d)
{
    if (d < 0.0f) d = -d;
    return d >= 1.0f ? 0.0f : 1.0f - d;
}

static inline float spine_slot_x_f(float d)
{
    return SPINE_ACTIVE_X + d * SPINE_STRIDE;
}

// The falloff interpolated between table entries, fading from the last entry
// to 0 over one more slot -- so a slot leaving the drawn range dissolves
// instead of vanishing.
static inline float spine_slot_alpha_f(float d)
{
    int   k;
    float f, a, b;
    if (d < 0.0f) d = -d;
    if (d >= (float)SPINE_FALLOFF_N) return 0.0f;
    k = (int)d;
    f = d - (float)k;
    a = SPINE_FALLOFF[k];
    b = (k + 1 < SPINE_FALLOFF_N) ? SPINE_FALLOFF[k + 1] : 0.0f;
    return spine_lerp(a, b, f);
}

#ifdef __cplusplus
}
#endif

#endif // JF_SPINE_H
