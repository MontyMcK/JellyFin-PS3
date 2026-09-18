// Dialogue / centre-channel handling — see centermix.h.

#include "centermix.h"
#include "jf_paths.h"     // jf_data_path()
#include <stdio.h>

#define CENTERMIX_FILE "jellyfin_centermix.txt"

// PS3 CellAudio slot indices (see centermix.h for the derivation).
#define SLOT_FL  0
#define SLOT_FR  1
#define SLOT_FC  2

#define LEVEL_3DB 0.7071067811865476f

static center_mode_t s_mode = CENTER_NORMAL;

// See centermix.h: PHANTOM is retired and is not in this list.
const center_mode_t CENTERMIX_ORDER[] = {
    CENTER_NORMAL,
    CENTER_P3,
    CENTER_P6,
    CENTER_P10,
};
const int CENTERMIX_ORDER_N =
    (int)(sizeof(CENTERMIX_ORDER) / sizeof(CENTERMIX_ORDER[0]));

center_mode_t centermix_sanitize(int v) {
    if (v < CENTER_NORMAL || v >= CENTER_COUNT) return CENTER_NORMAL;
    if (v == CENTER_PHANTOM || v == CENTER_STEREO) return CENTER_NORMAL;
    return (center_mode_t)v;
}

center_mode_t centermix_get(void) { return s_mode; }

void centermix_set(center_mode_t m) {
    s_mode = centermix_sanitize((int)m);
    centermix_save();
}

void centermix_cycle(void) {
    int i = 0;
    for (int k = 0; k < CENTERMIX_ORDER_N; k++)
        if (CENTERMIX_ORDER[k] == s_mode) { i = k; break; }
    centermix_set(CENTERMIX_ORDER[(i + 1) % CENTERMIX_ORDER_N]);
}

const char *centermix_label(void) {
    switch (s_mode) {
    case CENTER_NORMAL:  return "Off";
    case CENTER_P3:      return "+3 dB";
    case CENTER_P6:      return "+6 dB";
    case CENTER_P10:     return "+10 dB";
    case CENTER_PHANTOM: return "Phantom";   // retired; unreachable via the UI
    case CENTER_STEREO:  return "Stereo";    // retired; unreachable via the UI
    default:             return "Off";
    }
}

bool centermix_active(void) { return s_mode != CENTER_NORMAL; }

static float centermix_gain(void) {
    switch (s_mode) {
    case CENTER_P3:  return 1.4125375f;   // +3 dB
    case CENTER_P6:  return 1.9952624f;   // +6 dB
    case CENTER_P10: return 3.1622777f;   // +10 dB
    default:         return 1.0f;
    }
}

static inline float clamp1(float v) {
    if (v >  1.0f) return  1.0f;
    if (v < -1.0f) return -1.0f;
    return v;
}

void centermix_apply(float *frames, int n, int ch) {
    if (s_mode == CENTER_NORMAL || ch < 3 || n <= 0) return;

    if (s_mode == CENTER_PHANTOM) {
        // Fold centre into the two channels the chain is certainly rendering,
        // then mute the slot so a sink that DOES render it cannot play the
        // dialogue twice.  -3 dB into each preserves the centre's energy,
        // which is the same convention the decoders' own downmixes use.
        for (int i = 0; i < n; i++) {
            float *d = frames + (size_t)i * ch;
            const float c = d[SLOT_FC];
            d[SLOT_FL] = clamp1(d[SLOT_FL] + LEVEL_3DB * c);
            d[SLOT_FR] = clamp1(d[SLOT_FR] + LEVEL_3DB * c);
            d[SLOT_FC] = 0.0f;
        }
        return;
    }

    if (s_mode == CENTER_STEREO) {
        // LoRo: L' = g*(L + .707*C + .707*Ls + .707*Lb), same for R, with
        // g = -3 dB of headroom so a lossless source sitting near full scale
        // does not spend the whole film against the clamp.
        const float g = LEVEL_3DB;
        for (int i = 0; i < n; i++) {
            float *d = frames + (size_t)i * ch;
            float l = d[SLOT_FL], r = d[SLOT_FR];
            l += LEVEL_3DB * d[SLOT_FC];
            r += LEVEL_3DB * d[SLOT_FC];
            if (ch > 5) {                       // surround pair
                l += LEVEL_3DB * d[4];
                r += LEVEL_3DB * d[5];
            }
            if (ch > 7) {                       // rear pair (7.1 source)
                l += LEVEL_3DB * d[6];
                r += LEVEL_3DB * d[7];
            }
            for (int c = 2; c < ch; c++) d[c] = 0.0f;
            d[SLOT_FL] = clamp1(l * g);
            d[SLOT_FR] = clamp1(r * g);
        }
        return;
    }

    const float g = centermix_gain();
    for (int i = 0; i < n; i++) {
        float *d = frames + (size_t)i * ch;
        d[SLOT_FC] = clamp1(d[SLOT_FC] * g);
    }
}

// Missing file => Normal, which is the behaviour that shipped.
void centermix_load(void) {
    FILE *f = fopen(jf_data_path(CENTERMIX_FILE), "r");
    if (!f) return;
    int v = 0;
    if (fscanf(f, "%d", &v) == 1)
        s_mode = centermix_sanitize(v);
    fclose(f);
}

void centermix_save(void) {
    FILE *f = fopen(jf_data_path(CENTERMIX_FILE), "w");
    if (!f) return;
    fprintf(f, "%d\n", (int)s_mode);
    fclose(f);
}
