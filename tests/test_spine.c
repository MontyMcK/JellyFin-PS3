// Host test for source/ui/render/spine.h -- the XMB depth model's geometry.
//
// What this is guarding.  The spine is the one set of numbers every screen
// hangs off once stage S1 lands, and the ways it goes wrong are all invisible
// in source and obvious on a TV: a label that collides with the divider
// halfway through a transition, a falloff that pops, a slot that runs off the
// right edge before the design says it should, an easing curve that
// overshoots and makes the whole UI bounce.  Each of those is asserted here.
//
// Section 1 checks the table against README 2.9 written out by hand.  That is
// the one place a copy of the numbers is the right assertion: the README is
// the spec, and a typo in spine.h is exactly the failure being looked for.

#include <stdio.h>
#include <math.h>
#include "spine.h"

static int fails = 0;

static void ck(int cond, const char *what)
{
    if (!cond) { printf("  FAIL: %s\n", what); fails++; }
}

static void ck_near(float got, float want, float tol, const char *what)
{
    float d = got - want;
    if (d < 0.0f) d = -d;
    if (!(d <= tol)) {
        printf("  FAIL: %s (got %.5f want %.5f tol %.5f)\n", what, got, want, tol);
        fails++;
    }
}

// --- 1. the table is README 2.9 ------------------------------------------
static void test_table(void)
{
    static const float want[SPINE_LEVELS][9] = {
        // icon_y active label glow  div_y div_a art_w art_h title
        {  232,   64,  15.0, 0.12,  214,  0,   128,  192,  30 },
        {  141,   56,  13.5, 0.09,  214,  1,   200,  300,  36 },
        {   74,   48,  13.0, 0.06,  144,  1,   216,  324,  25 },
    };
    static const float falloff[SPINE_FALLOFF_N] = { 1, .62f, .46f, .33f, .22f, .14f };
    int l, i;

    printf("1. table vs README 2.9\n");
    for (l = 0; l < SPINE_LEVELS; l++) {
        const spine_level *L = &SPINE_LEVEL[l];
        const float got[9] = { L->icon_y, L->active_px, L->label_px, L->glow_a,
                               L->divider_y, L->divider_a, L->art_w, L->art_h,
                               L->title_px };
        for (i = 0; i < 9; i++) {
            char what[64];
            snprintf(what, sizeof what, "L%d field %d", l + 1, i);
            ck_near(got[i], want[l][i], 1e-6f, what);
        }
    }
    ck_near(SPINE_STRIDE,   200.0f, 0, "stride 200");
    ck_near(SPINE_ACTIVE_X, 230.0f, 0, "active x 230");
    ck_near(SPINE_IDLE_PX,   44.0f, 0, "idle icon 44");
    ck_near(SPINE_RULE_W,    26.0f, 0, "underline 26 wide");
    ck_near(SPINE_RULE_H,     2.0f, 0, "underline 2 high");
    for (i = 0; i < SPINE_FALLOFF_N; i++)
        ck_near(SPINE_FALLOFF[i], falloff[i], 1e-6f, "falloff table");
    ck(SPINE_MOTION_US == 280000ULL, "transition is 280 ms");
}

// --- 2. evaluation --------------------------------------------------------
static void test_eval(void)
{
    int l;
    printf("2. spine_eval\n");

    // Settled levels are the table exactly -- including the divider alpha,
    // which the clearance factor must leave alone at rest.
    for (l = 0; l < SPINE_LEVELS; l++) {
        spine_level e = spine_eval((float)l);
        const spine_level *t = &SPINE_LEVEL[l];
        ck(e.icon_y == t->icon_y && e.active_px == t->active_px &&
           e.label_px == t->label_px && e.glow_a == t->glow_a &&
           e.divider_y == t->divider_y && e.art_w == t->art_w &&
           e.art_h == t->art_h && e.title_px == t->title_px,
           "integer depth returns the table row");
        ck(e.divider_a == t->divider_a,
           "divider alpha at rest is the table value");
    }

    // Midpoints are midpoints (for everything the clearance factor does not
    // touch).
    {
        spine_level m = spine_eval(0.5f);
        ck_near(m.icon_y,    (232 + 141) * 0.5f, 1e-4f, "L1-L2 midpoint icon y");
        ck_near(m.active_px, (64 + 56) * 0.5f,   1e-4f, "L1-L2 midpoint icon size");
        m = spine_eval(1.5f);
        ck_near(m.icon_y,    (141 + 74) * 0.5f,  1e-4f, "L2-L3 midpoint icon y");
        ck_near(m.divider_y, (214 + 144) * 0.5f, 1e-4f, "L2-L3 midpoint divider");
    }

    // Out-of-range depths clamp instead of extrapolating off the table.
    {
        spine_level lo = spine_eval(-0.7f), hi = spine_eval(3.4f);
        ck(lo.icon_y == SPINE_LEVEL[0].icon_y, "depth < 0 clamps to L1");
        ck(hi.icon_y == SPINE_LEVEL[2].icon_y, "depth > 2 clamps to L3");
    }

    // The camera only ever pushes forward: across 0..2 the spine rises and
    // shrinks monotonically, and so does its glow.  A non-monotonic triple
    // would make the row bob during a transition.
    {
        int   i, ok_y = 1, ok_px = 1, ok_lab = 1, ok_glow = 1;
        spine_level prev = spine_eval(0.0f);
        for (i = 1; i <= 2000; i++) {
            spine_level c = spine_eval((float)i / 1000.0f);
            if (c.icon_y    > prev.icon_y)    ok_y = 0;
            if (c.active_px > prev.active_px) ok_px = 0;
            if (c.label_px  > prev.label_px)  ok_lab = 0;
            if (c.glow_a    > prev.glow_a)    ok_glow = 0;
            prev = c;
        }
        ck(ok_y,    "icon y is monotonic (rises) across 0..2");
        ck(ok_px,   "active icon shrinks monotonically");
        ck(ok_lab,  "active label shrinks monotonically");
        ck(ok_glow, "glow fades monotonically");
    }
}

// --- 3. the divider never cuts through the label stack ---------------------
static void test_divider_clearance(void)
{
    int   i, bad = 0;
    float worst = 1e9f, first_vis = -1.0f;
    printf("3. divider vs label stack\n");

    for (i = 0; i <= 2000; i++) {
        float d = (float)i / 1000.0f;
        spine_level e = spine_eval(d);
        float gap = e.divider_y - spine_rule_bottom(&e);
        if (e.divider_a > 0.0f) {
            if (first_vis < 0.0f) first_vis = d;
            if (gap < 0.0f) bad++;
            if (gap < worst) worst = gap;
        }
        if (e.divider_a < 0.0f || e.divider_a > 1.0f) bad++;
    }
    printf("   divider first visible at depth %.3f; worst clearance %.2f px\n",
           first_vis, worst);
    ck(bad == 0, "no depth draws the divider through the underline");
    ck(spine_eval(0.0f).divider_a == 0.0f, "divider not drawn at L1");

    // The label stack must also fit ABOVE the divider at rest with room to
    // spare -- this is the check that fails first if D1 is revisited and the
    // icons grow again.
    ck(SPINE_LEVEL[1].divider_y - spine_rule_bottom(&SPINE_LEVEL[1])
       >= SPINE_DIVIDER_CLEAR, "L2 label stack clears the divider at rest");
    ck(SPINE_LEVEL[2].divider_y - spine_rule_bottom(&SPINE_LEVEL[2])
       >= SPINE_DIVIDER_CLEAR, "L3 label stack clears the divider at rest");

    // And the whole stack stays on the screen at L3, the highest it goes.
    ck(SPINE_LEVEL[2].icon_y - SPINE_LEVEL[2].active_px * 0.5f > 0.0f,
       "L3 active icon is below the top edge");
}

// --- 4. slots and falloff ---------------------------------------------------
static void test_slots(void)
{
    int a, i;
    printf("4. slot layout\n");

    ck(spine_slot_x(3, 3) == SPINE_ACTIVE_X, "active slot sits at the active x");
    ck(spine_slot_x(4, 3) - spine_slot_x(3, 3) == SPINE_STRIDE, "slots are one stride apart");
    ck(spine_slot_x(0, 1) < SPINE_ACTIVE_X, "earlier slots are to the left");

    for (a = 0; a < 16; a++)
        for (i = 0; i < 16; i++) {
            float al = spine_slot_alpha(i, a);
            if (i == a) ck(al == 1.0f, "active slot is opaque");
            if (spine_slot_alpha(i, a) != spine_slot_alpha(a, i))
                ck(0, "falloff is symmetric in distance");
            if ((i - a >= SPINE_FALLOFF_N) || (a - i >= SPINE_FALLOFF_N))
                ck(al == 0.0f, "slots past the falloff are not drawn");
        }

    // Monotonic falloff: nothing further away is brighter.
    for (i = 1; i < SPINE_FALLOFF_N; i++)
        ck(SPINE_FALLOFF[i] < SPINE_FALLOFF[i - 1], "falloff strictly decreases");

    // Horizontal fit on the 1280 authoring canvas.  The design crops the row
    // at the LEFT edge on purpose, but the neighbour one slot to the left must
    // still be whole (Search beside Home), and the furthest slot the falloff
    // draws must still end on-screen -- otherwise a drawn icon is cut by the
    // right edge, which is not the "trailing off into empty air" the design
    // shows.  At 16:9 UIS_W/UIS_H scale both axes by the same factor, so this
    // holds at 720p and 1080p alike.
    ck(spine_slot_x(0, 1) - SPINE_IDLE_PX * 0.5f >= 0.0f,
       "left neighbour icon is whole");
    ck(spine_slot_x(SPINE_FALLOFF_N - 1, 0) + SPINE_IDLE_PX * 0.5f <= 1280.0f,
       "furthest drawn slot ends on-screen");
    ck(SPINE_ACTIVE_X - SPINE_LEVEL[0].active_px * 0.5f >= 0.0f,
       "the largest active icon (L1) fits left of its centre");
}

// --- 5. easing and motion -------------------------------------------------
static void test_motion(void)
{
    int   i, mono = 1, inside = 1;
    float prev = 0.0f;
    spine_motion m;
    const unsigned long long T = SPINE_MOTION_US, t0 = 5000000ULL;

    printf("5. easing and motion\n");
    ck(spine_ease(0.0f) == 0.0f && spine_ease(1.0f) == 1.0f, "ease endpoints");
    ck(spine_ease(-1.0f) == 0.0f && spine_ease(2.0f) == 1.0f, "ease clamps");
    ck_near(spine_ease(0.5f), 0.5f, 1e-6f, "ease is symmetric about the middle");
    for (i = 1; i <= 1000; i++) {
        float v = spine_ease((float)i / 1000.0f);
        if (v < prev) mono = 0;
        if (v < 0.0f || v > 1.0f) inside = 0;
        prev = v;
    }
    ck(mono, "ease is monotonic (no overshoot)");
    ck(inside, "ease stays in [0,1]");
    // Slow-in / slow-out: the first and last 5% of time cover well under 5%
    // of the distance.
    ck(spine_ease(0.05f) < 0.01f && spine_ease(0.95f) > 0.99f,
       "ease is slow at both ends");

    spine_motion_init(&m, 0.0f);
    ck(spine_motion_value(&m, t0) == 0.0f, "an idle motion holds its value");
    ck(!spine_motion_busy(&m, t0), "an idle motion is not busy");

    spine_motion_go(&m, SPINE_L2, t0);
    ck(spine_motion_value(&m, t0) == 0.0f, "a leg starts where it was");
    ck(spine_motion_busy(&m, t0 + T / 2), "busy mid-leg");
    ck_near(spine_motion_value(&m, t0 + T / 2), 0.5f, 1e-6f, "halfway in time is halfway");
    ck(spine_motion_value(&m, t0 + T) == 1.0f, "a leg lands exactly on its level");
    ck(spine_motion_value(&m, t0 + 10 * T) == 1.0f, "and stays there");
    ck(!spine_motion_busy(&m, t0 + T), "not busy once landed");
    ck(spine_motion_value(&m, t0 - 1000) == 0.0f,
       "a clock read before the leg began returns its start");

    // Retarget mid-flight: continuous in position at the join.
    {
        unsigned long long tj = t0 + 20 * T + T / 3;
        float before, after;
        spine_motion_init(&m, 0.0f);
        spine_motion_go(&m, SPINE_L2, t0 + 20 * T);
        before = spine_motion_value(&m, tj);
        spine_motion_go(&m, SPINE_L1, tj);
        after = spine_motion_value(&m, tj);
        ck(before == after, "retarget is continuous in position");
        ck(spine_motion_value(&m, tj + T) == 0.0f, "retargeted leg lands on the new level");
    }

    // Re-requesting the level already in flight must not restart the clock
    // (a held button would otherwise freeze the slide at its start).
    {
        float v;
        spine_motion_init(&m, 1.0f);
        spine_motion_go(&m, SPINE_L3, t0);
        spine_motion_go(&m, SPINE_L3, t0 + T / 2);
        v = spine_motion_value(&m, t0 + T);
        ck(v == 2.0f, "repeating the target does not restart the leg");
    }

    // The X-from-L1 route jumps two levels in one leg of the same length.
    spine_motion_init(&m, 0.0f);
    spine_motion_go(&m, SPINE_L3, t0);
    ck(spine_motion_value(&m, t0 + T) == 2.0f, "0 -> 2 lands in one leg");
    ck_near(spine_motion_value(&m, t0 + T / 2), 1.0f, 1e-6f, "0 -> 2 passes L2 at halfway");
}

// --- 6. approach motion and fractional slots ------------------------------
static float run_approach(float from, float to, int ms, double fps, double secs)
{
    spine_approach a;
    int i, n = (int)(fps * secs + 0.5);
    unsigned long long dt = (unsigned long long)(1000000.0 / fps + 0.5);
    spine_approach_init(&a, from, ms);
    spine_approach_set(&a, to);
    for (i = 0; i < n; i++) spine_approach_step(&a, dt);
    return a.value;
}

static void test_approach(void)
{
    spine_approach a;
    int i, mono = 1, inside = 1;
    float prev;

    printf("6. approach motion + fractional slots\n");

    // XMP's gainOf(): the tabulated short durations, then the formula.
    ck_near(spine_approach_gain(0),   1.0f,        1e-7f, "gain(0 ms) = 1");
    ck_near(spine_approach_gain(50),  0.63694263f, 1e-7f, "gain(50 ms) = table[3]");
    ck_near(spine_approach_gain(200), 1.0f / (7 * 0.33333299f + 2.2f), 1e-6f,
            "gain(200 ms) = 1/((12-5)*0.3333+2.2)");

    // At 60 Hz, 12 frames of a 200 ms move cover 1-(1-g)^12 of the way,
    // which is the curve the XMB draws.
    {
        float g = spine_approach_gain(200), want = 1.0f - powf(1.0f - g, 12.0f);
        ck_near(run_approach(0.0f, 1.0f, 200, 60.0, 0.2), want, 1e-4f,
                "12 frames at 60 Hz reproduce the per-frame curve");
    }

    // Frame-rate independence: the same 0.25 s at 60, 36 and 20 fps lands
    // within a couple of percent of the same place.  0.25 s is a whole number
    // of frames at all three (15 / 9 / 5), so every run covers the same
    // time.  Without the dt scaling a 36 fps console would drift at 60% speed.
    {
        float v60 = run_approach(0.0f, 1.0f, 320, 60.0, 0.25);
        float v36 = run_approach(0.0f, 1.0f, 320, 36.0, 0.25);
        float v20 = run_approach(0.0f, 1.0f, 320, 20.0, 0.25);
        ck_near(v36, v60, 0.02f, "36 fps tracks 60 fps");
        ck_near(v20, v60, 0.03f, "20 fps tracks 60 fps");
    }

    // Monotonic, never overshoots, lands exactly (the snap), and a long
    // stall cannot throw it past the target.
    spine_approach_init(&a, 0.0f, 200);
    spine_approach_set(&a, 3.0f);
    prev = 0.0f;
    for (i = 0; i < 120; i++) {
        float v = spine_approach_step(&a, 16667);
        if (v < prev) mono = 0;
        if (v > 3.0f) inside = 0;
        prev = v;
    }
    ck(mono, "approach is monotonic toward its target");
    ck(inside, "approach never overshoots");
    ck(a.value == 3.0f && !spine_approach_busy(&a), "approach lands exactly");
    spine_approach_init(&a, 0.0f, 200);
    spine_approach_set(&a, 1.0f);
    spine_approach_step(&a, 5000000ULL);
    ck(a.value <= 1.0f && a.value > 0.99f, "a 5 s stall lands, not overshoots");

    // Retarget mid-move: position is continuous (the value is simply chased
    // from where it is).
    spine_approach_init(&a, 0.0f, 200);
    spine_approach_set(&a, 1.0f);
    for (i = 0; i < 4; i++) spine_approach_step(&a, 16667);
    prev = a.value;
    spine_approach_set(&a, -1.0f);
    ck(a.value == prev, "retarget does not move the value");
    spine_approach_step(&a, 16667);
    ck(a.value < prev, "and the next step heads for the new target");

    // Fractional slots agree with the integer table and are continuous.
    for (i = 0; i <= 7; i++)
        ck_near(spine_slot_alpha_f((float)i), spine_slot_alpha(i, 0), 1e-7f,
                "fractional falloff equals the table at whole slots");
    {
        int ok = 1;
        float p = spine_slot_alpha_f(0.0f);
        for (i = 1; i <= 7000; i++) {
            float v = spine_slot_alpha_f((float)i / 1000.0f);
            if (v > p + 1e-6f || p - v > 0.02f) ok = 0;   // falls, no jumps
            p = v;
        }
        ck(ok, "fractional falloff is continuous and never rises");
    }
    ck(spine_near(0.0f) == 1.0f && spine_near(1.0f) == 0.0f &&
       spine_near(-0.25f) == 0.75f, "nearness");
    ck(spine_slot_x_f(0.5f) == SPINE_ACTIVE_X + 0.5f * SPINE_STRIDE,
       "slot x is linear in distance");
}

int main(void)
{
    test_table();
    test_eval();
    test_divider_clearance();
    test_slots();
    test_motion();
    test_approach();
    if (fails) { printf("\ntest_spine: %d FAILED\n", fails); return 1; }
    printf("\ntest_spine: all passed\n");
    return 0;
}
