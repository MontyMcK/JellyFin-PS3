// liba52 planar output → PS3 CellAudio interleaved order.  See ac3_map.h.

#include "ac3_map.h"
#include <inttypes.h>   /* a52.h uses uint*_t but does not include this itself */
#include "a52/a52.h"

// ---- Channel order derivation (from source, not memory) ----
//
// PS3 side — the 8ch CellAudio frame order is
//     0=FL 1=FR 2=FC 3=LFE 4=SL 5=SR 6=BL 7=BR
// derived from movian src/arch/ps3/ps3_audio.c: its AV_CH_LAYOUT_7POINT1
// branch vec_perms libav's frame tail (BL BR SL SR) into (SL SR BL BR)
// before the DMA write, i.e. the PS3 expects surrounds at 4/5 and rears at
// 6/7; the first four slots pass through untouched from libav's
// FL FR FC LFE.  A 5.1 program therefore fills slots 0-5 and the output
// stage zeroes 6/7.  This file emits the six-wide program (FL FR FC LFE SL
// SR); out_ch==2 emits FL FR.
//
// liba52 side — a52dec-salsa (Debian mirror of a52dec 0.8.0):
//   * parse.c:761-763: `samples = state->samples; if (state->output &
//     A52_LFE) samples += 256;` — when LFE is granted it occupies PLANE 0
//     of a52_samples(), and the full-bandwidth channels start at plane 1.
//   * parse.c:822-828: the LFE coefficients decode into `samples - 256`,
//     confirming plane 0.
//   * downmix.c:545-549 (CONVERT(A52_3F2R, A52_3F)): surround planes at
//     offsets 768 (=plane 3) and 1024 (=plane 4) fold into planes 0 and 2 —
//     so for 3F2R the fbw plane order is L(0) C(1) R(2) SL(3) SR(4).
//   * downmix.c:579-583 (CONVERT(A52_3F2R, A52_2F2R)): mix3to2 folds C
//     (plane 1) into L/R, then planes 3,4 move to 2,3 — consistent with the
//     same order, and giving 2F2R's order L R SL SR.
//   * libao/float2s16.c:158-167 (a52dec's own output driver, the reference
//     consumer): its A52_3F2R|A52_LFE case emits plane1,3,4,5,2,0 as
//     L,R,SL,SR,C,LFE (a52dec interleaves in the old OSS order) — the same
//     plane meanings used here.
//
// Empirically confirmed by tests/test_ac3_decode.c: an ffmpeg-encoded 5.1
// file with a distinct tone per speaker decodes through this exact map with
// every PS3 slot dominated by its own tone (>90 dB margin), matching
// ffmpeg's own decoder channel-for-channel.
//
// Mono surround (2F1R/3F1R) is sent to BOTH SL and SR at -3 dB
// (LEVEL_3DB = 1/sqrt(2) — a52_internal.h:92, the constant liba52's own
// downmix level math uses throughout downmix.c:70-116) so the surround
// energy matches a proper decode instead of doubling.

#define PS3_FL   0
#define PS3_FR   1
#define PS3_FC   2
#define PS3_LFE  3
#define PS3_SL   4
#define PS3_SR   5

#define LEVEL_3DB 0.7071067811865476f

void ac3_map_block(const float *planes, int a52_flags, float *out, int out_ch)
{
    const int   lfe  = (a52_flags & A52_LFE) ? 1 : 0;
    const float *bass = planes;              // valid only when lfe
    const float *fbw  = planes + (lfe ? 256 : 0);
    const int   cfg   = a52_flags & A52_CHANNEL_MASK;

    for (int i = 0; i < 256; i++) {
        float *d = out + i * out_ch;
        for (int c = 0; c < out_ch; c++) d[c] = 0.0f;

        switch (cfg) {
        case A52_MONO:
        case A52_CHANNEL1:
        case A52_CHANNEL2:
            if (out_ch == 6) {
                d[PS3_FC] = fbw[i];
            } else {
                d[PS3_FL] = d[PS3_FR] = fbw[i];
            }
            break;
        case A52_CHANNEL:                    // dual independent mono
        case A52_STEREO:
        case A52_DOLBY:
            d[PS3_FL] = fbw[i];
            d[PS3_FR] = fbw[256 + i];
            break;
        case A52_3F:
            d[PS3_FL] = fbw[i];
            d[PS3_FR] = fbw[512 + i];
            if (out_ch == 6)
                d[PS3_FC] = fbw[256 + i];
            else {                           // fold centre into L/R at -3 dB
                d[PS3_FL] += LEVEL_3DB * fbw[256 + i];
                d[PS3_FR] += LEVEL_3DB * fbw[256 + i];
            }
            break;
        case A52_2F1R:
            d[PS3_FL] = fbw[i];
            d[PS3_FR] = fbw[256 + i];
            if (out_ch == 6) {
                d[PS3_SL] = LEVEL_3DB * fbw[512 + i];
                d[PS3_SR] = LEVEL_3DB * fbw[512 + i];
            }
            break;
        case A52_3F1R:
            d[PS3_FL] = fbw[i];
            d[PS3_FR] = fbw[512 + i];
            if (out_ch == 6) {
                d[PS3_FC] = fbw[256 + i];
                d[PS3_SL] = LEVEL_3DB * fbw[768 + i];
                d[PS3_SR] = LEVEL_3DB * fbw[768 + i];
            } else {
                d[PS3_FL] += LEVEL_3DB * fbw[256 + i];
                d[PS3_FR] += LEVEL_3DB * fbw[256 + i];
            }
            break;
        case A52_2F2R:
            d[PS3_FL] = fbw[i];
            d[PS3_FR] = fbw[256 + i];
            if (out_ch == 6) {
                d[PS3_SL] = fbw[512 + i];
                d[PS3_SR] = fbw[768 + i];
            }
            break;
        case A52_3F2R:
            d[PS3_FL] = fbw[i];
            d[PS3_FR] = fbw[512 + i];
            if (out_ch == 6) {
                d[PS3_FC] = fbw[256 + i];
                d[PS3_SL] = fbw[768 + i];
                d[PS3_SR] = fbw[1024 + i];
            } else {
                d[PS3_FL] += LEVEL_3DB * fbw[256 + i];
                d[PS3_FR] += LEVEL_3DB * fbw[256 + i];
            }
            break;
        default:
            break;                           // leaves silence
        }

        if (lfe && out_ch == 6)
            d[PS3_LFE] = bass[i];
    }
}
