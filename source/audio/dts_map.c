// libdca planar output → PS3 CellAudio interleaved order.  See dts_map.h.

#include "dts_map.h"
#include <inttypes.h>   /* dca.h uses uint*_t but does not include this itself */
#include "dca/dca.h"

// ---- Channel order derivation (from source, not memory) ----
//
// PS3 side — identical to the AC-3 path; see ac3_map.c for the full
// derivation from movian/src/arch/ps3/ps3_audio.c:
//     0=FL 1=FR 2=FC 3=LFE 4=SL 5=SR  (6=BL 7=BR zeroed by the output stage)
//
// libdca side — this is where DTS differs from AC-3, in TWO ways:
//
// 1. LFE plane position.  liba52 puts LFE FIRST (plane 0) and shifts the
//    full-bandwidth channels up by one.  libdca puts it LAST:
//    dca_parse.c:1015-1022 writes the interpolated LFE to
//    `&state->samples[256*i_channels]` where
//    `i_channels = dca_channels[state->output & DCA_CHANNEL_MASK]` — i.e.
//    the plane AFTER the granted config's full-bandwidth channels.  So
//    fbw channel i is plane i, and LFE (when granted) is plane
//    dca_channels[cfg].
//
// 2. Full-bandwidth order.  AC-3's 3F is L C R; DTS's is **C first**.
//    Derived from libdca's own downmix code, which was adapted from
//    liba52's for exactly this difference:
//      * dca_downmix.c mix32to2() (3F2R → STEREO) takes `common = samples[i]`
//        — plane 0 — and adds it to BOTH outputs, then folds plane 1 + plane 3
//        into left and plane 2 + plane 4 into right.  Centre is the channel
//        common to both, so 3F2R is C(0) L(1) R(2) SL(3) SR(4).
//        (liba52's mix32to2 takes `common = samples[i + 256]`, plane 1,
//        because its order is L C R SL SR — the one-line difference that
//        makes a shared map file impossible.)
//      * dca_downmix.c mix3to2() (3F → STEREO) likewise takes plane 0 as
//        `common` and emits plane1+C, plane2+C: 3F is C(0) L(1) R(2).
//      * mix32toS() (3F2R → DOLBY) agrees: common = plane 0, surrounds are
//        planes 3 and 4, L/R are planes 1 and 2.
//    This matches the DTS specification's AMODE table, where the 3/2 and 3/0
//    arrangements are transmitted as C, L, R[, Ls, Rs].
//
// Plane table used below (granted cfg → plane meaning):
//
//   MONO / CHANNEL1/2        : 0=C(mono)
//   CHANNEL (dual mono)      : 0=L 1=R
//   STEREO / SUMDIFF / TOTAL : 0=L 1=R
//   DOLBY                    : 0=Lt 1=Rt        (Dolby Surround encoded pair)
//   3F                       : 0=C 1=L 2=R
//   2F1R                     : 0=L 1=R 2=S
//   3F1R                     : 0=C 1=L 2=R 3=S
//   2F2R                     : 0=L 1=R 2=SL 3=SR
//   3F2R                     : 0=C 1=L 2=R 3=SL 4=SR
//   4F2R                     : not requestable here (DCA_CHANNEL_MAX is 3F2R,
//                              dca.h:60) — falls to `default` = silence.
//   + DCA_LFE                : LFE at plane dca_channels[cfg]
//
// Verified end to end by tests/test_dts_decode.c: an ffmpeg-encoded 5.1 DTS
// file with a distinct tone per speaker decodes through this exact map with
// every PS3 slot dominated by its own tone, matching ffmpeg's own decoder
// channel-for-channel.
//
// Mono surround (2F1R/3F1R) is sent to BOTH SL and SR at -3 dB, the same
// convention (and the same constant) ac3_map.c uses, so the surround energy
// matches a proper decode instead of doubling.

#define PS3_FL   0
#define PS3_FR   1
#define PS3_FC   2
#define PS3_LFE  3
#define PS3_SL   4
#define PS3_SR   5

#define LEVEL_3DB 0.7071067811865476f

// Full-bandwidth channel count per granted config — the same table libdca
// itself uses to place the LFE plane (tables.h: dca_channels[]), restricted
// to the configs this map handles.
static int dts_fbw_channels(int cfg)
{
    switch (cfg) {
    case DCA_MONO:            return 1;
    case DCA_CHANNEL:         return 2;
    case DCA_STEREO:          return 2;
    case DCA_STEREO_SUMDIFF:  return 2;
    case DCA_STEREO_TOTAL:    return 2;
    case DCA_DOLBY:           return 2;
    case DCA_3F:              return 3;
    case DCA_2F1R:            return 3;
    case DCA_3F1R:            return 4;
    case DCA_2F2R:            return 4;
    case DCA_3F2R:            return 5;
    default:                  return 0;
    }
}

void dts_map_block(const float *planes, int dca_flags, float *out, int out_ch)
{
    const int cfg = dca_flags & DCA_CHANNEL_MASK;
    const int fbw = dts_fbw_channels(cfg);
    const int lfe = ((dca_flags & DCA_LFE) && fbw > 0) ? 1 : 0;
    // LFE is the plane after the full-bandwidth channels (dca_parse.c:1015).
    const float *bass = planes + 256 * fbw;   // valid only when lfe

    for (int i = 0; i < 256; i++) {
        float *d = out + i * out_ch;
        for (int c = 0; c < out_ch; c++) d[c] = 0.0f;

        switch (cfg) {
        case DCA_MONO:
            if (out_ch == 6) {
                d[PS3_FC] = planes[i];
            } else {
                d[PS3_FL] = d[PS3_FR] = planes[i];
            }
            break;
        case DCA_CHANNEL:                 // dual independent mono
        case DCA_STEREO:
        case DCA_STEREO_SUMDIFF:
        case DCA_STEREO_TOTAL:
        case DCA_DOLBY:
            d[PS3_FL] = planes[i];
            d[PS3_FR] = planes[256 + i];
            break;
        case DCA_3F:                      // C L R
            d[PS3_FL] = planes[256 + i];
            d[PS3_FR] = planes[512 + i];
            if (out_ch == 6) {
                d[PS3_FC] = planes[i];
            } else {                      // fold centre into L/R at -3 dB
                d[PS3_FL] += LEVEL_3DB * planes[i];
                d[PS3_FR] += LEVEL_3DB * planes[i];
            }
            break;
        case DCA_2F1R:                    // L R S
            d[PS3_FL] = planes[i];
            d[PS3_FR] = planes[256 + i];
            if (out_ch == 6) {
                d[PS3_SL] = LEVEL_3DB * planes[512 + i];
                d[PS3_SR] = LEVEL_3DB * planes[512 + i];
            }
            break;
        case DCA_3F1R:                    // C L R S
            d[PS3_FL] = planes[256 + i];
            d[PS3_FR] = planes[512 + i];
            if (out_ch == 6) {
                d[PS3_FC] = planes[i];
                d[PS3_SL] = LEVEL_3DB * planes[768 + i];
                d[PS3_SR] = LEVEL_3DB * planes[768 + i];
            } else {
                d[PS3_FL] += LEVEL_3DB * planes[i];
                d[PS3_FR] += LEVEL_3DB * planes[i];
            }
            break;
        case DCA_2F2R:                    // L R SL SR
            d[PS3_FL] = planes[i];
            d[PS3_FR] = planes[256 + i];
            if (out_ch == 6) {
                d[PS3_SL] = planes[512 + i];
                d[PS3_SR] = planes[768 + i];
            }
            break;
        case DCA_3F2R:                    // C L R SL SR
            d[PS3_FL] = planes[256 + i];
            d[PS3_FR] = planes[512 + i];
            if (out_ch == 6) {
                d[PS3_FC] = planes[i];
                d[PS3_SL] = planes[768  + i];
                d[PS3_SR] = planes[1024 + i];
            } else {
                d[PS3_FL] += LEVEL_3DB * planes[i];
                d[PS3_FR] += LEVEL_3DB * planes[i];
            }
            break;
        default:
            break;                        // leaves silence
        }

        if (lfe && out_ch == 6)
            d[PS3_LFE] = bass[i];
    }
}
