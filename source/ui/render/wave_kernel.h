// Stage 1 of the wave geometry pipeline: a damped spring chain.
//
//   wave_kernel.h  -> node positions      (this file)
//   wave_spline.h  -> dense smooth curve through them
//   wave_ribbon.h  -> triangle-strip vertices for the renderer
//
// Written to the model in docs/wave-spec.md section 1: DAMPING / LENGTH /
// TENSION / TIMESTEP / PERTURBATION describing a damped spring chain
// integrated at a fixed timestep with a small perturbation injected so it
// never settles, plus section 3b's two travelling waves at different speeds
// and amplitudes.  The constants below are ours; only the *shape* of the
// model comes from the spec, and no geometry, shader or captured data is
// reproduced (.clinerules rule 10).
//
// House rules (.clinerules rule 7): header-only, pure C, no libm, no PS3
// headers, struct-of-arrays, deterministic.  The sine is source/spu/
// jf_anim_kernel.h's ak_sinf with a tighter range reduction and one more
// term -- see the comment on wk_sinf for why it diverges there rather than
// copying it outright.

#ifndef WAVE_KERNEL_H
#define WAVE_KERNEL_H

#include <stdint.h>
#include <string.h>     /* memcpy, for the square-root seed; not libm */

#define WK_TWO_PI       6.28318530718f
#define WK_PI           3.14159265359f
#define WK_HALF_PI      1.57079632679f
#define WK_INV_TWO_PI   0.15915494309f

// --- physics -------------------------------------------------------------
// TENSION is the one parameter the spec measures as structurally constant
// across all 54 reference blocks, so it is a constant here too.  DAMPING sits
// in the spec's observed 1e-4 .. 3e-4 band: light enough that motion persists
// and drifts rather than decaying, which is what stops it reading as a loop.
#define WK_TENSION      0.25f
#define WK_DAMPING      2.0e-4f    // per unit TIME, not per substep -- see wk_step
#define WK_DRIVE_K      0.02f      // restoring stiffness toward the target field

// PERTURBATION normalisation.  The spec's knob runs 0 .. 0.1, but it is a
// tuning value in the reference's units, not an acceleration in ours, so it
// needs a scale.  The size of that scale is set by how lightly damped this
// system is: at WK_DAMPING 2e-4 the decay time is 5000 time units, about 24
// seconds of frames, so broadband forcing accumulates for that long before
// damping balances it.  Feeding the raw knob in as an acceleration put the
// chain permanently against the soft clip within two frames.
//
// WK_NOISE_SCALE is calibrated, not derived: tests/test_wave_kernel.c
// measures RMS displacement against perturbation and pins the result, because
// the analytic estimate depends on which spatial modes the noise lands in and
// the lowest ones dominate by four orders of magnitude.  At the spec's
// maximum PERTURBATION of 0.1 this gives an RMS displacement of a few
// hundredths -- a liveliness term under the drive, which is what it is for.
#define WK_NOISE_SCALE  4.0e-3f

// --- travelling-wave drive ----------------------------------------------
// Two waves at different wavenumbers and speeds (spec section 3b).  They are
// a moving TARGET DISPLACEMENT, not a raw acceleration: the chain is pulled
// toward the field with stiffness WK_DRIVE_K, so the response amplitude is
// bounded by the field's own amplitude instead of by the resonance of the
// lowest chain mode.  Driving a 96-node chain with a raw acceleration of 0.35
// would settle at roughly A / omega_0^2 = 1300 and live permanently in the
// clip.
//
// Speeds are radians per unit of simulation time.  At the spec's TIMESTEP of
// 2 and 60 frames a second that is 120 time units a second, so W1 gives a
// period near 8 s and W2 near 12.6 s.  Their ratio is deliberately not a
// simple fraction, so the pair does not repeat on any short cycle.
#define WK_A1           0.35f
#define WK_K1           2.0f       // cycles across the span
#define WK_W1           0.0065f    // rad per unit time
#define WK_A2           0.18f
#define WK_K2           5.0f
#define WK_W2           0.0041f
#define WK_Z_MIX        0.4f       // z follows y at reduced amplitude

// Drive envelope: the outer WK_EDGE of the span at each end tapers the drive
// to zero with a smoothstep, so the pinned boundary is approached smoothly
// instead of fighting a forced node next to a clamped one.
#define WK_EDGE         0.12f

// --- integration ---------------------------------------------------------
// Semi-implicit (symplectic) Euler.  Velocity is updated first from the
// CURRENT positions, then position from the NEW velocity.  The two passes are
// separate loops on purpose: the velocity pass reads positions and writes
// only velocities, so every node sees the same old position array and the
// sweep has no direction bias.  A single fused loop would read y[i-1] already
// updated -- a Gauss-Seidel sweep that quietly biases the wave one way.
//
// Stability: the three-point stencil has a maximum eigenvalue of 4*TENSION,
// and the drive spring adds DRIVE_K on top, so the fastest mode is
//   omega_max = sqrt(4*TENSION + DRIVE_K) = sqrt(1.02) = 1.00995
// Symplectic Euler on a harmonic oscillator is stable while h*omega < 2, so
//   h_max = WK_CFL * 2 / omega_max
// The spec's TIMESTEP runs 2 .. 6.72, above that, hence the substepping.
//
// WK_CFL IS 0.5, NOT THE 0.9 THAT BARE STABILITY WOULD ALLOW, and the
// difference is visible rather than theoretical.  Symplectic Euler does not
// merely stay bounded below h*omega = 2; it traces an ellipse in phase space
// that is elongated by
//   1 / sqrt(1 - (h*omega/2)^2)
// so a mode's apparent amplitude is inflated by that factor.  At WK_CFL 0.9
// (h*omega = 1.8) that is 2.3x.  PERTURBATION injects spatially white noise,
// which excites every mode including the Nyquist one -- exactly the mode
// closest to the limit -- so the highest spatial frequency got amplified 2.3x
// and slammed into the soft clip at dt 3.5 and above.  At WK_CFL 0.5
// (h*omega = 1.0) the factor is 1.15 and the noise stays where it was put.
// The cost is a handful more substeps on a 96-node chain.
#define WK_CFL          0.5f
#define WK_H_MAX        0.99015f
// dt is clamped to WK_DT_MAX first, so the most substeps ever needed is
// WK_DT_MAX / WK_H_MAX + 1 = 65.6.  The cap sits above that so it is a
// runaway guard and never silently hands back a substep longer than CFL
// allows for a dt the API accepts.
#define WK_MAX_SUBSTEPS 80
#define WK_DT_MAX       64.0f      // defensive clamp; spec TIMESTEP tops out at 6.72

// --- output bounds -------------------------------------------------------
// The soft clip is a safety net, not part of the physics.  Below the knee it
// is the EXACT identity, so normal motion is untouched; a classic x - x^3/3
// clip would shave 0.3% off a 0.1 displacement every single frame and act as
// a second, undeclared damping term.
#define WK_KNEE         0.80f
#define WK_LIMIT        1.00f

// Beyond this the float->int in the range reduction stops being meaningful
// (and would be undefined past INT_MAX).  The chain wraps its own phases into
// [0, 2pi) so it never comes near it; the guard is for callers.
#define WK_SIN_MAX      1.0e6f

typedef struct {
    float   *y, *z;         // node displacement, caller-owned, n each
    float   *vy, *vz;       // node velocity,     caller-owned, n each
    int      n;             // node count, >= 2; 0 marks an inert chain
    uint32_t rng;           // xorshift32 state, never 0
    float    ph1, ph2;      // travelling-wave phases, each wrapped to [0, 2pi)
} wk_chain;

// sin(x) for any x.  Returns 0 for NaN and for magnitudes past WK_SIN_MAX
// rather than invoking an undefined float->int conversion.
//
// This is jf_anim_kernel.h's ak_sinf with two changes, both measured rather
// than assumed (tests/test_wave_kernel.c prints the worst error against
// libm):
//
//   The reduction folds into [-pi/2, pi/2] rather than stopping at [-pi, pi].
//   An odd Taylor polynomial is at its worst exactly at the end of its range,
//   and at pi the degree-7 form is off by 7.5e-2 -- ak_sinf's "under 1e-4
//   across the reduced range" holds near zero and nowhere near pi.  sin is
//   symmetric about +/-pi/2, so the fold is two compares and a subtract.
//
//   One more term (x^9/9!).  After the fold the worst case is at pi/2, where
//   degree 7 leaves 1.6e-4 and degree 9 leaves about 4e-6.
//
// ak_sinf keeps its coefficients because the PPU-vs-SPU benchmark it serves
// has to run identical arithmetic on both sides to be a fair race.  Nothing
// races the wave kernel, so accuracy wins instead.
// Test-only call counter, for measuring the JellyWave perf refactor's actual
// effect on trig call count.  Only exists under JW_PROFILE (a test build
// define -- see tests/test_wave_gel.c); the counter's STORAGE is owned by
// whichever test defines JW_PROFILE, not by this header, so the shipped
// (non-JW_PROFILE) build stays global-free per house rule 7.
#ifdef JW_PROFILE
extern unsigned long g_wk_sinf_calls;
#endif

static inline float wk_sinf(float x)
{
    float k, r, x2;
#ifdef JW_PROFILE
    g_wk_sinf_calls++;
#endif
    if (!(x > -WK_SIN_MAX && x < WK_SIN_MAX)) return 0.0f;   // also catches NaN
    k = x * WK_INV_TWO_PI;
    r = (k >= 0.0f) ? (float)(int)(k + 0.5f) : (float)(int)(k - 0.5f);
    x -= r * WK_TWO_PI;                                      // now in [-pi, pi]
    if (x >  WK_HALF_PI) x =  WK_PI - x;                     // fold to [-pi/2, pi/2]
    if (x < -WK_HALF_PI) x = -WK_PI - x;
    x2 = x * x;
    return x * (1.0f + x2 * (-0.16666667f + x2 * (0.00833333f +
                x2 * (-0.000198413f + x2 * 2.7557319e-6f))));
}

// Wrap into [0, 2pi).  Used on the drive phases so a session of any length
// keeps full float precision in the sine argument instead of losing a bit of
// mantissa every time the accumulated time doubles.
static inline float wk_wrap2pi(float a)
{
    float q;
    if (!(a > -WK_SIN_MAX && a < WK_SIN_MAX)) return 0.0f;
    q = (float)(int)(a * WK_INV_TWO_PI);
    a -= q * WK_TWO_PI;
    if (a < 0.0f) a += WK_TWO_PI;
    if (a >= WK_TWO_PI) a -= WK_TWO_PI;     // guards the rounding edge
    return a;
}

// Monotone soft clip, exact identity for |x| <= WK_KNEE, asymptotic to
// WK_LIMIT above it, C1 continuous at the knee (both one-sided derivatives
// are 1).
//
// The bound is |out| <= WK_LIMIT, inclusive: the asymptote is approached but
// never exceeded in exact arithmetic, and in float the ratio t/(t+span)
// rounds to 1 once t is large enough, so a sufficiently extreme input lands
// exactly on WK_LIMIT.  That is still inside the renderer's [-1, 1], which is
// what the bound exists to protect.
static inline float wk_softclip(float x)
{
    const float span = WK_LIMIT - WK_KNEE;
    float a, t;
    if (x != x) return 0.0f;                       // NaN in, defined out
    a = (x < 0.0f) ? -x : x;
    if (a <= WK_KNEE) return x;
    t = a - WK_KNEE;
    a = WK_KNEE + span * (t / (t + span));
    return (x < 0.0f) ? -a : a;
}

// xorshift32.  Zero is the one state it cannot leave, so wk_init substitutes
// a fixed nonzero seed for it.
static inline uint32_t wk_rand(uint32_t *s)
{
    uint32_t x = *s;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    *s = x;
    return x;
}

// Uniform in [-1, 1).
static inline float wk_rand_bi(uint32_t *s)
{
    return (float)(int32_t)wk_rand(s) * (1.0f / 2147483648.0f);
}

// sqrt(x) without libm, for the one place this kernel needs it: scaling the
// perturbation by sqrt(h).  Evaluated once per wk_step, not per node.  The
// exponent-halving seed plus three Newton steps on the reciprocal square root
// reaches float precision; memcpy keeps the type pun well defined in both C
// and C++, and copying the object representation preserves the IEEE754
// pattern on either endianness, so host and PPU agree.
static inline float wk_sqrtf(float x)
{
    uint32_t i;
    float    h, y;
    if (!(x > 0.0f)) return 0.0f;
    memcpy(&i, &x, sizeof i);
    i = 0x5f3759dfu - (i >> 1);
    memcpy(&y, &i, sizeof y);
    h = 0.5f * x;
    y = y * (1.5f - h * y * y);
    y = y * (1.5f - h * y * y);
    y = y * (1.5f - h * y * y);
    return x * y;
}

// Drive envelope at normalised position u in [0, 1]: 0 at both ends, 1 across
// the middle, smoothstep over the outer WK_EDGE at each end.
static inline float wk_envelope(float u)
{
    float d = (u < 1.0f - u) ? u : 1.0f - u;
    float s = d * (1.0f / WK_EDGE);
    if (s <= 0.0f) return 0.0f;
    if (s >= 1.0f) return 1.0f;
    return s * s * (3.0f - 2.0f * s);
}

// Bind caller-owned arrays and reset to rest.  Returns 1 on success, 0 if the
// chain is degenerate (fewer than 2 nodes, or any null array), in which case
// the chain is marked inert and wk_step is a safe no-op.
static inline int wk_init(wk_chain *c, float *y, float *z, float *vy, float *vz,
                          int n, uint32_t seed)
{
    int i;
    if (!c) return 0;
    c->y = y; c->z = z; c->vy = vy; c->vz = vz;
    c->n = 0;
    c->rng = seed ? seed : 0x9E3779B9u;
    c->ph1 = 0.0f;
    c->ph2 = 0.0f;
    if (!y || !z || !vy || !vz || n < 2) return 0;
    c->n = n;
    for (i = 0; i < n; i++) {
        y[i] = 0.0f; z[i] = 0.0f; vy[i] = 0.0f; vz[i] = 0.0f;
    }
    return 1;
}

// Advance the chain by dt (the spec's TIMESTEP), with perturbation amplitude
// perturb (the spec's PERTURBATION, 0 .. 0.1) and drive scaling the
// travelling-wave field.
//
// drive is a plain scale on the target field: 1 is the full look, 0 turns the
// travelling waves off and leaves tension, the drive spring's pull toward
// zero, and damping.  It exists so the damped behaviour can be exercised on
// its own without a second near-duplicate of this function, and it doubles as
// the liveliness dial the spec asks for alongside PERTURBATION.
//
// Boundary condition: node 0 and node n-1 are PINNED -- position and velocity
// both held at zero -- matching the spec's fixed endpoint.  The envelope takes
// the drive to zero there, so nothing is forcing against the clamp.
//
// Non-finite or non-positive dt leaves the chain untouched.
static inline void wk_step(wk_chain *c, float dt, float perturb, float drive)
{
    int nsub, step, i, last;
    float h, inv, damp, nscale;

    if (!c || c->n < 2) return;
    if (!(dt > 0.0f) || !(dt < 1.0e30f)) return;     // rejects 0, negative, NaN, inf
    if (dt > WK_DT_MAX) dt = WK_DT_MAX;
    if (!(perturb >= 0.0f)) perturb = 0.0f;          // rejects negative and NaN
    if (!(drive   >= 0.0f)) drive   = 0.0f;

    nsub = (int)(dt * (1.0f / WK_H_MAX)) + 1;
    if (nsub > WK_MAX_SUBSTEPS) nsub = WK_MAX_SUBSTEPS;
    h = dt / (float)nsub;

    last = c->n - 1;
    inv  = 1.0f / (float)last;                       // last >= 1, so this is finite

    // BOTH of these scale with h, and neither is optional, because nsub
    // changes with dt: at dt 2 it is 3 substeps and at dt 6.72 it is 7.  A
    // term that does not scale correctly would make the simulation depend on
    // how the frame's dt happened to be subdivided rather than on dt itself.
    //
    //   Damping is a rate per unit TIME, so the per-substep factor is
    //   1 - WK_DAMPING*h.  Applying a fixed 1 - WK_DAMPING per substep instead
    //   would make the decay rate proportional to the substep count.
    //
    //   The perturbation is white noise, so its impulse scales with sqrt(h),
    //   not h.  With h the injected variance comes to a^2*dt^2/nsub -- halve
    //   the substep and you halve the energy the noise puts in.  With sqrt(h)
    //   it is a^2*dt, which is what "this much liveliness per unit time"
    //   should mean.
    damp   = 1.0f - WK_DAMPING * h;
    if (damp < 0.0f) damp = 0.0f;                    // unreachable at WK_H_MAX; cheap
    if (damp > 1.0f) damp = 1.0f;
    nscale = perturb * WK_NOISE_SCALE * wk_sqrtf(h);

    // Pin the ends before anything reads them, so a caller that seeded the
    // arrays by hand cannot feed a moving endpoint into the stencil for a
    // step.  Nothing below writes index 0 or last, so once is enough.
    c->y[0]     = 0.0f; c->z[0]     = 0.0f;
    c->vy[0]    = 0.0f; c->vz[0]    = 0.0f;
    c->y[last]  = 0.0f; c->z[last]  = 0.0f;
    c->vy[last] = 0.0f; c->vz[last] = 0.0f;

    for (step = 0; step < nsub; step++) {
        // Phases advance per substep so the drive is continuous regardless of
        // how the frame's dt was split.
        c->ph1 = wk_wrap2pi(c->ph1 + WK_W1 * h);
        c->ph2 = wk_wrap2pi(c->ph2 + WK_W2 * h);

        // Pass 1 -- velocities, from the current positions only.
        for (i = 1; i < last; i++) {
            float u  = (float)i * inv;
            float ph = u * WK_TWO_PI;
            float env, ty, tz, ay, az;

            // Moving target field: two travelling waves, enveloped to zero at
            // the pinned ends.
            env = wk_envelope(u);
            ty  = (WK_A1 * wk_sinf(WK_K1 * ph - c->ph1) +
                   WK_A2 * wk_sinf(WK_K2 * ph + c->ph2)) * env * drive;
            tz  = ty * WK_Z_MIX;

            // Tension (discrete Laplacian) + pull toward the target field.
            ay = WK_TENSION * (c->y[i-1] - 2.0f * c->y[i] + c->y[i+1])
               + WK_DRIVE_K * (ty - c->y[i]);
            az = WK_TENSION * (c->z[i-1] - 2.0f * c->z[i] + c->z[i+1])
               + WK_DRIVE_K * (tz - c->z[i]);

            // Perturbation keeps it from settling (spec section 1), enveloped
            // so it too vanishes at the clamp.  nscale already carries the
            // sqrt(h) and the normalisation.
            if (nscale > 0.0f) {
                ay += wk_rand_bi(&c->rng) * nscale * env;
                az += wk_rand_bi(&c->rng) * nscale * env;
            }

            c->vy[i] = (c->vy[i] + ay * h) * damp;
            c->vz[i] = (c->vz[i] + az * h) * damp;
        }

        // Pass 2 -- positions, from the new velocities, then the clip.
        //
        // The clip runs EVERY SUBSTEP, and it can only do that because
        // wk_softclip is the exact identity below the knee.  A classic
        // x - x^3/3 clip contracts everywhere, so applying it repeatedly
        // would be an undeclared damping term whose strength depended on the
        // substep count; this one costs nothing at all until something is
        // actually running away.
        //
        // Doing it here rather than once at the end is what makes "finite
        // output" hold unconditionally.  Bounded positions bound the
        // three-point stencil, which bounds the acceleration at roughly
        // 4*TENSION + DRIVE_K, and a bounded acceleration against the damping
        // rate bounds the velocity at a/(WK_DAMPING) -- large under an absurd
        // perturbation, but finite.  Clipping only at the end of the step let
        // velocity accumulate across substeps with nothing bounding it, and a
        // hundred times the spec's PERTURBATION eventually overflowed to
        // infinity, which the clip then turned into a NaN.
        for (i = 1; i < last; i++) {
            c->y[i] = wk_softclip(c->y[i] + c->vy[i] * h);
            c->z[i] = wk_softclip(c->z[i] + c->vz[i] * h);
        }
    }
}

// Kinetic + tension potential + the drive spring's own potential.  A
// diagnostic for the host tests (.clinerules rule 9 asks for bounded-energy
// invariants, not golden values), not used by the render path.
//
// With drive = 0 the target field is zero everywhere, so this is the exact
// Lyapunov function of the damped system and must not grow.  With the drive
// on it is only an approximation -- it cannot see the moving target, which is
// pumping energy in on purpose -- so it is a boundedness check there, not a
// decay one.
static inline float wk_energy(const wk_chain *c)
{
    float e = 0.0f;
    int i;
    if (!c || c->n < 2) return 0.0f;
    for (i = 0; i < c->n; i++) {
        e += 0.5f * (c->vy[i] * c->vy[i] + c->vz[i] * c->vz[i]);
        e += 0.5f * WK_DRIVE_K * (c->y[i] * c->y[i] + c->z[i] * c->z[i]);
        if (i < c->n - 1) {
            float dy = c->y[i+1] - c->y[i];
            float dz = c->z[i+1] - c->z[i];
            e += 0.5f * WK_TENSION * (dy * dy + dz * dz);
        }
    }
    return e;
}

#endif // WAVE_KERNEL_H
