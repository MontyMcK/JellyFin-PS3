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

center_mode_t centermix_get(void) { return s_mode; }

void centermix_set(center_mode_t m) {
    if (m < CENTER_NORMAL || m >= CENTER_COUNT) m = CENTER_NORMAL;
    s_mode = m;
    centermix_save();
}

void centermix_cycle(void) {
    centermix_set((center_mode_t)((s_mode + 1) % CENTER_COUNT));
}

const char *centermix_label(void) {
    switch (s_mode) {
    case CENTER_P3:      return "+3 dB";
    case CENTER_P6:      return "+6 dB";
    case CENTER_P10:     return "+10 dB";
    case CENTER_PHANTOM: return "Phantom";
    default:             return "Normal";
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
    if (fscanf(f, "%d", &v) == 1 && v >= CENTER_NORMAL && v < CENTER_COUNT)
        s_mode = (center_mode_t)v;
    fclose(f);
}

void centermix_save(void) {
    FILE *f = fopen(jf_data_path(CENTERMIX_FILE), "w");
    if (!f) return;
    fprintf(f, "%d\n", (int)s_mode);
    fclose(f);
}
