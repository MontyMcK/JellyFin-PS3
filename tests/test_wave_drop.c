// wave_drop.h: JellyDrop -- the JellyWave ribbons lofted round the bell.
#include <stdio.h>
#include <string.h>
#include <math.h>

#include "wave_field.h"
#include "wave_drop.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static jw_vert A[JW_VERTS], B[JW_VERTS];

static int finite_vert(const jw_vert *v)
{
    return v->x == v->x && v->y == v->y && v->z == v->z;
}

int main(void)
{
    static wf_field f;
    jd_motion mo;
    jd_pose   P;
    int i, li;

    // 1. the ring is closed, and every half-width is a real tube
    for (int k = 0; k < 9; k++)
        if (k != 8) CHECK(fabsf(JD_RING[0][k] - JD_RING[JD_RING_N - 1][k]) < 1e-4f);
    for (i = 0; i < JD_RING_N; i++) {
        CHECK(JD_RING[i][2] > 2.0f && JD_RING[i][2] < 12.0f);
        CHECK(fabsf(JD_RING[i][3] * JD_RING[i][3] + JD_RING[i][4] * JD_RING[i][4] - 1.0f) < 1e-3f);
        CHECK(JD_RING[i][0] > -40.0f && JD_RING[i][0] < 40.0f);
        CHECK(JD_RING[i][1] > -40.0f && JD_RING[i][1] < 40.0f);
    }

    // 2. jd_expn against libm where it matters (the design's bumps and slews)
    for (float x = 0.0f; x < 8.0f; x += 0.05f) {
        const float e = expf(-x), a = jd_expn(x);
        CHECK(fabsf(a - e) <= 3e-4f * e + 1e-7f);
    }
    CHECK(jd_expn(-1.0f) == 1.0f);
    CHECK(jd_expn(100.0f) == 0.0f);
    CHECK(jd_floor(-0.5f) == -1.0f && jd_floor(1.5f) == 1.0f && jd_floor(-2.0f) == -2.0f);

    wf_init(&f, 0);
    for (i = 0; i < 400; i++) wf_step(&f, 0.25f, 0.011f, 0.62f);
    jd_motion_init(&mo);

    // 3. morph 0 IS JellyWave: bit for bit, every layer
    jd_place(&mo, 0.0f, 16.0f / 9.0f, 0.0f, 0.02f, 0.40f, &P);
    CHECK(P.morph == 0.0f);
    for (li = 0; li < JW_LAYERS; li++) {
        const float *disp = f.sy[li];
        CHECK(jw_build_layer_k(&JW_LAYER[li], disp, WF_SAMPLES, 16.0f / 9.0f, jw_key_dir(), A, JW_VERTS) == JW_VERTS);
        CHECK(jd_build_layer_k(&JW_LAYER[li], li, disp, WF_SAMPLES, 16.0f / 9.0f, jw_key_dir(), &P, B, JW_VERTS) == JW_VERTS);
        CHECK(memcmp(A, B, sizeof A) == 0);
        CHECK(jd_layer_alpha(&JW_LAYER[li], li, 0.0f) == JW_LAYER[li].alpha);
    }

    // 4. every point of the morph is finite, on camera and in the guard band;
    //    at morph 1 the mark is centred where it was put and the right size
    for (int step = 0; step <= 20; step++) {
        const float m = (float)step / 20.0f;
        jd_place(&mo, m, 16.0f / 9.0f, 0.0f, 0.02f, 0.40f, &P);
        for (li = 0; li < JW_LAYERS; li++) {
            int bad = 0;
            CHECK(jd_build_layer_k(&JW_LAYER[li], li, f.sy[li], WF_SAMPLES, 16.0f / 9.0f,
                                   jw_key_dir(), &P, B, JW_VERTS) == JW_VERTS);
            for (i = 0; i < JW_VERTS; i++)
                if (!B[i].ok || !finite_vert(&B[i]) || fabsf(B[i].x) > 4.0f || fabsf(B[i].y) > 4.0f) bad++;
            CHECK(bad == 0);
            if (bad) printf("  morph %.2f layer %d: %d bad vertices\n", m, li, bad);
        }
    }
    {
        float x0 = 9, x1 = -9, y0 = 9, y1 = -9;
        jd_place(&mo, 1.0f, 16.0f / 9.0f, 0.0f, 0.02f, 0.40f, &P);
        jd_build_layer_k(&JW_LAYER[0], 0, f.sy[0], WF_SAMPLES, 16.0f / 9.0f, jw_key_dir(), &P, B, JW_VERTS);
        for (i = 0; i < JW_VERTS; i++) {
            if (B[i].x < x0) x0 = B[i].x;
            if (B[i].x > x1) x1 = B[i].x;
            if (B[i].y < y0) y0 = B[i].y;
            if (B[i].y > y1) y1 = B[i].y;
        }
        printf("mark at rest: x [%.3f, %.3f]  y [%.3f, %.3f]  height %.3f of the screen\n",
               x0, x1, y0, y1, (y1 - y0) * 0.5f);
        CHECK(fabsf(0.5f * (x0 + x1)) < 0.08f);             // centred (the rest pose)
        CHECK(fabsf(0.5f * (y0 + y1) - 0.02f) < 0.10f);
        CHECK((y1 - y0) * 0.5f > 0.30f && (y1 - y0) * 0.5f < 0.55f);
        CHECK(jd_layer_alpha(&JW_LAYER[2], 2, 1.0f) == 80);
    }

    // 5. the painter's sort still holds a ring together: strips come out in
    //    a permutation, never repeated
    {
        int order[JW_SECTION], seen[JW_SECTION];
        memset(seen, 0, sizeof seen);
        jw_strip_order(B, order);
        for (i = 0; i < JW_SECTION; i++) { CHECK(order[i] >= 0 && order[i] < JW_SECTION); seen[order[i]]++; }
        for (i = 0; i < JW_SECTION; i++) CHECK(seen[i] == 1);
    }

    // 6. ten minutes of a heavy beat and then silence: every spring settles,
    //    it stays on screen, bouncing off the edges, nothing goes non-finite
    {
        jd_in in;
        float max_wx = 0, max_wy = 0;
        jd_motion_init(&mo);
        for (i = 0; i < 60 * 600; i++) {
            const float t = i / 60.0f;
            memset(&in, 0, sizeof in);
            if (i < 60 * 540) {
                const float bt = t * 2.2f, sp = (bt - floorf(bt)) / 2.2f, k = expf(-sp * 7.0f);
                in.sub = in.bass = 0.2f + 0.8f * k;
                in.lowmid = in.mid = 0.5f; in.high = in.air = 0.6f;
                in.rms = 0.9f;
                if (floorf(bt) != floorf((i - 1) / 60.0f * 2.2f)) { in.onset = 1; in.onset_strength = 1; }
                if (i % (60 * 30) == 0) in.drop = 1;
            }
            jd_motion_step(&mo, &in, 1.0f / 60.0f);
            if (fabsf(mo.wx) > max_wx) max_wx = fabsf(mo.wx);
            if (fabsf(mo.wy) > max_wy) max_wy = fabsf(mo.wy);
        }
        printf("wander max |x| %.0f px  |y| %.0f px (1280x720 stage)\n", max_wx, max_wy);
        // 2026-09-27: it roams the WHOLE screen and bounces off its edges --
        // never past them (the bell's half extent inside 640 x 360), and it
        // really does travel out to them.
        CHECK(max_wx <= 522.5f && max_wy <= 252.5f);
        CHECK(max_wx > 400.0f && max_wy > 150.0f);
        CHECK(mo.swell == mo.swell && mo.bounce == mo.bounce && mo.jig == mo.jig);
        CHECK(fabsf(mo.bounce) < 0.05f && fabsf(mo.jig) < 0.05f && fabsf(mo.drop) < 0.05f);
        jd_place(&mo, 1.0f, 16.0f / 9.0f, 0.0f, 0.02f, 0.40f, &P);
        for (i = 0; i < 9; i++) CHECK(P.R[i] == P.R[i]);
        // R stays a rotation
        for (int a = 0; a < 3; a++) for (int b = 0; b < 3; b++) {
            const float d = P.R[a * 3] * P.R[b * 3] + P.R[a * 3 + 1] * P.R[b * 3 + 1] + P.R[a * 3 + 2] * P.R[b * 3 + 2];
            CHECK(fabsf(d - (a == b ? 1.0f : 0.0f)) < 1e-3f);
        }
    }

    // 7. the core: every shell projects, and it is off until the bell closes
    {
        float cx, cy, rx[JD_CORE_K], ry[JD_CORE_K], gc, gr, ac, ar;
        jd_motion_init(&mo);
        jd_place(&mo, 0.5f, 16.0f / 9.0f, 0.0f, 0.02f, 0.40f, &P);
        CHECK(!jd_core_shell(&P, 0, 16.0f / 9.0f, &cx, &cy, rx, ry, &gc, &gr, &ac, &ar));
        jd_place(&mo, 1.0f, 16.0f / 9.0f, 0.0f, 0.02f, 0.40f, &P);
        for (int k = 0; k < JD_CORE_SHELLS; k++) {
            CHECK(jd_core_shell(&P, k, 16.0f / 9.0f, &cx, &cy, rx, ry, &gc, &gr, &ac, &ar));
            CHECK(fabsf(cx) < 0.3f && fabsf(cy) < 0.4f);
            CHECK(gc > 0.0f && gc < 2.0f && ac > 0.0f && ac <= 1.0f);
        }
    }

    // 8. the orbit: both halves populated, the near half bigger on average
    {
        jd_motes ms;
        jd_sprite s;
        int nf = 0, nn = 0;
        float rf = 0, rn = 0;
        jd_motes_init(&ms, 7u);
        for (i = 0; i < 600; i++) jd_motes_step(&ms, 1.0f / 60.0f, 1.0f, 0.3f, (i % 60) == 0 ? 0.8f : 0.0f);
        jd_place(&mo, 1.0f, 16.0f / 9.0f, 0.0f, 0.02f, 0.40f, &P);
        for (i = 0; i < JD_MOTES; i++) {
            jd_motes_sprite(&ms, i, &P, 16.0f / 9.0f, 1.0f, 0.5f, &s);
            if (!s.a) continue;
            CHECK(s.x == s.x && s.y == s.y && s.r_px > 0.0f && s.r_px < 40.0f);
            if (s.front) { nn++; rn += s.r_px; } else { nf++; rf += s.r_px; }
        }
        printf("orbit: %d behind (mean r %.2f px)  %d in front (mean r %.2f px)\n",
               nf, nf ? rf / nf : 0.0f, nn, nn ? rn / nn : 0.0f);
        CHECK(nf > 10 && nn > 10);
        CHECK(rn / nn > rf / nf);
        jd_motes_sprite(&ms, 0, &P, 16.0f / 9.0f, 0.0f, 0.5f, &s);
        CHECK(s.a == 0);                                   // invisible while morphing in
    }

    if (fails) { printf("test_wave_drop: %d FAILED\n", fails); return 1; }
    printf("test_wave_drop: all passed\n");
    return 0;
}
