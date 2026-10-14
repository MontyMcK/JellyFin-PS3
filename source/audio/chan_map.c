// Channel mapping: see chan_map.h.

#include "chan_map.h"

#include <stdbool.h>
#include <string.h>

#define S_FL  0
#define S_FR  1
#define S_FC  2
#define S_LFE 3
#define S_SL  4
#define S_SR  5

#define LEVEL_3DB 0.7071067811865476f

// The weights of one input channel in the six slots.
static void weights_of(int pos, bool has_side, float w[6]) {
    memset(w, 0, 6 * sizeof(float));
    switch (pos) {
    case CH_POS_FRONT_LEFT:   w[S_FL] = 1.0f; break;
    case CH_POS_FRONT_RIGHT:  w[S_FR] = 1.0f; break;
    case CH_POS_FRONT_CENTER: w[S_FC] = 1.0f; break;
    case CH_POS_LFE:          w[S_LFE] = 1.0f; break;
    case CH_POS_SIDE_LEFT:    w[S_SL] = 1.0f; break;
    case CH_POS_SIDE_RIGHT:   w[S_SR] = 1.0f; break;
    case CH_POS_BACK_LEFT:    w[S_SL] = has_side ? LEVEL_3DB : 1.0f; break;
    case CH_POS_BACK_RIGHT:   w[S_SR] = has_side ? LEVEL_3DB : 1.0f; break;
    case CH_POS_BACK_CENTER:  w[S_SL] = w[S_SR] = LEVEL_3DB; break;
    default: break;
    }
}

void chan_wave_positions(int ch, unsigned char *pos) {
    enum { L = CH_POS_FRONT_LEFT, R = CH_POS_FRONT_RIGHT, C = CH_POS_FRONT_CENTER, LFE = CH_POS_LFE, BL = CH_POS_BACK_LEFT,
           BR = CH_POS_BACK_RIGHT, BC = CH_POS_BACK_CENTER, SL = CH_POS_SIDE_LEFT, SR = CH_POS_SIDE_RIGHT };
    static const unsigned char ORDER[9][8] = {
        { 0 },
        { C },
        { L, R },
        { L, R, C },
        { L, R, BL, BR },
        { L, R, C, BL, BR },
        { L, R, C, LFE, BL, BR },
        { L, R, C, LFE, BC, SL, SR },
        { L, R, C, LFE, BL, BR, SL, SR },
    };
    for (int i = 0; i < ch; i++) pos[i] = (ch >= 1 && ch <= 8 && i < 8) ? ORDER[ch][i] : (unsigned char)CH_POS_UNKNOWN;
}

void chan_map_frames(const float *in, int frames, int ch, const unsigned char *pos, float *out, int out_ch) {
    // the usual order, for a stream whose positions are all unknown
    static const unsigned char DEFAULT_ORDER[6] = { CH_POS_FRONT_LEFT, CH_POS_FRONT_RIGHT, CH_POS_FRONT_CENTER,
                                                    CH_POS_LFE, CH_POS_SIDE_LEFT, CH_POS_SIDE_RIGHT };
    unsigned char p[8];
    bool any_known = false;
    for (int c = 0; c < ch && c < 8; c++) { p[c] = pos ? pos[c] : CH_POS_UNKNOWN; if (p[c]) any_known = true; }
    if (!any_known) for (int c = 0; c < ch && c < 8; c++) p[c] = c < 6 ? DEFAULT_ORDER[c] : CH_POS_UNKNOWN;
    if (ch == 1 && !any_known) p[0] = CH_POS_FRONT_CENTER;

    bool has_side = false, has_fl = false, has_fr = false, has_c = false, has_surround = false;
    for (int c = 0; c < ch && c < 8; c++) {
        if (p[c] == CH_POS_SIDE_LEFT || p[c] == CH_POS_SIDE_RIGHT) has_side = true;
        if (p[c] == CH_POS_FRONT_LEFT) has_fl = true;
        if (p[c] == CH_POS_FRONT_RIGHT) has_fr = true;
        if (p[c] == CH_POS_FRONT_CENTER) has_c = true;
        if (p[c] == CH_POS_SIDE_LEFT || p[c] == CH_POS_SIDE_RIGHT || p[c] == CH_POS_BACK_LEFT ||
            p[c] == CH_POS_BACK_RIGHT || p[c] == CH_POS_BACK_CENTER) has_surround = true;
    }
    float w[8][6];
    for (int c = 0; c < ch && c < 8; c++) weights_of(p[c], has_side, w[c]);

    const bool mono = !has_fl && !has_fr && has_c && !has_surround;      // one centre channel and nothing else
    // two wide: front + 0.707 centre + 0.707 surrounds, scaled to fit; plain stereo keeps unity
    const float down = (has_c && !mono) || has_surround ? 1.0f / (1.0f + 2.0f * LEVEL_3DB) : 1.0f;

    for (int i = 0; i < frames; i++) {
        float s[6] = { 0, 0, 0, 0, 0, 0 };
        for (int c = 0; c < ch && c < 8; c++) {
            const float v = in[(size_t)i * (size_t)ch + (size_t)c];
            for (int k = 0; k < 6; k++) s[k] += w[c][k] * v;
        }
        float *d = out + (size_t)i * (size_t)out_ch;
        if (out_ch == 6) {
            for (int k = 0; k < 6; k++) d[k] = s[k];
        } else {
            if (mono) {
                d[0] = d[1] = s[S_FC];
            } else {
                d[0] = (s[S_FL] + LEVEL_3DB * s[S_FC] + LEVEL_3DB * s[S_SL]) * down;
                d[1] = (s[S_FR] + LEVEL_3DB * s[S_FC] + LEVEL_3DB * s[S_SR]) * down;
            }
        }
    }
}
