// Host test for render/wave_snow.h.
#include <stdio.h>
#include <math.h>
#include <string.h>
#include "wave_snow.h"

static int g_fail = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("  FAIL %s:%d: ", __FILE__, __LINE__); \
    printf(__VA_ARGS__); printf("\n"); g_fail++; } } while (0)

static ws_state  st;
static ws_sprite sp[WS_MAX];

int main(void)
{
    static float wv[WS_WV];
    ws_ctl c = { 0.5f, 0.5f, 0.0f, 0.5f, 0, -0.3f, 0, 0, 0, 0, 0, 0, 0 };
    int i, f, near = 0, vis;
    ws_init(&st, WS_COUNT_DEF, 1234u);
    for (i = 0; i < st.n; i++) near += st.z[i] < 0.22f;
    printf("  snow: %d particles, %d near (bokeh)\n", st.n, near);
    CHECK(near > st.n / 20 && near < st.n / 4, "bokeh share %d/%d", near, st.n);

    // ten minutes, with a kick every second: everything stays finite and on the field
    for (f = 0; f < 60 * 600; f++) {
        c.kick = (f % 60) == 0 ? 1.0f : 0.0f;
        ws_step(&st, &c, 1.0f / 60.0f);
    }
    for (i = 0; i < st.n; i++) {
        CHECK(st.x[i] == st.x[i] && st.y[i] == st.y[i], "NaN particle %d", i);
        CHECK(st.x[i] >= -WS_X_EDGE - 0.01f && st.x[i] <= WS_X_EDGE + 0.01f, "x out %d %f", i, st.x[i]);
        CHECK(st.y[i] >= -WS_Y_EDGE - 0.2f && st.y[i] <= WS_Y_EDGE + 0.25f, "y out %d %f", i, st.y[i]);
    }

    // a kick moves nothing: the sub-bass may glint the field, never push it
    {
        float vy0 = 0.0f, vy1 = 0.0f;
        c.kick = 0.0f;
        for (f = 0; f < 120; f++) ws_step(&st, &c, 1.0f / 60.0f);
        for (i = 0; i < st.n; i++) vy0 += st.vy[i];
        c.kick = 1.0f; ws_step(&st, &c, 1.0f / 60.0f);
        for (i = 0; i < st.n; i++) vy1 += st.vy[i];
        printf("  snow: mean vy %.4f -> %.4f on a kick\n", vy0 / st.n, vy1 / st.n);
        CHECK(vy1 / st.n < vy0 / st.n + 0.01f, "the kick lifts the field");
    }

    // the field fills the screen: every quarter of it holds particles
    {
        int q[4] = { 0, 0, 0, 0 };
        for (i = 0; i < st.n; i++)
            if (st.x[i] > -1.0f && st.x[i] < 1.0f && st.y[i] > -1.0f && st.y[i] < 1.0f)
                q[(st.x[i] >= 0.0f) + 2 * (st.y[i] >= 0.0f)]++;
        printf("  snow: quadrants %d %d %d %d\n", q[0], q[1], q[2], q[3]);
        for (i = 0; i < 4; i++) CHECK(q[i] > st.n / 10, "quadrant %d holds %d", i, q[i]);
    }

    // a rising wave lifts what floats near the band, and only there
    {
        float near_vy = 0.0f, far_vy = 0.0f; int nn = 0, nf = 0;
        for (i = 0; i < WS_WV; i++) wv[i] = 0.8f;
        c.wv = wv;
        for (i = 0; i < st.n; i++) st.vy[i] = 0.0f;
        ws_step(&st, &c, 1.0f / 60.0f);
        for (i = 0; i < st.n; i++) {
            const float d = st.y[i] - c.band_y;
            if (d > -0.2f && d < 0.2f) { near_vy += st.vy[i]; nn++; }
            if (d > 0.9f || d < -0.9f) { far_vy += st.vy[i]; nf++; }
        }
        near_vy /= nn ? nn : 1; far_vy /= nf ? nf : 1;
        printf("  snow: wave lift near %.4f far %.4f\n", near_vy, far_vy);
        CHECK(near_vy > far_vy + 0.005f, "the wave does not carry the particles near it");
        c.wv = 0;
    }

    // obstacles: after a minute nothing near enough to collide is inside the
    // box, while far particles still pass behind it
    {
        static const ws_rect box = { -0.70f, -0.30f, -0.10f, 0.45f };
        int inside_near = 0, inside_far = 0, hits = 0, f2;
        c.obst = &box; c.n_obst = 1;
        for (f2 = 0; f2 < 60 * 60; f2++) {
            ws_step(&st, &c, 1.0f / 60.0f);
            for (i = 0; i < st.n; i++) if (st.spark[i] > 0.5f) hits++;
        }
        for (i = 0; i < st.n; i++) {
            const int in = st.x[i] > box.x0 + 0.01f && st.x[i] < box.x1 - 0.01f &&
                           st.y[i] > box.y0 + 0.01f && st.y[i] < box.y1 - 0.01f;
            if (!in) continue;
            if (st.z[i] < WS_COLLIDE_Z) inside_near++; else inside_far++;
        }
        printf("  snow: obstacle -> %d near particles inside (want 0), %d far behind it, %d glint-frames\n",
               inside_near, inside_far, hits);
        CHECK(inside_near == 0, "%d colliding particles got inside the box", inside_near);
        CHECK(inside_far > 0, "no far particle passes behind the box");
        CHECK(hits > 0, "no impact ever glints");
        c.obst = 0; c.n_obst = 0;
    }

    // a beat ring: particles it passes glint; the push is VERY subtle --
    // measured against the same field stepped WITHOUT the ring
    {
        static ws_state ref;
        float off = 0.0f, worst = 0.0f;
        int glint = 0, f2;
        for (f2 = 0; f2 < 120; f2++) ws_step(&st, &c, 1.0f / 60.0f);
        for (i = 0; i < st.n; i++) st.spark[i] = 0.0f;
        ref = st;
        for (f2 = 0; f2 < 60; f2++) {
            ws_ctl cr = c;
            cr.ring_x = 0.0f; cr.ring_y = -0.5f; cr.ring_a = 1.0f;
            cr.ring_r = 0.95f * (float)f2 / 60.0f;
            ws_step(&st, &cr, 1.0f / 60.0f);
            ws_step(&ref, &c, 1.0f / 60.0f);
        }
        for (i = 0; i < st.n; i++) {
            float dx = st.x[i] - ref.x[i], dy = st.y[i] - ref.y[i];
            // a particle that wrapped in one field and not the other is not
            // a push: measure across the wrap
            if (dx >  WS_X_EDGE) dx -= 2.0f * WS_X_EDGE;
            if (dx < -WS_X_EDGE) dx += 2.0f * WS_X_EDGE;
            if (dy >  WS_Y_EDGE) dy -= 2.0f * WS_Y_EDGE;
            if (dy < -WS_Y_EDGE) dy += 2.0f * WS_Y_EDGE;
            const float d = sqrtf(dx * dx + dy * dy);
            if (st.spark[i] > 0.2f) glint++;
            off += d; if (d > worst) worst = d;
        }
        printf("  snow: beat ring -> %d particles glinting; pushed %.4f on average, %.4f at most\n",
               glint, off / st.n, worst);
        CHECK(glint > st.n / 10, "the ring touches too few particles (%d)", glint);
        CHECK(worst < 0.03f, "the ring shoves particles too far (%.4f)", worst);
    }

    // presence 0 draws nothing; presence 1 draws most of them
    c.kick = 0.0f;
    vis = ws_shade(&st, &c, 0.0f, sp, WS_MAX);
    CHECK(vis == 0, "%d particles drawn at zero presence", vis);
    vis = ws_shade(&st, &c, 1.0f, sp, WS_MAX);
    printf("  snow: %d of %d drawn\n", vis, st.n);
    CHECK(vis > st.n / 2, "only %d drawn", vis);

    printf(g_fail ? "FAILED: %d check(s)\n" : "OK\n", g_fail);
    return g_fail != 0;
}
