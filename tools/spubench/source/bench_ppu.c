// PPU-side implementations of the same animation workload.
//
// Three of them, because "the PPU is slow" is a claim that has to survive its
// own best effort:
//   1. scalar, straight from the shared kernel;
//   2. scalar over SoA arrays, so the layout change is separated from the
//      processor change;
//   3. AltiVec, four objects at a time -- the PPU has a 128-bit SIMD unit and
//      comparing a hand-vectorised SPU against a scalar PPU would flatter the
//      SPU by roughly the vector width and prove nothing.

#include <altivec.h>
#include <string.h>

#include "spu_job.h"
#include "anim_kernel.h"
#include "bench_ppu.h"

// --- 1. scalar over AoS --------------------------------------------------

void ppu_step_aos(float *p, u32 n, float dt, float phase)
{
    for (u32 i = 0; i < n; i++) {
        float *o = p + (size_t)i * 8;
        ak_step_scalar(&o[0], &o[1], &o[2], &o[3], &o[4], &o[5], &o[6], &o[7],
                       dt, phase);
    }
}

// --- 2. scalar over SoA --------------------------------------------------

void ppu_step_soa_scalar(float *const f[SPUB_NFIELD], u32 first, u32 n,
                         float dt, float phase)
{
    for (u32 i = first; i < first + n; i++)
        ak_step_scalar(&f[F_X][i], &f[F_Y][i], &f[F_VX][i], &f[F_VY][i],
                       &f[F_ROT][i], &f[F_SCALE][i], &f[F_ALPHA][i],
                       &f[F_LIFE][i], dt, phase);
}

// --- 3. AltiVec over SoA -------------------------------------------------

static inline vector float vsin_ppu(vector float x)
{
    const vector float inv2pi = vec_splats(AK_INV_TWO_PI);
    const vector float two_pi = vec_splats(AK_TWO_PI);
    const vector float half   = vec_splats(0.5f);
    const vector float mhalf  = vec_splats(-0.5f);
    const vector float zero   = vec_splats(0.0f);

    vector float k = vec_madd(x, inv2pi, zero);
    vector bool int neg = vec_cmpgt(zero, k);
    vector float bias = vec_sel(half, mhalf, neg);
    vector float r = vec_ctf(vec_cts(vec_add(k, bias), 0), 0);
    x = vec_sub(x, vec_madd(r, two_pi, zero));

    vector float x2 = vec_madd(x, x, zero);
    vector float p = vec_madd(x2, vec_splats(-0.00019841f), vec_splats(0.00833333f));
    p = vec_madd(x2, p, vec_splats(-0.16666667f));
    p = vec_madd(x2, p, vec_splats(1.0f));
    return vec_madd(x, p, zero);
}

// n must be a multiple of 4 and every array 16-byte aligned.
void ppu_step_soa_vmx(float *const f[SPUB_NFIELD], u32 first, u32 n,
                      float dt_s, float phase_s)
{
    vector float *X  = (vector float *)(f[F_X]     + first);
    vector float *Y  = (vector float *)(f[F_Y]     + first);
    vector float *VX = (vector float *)(f[F_VX]    + first);
    vector float *VY = (vector float *)(f[F_VY]    + first);
    vector float *RO = (vector float *)(f[F_ROT]   + first);
    vector float *SC = (vector float *)(f[F_SCALE] + first);
    vector float *AL = (vector float *)(f[F_ALPHA] + first);
    vector float *LI = (vector float *)(f[F_LIFE]  + first);

    const vector float dt      = vec_splats(dt_s);
    const vector float phase   = vec_splats(phase_s);
    const vector float cx      = vec_splats(AK_CX);
    const vector float cy      = vec_splats(AK_CY);
    const vector float attract = vec_splats(AK_ATTRACT);
    const vector float damp    = vec_splats(AK_DAMP);
    const vector float tamp    = vec_splats(AK_TURB_AMP);
    const vector float tfx     = vec_splats(AK_TURB_FX);
    const vector float tfy     = vec_splats(AK_TURB_FY);
    const vector float rotk    = vec_splats(AK_ROT_K);
    const vector float life0   = vec_splats(AK_LIFE0);
    const vector float invlife = vec_splats(1.0f / AK_LIFE0);
    const vector float one     = vec_splats(1.0f);
    const vector float zero    = vec_splats(0.0f);
    const vector float three   = vec_splats(3.0f);
    const vector float p2      = vec_splats(0.2f);
    const vector float eps     = vec_splats(0.0000001f);

    u32 nv = n >> 2;
    for (u32 i = 0; i < nv; i++) {
        vector float x = X[i], y = Y[i], vx = VX[i], vy = VY[i];
        vector float life = LI[i];

        vector float ax = vec_madd(vec_sub(cx, x), attract, zero);
        vector float ay = vec_madd(vec_sub(cy, y), attract, zero);

        vector float tx = vec_madd(tamp, vsin_ppu(vec_madd(y, tfy, phase)), zero);
        vector float ty = vec_madd(tamp, vsin_ppu(vec_sub(vec_madd(x, tfx, zero), phase)), zero);

        vx = vec_madd(vx, damp, vec_madd(vec_add(ax, tx), dt, zero));
        vy = vec_madd(vy, damp, vec_madd(vec_add(ay, ty), dt, zero));

        x = vec_madd(vx, dt, x);
        y = vec_madd(vy, dt, y);

        vector float rot = vec_madd(vec_madd(vec_add(vx, vy), rotk, zero), dt, RO[i]);

        life = vec_sub(life, dt);
        vector bool int dmask = vec_cmpgt(eps, life);
        vector float dead = vec_sel(zero, one, dmask);
        life = vec_madd(dead, life0, life);
        vector float keep = vec_sub(one, dead);
        x  = vec_madd(keep, x, vec_madd(dead, cx, zero));
        y  = vec_madd(keep, y, vec_madd(dead, cy, zero));
        vx = vec_madd(keep, vx, zero);
        vy = vec_madd(keep, vy, zero);

        vector float a = vec_madd(life, invlife, zero);
        a = vec_sel(a, one,  vec_cmpgt(a, one));
        a = vec_sel(a, zero, vec_cmpgt(zero, a));

        vector float sc = vec_madd(p2, vsin_ppu(vec_madd(life, three, phase)), one);

        X[i] = x; Y[i] = y; VX[i] = vx; VY[i] = vy;
        RO[i] = rot; SC[i] = sc; AL[i] = a; LI[i] = life;
    }
}

float ppu_checksum(float *const f[SPUB_NFIELD], u32 n)
{
    float s = 0.0f;
    for (u32 i = 0; i < n; i++) s += f[F_X][i] + f[F_Y][i];
    return s;
}
