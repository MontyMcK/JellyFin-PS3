/*
 * Host-side unit test for the liba52-plane → PS3-slot channel map
 * (source/audio/ac3_map.c).  No PS3 headers, no decoder — it feeds planar
 * blocks whose planes carry distinct constant values and asserts every PS3
 * slot receives the value (and scale) the derivation comment promises.
 *
 * Build & run:  make -f Makefile.host  (see that file; runs on any host cc)
 */
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <inttypes.h>   /* a52.h uses uint*_t but does not include this itself */

#include "ac3_map.h"
#include "a52/a52.h"

#define LEVEL_3DB 0.7071067811865476f

static int g_failures = 0;

/* planes: plane k filled with (k+1).0f so any misrouted plane is visible */
static void fill_planes(float *planes, int nplanes) {
    for (int k = 0; k < nplanes; k++)
        for (int i = 0; i < 256; i++)
            planes[k * 256 + i] = (float)(k + 1);
}

static void expect(const char *name, int slot, float got, float want) {
    if (fabsf(got - want) > 1e-5f) {
        printf("FAIL %s slot %d: got %f want %f\n", name, slot, got, want);
        g_failures++;
    }
}

/* Check one config: expected[] holds the wanted value for the 6 PS3 slots
 * (FL FR FC LFE SL SR) given fill_planes() input. */
static void check6(const char *name, int flags, int nplanes,
                   const float expected[6]) {
    float planes[6 * 256];
    float out[6 * 256];
    fill_planes(planes, nplanes);
    ac3_map_block(planes, flags, out, 6);
    /* spot-check first and last frame — the map is per-frame uniform */
    for (int i = 0; i < 256; i += 255)
        for (int s = 0; s < 6; s++)
            expect(name, s, out[i * 6 + s], expected[s]);
}

int main(void) {
    /* A52_3F2R|LFE: planes = LFE(1) L(2) C(3) R(4) SL(5) SR(6)
     * PS3 slots FL FR FC LFE SL SR = L R C LFE SL SR = 2 4 3 1 5 6 */
    { const float e[6] = {2, 4, 3, 1, 5, 6};
      check6("3F2R|LFE", A52_3F2R | A52_LFE, 6, e); }

    /* A52_3F2R (no LFE): planes = L(1) C(2) R(3) SL(4) SR(5) */
    { const float e[6] = {1, 3, 2, 0, 4, 5};
      check6("3F2R", A52_3F2R, 5, e); }

    /* A52_2F2R: planes = L(1) R(2) SL(3) SR(4) */
    { const float e[6] = {1, 2, 0, 0, 3, 4};
      check6("2F2R", A52_2F2R, 4, e); }

    /* A52_3F: planes = L(1) C(2) R(3) */
    { const float e[6] = {1, 3, 2, 0, 0, 0};
      check6("3F", A52_3F, 3, e); }

    /* A52_3F1R|LFE: planes = LFE(1) L(2) C(3) R(4) S(5); S → SL,SR at -3dB */
    { const float e[6] = {2, 4, 3, 1, 5 * LEVEL_3DB, 5 * LEVEL_3DB};
      check6("3F1R|LFE", A52_3F1R | A52_LFE, 5, e); }

    /* A52_2F1R: planes = L(1) R(2) S(3) */
    { const float e[6] = {1, 2, 0, 0, 3 * LEVEL_3DB, 3 * LEVEL_3DB};
      check6("2F1R", A52_2F1R, 3, e); }

    /* A52_STEREO: planes = L(1) R(2) */
    { const float e[6] = {1, 2, 0, 0, 0, 0};
      check6("STEREO", A52_STEREO, 2, e); }

    /* A52_MONO: plane = M(1) → FC on the 6ch output */
    { const float e[6] = {0, 0, 1, 0, 0, 0};
      check6("MONO", A52_MONO, 1, e); }

    /* Stereo (out_ch == 2) outputs */
    {
        float planes[6 * 256], out[2 * 256];
        fill_planes(planes, 2);
        ac3_map_block(planes, A52_STEREO, out, 2);
        expect("STEREO/2ch", 0, out[0], 1);
        expect("STEREO/2ch", 1, out[1], 2);
        fill_planes(planes, 1);
        ac3_map_block(planes, A52_MONO, out, 2);
        expect("MONO/2ch", 0, out[0], 1);   /* mono duplicated to L and R */
        expect("MONO/2ch", 1, out[1], 1);
    }

    if (g_failures) {
        printf("test_ac3_map: %d FAILURES\n", g_failures);
        return 1;
    }
    printf("test_ac3_map: all checks passed\n");
    return 0;
}
