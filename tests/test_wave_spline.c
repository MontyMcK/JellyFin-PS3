// Host test for stage 2 of the wave geometry pipeline,
// source/ui/render/wave_spline.h.
//
// The two properties this stage exists for are provable rather than
// statistical, so they are tested as such:
//
//   the basis is a partition of unity with no negative lobe, so every output
//   sample is a convex combination of four control values and the curve
//   CANNOT leave the range of its inputs -- that is what makes overshoot
//   impossible rather than merely unlikely;
//
//   the clamped end controls make the curve pass exactly through the first
//   and last node, which is what lets stage 3 rely on the chain's pinned ends
//   landing on the screen edge.
//
// libm is used here; the header may not.

#include <stdio.h>
#include <stdint.h>
#include <math.h>
#include <string.h>

#include "../source/ui/render/wave_kernel.h"
#include "../source/ui/render/wave_spline.h"

#define NODES   96
#define SAMPLES 512

static int failures = 0;

#define CHECK(cond, ...) do {                                   \
    if (!(cond)) {                                              \
        printf("  FAIL %s:%d: ", __FILE__, __LINE__);           \
        printf(__VA_ARGS__);                                    \
        printf("\n");                                           \
        failures++;                                             \
    }                                                           \
} while (0)

static float ox[8192], oy[8192], oz[8192];

// --- the basis -----------------------------------------------------------

static void test_basis(void)
{
    float t;
    float worst_sum = 0.0f, most_negative = 0.0f;

    for (t = 0.0f; t <= 1.0f; t += 0.0001f) {
        float b0, b1, b2, b3, sum;
        ws_basis(t, &b0, &b1, &b2, &b3);
        sum = b0 + b1 + b2 + b3;
        if (fabsf(sum - 1.0f) > worst_sum) worst_sum = fabsf(sum - 1.0f);
        if (b0 < most_negative) most_negative = b0;
        if (b1 < most_negative) most_negative = b1;
        if (b2 < most_negative) most_negative = b2;
        if (b3 < most_negative) most_negative = b3;
    }
    printf("  basis: worst |sum - 1| = %.2e, most negative lobe = %.2e\n",
           worst_sum, most_negative);

    CHECK(worst_sum < 1.0e-6f, "basis is not a partition of unity (%.3e)", worst_sum);
    CHECK(most_negative >= 0.0f, "basis has a negative lobe (%.3e) -- "
          "the convex-hull bound no longer holds", most_negative);

    // The two endpoint identities the clamping relies on.
    {
        float b0, b1, b2, b3;
        ws_basis(0.0f, &b0, &b1, &b2, &b3);
        CHECK(fabsf(b0 - 1.0f/6.0f) < 1e-6f && fabsf(b1 - 4.0f/6.0f) < 1e-6f &&
              fabsf(b2 - 1.0f/6.0f) < 1e-6f && fabsf(b3) < 1e-6f,
              "basis at t=0 is not (1,4,1,0)/6");
        ws_basis(1.0f, &b0, &b1, &b2, &b3);
        CHECK(fabsf(b0) < 1e-6f && fabsf(b1 - 1.0f/6.0f) < 1e-6f &&
              fabsf(b2 - 4.0f/6.0f) < 1e-6f && fabsf(b3 - 1.0f/6.0f) < 1e-6f,
              "basis at t=1 is not (0,1,4,1)/6");
    }
}

// --- contract ------------------------------------------------------------

static void test_degenerate_input(void)
{
    float y[4] = { 0.0f, 1.0f, 0.0f, -1.0f };
    float z[4] = { 0.0f, 0.0f, 0.0f,  0.0f };

    CHECK(ws_build(y, z, 1, -1.0f, 1.0f, ox, oy, oz, 64) == 0, "n = 1 should fail");
    CHECK(ws_build(y, z, 0, -1.0f, 1.0f, ox, oy, oz, 64) == 0, "n = 0 should fail");
    CHECK(ws_build(y, z, -3, -1.0f, 1.0f, ox, oy, oz, 64) == 0, "negative n should fail");
    CHECK(ws_build(y, z, 4, -1.0f, 1.0f, ox, oy, oz, 1) == 0, "m = 1 should fail");
    CHECK(ws_build(y, z, 4, -1.0f, 1.0f, ox, oy, oz, 0) == 0, "m = 0 should fail");
    CHECK(ws_build(NULL, z, 4, -1.0f, 1.0f, ox, oy, oz, 64) == 0, "null y should fail");
    CHECK(ws_build(y, NULL, 4, -1.0f, 1.0f, ox, oy, oz, 64) == 0, "null z should fail");
    CHECK(ws_build(y, z, 4, -1.0f, 1.0f, NULL, oy, oz, 64) == 0, "null ox should fail");
    CHECK(ws_build(y, z, 4, -1.0f, 1.0f, ox, NULL, oz, 64) == 0, "null oy should fail");
    CHECK(ws_build(y, z, 4, -1.0f, 1.0f, ox, oy, NULL, 64) == 0, "null oz should fail");

    // The minimum viable request: two nodes, two samples.
    CHECK(ws_build(y, z, 2, -1.0f, 1.0f, ox, oy, oz, 2) == 2, "n=2, m=2 should work");
    CHECK(ox[0] == -1.0f && ox[1] == 1.0f, "two-sample x should be the span ends");
}

static void test_endpoints_exact(void)
{
    // Deliberately asymmetric, so a bug that happens to be symmetric shows.
    float y[6] = { -0.7f, 0.2f, 0.9f, -0.4f, 0.1f, 0.55f };
    float z[6] = {  0.3f, -0.1f, 0.4f, 0.8f, -0.6f, -0.2f };
    int   m;

    m = ws_build(y, z, 6, -1.0f, 1.0f, ox, oy, oz, SAMPLES);
    CHECK(m == SAMPLES, "build returned %d, expected %d", m, SAMPLES);

    CHECK(oy[0] == y[0], "first sample y is %.9f, node 0 is %.9f", oy[0], y[0]);
    CHECK(oz[0] == z[0], "first sample z is %.9f, node 0 is %.9f", oz[0], z[0]);
    CHECK(oy[m-1] == y[5], "last sample y is %.9f, node 5 is %.9f", oy[m-1], y[5]);
    CHECK(oz[m-1] == z[5], "last sample z is %.9f, node 5 is %.9f", oz[m-1], z[5]);

    CHECK(ox[0] == -1.0f, "first sample x is %.9f, expected -1", ox[0]);
    CHECK(ox[m-1] == 1.0f, "last sample x is %.9f, expected 1", ox[m-1]);

    // And it holds for the smallest and a large sample count too.
    {
        static const int counts[] = { 2, 3, 7, 64, 4096 };
        size_t k;
        for (k = 0; k < sizeof counts / sizeof counts[0]; k++) {
            int mm = ws_build(y, z, 6, -1.0f, 1.0f, ox, oy, oz, counts[k]);
            CHECK(mm == counts[k], "build(%d) returned %d", counts[k], mm);
            CHECK(oy[0] == y[0] && oy[mm-1] == y[5],
                  "endpoints not exact at m = %d", counts[k]);
        }
    }
}

// The convex-hull property, which is the whole reason for choosing this basis.
static void test_no_overshoot(void)
{
    float y[NODES], z[NODES];
    int   i, m;
    float lo = 1e30f, hi = -1e30f;
    float over = 0.0f;

    // A deliberately nasty control polygon: alternating extremes, which is the
    // worst case for any interpolating scheme (Catmull-Rom would ring well
    // past the input range here).
    for (i = 0; i < NODES; i++) {
        y[i] = (i & 1) ? 0.9f : -0.9f;
        z[i] = (i % 3 == 0) ? 0.8f : -0.5f;
        if (y[i] < lo) lo = y[i];
        if (y[i] > hi) hi = y[i];
    }

    m = ws_build(y, z, NODES, -1.0f, 1.0f, ox, oy, oz, 4096);
    CHECK(m == 4096, "build failed");
    for (i = 0; i < m; i++) {
        if (oy[i] > hi && oy[i] - hi > over) over = oy[i] - hi;
        if (oy[i] < lo && lo - oy[i] > over) over = lo - oy[i];
    }
    printf("  overshoot: worst excursion past the control range = %.2e\n", over);
    CHECK(over <= 1.0e-6f, "curve overshot the control range by %.3e", over);
}

static void test_flat_and_repeated(void)
{
    float y[NODES], z[NODES];
    int   i, m;

    // A completely flat wave: every sample must be exactly the constant.  A
    // convex combination of identical values is that value, so this is exact,
    // not approximate.
    for (i = 0; i < NODES; i++) { y[i] = 0.42f; z[i] = -0.17f; }
    m = ws_build(y, z, NODES, -1.0f, 1.0f, ox, oy, oz, 1024);
    CHECK(m == 1024, "flat build failed");
    for (i = 0; i < m; i++) {
        CHECK(oy[i] == 0.42f, "flat curve drifted at %d: %.9f", i, oy[i]);
        CHECK(oz[i] == -0.17f, "flat z drifted at %d: %.9f", i, oz[i]);
        if (failures) return;
    }

    // All zeros -- the chain's rest state.
    for (i = 0; i < NODES; i++) { y[i] = 0.0f; z[i] = 0.0f; }
    m = ws_build(y, z, NODES, -1.0f, 1.0f, ox, oy, oz, 1024);
    for (i = 0; i < m; i++) {
        CHECK(oy[i] == 0.0f && oz[i] == 0.0f, "rest state is not flat at %d", i);
        if (failures) return;
    }

    // Repeated interior values must not produce a kink or a NaN.
    for (i = 0; i < NODES; i++) y[i] = (i < NODES / 2) ? 0.5f : 0.5f;
    y[NODES / 2] = -0.5f;
    m = ws_build(y, z, NODES, -1.0f, 1.0f, ox, oy, oz, 1024);
    for (i = 0; i < m; i++) {
        CHECK(isfinite(oy[i]), "repeated-value input produced a non-finite sample");
        if (failures) return;
    }
}

static void test_x_is_uniform_and_monotone(void)
{
    float y[NODES], z[NODES];
    int   i, m;
    float step, worst = 0.0f;

    for (i = 0; i < NODES; i++) { y[i] = 0.3f * wk_sinf((float)i * 0.2f); z[i] = 0.0f; }
    m = ws_build(y, z, NODES, -1.0f, 1.0f, ox, oy, oz, SAMPLES);
    CHECK(m == SAMPLES, "build failed");

    step = ox[1] - ox[0];
    for (i = 1; i < m; i++) {
        float d = ox[i] - ox[i-1];
        CHECK(d > 0.0f, "x is not monotone at %d", i);
        if (failures) return;
        if (fabsf(d - step) > worst) worst = fabsf(d - step);
    }
    printf("  x spacing: nominal %.6f, worst deviation %.2e\n", step, worst);
    CHECK(worst < 1.0e-6f, "x spacing is not uniform (worst deviation %.3e)", worst);
}

// Smoothness.  The second difference of the sampled curve measures how sharply
// it turns; a polyline through the raw nodes would show a step at every node,
// while the spline should not.  The comparison is the point -- an absolute
// threshold would just be a magic number.
static void test_smoothness(void)
{
    float y[NODES], z[NODES];
    int   i, m;
    float spline_max = 0.0f, linear_max = 0.0f;
    static float ly[8192];

    // Square-ish input: the hardest case for a smoother.
    for (i = 0; i < NODES; i++) { y[i] = ((i / 8) & 1) ? 0.6f : -0.6f; z[i] = 0.0f; }

    m = ws_build(y, z, NODES, -1.0f, 1.0f, ox, oy, oz, 2048);
    CHECK(m == 2048, "build failed");

    // The same node sequence sampled by straight linear interpolation, at the
    // same density, as the thing to beat.
    for (i = 0; i < m; i++) {
        float u  = (float)i * (float)(NODES - 1) / (float)(m - 1);
        int   j  = (int)u;
        float ft = u - (float)j;
        if (j >= NODES - 1) { j = NODES - 2; ft = 1.0f; }
        ly[i] = y[j] + (y[j+1] - y[j]) * ft;
    }

    for (i = 1; i < m - 1; i++) {
        float s = fabsf(oy[i+1] - 2.0f * oy[i] + oy[i-1]);
        float l = fabsf(ly[i+1] - 2.0f * ly[i] + ly[i-1]);
        if (s > spline_max) spline_max = s;
        if (l > linear_max) linear_max = l;
        CHECK(isfinite(oy[i]), "non-finite sample at %d", i);
        if (failures) return;
    }
    printf("  smoothness: worst 2nd difference, spline %.3e vs linear %.3e\n",
           spline_max, linear_max);
    CHECK(spline_max < linear_max,
          "the spline is no smoother than linear interpolation (%.3e vs %.3e)",
          spline_max, linear_max);
}

// The real input: whatever stage 1 produces has to go through stage 2 cleanly.
static void test_against_live_chain(void)
{
    static float y[NODES], z[NODES], vy[NODES], vz[NODES];
    wk_chain c;
    int i, m;
    float lo = 1e30f, hi = -1e30f;

    wk_init(&c, y, z, vy, vz, NODES, 20250920u);
    for (i = 0; i < 900; i++) wk_step(&c, 2.0f, 0.1f, 1.0f);

    for (i = 0; i < NODES; i++) {
        if (y[i] < lo) lo = y[i];
        if (y[i] > hi) hi = y[i];
    }

    m = ws_build(y, z, NODES, -1.0f, 1.0f, ox, oy, oz, SAMPLES);
    CHECK(m == SAMPLES, "build on a live chain failed");
    for (i = 0; i < m; i++) {
        CHECK(isfinite(oy[i]) && isfinite(oz[i]) && isfinite(ox[i]),
              "non-finite sample at %d", i);
        if (failures) return;
        CHECK(oy[i] >= lo - 1e-6f && oy[i] <= hi + 1e-6f,
              "sample %d (%.6f) left the chain's range [%.6f, %.6f]", i, oy[i], lo, hi);
        if (failures) return;
    }
    // The chain pins its ends at zero, so the curve must start and end there.
    CHECK(oy[0] == 0.0f && oy[m-1] == 0.0f,
          "the curve does not start and end on the chain's pinned nodes");
    printf("  live chain: %d samples over node range [%.4f, %.4f]\n", m, lo, hi);
}

static void test_determinism(void)
{
    float y[NODES], z[NODES];
    static float ax[8192], ay_[8192], az[8192];
    int i;

    for (i = 0; i < NODES; i++) {
        y[i] = 0.4f * wk_sinf((float)i * 0.37f);
        z[i] = 0.2f * wk_sinf((float)i * 0.11f);
    }
    ws_build(y, z, NODES, -1.0f, 1.0f, ax, ay_, az, SAMPLES);
    ws_build(y, z, NODES, -1.0f, 1.0f, ox, oy, oz, SAMPLES);

    CHECK(memcmp(ax, ox, SAMPLES * sizeof(float)) == 0, "x is not deterministic");
    CHECK(memcmp(ay_, oy, SAMPLES * sizeof(float)) == 0, "y is not deterministic");
    CHECK(memcmp(az, oz, SAMPLES * sizeof(float)) == 0, "z is not deterministic");
}

int main(void)
{
    printf("wave_spline\n");
    test_basis();
    test_degenerate_input();
    test_endpoints_exact();
    test_no_overshoot();
    test_flat_and_repeated();
    test_x_is_uniform_and_monotone();
    test_smoothness();
    test_against_live_chain();
    test_determinism();

    if (failures) {
        printf("wave_spline: %d FAILURE(S)\n", failures);
        return 1;
    }
    printf("wave_spline: all checks passed\n");
    return 0;
}
