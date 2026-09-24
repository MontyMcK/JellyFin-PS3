// Host test for the renderer seam, source/ui/render/wave_field.h.
//
// The three stage tests each prove their own stage in isolation.  What they
// cannot see is the thing the renderer actually depends on: that three chains
// driven through one adapter stay bounded, stay distinct from each other, stay
// pinned at the screen edges, and come back the same way twice.  Those are
// properties of the COMBINATION, and every one of them is a bug that would
// show up on a TV rather than in a stage test.
//
// Invariants, not golden values (.clinerules rule 9).  The one place a
// measured number appears is the amplitude band, and it is asserted as a range
// with slack rather than as an equality -- the point of that check is "the
// soft clip never engages and the wave never goes flat", not "the peak is
// 0.653".
//
// libm is used here; the header may not.

#include <stdio.h>
#include <stdint.h>
#include <math.h>
#include <string.h>
#include <stdlib.h>

#include "../source/ui/render/wave_field.h"

static int failures = 0;

#define CHECK(cond, ...) do {                                   \
    if (!(cond)) {                                              \
        printf("  FAIL %s:%d: ", __FILE__, __LINE__);           \
        printf(__VA_ARGS__);                                    \
        printf("\n");                                           \
        failures++;                                             \
    }                                                           \
} while (0)

// One field is 9 KB or so; keep them off the stack.
static wf_field fa, fb;

// Nominal frame parameters: the spec's PERTURBATION and a full drive, at the
// dt the warm-up uses.
#define DT       2.0f
#define PERTURB  0.02f
#define DRIVE    1.0f

static int all_finite(const wf_field *f)
{
    int l, i;
    for (l = 0; l < WF_LAYERS; l++)
        for (i = 0; i < WF_SAMPLES; i++)
            if (!isfinite(f->sy[l][i]) || !isfinite(f->sz[l][i])) return 0;
    for (i = 0; i < WF_SAMPLES; i++)
        if (!isfinite(f->sx[i])) return 0;
    return 1;
}

static float peak(const wf_field *f)
{
    float mx = 0.0f;
    int   l, i;
    for (l = 0; l < WF_LAYERS; l++)
        for (i = 0; i < WF_SAMPLES; i++) {
            float a = fabsf(f->sy[l][i]);
            if (a > mx) mx = a;
        }
    return mx;
}

// ---------------------------------------------------------------------------
// A null field, a bad layer and a NaN position must all be survivable: the
// renderer calls wf_disp once per column per ribbon per frame, so a field that
// failed to initialise has to degrade to a flat wave rather than to a crash or
// to garbage geometry.
static void test_degenerate_input(void)
{
    wf_field *nul = NULL;
    static wf_field dead;

    CHECK(wf_init(nul, 0) == 0, "wf_init(NULL) should report failure");

    // Never initialised: live is whatever the BSS holds, which is 0.
    memset(&dead, 0, sizeof dead);
    CHECK(wf_disp(&dead, 0, 0.5f) == 0.0f, "inert field must read flat");
    wf_step(&dead, DT, PERTURB, DRIVE);          // must not crash
    wf_resample(&dead);                          // must not crash

    CHECK(wf_init(&fa, 0) == 1, "wf_init should succeed");
    CHECK(wf_disp(&fa, -1, 0.5f) == 0.0f, "layer -1 must read flat");
    CHECK(wf_disp(&fa, WF_LAYERS, 0.5f) == 0.0f, "layer N must read flat");
    CHECK(wf_disp(&fa, 0, NAN) == 0.0f, "NaN position must read flat");
    CHECK(isfinite(wf_disp(&fa, 0, -5.0f)), "u below range must be finite");
    CHECK(isfinite(wf_disp(&fa, 0,  5.0f)), "u above range must be finite");

    // A null field passed to the stage 3 bridge, and a bridge call with no
    // scratch, must both refuse rather than write somewhere.
    {
        static wr_vert out[2 * WF_SAMPLES];
        static float   sx[WF_SAMPLES], sy[WF_SAMPLES];
        CHECK(wf_strip(nul, 0, -1, 1, 0, 0.2f, 0.01f, 1,2,3, 4,5,6,
                       sx, sy, out, 2 * WF_SAMPLES) == 0,
              "wf_strip(NULL) should refuse");
        CHECK(wf_strip(&fa, 0, -1, 1, 0, 0.2f, 0.01f, 1,2,3, 4,5,6,
                       NULL, sy, out, 2 * WF_SAMPLES) == 0,
              "wf_strip with no scratch should refuse");
        CHECK(wf_strip(&fa, WF_LAYERS, -1, 1, 0, 0.2f, 0.01f, 1,2,3, 4,5,6,
                       sx, sy, out, 2 * WF_SAMPLES) == 0,
              "wf_strip with a bad layer should refuse");
    }
}

// ---------------------------------------------------------------------------
// The warm-up is the whole reason wf_init is not just three wk_init calls.
// Assert that it works: the field must already be moving when init returns,
// not ramping up over the first seconds on screen.
static void test_warmup_lands_in_band(void)
{
    static wf_field cold;
    float p_warm, p_cold;

    CHECK(wf_init(&fa, 0) == 1, "wf_init should succeed");
    p_warm = peak(&fa);

    // The same field with the warm-up skipped, for contrast.
    memset(&cold, 0, sizeof cold);
    {
        int l;
        for (l = 0; l < WF_LAYERS; l++)
            wk_init(&cold.ch[l], cold.ny[l], cold.nz[l],
                    cold.vy[l], cold.vz[l], WF_NODES, WF_SEED[l]);
        cold.live = 1;
        wf_resample(&cold);
    }
    p_cold = peak(&cold);

    printf("  warm-up: peak after wf_init = %.4f, without warm-up = %.4f\n",
           p_warm, p_cold);

    CHECK(p_cold == 0.0f, "a cold field should be exactly flat");
    CHECK(p_warm > 0.15f,
          "wf_init must return a field that is already moving (peak %.4f)",
          p_warm);
    CHECK(p_warm < WK_KNEE,
          "wf_init must not return a field sitting on the soft clip (%.4f)",
          p_warm);
}

// ---------------------------------------------------------------------------
// The pinned ends are what keep the ribbons anchored to the screen edges.
// Stage 1 pins node 0 and node n-1; stage 2 reaches its end controls exactly.
// Through the adapter that has to come out BIT EXACT, not merely small --
// a crest that drifts a pixel off the edge leaves a visible notch.
static void test_edges_pinned_exactly(void)
{
    int frame, l;

    CHECK(wf_init(&fa, 0) == 1, "wf_init should succeed");
    for (frame = 0; frame < 400; frame++) {
        wf_step(&fa, DT, PERTURB, DRIVE);
        for (l = 0; l < WF_LAYERS; l++) {
            CHECK(fa.sy[l][0] == 0.0f,
                  "layer %d left edge drifted to %g at frame %d",
                  l, fa.sy[l][0], frame);
            CHECK(fa.sy[l][WF_SAMPLES - 1] == 0.0f,
                  "layer %d right edge drifted to %g at frame %d",
                  l, fa.sy[l][WF_SAMPLES - 1], frame);
            CHECK(wf_disp(&fa, l, 0.0f) == 0.0f, "wf_disp(u=0) not pinned");
            CHECK(wf_disp(&fa, l, 1.0f) == 0.0f, "wf_disp(u=1) not pinned");
        }
        if (failures) return;       // one report is enough
    }
}

// ---------------------------------------------------------------------------
// The renderer multiplies this by a pixel amplitude and adds it to a baseline,
// then clamps to the screen.  If the displacement ever left [-1,1] the clamp
// would start doing the work and the wave would flatten against the screen
// edge; if it decayed to zero the wave would stop.  Neither may happen, over a
// much longer run than anyone will watch.
static void test_bounded_and_alive(void)
{
    float lo = 1e30f, hi = 0.0f;
    int   frame;

    CHECK(wf_init(&fa, 0) == 1, "wf_init should succeed");
    for (frame = 0; frame < 20000; frame++) {
        float p;
        wf_step(&fa, DT, PERTURB, DRIVE);
        if (!all_finite(&fa)) {
            CHECK(0, "non-finite sample at frame %d", frame);
            return;
        }
        p = peak(&fa);
        if (p < lo) lo = p;
        if (p > hi) hi = p;
    }
    printf("  20k frames: per-frame peak in [%.4f, %.4f]\n", lo, hi);

    CHECK(hi <= 1.0f, "displacement left [-1,1]: %.6f", hi);
    CHECK(hi < WK_KNEE,
          "the soft clip engaged (peak %.4f >= knee %.2f); the physics is no "
          "longer running in the identity region", hi, WK_KNEE);
    CHECK(lo > 0.02f, "the wave went flat (min per-frame peak %.5f)", lo);
}

// ---------------------------------------------------------------------------
// Three ribbons that move together look like one ribbon drawn three times.
// WF_RATE / WF_DRIVE / WF_SEED exist to prevent that, so assert they do.
static void test_layers_are_separated(void)
{
    double sep[3] = { 0, 0, 0 };
    int    frame, n = 0;
    static const int A[3] = { 0, 0, 1 }, B[3] = { 1, 2, 2 };

    CHECK(wf_init(&fa, 0) == 1, "wf_init should succeed");
    for (frame = 0; frame < 4000; frame++) {
        wf_step(&fa, DT, PERTURB, DRIVE);
        if (frame % 10) continue;
        {
            int p, i;
            for (p = 0; p < 3; p++) {
                float mx = 0.0f;
                for (i = 0; i < WF_SAMPLES; i++) {
                    float d = fabsf(fa.sy[A[p]][i] - fa.sy[B[p]][i]);
                    if (d > mx) mx = d;
                }
                sep[p] += mx;
            }
            n++;
        }
    }
    printf("  mean pairwise separation: 0-1 %.4f  0-2 %.4f  1-2 %.4f\n",
           sep[0] / n, sep[1] / n, sep[2] / n);

    // The amplitude itself is around 0.4, so a separation of 0.1 is already
    // "clearly different curves".  This is a floor, not a target.
    CHECK(sep[0] / n > 0.10, "layers 0 and 1 move together (%.4f)", sep[0] / n);
    CHECK(sep[1] / n > 0.10, "layers 0 and 2 move together (%.4f)", sep[1] / n);
    CHECK(sep[2] / n > 0.10, "layers 1 and 2 move together (%.4f)", sep[2] / n);
}

// ---------------------------------------------------------------------------
// Same seed, same sequence of calls, same field -- bit for bit.  The kernel's
// noise is an xorshift carried in the chain, so this holds exactly, and it is
// what makes a visual regression reproducible instead of a coin flip.
static void test_determinism(void)
{
    int frame;

    CHECK(wf_init(&fa, 0) == 1, "wf_init a should succeed");
    CHECK(wf_init(&fb, 0) == 1, "wf_init b should succeed");
    CHECK(memcmp(fa.sy, fb.sy, sizeof fa.sy) == 0,
          "two fields with the same seed differ straight out of wf_init");

    for (frame = 0; frame < 500; frame++) {
        wf_step(&fa, DT, PERTURB, DRIVE);
        wf_step(&fb, DT, PERTURB, DRIVE);
    }
    CHECK(memcmp(fa.sy, fb.sy, sizeof fa.sy) == 0,
          "two fields with the same seed diverged over 500 frames");
    CHECK(memcmp(fa.sz, fb.sz, sizeof fa.sz) == 0,
          "the z channel diverged over 500 frames");

    // A different seed must give a different field, or the seed argument is
    // decorative.
    CHECK(wf_init(&fb, 12345) == 1, "seeded wf_init should succeed");
    CHECK(memcmp(fa.sy, fb.sy, sizeof fa.sy) != 0,
          "a different seed produced an identical field");
}

// ---------------------------------------------------------------------------
// wf_disp is what the renderer calls, once per column.  It must agree with the
// samples it interpolates, stay inside the curve's own range (so the sampling
// cannot introduce an overshoot the spline stage was careful to rule out), and
// be monotone in the sense that it never jumps between adjacent columns.
static void test_disp_interpolation(void)
{
    int   l, i, frame;
    float worst_node = 0.0f, worst_jump = 0.0f;

    CHECK(wf_init(&fa, 0) == 1, "wf_init should succeed");
    for (frame = 0; frame < 200; frame++) wf_step(&fa, DT, PERTURB, DRIVE);

    for (l = 0; l < WF_LAYERS; l++) {
        float lo = fa.sy[l][0], hi = fa.sy[l][0];
        for (i = 0; i < WF_SAMPLES; i++) {
            if (fa.sy[l][i] < lo) lo = fa.sy[l][i];
            if (fa.sy[l][i] > hi) hi = fa.sy[l][i];
        }

        // Exactly on a sample, wf_disp must return that sample.
        for (i = 0; i < WF_SAMPLES; i++) {
            float u = (float)i / (float)(WF_SAMPLES - 1);
            float d = fabsf(wf_disp(&fa, l, u) - fa.sy[l][i]);
            if (d > worst_node) worst_node = d;
        }

        // Between samples it must stay within the curve's range, and adjacent
        // columns must not jump.  1921 probes is one per pixel column at 1080p
        // plus the right edge.
        {
            float prev = wf_disp(&fa, l, 0.0f);
            for (i = 1; i <= 1920; i++) {
                float u = (float)i / 1920.0f;
                float v = wf_disp(&fa, l, u);
                float j = fabsf(v - prev);
                CHECK(v >= lo - 1e-6f && v <= hi + 1e-6f,
                      "layer %d: wf_disp %.6f left the curve range [%.6f, %.6f]",
                      l, v, lo, hi);
                if (j > worst_jump) worst_jump = j;
                prev = v;
            }
        }
    }
    printf("  wf_disp: worst error on a sample %.3g, worst column-to-column "
           "step %.5f\n", worst_node, worst_jump);

    CHECK(worst_node <= 1e-6f,
          "wf_disp does not reproduce its own samples (worst %.3g)",
          worst_node);
    // At 1920 columns over a curve whose peak is under 0.7, a step of 0.02
    // would be a visible kink.
    CHECK(worst_jump < 0.02f,
          "wf_disp jumps between adjacent columns (worst %.5f)", worst_jump);
}

// ---------------------------------------------------------------------------
// The stage 3 bridge.  Not on the renderer's current path, but it is reachable
// code, so it gets the same contract check the ribbon test applies: finite,
// inside clip space, opaque, and the right count.
static void test_strip_bridge(void)
{
    static wr_vert out[2 * WF_SAMPLES];
    static float   sx[WF_SAMPLES], sy[WF_SAMPLES];
    int n, i, frame;

    CHECK(wf_init(&fa, 0) == 1, "wf_init should succeed");
    for (frame = 0; frame < 100; frame++) wf_step(&fa, DT, PERTURB, DRIVE);

    n = wf_strip(&fa, 0, -1.0f, 1.0f, -0.55f, 0.18f, 0.01f,
                 0x11, 0x22, 0x33, 0x44, 0x55, 0x66,
                 sx, sy, out, 2 * WF_SAMPLES);
    CHECK(n == 2 * WF_SAMPLES, "expected %d vertices, got %d",
          2 * WF_SAMPLES, n);

    for (i = 0; i < n; i++) {
        CHECK(isfinite(out[i].x) && isfinite(out[i].y),
              "vertex %d is not finite", i);
        CHECK(out[i].x >= -1.0f && out[i].x <= 1.0f,
              "vertex %d x = %f outside clip space", i, out[i].x);
        CHECK(out[i].y >= -1.0f && out[i].y <= 1.0f,
              "vertex %d y = %f outside clip space", i, out[i].y);
        CHECK(out[i].z == 0.0f && out[i].w == 1.0f,
              "vertex %d has z/w = %f/%f", i, out[i].z, out[i].w);
        CHECK((out[i].rgba & 0xFFu) == 0xFFu,
              "vertex %d is not opaque (rgba %08x)", i, out[i].rgba);
        if (failures) return;
    }

    // Too little room must refuse rather than overrun.
    CHECK(wf_strip(&fa, 0, -1.0f, 1.0f, -0.55f, 0.18f, 0.01f,
                   0x11, 0x22, 0x33, 0x44, 0x55, 0x66,
                   sx, sy, out, 2 * WF_SAMPLES - 1) == 0,
          "wf_strip should refuse a short buffer");
    printf("  strip bridge: %d vertices, %d bytes, stride %d\n",
           n, (int)(n * (int)sizeof(wr_vert)), (int)sizeof(wr_vert));
}

// ---------------------------------------------------------------------------
// A frame that arrives late must not detonate the wave.  wk_step clamps dt and
// subdivides it, so a long delta should produce more motion, never a blow-up,
// and a zero or negative delta should leave the field exactly alone.
static void test_hostile_dt(void)
{
    static wf_field before;
    int frame;

    CHECK(wf_init(&fa, 0) == 1, "wf_init should succeed");
    memcpy(&before, &fa, sizeof before);

    wf_step(&fa, 0.0f, PERTURB, DRIVE);
    CHECK(memcmp(fa.sy, before.sy, sizeof fa.sy) == 0, "dt=0 moved the field");
    wf_step(&fa, -3.0f, PERTURB, DRIVE);
    CHECK(memcmp(fa.sy, before.sy, sizeof fa.sy) == 0, "dt<0 moved the field");
    wf_step(&fa, NAN, PERTURB, DRIVE);
    CHECK(memcmp(fa.sy, before.sy, sizeof fa.sy) == 0, "dt=NaN moved the field");
    wf_step(&fa, INFINITY, PERTURB, DRIVE);
    CHECK(memcmp(fa.sy, before.sy, sizeof fa.sy) == 0, "dt=inf moved the field");

    // Absurd deltas, and an absurd perturbation, for a long time.
    for (frame = 0; frame < 2000; frame++) {
        wf_step(&fa, 1000.0f, 2.0f, 4.0f);
        if (!all_finite(&fa)) {
            CHECK(0, "non-finite sample under hostile dt at frame %d", frame);
            return;
        }
    }
    printf("  hostile dt: survived 2000 frames at dt=1000, peak %.4f\n",
           peak(&fa));
    CHECK(peak(&fa) <= 1.0f, "hostile dt left [-1,1]: %.6f", peak(&fa));

    // And it must recover: back to nominal, the wave keeps running.
    for (frame = 0; frame < 200; frame++) wf_step(&fa, DT, PERTURB, DRIVE);
    CHECK(all_finite(&fa), "field did not recover after hostile dt");
    CHECK(peak(&fa) > 0.02f, "field went flat after hostile dt");
}

int main(void)
{
    printf("wave_field\n");
    test_degenerate_input();
    test_warmup_lands_in_band();
    test_edges_pinned_exactly();
    test_bounded_and_alive();
    test_layers_are_separated();
    test_determinism();
    test_disp_interpolation();
    test_strip_bridge();
    test_hostile_dt();

    if (failures) {
        printf("wave_field: %d FAILURE(S)\n", failures);
        return 1;
    }
    printf("wave_field: all checks passed\n");
    return 0;
}
