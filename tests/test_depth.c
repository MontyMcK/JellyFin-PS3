// Host tests for source/ui/render/depth.h -- the depth engine's layouts.
//
//   cc -std=c99 -Wall -Wextra -I../source/ui/render -o test_depth test_depth.c -lm
//
// What this guards:
//
//   1. THE REFACTOR.  spine6 drew Home from geometry that lived as file
//      statics in xmb/ui_home_stage.inc (q1_slot, q2_slot, q_box).  That code
//      is reproduced below VERBATIM as a reference, and the engine must give
//      the same box for every slot, swing and shape -- so moving Home onto
//      the engine cannot have moved a single poster.
//   2. THE SWING.  At e = 0 an item is exactly on its column slot, at e = 1
//      exactly on its L2 slot (queue or grid cell), and at the grid hand-over
//      threshold no item is more than ~1 px from its cell at 1080p.
//   3. FIT.  The focused item, the facts column and every visible queue item
//      stay on screen and clear of each other at 720p and 1080p.
//   4. NAVIGATION STATE.  Empty categories are skipped; remembered focus
//      clamps to what is loaded and stays inside the visible window.
//   5. FADES.  Every level fade is monotonic and in [0, 1], and the L1 label
//      has left before the L2 facts arrive.

#include <stdio.h>
#include <string.h>
#include <math.h>
#include "depth.h"

static int g_fail = 0, g_checks = 0;

#define CHECK(c, ...) do {                                     \
    g_checks++;                                                \
    if (!(c)) {                                                \
        g_fail++;                                              \
        printf("FAIL %s:%d: ", __FILE__, __LINE__);            \
        printf(__VA_ARGS__);                                   \
        printf("\n");                                          \
    }                                                          \
} while (0)

static int feq(float a, float b) { return fabsf(a - b) < 1e-4f; }
static int box_eq(const depth_box *a, const depth_box *b)
{
    return feq(a->x, b->x) && feq(a->top, b->top) && feq(a->w, b->w) &&
           feq(a->h, b->h) && feq(a->op, b->op) && feq(a->veil, b->veil) &&
           feq(a->refl, b->refl);
}

// ---------------------------------------------------------------------------
// The reference: ui_home_stage.inc as spine6 built it (commit 3cbab08),
// copied with only the QBox -> depth_box rename.
// ---------------------------------------------------------------------------
#define Q_KMIN   (-1)
#define Q_KMAX     5
#define Q_NK     (Q_KMAX - Q_KMIN + 1)
#define Q_GAP     24.0f
static const float Q2_W[Q_NK]   = { 200, 200, 104,  92,  80,  70,  62 };
static const float Q2_H[Q_NK]   = { 300, 300, 156, 138, 120, 105,  93 };
static const float Q2_BOT[Q_NK] = { 566, 566, 552, 550, 548, 545, 543 };
static const float Q2_OP[Q_NK]  = { 0.0f, 1.0f, 0.62f, 0.48f, 0.36f, 0.26f, 0.0f };
static const float Q2_VEIL[Q_NK]= { 0.0f, 0.0f, 0.13f, 0.18f, 0.22f, 0.26f, 0.26f };
static const float Q1_W[Q_NK]   = { 128, 128, 106,  86,  70,  60,  50 };
static const float Q1_H[Q_NK]   = { 192, 192, 159, 129, 105,  90,  75 };
static const float Q1_OP[Q_NK]  = { 0.0f, 1.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0.0f };
static const float Q1_VEIL[Q_NK]= { 0.0f, 0.0f, 0.34f, 0.58f, 0.7f, 0.7f, 0.7f };
#define Q1_TOP   318.0f
#define Q1_GAP    14.0f
static float q_shape_w(float w, int sq) { return sq ? w * 1.2f : w; }
static depth_box q2_slot(int k, int sq) {
    const int i = k - Q_KMIN;
    depth_box b;
    b.w = q_shape_w(Q2_W[i], sq);
    b.h = sq ? b.w : Q2_H[i];
    b.top = Q2_BOT[i] - b.h;
    b.op = Q2_OP[i];
    b.veil = Q2_VEIL[i];
    b.refl = k <= 0 ? 46.0f : 0.18f * b.h;
    if (k == 0)      b.x = 64.0f;
    else if (k < 0)  b.x = 64.0f - b.w - 40.0f;
    else {
        float x = 836.0f + (q_shape_w(200.0f, sq) - 200.0f);
        for (int j = 1; j < k; j++) x += q_shape_w(Q2_W[j - Q_KMIN], sq) + Q_GAP;
        b.x = x;
    }
    return b;
}
static depth_box q1_slot(int k, int sq, float anchor_x) {
    const int i = k - Q_KMIN;
    depth_box b;
    b.w = q_shape_w(Q1_W[i], sq);
    b.h = sq ? b.w : Q1_H[i];
    b.op = Q1_OP[i];
    b.veil = Q1_VEIL[i];
    b.refl = 0.0f;
    b.x = anchor_x - b.w / 2.0f;
    float top = Q1_TOP;
    if (k > 0)
        for (int j = 0; j < k; j++) {
            const float wj = q_shape_w(Q1_W[j - Q_KMIN], sq);
            top += (sq ? wj : Q1_H[j - Q_KMIN]) + Q1_GAP;
        }
    b.top = top;
    return b;
}
static int q_box(float k, float e, int sq, float anchor_x, depth_box *out) {
    if (k <= (float)Q_KMIN || k >= (float)Q_KMAX) return 0;
    int k0 = (int)floorf(k);
    const float f = k - (float)k0;
    depth_box a1 = q1_slot(k0, sq, anchor_x), a2 = q2_slot(k0, sq);
    depth_box b1 = q1_slot(k0 + 1, sq, anchor_x), b2 = q2_slot(k0 + 1, sq);
    const depth_box a = depth_box_lerp(&a1, &a2, e);
    const depth_box b = depth_box_lerp(&b1, &b2, e);
    *out = depth_box_lerp(&a, &b, f);
    return out->op > 0.004f;
}

// ---------------------------------------------------------------------------

static void test_home_equivalence(void)
{
    static const float anchors[] = { 230.0f, 130.0f, 430.0f, 317.5f };
    for (int sq = 0; sq <= 1; sq++) {
        const int shape = sq ? DEPTH_SQUARE : DEPTH_POSTER;
        for (int k = Q_KMIN; k <= Q_KMAX; k++) {
            depth_box a = depth_queue_slot(k, shape), b = q2_slot(k, sq);
            CHECK(box_eq(&a, &b), "queue slot k=%d sq=%d differs from spine6", k, sq);
            for (unsigned ai = 0; ai < sizeof anchors / sizeof anchors[0]; ai++) {
                a = depth_column_slot(k, shape, anchors[ai]);
                b = q1_slot(k, sq, anchors[ai]);
                CHECK(box_eq(&a, &b), "column slot k=%d sq=%d anchor=%.1f differs", k, sq, anchors[ai]);
            }
        }
        for (float k = -1.5f; k <= 5.5f; k += 0.125f)
            for (float e = 0.0f; e <= 1.0001f; e += 0.0625f) {
                depth_box a, b;
                const int ra = depth_column_queue_box(k, e, shape, 230.0f, &a);
                const int rb = q_box(k, e, sq, 230.0f, &b);
                CHECK(ra == rb, "swing visibility k=%.3f e=%.3f sq=%d", k, e, sq);
                if (ra && rb)
                    CHECK(box_eq(&a, &b), "swing box k=%.3f e=%.3f sq=%d differs", k, e, sq);
            }
    }
}

static void test_swing_endpoints(void)
{
    for (int shape = 0; shape <= 2; shape++)
        for (int k = 0; k <= 3; k++) {
            depth_box s, c = depth_column_slot(k, shape, 230.0f), q = depth_queue_slot(k, shape);
            if (depth_column_queue_box((float)k, 0.0f, shape, 230.0f, &s))
                CHECK(box_eq(&s, &c), "e=0 is not the column, k=%d shape=%d", k, shape);
            if (depth_column_queue_box((float)k, 1.0f, shape, 230.0f, &s))
                CHECK(box_eq(&s, &q), "e=1 is not the queue, k=%d shape=%d", k, shape);
        }
    // The swing is eased time: flat at both ends, monotonic between.
    CHECK(depth_swing(0.0f) == 0.0f && depth_swing(1.0f) == 1.0f, "swing endpoints");
    CHECK(depth_swing(2.0f) == 1.0f && depth_swing(-1.0f) == 0.0f, "swing clamps");
    float prev = 0.0f;
    for (float d = 0.0f; d <= 1.0f; d += 0.01f) {
        const float e = depth_swing(d);
        CHECK(e >= prev - 1e-6f, "swing not monotonic at %.2f", d);
        prev = e;
    }
}

// A 1920x1080 library grid laid out the way ui_lists.cpp does it: 5 x 2
// posters, first cell at (x0, y0), card_w + gap pitch, row stride.
static depth_grid grid_1080(int scroll)
{
    depth_grid g;
    g.cols = 5; g.vis = 10; g.scroll = scroll;
    g.cell_w = 198.0f; g.cell_h = 297.0f;
    g.pitch_x = 198.0f + 36.0f; g.pitch_y = 297.0f + 75.0f;
    g.x0 = (1920.0f - (5 * 198.0f + 4 * 36.0f)) * 0.5f;
    g.y0 = 381.0f;
    return g;
}

static void test_grid_swing(void)
{
    const depth_xform xf = depth_xform_make(0, 0, 1920, 1080, 0);
    for (int scroll = 0; scroll <= 10; scroll += 5) {
        const depth_grid g = grid_1080(scroll);
        for (int focus = scroll; focus < scroll + g.vis; focus++) {
            float worst = 0.0f;
            for (int idx = focus - 12; idx <= focus + 12; idx++) {
                if (idx < 0) continue;
                depth_box s, cell = depth_grid_cell(&g, idx);
                depth_column_grid_box(idx, focus, 1.0f, DEPTH_POSTER, 230.0f, &xf, &g, &s);
                CHECK(box_eq(&s, &cell), "e=1 is not the grid cell, idx=%d focus=%d", idx, focus);
                // Inside the window the cell is opaque, outside it is not drawn.
                const int in = idx >= scroll && idx < scroll + g.vis;
                CHECK((cell.op == 1.0f) == in, "cell opacity idx=%d scroll=%d", idx, scroll);

                // e = 0: only the column's own items are visible.
                depth_column_grid_box(idx, focus, 0.0f, DEPTH_POSTER, 230.0f, &xf, &g, &s);
                const int k = idx - focus;
                if (k < 0 || k > 2)
                    CHECK(s.op == 0.0f, "item outside the column visible at L1, k=%d", k);
                else {
                    depth_box c = depth_column_slot(k, DEPTH_POSTER, 230.0f);
                    depth_box cs = depth_to_screen(&xf, &c, 0.0f);
                    CHECK(box_eq(&s, &cs), "e=0 is not the column slot, k=%d", k);
                }

                // The hand-over: within ~1 px of the cell at 1080p.
                if (in) {
                    depth_column_grid_box(idx, focus, DEPTH_HANDOFF_E, DEPTH_POSTER,
                                          230.0f, &xf, &g, &s);
                    const float dx = fabsf(s.x - cell.x), dy = fabsf(s.top - cell.top);
                    const float dw = fabsf(s.w - cell.w), dh = fabsf(s.h - cell.h);
                    float m = dx > dy ? dx : dy;
                    m = m > dw ? m : dw;
                    m = m > dh ? m : dh;
                    if (m > worst) worst = m;
                }
            }
            CHECK(worst <= 1.5f, "hand-over is %.2f px from the grid (focus %d)", worst, focus);
        }
    }
    // Negative relative indices land on the rows above, not wrapped.
    const depth_grid g = grid_1080(5);
    depth_box c = depth_grid_cell(&g, 4);
    CHECK(feq(c.top, g.y0 - g.pitch_y) && feq(c.x, g.x0 + 4 * g.pitch_x) && c.op == 0.0f,
          "cell above the window (%.1f, %.1f)", c.x, c.top);
}

static void test_fit(void)
{
    static const int res[2][2] = { { 1280, 720 }, { 1920, 1080 } };
    for (int r = 0; r < 2; r++) {
        const depth_xform xf = depth_xform_make(0, 0, res[r][0], res[r][1], 0);
        const float W = (float)res[r][0], H = (float)res[r][1];
        for (int shape = 0; shape <= 2; shape += 2) {       // posters and squares
            // Queue: every visible slot on screen horizontally except the
            // last square (0.26 opacity), which the canvas lets run off the
            // right edge "into empty air".
            for (int k = 0; k <= 4; k++) {
                depth_box b = depth_queue_slot(k, shape);
                depth_box s = depth_to_screen(&xf, &b, 0.0f);
                if (shape == DEPTH_SQUARE && k == 4) continue;
                CHECK(s.x >= 0.0f && s.x + s.w <= W + 0.5f, "queue k=%d shape=%d off screen at %dp", k, shape, res[r][1]);
                CHECK(s.top >= 0.0f && s.top + s.h <= H, "queue k=%d vertical at %dp", k, res[r][1]);
            }
            // The facts column sits between the focus and the queue.
            const depth_facts f = depth_facts_layout(shape, 1, 1.0f, 0.0f);
            const depth_box f0 = depth_queue_slot(0, shape), q1 = depth_queue_slot(1, shape);
            CHECK(f.x > f0.x + f0.w, "facts overlap the focus, shape=%d", shape);
            CHECK(f.x + DEPTH_FACTS_W < q1.x, "facts run under the queue, shape=%d (%.1f vs %.1f)",
                  shape, f.x + DEPTH_FACTS_W, q1.x);
            // The column's focused item is wholly on screen above the hints bar.
            depth_box c = depth_column_slot(0, shape, SPINE_ACTIVE_X);
            depth_box s = depth_to_screen(&xf, &c, 0.0f);
            CHECK(s.x >= 0.0f && s.top + s.h <= H - 70.0f * xf.ky,
                  "column focus off screen, shape=%d at %dp", shape, res[r][1]);
            // And its top clears the L1 underline.
            const spine_level L1 = SPINE_LEVEL[SPINE_L1];
            CHECK(c.top >= spine_rule_bottom(&L1) + 8.0f, "column top %.1f under the underline %.1f",
                  c.top, spine_rule_bottom(&L1));
        }
        // The L1 label sits right of the art and clear of the next slot's icon.
        const depth_l1_label L = depth_l1_label_layout(DEPTH_POSTER, SPINE_ACTIVE_X);
        CHECK(L.x > SPINE_ACTIVE_X + 64.0f, "label over the art");
        CHECK(L.y_title > L.y_eye && L.y_meta > L.y_title, "label stack order");
    }
    // Wide columns are the library's 224x126.
    depth_box w = depth_column_slot(0, DEPTH_WIDE, 230.0f);
    CHECK(feq(w.w, 224.0f) && feq(w.h, 126.0f), "wide column %.1fx%.1f", w.w, w.h);
}

static void test_rows(void)
{
    depth_row r[2];
    int n = depth_rows(2.0f, 5, r);
    CHECK(n == 1 && r[0].r == 2 && feq(r[0].w, 1.0f) && feq(r[0].dy, 0.0f), "row at rest");
    n = depth_rows(2.25f, 5, r);
    CHECK(n == 2 && r[0].r == 2 && r[1].r == 3, "two rows mid-glide");
    CHECK(feq(r[0].w + r[1].w, 1.0f), "weights sum to 1");
    CHECK(feq(r[0].dy, -0.25f * DEPTH_ROW_DY) && feq(r[1].dy, 0.75f * DEPTH_ROW_DY), "glide offsets");
    n = depth_rows(4.5f, 5, r);
    CHECK(n == 1 && r[0].r == 4, "past the last row");
    n = depth_rows(-0.5f, 5, r);
    CHECK(n == 1 && r[0].r == 0, "before the first row");
}

static void test_visitable(void)
{
    const unsigned char v[6] = { 0, 1, 0, 0, 1, 1 };
    CHECK(depth_step_visitable(v, 6, 1, +1) == 4, "down skips empties");
    CHECK(depth_step_visitable(v, 6, 4, -1) == 1, "up skips empties");
    CHECK(depth_step_visitable(v, 6, 1, -1) == 1, "nothing above stays");
    CHECK(depth_step_visitable(v, 6, 5, +1) == 5, "nothing below stays");
    CHECK(depth_nearest_visitable(v, 6, 2) == 1, "nearest: 2 -> 1 (4 is two away)");
    CHECK(depth_nearest_visitable(v, 6, 3) == 4, "nearest prefers down on a tie");
    CHECK(depth_nearest_visitable(v, 6, 5) == 5, "visitable stays");
    CHECK(depth_at_top(v, 6, 1) && !depth_at_top(v, 6, 4), "at_top");
    const unsigned char none[3] = { 0, 0, 0 };
    CHECK(depth_nearest_visitable(none, 3, 1) == 1, "no visitable rows");
}

static void test_focus_mem(void)
{
    depth_focus_mem m;
    int sel, scroll;
    depth_focus_init(&m);
    CHECK(!depth_focus_restore(&m, 3, 50, 5, 10, &sel, &scroll) && sel == 0 && scroll == 0,
          "nothing remembered");
    depth_focus_save(&m, 3, 27, 20);
    CHECK(depth_focus_restore(&m, 3, 50, 5, 10, &sel, &scroll) && sel == 27 && scroll == 20,
          "plain restore %d/%d", sel, scroll);
    // The library shrank: clamp, and keep the selection visible.
    CHECK(depth_focus_restore(&m, 3, 12, 5, 10, &sel, &scroll) && sel == 11 && scroll == 10,
          "shrunk restore %d/%d", sel, scroll);
    // A saved scroll that no longer shows the selection is re-derived.
    depth_focus_save(&m, 4, 3, 30);
    CHECK(depth_focus_restore(&m, 4, 50, 5, 10, &sel, &scroll) && sel == 3 && scroll == 0,
          "scroll re-derived %d/%d", sel, scroll);
    // A misaligned scroll snaps to its row.
    depth_focus_save(&m, 5, 13, 12);
    CHECK(depth_focus_restore(&m, 5, 50, 5, 10, &sel, &scroll) && sel == 13 && scroll == 10,
          "scroll aligned %d/%d", sel, scroll);
    // Nothing loaded: nothing restored.
    CHECK(!depth_focus_restore(&m, 3, 0, 5, 10, &sel, &scroll) && sel == 0, "empty library");
    depth_focus_forget(&m, 3);
    CHECK(!depth_focus_restore(&m, 3, 50, 5, 10, &sel, &scroll), "forgotten");
    // Out-of-range categories are ignored, not written.
    depth_focus_save(&m, -1, 5, 5);
    depth_focus_save(&m, DEPTH_MAX_CATS, 5, 5);
    CHECK(!depth_focus_restore(&m, DEPTH_MAX_CATS, 50, 5, 10, &sel, &scroll), "out of range");
}

static void test_fades(void)
{
    float pl = 2.0f, pf = -1.0f, po = -1.0f;
    for (float d = 0.0f; d <= 2.0001f; d += 0.01f) {
        const float l = depth_l1_label_alpha(d, 1.0f), f = depth_l2_facts_alpha(d);
        const float o = depth_over_l2(d);
        CHECK(l >= 0.0f && l <= 1.0f && f >= 0.0f && f <= 1.0f, "fade range at %.2f", d);
        CHECK(l <= pl + 1e-6f && f >= pf - 1e-6f && o >= po - 1e-6f, "fade monotonic at %.2f", d);
        // Never both on screen: the label is gone before the facts start.
        CHECK(l == 0.0f || f == 0.0f, "label and facts overlap at %.2f", d);
        pl = l; pf = f; po = o;
    }
    CHECK(depth_l1_label_alpha(0.0f, 1.0f) == 1.0f && depth_l2_facts_alpha(1.0f) == 1.0f,
          "fade endpoints");
    CHECK(depth_over_l2(1.0f) == 0.0f && depth_over_l2(2.0f) == 1.0f, "detail settle");
    CHECK(depth_queue_settle(3.0f, 3) == 1.0f && depth_queue_settle(3.5f, 3) == 0.0f,
          "queue settle");
    CHECK(depth_halo(0.0f) == 1.0f && depth_halo(0.7f) == 0.0f && depth_halo(-0.3f) > 0.5f,
          "halo");
    CHECK(!depth_grid_owns(0.998f) && depth_grid_owns(0.9995f) && depth_grid_owns(1.0f),
          "hand-over threshold");
}

static void test_refl_clamp(void)
{
    // Room to spare: untouched.
    CHECK(feq(depth_refl_clamp(266.0f, 300.0f, 46.0f, 4.0f, 664.0f), 46.0f), "refl fits");
    // Short of room: ends exactly on the floor.
    CHECK(feq(depth_refl_clamp(266.0f, 300.0f, 46.0f, 4.0f, 600.0f), 30.0f), "refl shortened");
    // The card itself reaches the floor: no reflection, never negative.
    CHECK(depth_refl_clamp(266.0f, 300.0f, 46.0f, 4.0f, 560.0f) == 0.0f, "refl gone");
}

static void test_mix(void)
{
    const unsigned a = 0x102030, b = 0xF0E0D0;
    CHECK(depth_mix_q(a, b, 0.0f) == a && depth_mix_q(a, b, 1.0f) == b, "mix endpoints");
    CHECK(depth_mix_q(a, b, -3.0f) == a && depth_mix_q(a, b, 7.0f) == b, "mix clamps");
    // At most 17 distinct colours over any fade: the text-run cache's budget.
    unsigned seen[64];
    int n = 0;
    for (int i = 0; i <= 1000; i++) {
        const unsigned c = depth_mix_q(a, b, (float)i / 1000.0f);
        int j;
        for (j = 0; j < n; j++) if (seen[j] == c) break;
        if (j == n && n < 64) seen[n++] = c;
    }
    CHECK(n <= 17, "%d distinct colours in a fade", n);
    CHECK(depth_a8(0.5f) == 128 && depth_a8(2.0f) == 255 && depth_a8(-1.0f) == 0, "a8");
}

static void test_xform(void)
{
    // Matches UIS_W / UIS_H: the SAFE rect / 1280x720, or the override percent.
    depth_xform xf = depth_xform_make(0, 0, 1920, 1080, 0);
    depth_box b = { 100.0f, 200.0f, 128.0f, 192.0f, 1.0f, 0.0f, 46.0f };
    depth_box s = depth_to_screen(&xf, &b, 10.0f);
    CHECK(feq(s.x, 150.0f) && feq(s.top, 315.0f) && feq(s.w, 192.0f) &&
          feq(s.h, 288.0f) && feq(s.refl, 69.0f), "1080p transform");
    // Under overscan the canvas fits INSIDE the inset: 58 px each side of a
    // 1920 screen leaves 1804, so authored 1280 lands exactly on 1920 - 58.
    xf = depth_xform_make(58, 32, 1920, 1080, 0);
    b.x = 1280.0f; b.top = 720.0f;
    s = depth_to_screen(&xf, &b, 0.0f);
    CHECK(fabsf(s.x - (1920.0f - 58.0f)) < 0.01f && fabsf(s.top - (1080.0f - 32.0f)) < 0.01f,
          "overscan: canvas edge %.2f,%.2f is not the safe edge", s.x, s.top);
    b.x = 100.0f; b.top = 200.0f;
    xf = depth_xform_make(0, 0, 1920, 1080, 100);
    s = depth_to_screen(&xf, &b, 0.0f);
    CHECK(feq(s.x, 100.0f) && feq(s.w, 128.0f), "uiscale 100 override");
    int x, y, w, h;
    depth_box r = { 10.4f, 10.6f, 20.5f, 1.49f, 1, 0, 0 };
    depth_rect(&r, &x, &y, &w, &h);
    CHECK(x == 10 && y == 11 && w == 21 && h == 1, "rounding %d %d %d %d", x, y, w, h);
}

static void test_follow(void)
{
    spine_approach a;
    depth_follow(&a, 4.0f, 1, SPINE_CATEGORY_MS, 0);
    CHECK(a.value == 4.0f && a.target == 4.0f, "snap");
    depth_follow(&a, 5.0f, 0, SPINE_CATEGORY_MS, 16667);
    CHECK(a.value > 4.0f && a.value < 5.0f, "step toward");
    for (int i = 0; i < 200; i++) depth_follow(&a, 5.0f, 0, SPINE_CATEGORY_MS, 16667);
    CHECK(a.value == 5.0f, "settles exactly");
}

int main(void)
{
    test_home_equivalence();
    test_swing_endpoints();
    test_grid_swing();
    test_fit();
    test_rows();
    test_visitable();
    test_focus_mem();
    test_fades();
    test_refl_clamp();
    test_mix();
    test_xform();
    test_follow();
    printf("test_depth: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
