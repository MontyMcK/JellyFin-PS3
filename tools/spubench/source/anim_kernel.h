// The animation workload, written once so every implementation does provably
// the same arithmetic.
//
// The point of the benchmark is PPU-vs-SPU, so the kernel must not quietly
// advantage either side.  libm's sinf() on the PPU against a polynomial on the
// SPU would be a rigged race; both therefore use the identical 7th-order odd
// polynomial below, and neither calls libm.  Results are compared with a
// relative tolerance rather than bit-equality because AltiVec's vmaddfp and
// the SPU's fma round differently from a scalar fmuls/fadds pair.
//
// Per object, per frame:
//   attraction toward a centre, 2-D turbulence, velocity damping,
//   Euler position integrate, rotation from velocity, life decay,
//   alpha from life, scale breathing, respawn when dead.
// That is 3 polynomial sines and roughly 40 multiply-adds -- deliberately
// representative of a particle/wave field rather than a trivially
// memory-bound copy, which would answer a question nobody asked.

#ifndef ANIM_KERNEL_H
#define ANIM_KERNEL_H

#define AK_TWO_PI      6.28318530718f
#define AK_INV_TWO_PI  0.15915494309f

// Tuning constants -- shared so every implementation is the same simulation.
#define AK_CX        640.0f     // attractor centre x
#define AK_CY        360.0f     // attractor centre y
#define AK_ATTRACT   0.35f
#define AK_DAMP      0.985f
#define AK_TURB_AMP  140.0f
#define AK_TURB_FX   0.011f
#define AK_TURB_FY   0.013f
#define AK_LIFE0     4.0f
#define AK_ROT_K     0.02f

// --- scalar reference ----------------------------------------------------

// sin(x) for any x, via range reduction to [-pi,pi] then an odd polynomial.
// The Taylor coefficients (1, -1/6, 1/120, -1/5040) hold under 1e-4 across the
// reduced range, which is far below anything a moving particle can show.
static inline float ak_sinf(float x)
{
    float k = x * AK_INV_TWO_PI;
    // round-to-nearest without libm: add/subtract the float "magic" constant
    float r = (k >= 0.0f) ? (float)(int)(k + 0.5f) : (float)(int)(k - 0.5f);
    x -= r * AK_TWO_PI;
    float x2 = x * x;
    return x * (1.0f + x2 * (-0.16666667f + x2 * (0.00833333f + x2 * (-0.00019841f))));
}

// One object, one frame.  Field pointers are passed individually so the same
// body serves both the SoA and the AoS layouts.
static inline void ak_step_scalar(float *px, float *py, float *pvx, float *pvy,
                                  float *prot, float *pscale, float *palpha,
                                  float *plife, float dt, float phase)
{
    float x = *px, y = *py, vx = *pvx, vy = *pvy;
    float life = *plife;

    float ax = (AK_CX - x) * AK_ATTRACT;
    float ay = (AK_CY - y) * AK_ATTRACT;

    float tx = AK_TURB_AMP * ak_sinf(y * AK_TURB_FY + phase);
    float ty = AK_TURB_AMP * ak_sinf(x * AK_TURB_FX - phase);

    vx = vx * AK_DAMP + (ax + tx) * dt;
    vy = vy * AK_DAMP + (ay + ty) * dt;

    x += vx * dt;
    y += vy * dt;

    float rot = *prot + (vx + vy) * AK_ROT_K * dt;

    life -= dt;
    // Respawn: life folds back to full and the object is thrown at the centre.
    // Branchless so the vector versions need no per-lane divergence.
    float dead = (life <= 0.0f) ? 1.0f : 0.0f;
    life = life + dead * AK_LIFE0;
    float keep = 1.0f - dead;
    x  = keep * x  + dead * AK_CX;
    y  = keep * y  + dead * AK_CY;
    vx = keep * vx;
    vy = keep * vy;

    float a = life * (1.0f / AK_LIFE0);
    if (a > 1.0f) a = 1.0f;
    if (a < 0.0f) a = 0.0f;

    float sc = 1.0f + 0.2f * ak_sinf(life * 3.0f + phase);

    *px = x; *py = y; *pvx = vx; *pvy = vy;
    *prot = rot; *pscale = sc; *palpha = a; *plife = life;
}

#endif
