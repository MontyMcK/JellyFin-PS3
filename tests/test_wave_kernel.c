// Host test for stage 1 of the wave geometry pipeline,
// source/ui/render/wave_kernel.h.
//
// Invariants, not golden values (.clinerules rule 9): finite, bounded,
// deterministic, pinned at the ends, energy-decaying when the drive is off,
// and defensive about the inputs the API says it is defensive about.
//
// The one test here that is a regression guard rather than an invariant is
// interior_actually_moves().  The previous implementation of this kernel
// computed the spring acceleration for every interior node and then assigned
// none of it -- the chain never moved.  A stability sweep and an energy-decay
// check both PASSED against that, because a chain that does not move is
// trivially stable and trivially decays.  Any test file for this kernel needs
// at least one assertion that motion happened.
//
// libm is used freely HERE.  The kernel may not use it; the test checking the
// kernel may, and comparing the polynomial sine against sinf() is a far
// better check than a hand-written table of expected values.

#include <stdio.h>
#include <stdint.h>
#include <math.h>
#include <string.h>

#include "../source/ui/render/wave_kernel.h"

#define NODES 96

static int failures = 0;

#define CHECK(cond, ...) do {                                   \
    if (!(cond)) {                                              \
        printf("  FAIL %s:%d: ", __FILE__, __LINE__);           \
        printf(__VA_ARGS__);                                    \
        printf("\n");                                           \
        failures++;                                             \
    }                                                           \
} while (0)

// A chain plus its storage, so tests can declare one in a line.
typedef struct {
    wk_chain c;
    float    y[4096], z[4096], vy[4096], vz[4096];
} chain_box;

static int box_init(chain_box *b, int n, uint32_t seed)
{
    return wk_init(&b->c, b->y, b->z, b->vy, b->vz, n, seed);
}

// --- the maths helpers ---------------------------------------------------

static void test_sine(void)
{
    float x, worst = 0.0f;
    for (x = -100.0f; x <= 100.0f; x += 0.001f) {
        float got = wk_sinf(x);
        float ref = sinf(x);
        float err = fabsf(got - ref);
        if (err > worst) worst = err;
    }
    printf("  sine: worst error vs libm over [-100,100] = %.2e\n", worst);
    CHECK(worst < 1.0e-5f, "sine error %.3e exceeds 1e-5", worst);

    // Guards: the kernel wraps its own phase so it never reaches these, but
    // the API promises a defined answer rather than an undefined cast.
    CHECK(wk_sinf((float)NAN) == 0.0f, "sine of NaN should be 0");
    CHECK(wk_sinf(1.0e30f) == 0.0f, "sine past WK_SIN_MAX should be 0");
    CHECK(wk_sinf(-1.0e30f) == 0.0f, "sine past -WK_SIN_MAX should be 0");
}

static void test_wrap2pi(void)
{
    float a;
    for (a = -500.0f; a <= 500.0f; a += 0.01f) {
        float w = wk_wrap2pi(a);
        CHECK(w >= 0.0f && w < WK_TWO_PI, "wrap(%f) = %f out of [0,2pi)", a, w);
        if (failures) return;
        // Wrapping must not change the sine, which is the only thing the
        // phase is ever used for.
        CHECK(fabsf(sinf(w) - sinf(a)) < 1.0e-3f,
              "wrap(%f) changed the sine: %f vs %f", a, sinf(w), sinf(a));
        if (failures) return;
    }
}

static void test_softclip(void)
{
    float x, prev;

    // Exact identity below the knee -- this is the property a classic
    // x - x^3/3 clip does NOT have, and the reason it is not used here: it
    // would act as an undeclared second damping term on every normal-sized
    // displacement, every frame.
    for (x = -WK_KNEE; x <= WK_KNEE; x += 0.001f)
        CHECK(wk_softclip(x) == x, "softclip(%f) should be identity", x);

    // Bounded, strictly, for anything finite.
    for (x = -1000.0f; x <= 1000.0f; x += 0.01f) {
        float v = wk_softclip(x);
        CHECK(v >= -WK_LIMIT && v <= WK_LIMIT, "softclip(%f) = %f escaped", x, v);
        if (failures) return;
    }
    CHECK(wk_softclip(1.0e30f) <= WK_LIMIT, "softclip of a huge value escaped");
    CHECK(wk_softclip((float)NAN) == 0.0f, "softclip of NaN should be 0");

    // Monotone.
    prev = wk_softclip(-50.0f);
    for (x = -50.0f; x <= 50.0f; x += 0.001f) {
        float v = wk_softclip(x);
        CHECK(v >= prev, "softclip not monotone at %f", x);
        if (failures) return;
        prev = v;
    }

    // Continuous at the knee.
    CHECK(fabsf(wk_softclip(WK_KNEE + 1.0e-5f) - WK_KNEE) < 1.0e-4f,
          "softclip is discontinuous at the knee");
}

// --- API defensiveness ---------------------------------------------------

static void test_degenerate_init(void)
{
    chain_box b;
    float dummy[4];

    CHECK(box_init(&b, 0, 1) == 0, "n = 0 should fail");
    CHECK(b.c.n == 0, "a failed init should leave the chain inert");
    wk_step(&b.c, 2.0f, 0.1f, 1.0f);            // must not crash or touch memory

    CHECK(box_init(&b, 1, 1) == 0, "n = 1 should fail");
    wk_step(&b.c, 2.0f, 0.1f, 1.0f);

    CHECK(box_init(&b, -5, 1) == 0, "negative n should fail");
    wk_step(&b.c, 2.0f, 0.1f, 1.0f);

    CHECK(wk_init(&b.c, NULL, dummy, dummy, dummy, 8, 1) == 0, "null y should fail");
    CHECK(wk_init(&b.c, dummy, NULL, dummy, dummy, 8, 1) == 0, "null z should fail");
    CHECK(wk_init(&b.c, dummy, dummy, NULL, dummy, 8, 1) == 0, "null vy should fail");
    CHECK(wk_init(&b.c, dummy, dummy, dummy, NULL, 8, 1) == 0, "null vz should fail");
    CHECK(wk_init(NULL, dummy, dummy, dummy, dummy, 8, 1) == 0, "null chain should fail");

    CHECK(wk_energy(&b.c) == 0.0f, "energy of an inert chain should be 0");

    // Two nodes is the documented minimum.  Both are ends, so it is valid but
    // completely inert -- it must not read off the ends of the arrays.
    CHECK(box_init(&b, 2, 1) == 1, "n = 2 should succeed");
    {
        int i;
        for (i = 0; i < 2000; i++) wk_step(&b.c, 6.72f, 0.1f, 1.0f);
        CHECK(b.y[0] == 0.0f && b.y[1] == 0.0f, "a 2-node chain should not move");
    }

    // Three nodes: exactly one interior node, which must move.
    CHECK(box_init(&b, 3, 1) == 1, "n = 3 should succeed");
    {
        int i;
        for (i = 0; i < 2000; i++) wk_step(&b.c, 2.0f, 0.05f, 1.0f);
        CHECK(isfinite(b.y[1]), "the single interior node went non-finite");
    }
}

static void test_rejects_bad_dt(void)
{
    chain_box b;
    float snap_y[NODES], snap_v[NODES];
    int i;
    uint32_t rng_before;
    float ph_before;

    box_init(&b, NODES, 4242);
    for (i = 0; i < 200; i++) wk_step(&b.c, 2.0f, 0.05f, 1.0f);

    memcpy(snap_y, b.y, sizeof snap_y);
    memcpy(snap_v, b.vy, sizeof snap_v);
    rng_before = b.c.rng;
    ph_before  = b.c.ph1;

    wk_step(&b.c, 0.0f, 0.05f, 1.0f);
    wk_step(&b.c, -1.0f, 0.05f, 1.0f);
    wk_step(&b.c, (float)NAN, 0.05f, 1.0f);
    wk_step(&b.c, (float)INFINITY, 0.05f, 1.0f);

    CHECK(memcmp(snap_y, b.y, sizeof snap_y) == 0, "a rejected dt changed positions");
    CHECK(memcmp(snap_v, b.vy, sizeof snap_v) == 0, "a rejected dt changed velocities");
    CHECK(b.c.rng == rng_before, "a rejected dt consumed randomness");
    CHECK(b.c.ph1 == ph_before, "a rejected dt advanced the phase");

    // A NaN perturbation or drive is coerced, not propagated.
    wk_step(&b.c, 2.0f, (float)NAN, (float)NAN);
    for (i = 0; i < NODES; i++)
        CHECK(isfinite(b.y[i]), "NaN perturb/drive leaked into node %d", i);
}

// --- the physics ---------------------------------------------------------

// The regression guard.  See the file header.
static void test_interior_actually_moves(void)
{
    chain_box b;
    int i, j;
    float peak = 0.0f;

    box_init(&b, NODES, 1);
    for (i = 0; i < 600; i++) {
        wk_step(&b.c, 2.0f, 0.0f, 1.0f);        // drive only, no noise
        for (j = 1; j < NODES - 1; j++) {
            float a = fabsf(b.y[j]);
            if (a > peak) peak = a;
        }
    }
    printf("  interior: peak |y| after 600 driven steps = %.4f\n", peak);
    CHECK(peak > 0.05f,
          "the interior never moved (peak |y| = %.6f) -- the solver is a no-op",
          peak);

    // And z must move too, at the reduced amplitude the mix asks for.
    {
        float pz = 0.0f;
        for (j = 1; j < NODES - 1; j++) {
            float a = fabsf(b.z[j]);
            if (a > pz) pz = a;
        }
        CHECK(pz > 0.0f, "the z channel never moved");
    }
}

static void test_ends_pinned(void)
{
    chain_box b;
    int i;

    box_init(&b, NODES, 7);
    // Deliberately seed the endpoints with junk: the step must re-pin them.
    b.y[0] = 0.9f; b.vy[NODES - 1] = -3.0f;

    for (i = 0; i < 5000; i++) wk_step(&b.c, 4.0f, 0.1f, 1.0f);

    CHECK(b.y[0] == 0.0f && b.z[0] == 0.0f, "node 0 position not pinned");
    CHECK(b.vy[0] == 0.0f && b.vz[0] == 0.0f, "node 0 velocity not pinned");
    CHECK(b.y[NODES-1] == 0.0f && b.z[NODES-1] == 0.0f, "last node position not pinned");
    CHECK(b.vy[NODES-1] == 0.0f && b.vz[NODES-1] == 0.0f, "last node velocity not pinned");
}

static void test_no_spontaneous_motion(void)
{
    chain_box b;
    int i, j;

    box_init(&b, NODES, 99);
    for (i = 0; i < 1000; i++) wk_step(&b.c, 2.0f, 0.0f, 0.0f);
    for (j = 0; j < NODES; j++) {
        CHECK(b.y[j] == 0.0f && b.z[j] == 0.0f,
              "node %d moved with no drive and no perturbation", j);
        if (failures) return;
    }
}

// Sweep the spec's TIMESTEP range and well past it, including values that
// force substepping and values above WK_DT_MAX.
static void test_stability_sweep(void)
{
    static const float dts[] = {
        0.01f, 0.1f, 0.5f, 1.0f, 1.7823f,       // at and below one substep
        2.0f, 3.5f, 5.0f, 6.72f,                // the spec's TIMESTEP range
        10.0f, 32.0f, 64.0f,                    // past it
        1000.0f, 1.0e6f                         // past WK_DT_MAX, must clamp
    };
    size_t k;
    float worst = 0.0f;

    for (k = 0; k < sizeof dts / sizeof dts[0]; k++) {
        chain_box b;
        int i, j;
        box_init(&b, NODES, 20250920u);
        for (i = 0; i < 4000; i++) {
            wk_step(&b.c, dts[k], 0.1f, 1.0f);
            for (j = 0; j < NODES; j++) {
                if (!isfinite(b.y[j]) || !isfinite(b.z[j]) ||
                    !isfinite(b.vy[j]) || !isfinite(b.vz[j])) {
                    CHECK(0, "non-finite at dt = %g, step %d, node %d", dts[k], i, j);
                    return;
                }
                if (fabsf(b.y[j]) > WK_LIMIT || fabsf(b.z[j]) > WK_LIMIT) {
                    CHECK(0, "escaped the clip at dt = %g, step %d, node %d: %f",
                          dts[k], i, j, b.y[j]);
                    return;
                }
                if (fabsf(b.y[j]) > worst) worst = fabsf(b.y[j]);
            }
        }
    }
    printf("  stability: peak |y| across the whole dt sweep = %.4f "
           "(knee %.2f, limit %.2f)\n", worst, WK_KNEE, WK_LIMIT);
}

// An extreme but valid perturbation -- a hundred times the spec's 0.1 -- must
// still be CONTAINED rather than diverging.  Saturating against the clip is
// the correct outcome here; that is what the clip is for.  What must not
// happen is a non-finite value or one outside the renderer's [-1, 1].
static void test_extreme_perturbation(void)
{
    chain_box b;
    int i, j;
    box_init(&b, NODES, 5150);
    for (i = 0; i < 4000; i++) {
        wk_step(&b.c, 6.72f, 10.0f, 1.0f);
        for (j = 0; j < NODES; j++) {
            if (!isfinite(b.y[j]) || fabsf(b.y[j]) > WK_LIMIT) {
                CHECK(0, "extreme perturbation escaped at step %d node %d: %f",
                      i, j, b.y[j]);
                return;
            }
        }
    }
}

static void test_energy_decays_undriven(void)
{
    chain_box b;
    int i, j;
    float e0, peak, e_end;

    box_init(&b, NODES, 1234);
    // Seed a displacement and let tension plus damping take it away.
    for (j = 1; j < NODES - 1; j++)
        b.y[j] = 0.4f * wk_sinf((float)j * (WK_TWO_PI / (NODES - 1)));

    e0 = wk_energy(&b.c);
    peak = e0;
    for (i = 0; i < 40000; i++) {
        float e;
        wk_step(&b.c, 0.5f, 0.0f, 0.0f);        // no drive, no noise
        e = wk_energy(&b.c);
        if (e > peak) peak = e;
    }
    e_end = wk_energy(&b.c);

    printf("  energy: start %.6f  peak %.6f (%.2f%% over)  end %.6f (%.1f%% of start)\n",
           e0, peak, 100.0 * (peak / e0 - 1.0), e_end, 100.0 * e_end / e0);

    // Symplectic Euler conserves a shadow Hamiltonian that oscillates within
    // O(h*omega) of the true energy, so a strict "never rises" assertion is
    // wrong for the integrator, not for the physics.  What must hold is that
    // it never RUNS AWAY and that damping wins over the long run.
    CHECK(peak < e0 * 1.10f, "undriven energy rose %.1f%% -- the integrator is pumping",
          100.0 * (peak / e0 - 1.0));
    CHECK(e_end < e0 * 0.5f, "undriven energy did not decay (end %.1f%% of start)",
          100.0 * e_end / e0);
}

static void test_determinism(void)
{
    chain_box a, b;
    int i;

    box_init(&a, NODES, 0xC0FFEE);
    box_init(&b, NODES, 0xC0FFEE);
    for (i = 0; i < 3000; i++) {
        wk_step(&a.c, 3.0f, 0.1f, 1.0f);
        wk_step(&b.c, 3.0f, 0.1f, 1.0f);
    }
    CHECK(memcmp(a.y,  b.y,  NODES * sizeof(float)) == 0, "y diverged");
    CHECK(memcmp(a.z,  b.z,  NODES * sizeof(float)) == 0, "z diverged");
    CHECK(memcmp(a.vy, b.vy, NODES * sizeof(float)) == 0, "vy diverged");
    CHECK(memcmp(a.vz, b.vz, NODES * sizeof(float)) == 0, "vz diverged");
    CHECK(a.c.rng == b.c.rng, "rng diverged");
    CHECK(a.c.ph1 == b.c.ph1 && a.c.ph2 == b.c.ph2, "phase diverged");

    // A zero seed is the one xorshift state that cannot escape itself; init
    // must substitute for it rather than hand back a dead generator.
    {
        chain_box zc;
        int j;
        float peak = 0.0f;
        box_init(&zc, NODES, 0);
        CHECK(zc.c.rng != 0, "a zero seed left the generator dead");
        for (i = 0; i < 400; i++) wk_step(&zc.c, 2.0f, 0.1f, 0.0f);
        for (j = 1; j < NODES - 1; j++)
            if (fabsf(zc.y[j]) > peak) peak = fabsf(zc.y[j]);
        CHECK(peak > 0.0f, "a zero seed produced no perturbation at all");
    }
}

static void test_long_chain(void)
{
    static chain_box b;         // 4096 * 4 floats, too big for the stack
    int i, j;
    const int n = 4096;

    CHECK(box_init(&b, n, 31337) == 1, "long chain init failed");
    for (i = 0; i < 500; i++) wk_step(&b.c, 2.0f, 0.1f, 1.0f);
    for (j = 0; j < n; j++) {
        if (!isfinite(b.y[j]) || fabsf(b.y[j]) > WK_LIMIT) {
            CHECK(0, "long chain broke at node %d: %f", j, b.y[j]);
            return;
        }
    }
}

// Phases are wrapped rather than accumulated, so a session of any length
// keeps full mantissa in the sine argument.  Half an hour at 60 Hz.
static void test_long_run(void)
{
    chain_box b;
    int i, j;
    box_init(&b, NODES, 8080);
    for (i = 0; i < 108000; i++) {
        wk_step(&b.c, 2.0f, 0.05f, 1.0f);
        if ((i & 8191) == 0) {
            for (j = 0; j < NODES; j++) {
                if (!isfinite(b.y[j]) || fabsf(b.y[j]) > WK_LIMIT) {
                    CHECK(0, "long run broke at step %d node %d: %f", i, j, b.y[j]);
                    return;
                }
            }
        }
    }
    CHECK(b.c.ph1 >= 0.0f && b.c.ph1 < WK_TWO_PI, "phase 1 escaped its range");
    CHECK(b.c.ph2 >= 0.0f && b.c.ph2 < WK_TWO_PI, "phase 2 escaped its range");
}

// Root-mean-square displacement over the back half of a run, which is the
// statistic both calibration tests below are built on.
static float rms_after_settling(int nodes, float dt, float perturb, float drive,
                                int steps)
{
    static chain_box b;
    int i, j;
    double acc = 0.0;
    long   cnt = 0;

    box_init(&b, nodes, 20250920u);
    for (i = 0; i < steps; i++) {
        wk_step(&b.c, dt, perturb, drive);
        if (i > steps / 2)
            for (j = 0; j < nodes; j++) { acc += (double)b.y[j] * b.y[j]; cnt++; }
    }
    return cnt ? (float)sqrt(acc / (double)cnt) : 0.0f;
}

// WK_NOISE_SCALE is a calibration constant, so it gets a calibration test.
// The perturbation has to stay a liveliness term UNDER the drive rather than
// becoming the signal -- fed in unscaled it pinned the chain against the soft
// clip within two frames -- and it has to scale linearly with the knob.
static void test_perturbation_scale(void)
{
    float a = rms_after_settling(NODES, 2.0f, 0.025f, 0.0f, 6000);
    float b = rms_after_settling(NODES, 2.0f, 0.050f, 0.0f, 6000);
    float c = rms_after_settling(NODES, 2.0f, 0.100f, 0.0f, 6000);

    printf("  perturbation: rms |y| at 0.025 / 0.05 / 0.10 = %.5f / %.5f / %.5f\n",
           a, b, c);

    CHECK(c > 0.002f && c < 0.06f,
          "perturbation rms %.5f at the spec maximum is outside the calibrated band", c);
    CHECK(fabsf(c / b - 2.0f) < 0.15f, "not linear in the knob: 0.10/0.05 = %.3f", c / b);
    CHECK(fabsf(b / a - 2.0f) < 0.15f, "not linear in the knob: 0.05/0.025 = %.3f", b / a);
}

// The damping rate and the noise impulse both have to scale with the substep
// length, because the substep count changes with dt: 3 substeps at dt 2 and 7
// at dt 6.72.  Get either wrong and the simulation depends on how the frame's
// dt happened to be subdivided rather than on dt itself.  Before this was
// fixed the damping was a fixed factor per substep and the noise scaled with
// h instead of sqrt(h).
static void test_timestep_independence(void)
{
    float a = rms_after_settling(NODES, 2.00f, 0.0f, 1.0f, 6000);
    float b = rms_after_settling(NODES, 3.50f, 0.0f, 1.0f, 6000);
    float c = rms_after_settling(NODES, 6.72f, 0.0f, 1.0f, 6000);
    float lo = a, hi = a, spread;

    if (b < lo) lo = b;
    if (b > hi) hi = b;
    if (c < lo) lo = c;
    if (c > hi) hi = c;
    spread = (hi - lo) / (0.5f * (hi + lo));

    printf("  timestep: driven rms |y| at dt 2 / 3.5 / 6.72 = %.5f / %.5f / %.5f "
           "(spread %.1f%%)\n", a, b, c, 100.0 * spread);

    CHECK(spread < 0.10f,
          "driven amplitude moved %.1f%% with the substep count", 100.0 * spread);
}

int main(void)
{
    printf("wave_kernel\n");
    test_sine();
    test_wrap2pi();
    test_softclip();
    test_degenerate_init();
    test_rejects_bad_dt();
    test_interior_actually_moves();
    test_ends_pinned();
    test_no_spontaneous_motion();
    test_stability_sweep();
    test_perturbation_scale();
    test_timestep_independence();
    test_extreme_perturbation();
    test_energy_decays_undriven();
    test_determinism();
    test_long_chain();
    test_long_run();

    if (failures) {
        printf("wave_kernel: %d FAILURE(S)\n", failures);
        return 1;
    }
    printf("wave_kernel: all checks passed\n");
    return 0;
}
