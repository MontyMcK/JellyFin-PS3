/*
 * Host-side unit test for the FFmpeg-order → PS3-slot channel map
 * (source/audio/truehd_map.c).  No decoder: it feeds interleaved int32 frames
 * whose channels carry distinct values and asserts every PS3 slot receives
 * the value (and scale) the derivation comment promises, for each layout the
 * TrueHD decoder can report.
 *
 * The 5.1 case is the one that would be silently wrong if the "back" channels
 * of AV_CH_LAYOUT_5POINT1_BACK were taken literally as rears instead of as
 * the surround pair, so it is checked in both the back and side spellings.
 *
 * Build & run:  make -f Makefile.host && ./test_truehd_map
 */
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <inttypes.h>

#include "truehd_map.h"

#define LEVEL_3DB 0.7071067811865476f
#define SCALE     (1.0f / 2147483648.0f)

static int g_failures = 0;

/* channel k carries (k+1) << 20 so a misrouted channel is unmistakable */
static int32_t chan_val(int k) { return (int32_t)(k + 1) << 20; }

static void expect(const char *name, int slot, float got, float want) {
    if (fabsf(got - want) > 1e-9f) {
        printf("FAIL %s slot %d: got %.9f want %.9f\n", name, slot,
               (double)got, (double)want);
        g_failures++;
    }
}

/* expected[] is in units of chan_val(k)*SCALE, i.e. "which channel, x gain" */
static void check(const char *name, uint64_t mask, int nb_ch, int out_ch,
                  const float expected[8]) {
    int32_t in[4 * 8];
    float   out[4 * 8];
    for (int i = 0; i < 4; i++)
        for (int k = 0; k < nb_ch; k++)
            in[i * nb_ch + k] = chan_val(k);
    truehd_map_t m;
    if (truehd_map_build(mask, nb_ch, out_ch, &m) != 0) {
        printf("FAIL %s: map_build rejected the layout\n", name);
        g_failures++;
        return;
    }
    if (m.out_ch != out_ch) {
        printf("FAIL %s: out_ch %d, want %d\n", name, m.out_ch, out_ch);
        g_failures++;
        return;
    }
    truehd_map_block(in, nb_ch, 4, &m, out);
    for (int i = 0; i < 4; i++)
        for (int s = 0; s < out_ch; s++)
            expect(name, s, out[i * out_ch + s], expected[s]);
}

/* helper: slot fed by source channel k at unity */
static float ch(int k)          { return (float)chan_val(k) * SCALE; }
static float ch_g(int k, float g) { return (float)chan_val(k) * SCALE * g; }

int main(void) {
    /* ---- 5.1, "back" spelling: FL FR FC LFE BL BR ----
     * The backs ARE the surround pair in a 5.1 mix, so they must land in the
     * PS3 surround slots 4/5 and leave the rear slots silent. */
    { const float e[8] = { ch(0), ch(1), ch(2), ch(3), ch(4), ch(5), 0, 0 };
      check("5.1(back)->8", 0x3F, 6, 8, e); }

    /* ---- 5.1, "side" spelling: FL FR FC LFE SL SR (mask 0x60F) ----
     * Same result: sides are the surrounds.  ffmpeg's own truehd encoder
     * produces this spelling, so it is what the decode test exercises. */
    { const float e[8] = { ch(0), ch(1), ch(2), ch(3), ch(4), ch(5), 0, 0 };
      check("5.1(side)->8", 0x60F, 6, 8, e); }

    /* ---- 7.1: FL FR FC LFE BL BR SL SR (mask 0x63F) ----
     * FFmpeg order puts the BACKS at indices 4/5 and the SIDES at 6/7,
     * because BL/BR are lower mask bits.  The PS3 wants the opposite:
     * surrounds (sides) in 4/5, rears (backs) in 6/7.  This swap is the whole
     * reason this file exists. */
    { const float e[8] = { ch(0), ch(1), ch(2), ch(3), ch(6), ch(7),
                           ch(4), ch(5) };
      check("7.1->8", 0x63F, 8, 8, e); }

    /* ---- 7.1 into a 5.1 program: rears fold into the surrounds at -3 dB */
    { const float e[8] = { ch(0), ch(1), ch(2), ch(3),
                           ch(6) + ch_g(4, LEVEL_3DB),
                           ch(7) + ch_g(5, LEVEL_3DB), 0, 0 };
      check("7.1->6", 0x63F, 8, 6, e); }

    /* ---- stereo ---- */
    { const float e[8] = { ch(0), ch(1), 0, 0, 0, 0, 0, 0 };
      check("stereo->8", 0x3, 2, 8, e); }

    /* ---- mono goes to the CENTRE speaker, not to L+R ---- */
    { const float e[8] = { 0, 0, ch(0), 0, 0, 0, 0, 0 };
      check("mono->8", 0x4, 1, 8, e); }

    /* ---- mono to a stereo port goes to both ---- */
    { const float e[8] = { ch(0), ch(0), 0, 0, 0, 0, 0, 0 };
      check("mono->2", 0x4, 1, 2, e); }

    /* ---- 2.1 (FL FR LFE): no centre, no surrounds ---- */
    { const float e[8] = { ch(0), ch(1), 0, ch(2), 0, 0, 0, 0 };
      check("2.1->8", 0xB, 3, 8, e); }

    /* ---- quad (FL FR BL BR): backs are the surround pair ---- */
    { const float e[8] = { ch(0), ch(1), 0, 0, ch(2), ch(3), 0, 0 };
      check("quad->8", 0x33, 4, 8, e); }

    /* ---- 4.0 (FL FR FC BC): the mono back centre feeds BOTH surrounds at
     * -3 dB, the same convention ac3_map.c and dts_map.c use ---- */
    { const float e[8] = { ch(0), ch(1), ch(2), 0,
                           ch_g(3, LEVEL_3DB), ch_g(3, LEVEL_3DB), 0, 0 };
      check("4.0->8", 0x107, 4, 8, e); }

    /* ---- 5.1 down to a stereo port: centre folded in, normalised so a
     * full-scale lossless mix cannot clip ---- */
    { const float g = 1.0f / (1.0f + LEVEL_3DB + LEVEL_3DB);
      const float e[8] = { ch_g(0, g) + ch_g(2, LEVEL_3DB * g) + ch_g(4, LEVEL_3DB * g),
                           ch_g(1, g) + ch_g(2, LEVEL_3DB * g) + ch_g(5, LEVEL_3DB * g),
                           0, 0, 0, 0, 0, 0 };
      check("5.1->2", 0x3F, 6, 2, e); }

    /* ---- a layout whose mask does not match the channel count is refused
     * rather than read out of bounds ---- */
    { truehd_map_t m;
      if (truehd_map_build(0x3F, 4, 8, &m) == 0) {
          printf("FAIL: mask/channel-count mismatch was accepted\n");
          g_failures++;
      }
      if (truehd_map_build(0, 0, 8, &m) == 0) {
          printf("FAIL: empty mask was accepted\n");
          g_failures++;
      } }

    if (g_failures) {
        printf("test_truehd_map: %d FAILURES\n", g_failures);
        return 1;
    }
    printf("test_truehd_map: all channel maps correct\n");
    return 0;
}
