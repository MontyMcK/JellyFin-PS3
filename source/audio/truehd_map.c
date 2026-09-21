// FFmpeg-ordered TrueHD output → PS3 CellAudio order.  See truehd_map.h.

#include "truehd_map.h"

// ---- Channel order derivation (from source, not memory) ----
//
// The vendored decoder writes interleaved samples in FFmpeg's NATIVE channel
// order, which is "ascending channel-mask bit order": for a mask, channel i
// of the output is the i-th set bit counting from bit 0.  That is what
// av_channel_layout_index_from_channel() computes (popcount of the mask below
// the channel's bit), and mlpdec.c's pack_output writes the channels in that
// index order.
//
// The masks the decoder can produce for TrueHD come from mlp_parse.c's
// truehd_layout() table: mono, stereo, 2.1, quad, 3.0(surround), 4.0, 5.0,
// 5.1 and their back/side variants, up to 7.1.  Note the table uses the
// *_BACK layouts for 5.x (AV_CH_LAYOUT_5POINT1_BACK = FL FR FC LFE BL BR):
// in a 5.1 mix those "back" channels ARE the surround pair, so they belong
// in the PS3's surround slots 4/5, not its rear slots 6/7.  A 7.1 mix is the
// only case where both pairs exist, and then the sides take 4/5 and the
// backs take 6/7 — the same split movian's ps3_audio.c makes when it permutes
// libav's 7.1 frame into PS3 order.
//
// Bit values (libavutil/channel_layout.h):
//   FL 0x1  FR 0x2  FC 0x4  LFE 0x8  BL 0x10  BR 0x20
//   FLC 0x40  FRC 0x80  BC 0x100  SL 0x200  SR 0x400

#define CH_FL   0x1ULL
#define CH_FR   0x2ULL
#define CH_FC   0x4ULL
#define CH_LFE  0x8ULL
#define CH_BL   0x10ULL
#define CH_BR   0x20ULL
#define CH_BC   0x100ULL
#define CH_SL   0x200ULL
#define CH_SR   0x400ULL

#define PS3_FL   0
#define PS3_FR   1
#define PS3_FC   2
#define PS3_LFE  3
#define PS3_SL   4
#define PS3_SR   5
#define PS3_BL   6
#define PS3_BR   7

#define LEVEL_3DB 0.7071067811865476f

static int popcount64(uint64_t v) {
    int n = 0;
    while (v) { v &= v - 1; n++; }
    return n;
}

// Source channel index of one mask bit, or -1 when the stream lacks it.
static int idx_of(uint64_t mask, uint64_t bit) {
    if (!(mask & bit)) return -1;
    return popcount64(mask & (bit - 1));
}

static void put(truehd_map_t *m, int slot, int src, float gain) {
    for (int k = 0; k < TRUEHD_MAX_SRC; k++) {
        if (m->src[slot][k] < 0) {
            m->src[slot][k]  = src;
            m->gain[slot][k] = gain;
            return;
        }
    }
}

int truehd_map_build(uint64_t ch_mask, int nb_ch, int out_ch, truehd_map_t *m)
{
    for (int s = 0; s < TRUEHD_MAX_SLOTS; s++)
        for (int k = 0; k < TRUEHD_MAX_SRC; k++) {
            m->src[s][k]  = -1;
            m->gain[s][k] = 0.0f;
        }
    m->out_ch = (out_ch == 8) ? 8 : (out_ch == 2) ? 2 : 6;

    if (!ch_mask || nb_ch <= 0 || popcount64(ch_mask) != nb_ch)
        return -1;                       // unusable: leaves silence

    const int fl  = idx_of(ch_mask, CH_FL);
    const int fr  = idx_of(ch_mask, CH_FR);
    const int fc  = idx_of(ch_mask, CH_FC);
    const int lfe = idx_of(ch_mask, CH_LFE);
    const int bl  = idx_of(ch_mask, CH_BL);
    const int br  = idx_of(ch_mask, CH_BR);
    const int bc  = idx_of(ch_mask, CH_BC);
    const int sl  = idx_of(ch_mask, CH_SL);
    const int sr  = idx_of(ch_mask, CH_SR);

    // Mono (front centre only) is the one layout with no L/R at all.
    if (fl < 0 && fr < 0 && fc >= 0) {
        if (m->out_ch == 2) {
            put(m, PS3_FL, fc, 1.0f);
            put(m, PS3_FR, fc, 1.0f);
        } else {
            put(m, PS3_FC, fc, 1.0f);
            if (lfe >= 0) put(m, PS3_LFE, lfe, 1.0f);
        }
        return 0;
    }
    if (fl < 0 || fr < 0) return -1;

    // The surround pair: sides when present, otherwise the backs (a 5.1 mix
    // labels its surrounds as BACK).  The rear pair exists only when both
    // pairs are present, i.e. a true 7.1 mix.
    const int surr_l = (sl >= 0) ? sl : bl;
    const int surr_r = (sr >= 0) ? sr : br;
    const int rear_l = (sl >= 0) ? bl : -1;
    const int rear_r = (sr >= 0) ? br : -1;

    if (m->out_ch == 2) {
        // Stereo port (the 8-channel open failed).  Fold centre and surrounds
        // in at -3 dB, then normalise by the summed coefficients so a
        // full-scale multichannel mix cannot clip — a lossless source has no
        // headroom to lend, and clipping is worse than 2-3 dB of level.
        float sum = 1.0f;
        if (fc >= 0)     sum += LEVEL_3DB;
        if (surr_l >= 0) sum += LEVEL_3DB;
        if (rear_l >= 0) sum += LEVEL_3DB;
        if (bc >= 0)     sum += LEVEL_3DB;
        const float g = (sum > 1.0f) ? 1.0f / sum : 1.0f;
        put(m, PS3_FL, fl, g);
        put(m, PS3_FR, fr, g);
        if (fc >= 0) {
            put(m, PS3_FL, fc, LEVEL_3DB * g);
            put(m, PS3_FR, fc, LEVEL_3DB * g);
        }
        if (surr_l >= 0 && surr_r >= 0) {
            put(m, PS3_FL, surr_l, LEVEL_3DB * g);
            put(m, PS3_FR, surr_r, LEVEL_3DB * g);
        }
        if (rear_l >= 0 && rear_r >= 0) {
            put(m, PS3_FL, rear_l, LEVEL_3DB * g);
            put(m, PS3_FR, rear_r, LEVEL_3DB * g);
        }
        if (bc >= 0) {
            put(m, PS3_FL, bc, LEVEL_3DB * g);
            put(m, PS3_FR, bc, LEVEL_3DB * g);
        }
        return 0;
    }

    put(m, PS3_FL, fl, 1.0f);
    put(m, PS3_FR, fr, 1.0f);
    if (fc  >= 0) put(m, PS3_FC,  fc,  1.0f);
    if (lfe >= 0) put(m, PS3_LFE, lfe, 1.0f);

    if (surr_l >= 0 && surr_r >= 0) {
        put(m, PS3_SL, surr_l, 1.0f);
        put(m, PS3_SR, surr_r, 1.0f);
    } else if (bc >= 0) {
        // Mono surround (3.0/4.0 with a back centre) to BOTH surrounds at
        // -3 dB — the same convention ac3_map.c and dts_map.c use, so the
        // surround energy matches a proper decode instead of doubling.
        put(m, PS3_SL, bc, LEVEL_3DB);
        put(m, PS3_SR, bc, LEVEL_3DB);
    }

    if (m->out_ch == 8) {
        if (rear_l >= 0) put(m, PS3_BL, rear_l, 1.0f);
        if (rear_r >= 0) put(m, PS3_BR, rear_r, 1.0f);
    } else if (rear_l >= 0 && rear_r >= 0) {
        // 7.1 source into a 5.1 program: fold the rears into the surrounds at
        // -3 dB rather than dropping them, so nothing in the mix disappears.
        put(m, PS3_SL, rear_l, LEVEL_3DB);
        put(m, PS3_SR, rear_r, LEVEL_3DB);
    }
    return 0;
}

void truehd_map_block(const int32_t *in, int nb_ch, int n,
                      const truehd_map_t *m, float *out)
{
    const float scale = 1.0f / 2147483648.0f;   // int32 full scale -> +-1.0
    const int   oc    = m->out_ch;
    for (int i = 0; i < n; i++) {
        const int32_t *s = in  + (size_t)i * nb_ch;
        float         *d = out + (size_t)i * oc;
        for (int c = 0; c < oc; c++) {
            float acc = 0.0f;
            for (int k = 0; k < TRUEHD_MAX_SRC; k++) {
                int src = m->src[c][k];
                if (src < 0) break;
                acc += (float)s[src] * scale * m->gain[c][k];
            }
            d[c] = acc;
        }
    }
}

// Planar variant — see truehd_map.h.  Same map, same gains, same output; the
// only difference is that each source channel is its own contiguous run
// instead of a stride inside one interleaved block.
void truehd_map_block_planar(const int32_t *const *planes, int nb_ch, int n,
                             const truehd_map_t *m, float *out)
{
    const float scale = 1.0f / 2147483648.0f;   // int32 full scale -> +-1.0
    const int   oc    = m->out_ch;
    (void)nb_ch;
    for (int i = 0; i < n; i++) {
        float *d = out + (size_t)i * oc;
        for (int c = 0; c < oc; c++) {
            float acc = 0.0f;
            for (int k = 0; k < TRUEHD_MAX_SRC; k++) {
                int src = m->src[c][k];
                if (src < 0) break;
                acc += (float)planes[src][i] * scale * m->gain[c][k];
            }
            d[c] = acc;
        }
    }
}
