/*
 * Host-side unit test for the libdca-plane → PS3-slot channel map
 * (source/audio/dts_map.c).  No PS3 headers, no decoder — it feeds planar
 * blocks whose planes carry distinct constant values and asserts every PS3
 * slot receives the value (and scale) the derivation comment promises.
 *
 * This is the AC-3 test's twin, and the pair is the point: DTS orders its
 * planes CENTRE FIRST and puts LFE LAST, exactly opposite to AC-3 on both
 * counts, so the two maps cannot share code and must not share assumptions.
 *
 * Build & run:  make -f Makefile.host  (see that file; runs on any host cc)
 */
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <inttypes.h>   /* dca.h uses uint*_t but does not include this itself */

#include "dts_map.h"
#include "dca/dca.h"

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
    dts_map_block(planes, flags, out, 6);
    /* spot-check first and last frame — the map is per-frame uniform */
    for (int i = 0; i < 256; i += 255)
        for (int s = 0; s < 6; s++)
            expect(name, s, out[i * 6 + s], expected[s]);
}

/* Same for the 2-wide (stereo port) output: expected[] is FL FR. */
static void check2(const char *name, int flags, int nplanes,
                   const float expected[2]) {
    float planes[6 * 256];
    float out[2 * 256];
    fill_planes(planes, nplanes);
    dts_map_block(planes, flags, out, 2);
    for (int i = 0; i < 256; i += 255)
        for (int s = 0; s < 2; s++)
            expect(name, s, out[i * 2 + s], expected[s]);
}

int main(void) {
    /* DCA_3F2R|LFE: planes = C(1) L(2) R(3) SL(4) SR(5) LFE(6)
     * PS3 slots FL FR FC LFE SL SR = L R C LFE SL SR = 2 3 1 6 4 5
     * Note LFE at plane 5 (the plane AFTER the 5 fbw channels) — the
     * opposite end from liba52, which puts it at plane 0. */
    { const float e[6] = {2, 3, 1, 6, 4, 5};
      check6("3F2R|LFE", DCA_3F2R | DCA_LFE, 6, e); }

    /* DCA_3F2R (no LFE): planes = C(1) L(2) R(3) SL(4) SR(5) */
    { const float e[6] = {2, 3, 1, 0, 4, 5};
      check6("3F2R", DCA_3F2R, 5, e); }

    /* DCA_2F2R: planes = L(1) R(2) SL(3) SR(4) — no centre in DTS's 2/2 */
    { const float e[6] = {1, 2, 0, 0, 3, 4};
      check6("2F2R", DCA_2F2R, 4, e); }

    /* DCA_2F2R|LFE: LFE is plane 4 (after the 4 fbw channels) */
    { const float e[6] = {1, 2, 0, 5, 3, 4};
      check6("2F2R|LFE", DCA_2F2R | DCA_LFE, 5, e); }

    /* DCA_3F1R: planes = C(1) L(2) R(3) S(4); mono surround to BOTH
     * rears at -3 dB */
    { const float e[6] = {2, 3, 1, 0, LEVEL_3DB * 4, LEVEL_3DB * 4};
      check6("3F1R", DCA_3F1R, 4, e); }

    /* DCA_2F1R: planes = L(1) R(2) S(3) */
    { const float e[6] = {1, 2, 0, 0, LEVEL_3DB * 3, LEVEL_3DB * 3};
      check6("2F1R", DCA_2F1R, 3, e); }

    /* DCA_3F: planes = C(1) L(2) R(3) */
    { const float e[6] = {2, 3, 1, 0, 0, 0};
      check6("3F", DCA_3F, 3, e); }

    /* DCA_STEREO: planes = L(1) R(2) */
    { const float e[6] = {1, 2, 0, 0, 0, 0};
      check6("STEREO", DCA_STEREO, 2, e); }

    /* DCA_MONO to a 5.1 port goes to the CENTRE speaker, not to L+R */
    { const float e[6] = {0, 0, 1, 0, 0, 0};
      check6("MONO", DCA_MONO, 1, e); }

    /* DCA_MONO|LFE: one fbw channel, so LFE is plane 1 */
    { const float e[6] = {0, 0, 1, 2, 0, 0};
      check6("MONO|LFE", DCA_MONO | DCA_LFE, 2, e); }

    /* ---- stereo port (out_ch == 2): centre and surrounds fold in ---- */

    /* 3F2R to stereo: L + C*-3dB, R + C*-3dB.  Surrounds are dropped here:
     * when the app asks libdca for STEREO the decoder folds them itself, so
     * this branch only ever sees a 5.1 layout on the way to a 2ch port when
     * the port open FAILED and the request was still 3F2R. */
    { const float e[2] = {2 + LEVEL_3DB * 1, 3 + LEVEL_3DB * 1};
      check2("3F2R->2ch", DCA_3F2R, 5, e); }

    /* MONO to stereo goes to both speakers */
    { const float e[2] = {1, 1};
      check2("MONO->2ch", DCA_MONO, 1, e); }

    /* An unknown/unsupported config must be silence, never garbage:
     * DCA_4F2R is above DCA_CHANNEL_MAX and can never be granted. */
    { const float e[6] = {0, 0, 0, 0, 0, 0};
      check6("4F2R(unsupported)", DCA_4F2R, 6, e); }

    if (g_failures) {
        printf("test_dts_map: %d FAILURES\n", g_failures);
        return 1;
    }
    printf("test_dts_map: all channel maps correct\n");
    return 0;
}
