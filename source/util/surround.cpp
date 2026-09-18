// Surround 5.1 (Alpha) setting store — see surround.h.

#include "surround.h"
#include "jf_paths.h"     // jf_data_path()
#include "audio_out.h"    // audio_out_lpcm_max_channels()
#include <stdio.h>

#define SURROUND_FILE "jellyfin_surround.txt"

static surround_mode_t s_mode = SURROUND_OFF;
// Session-scoped veto on the copy path (not persisted): set when the copied
// track turns out to be undecodable here (a coreless DTS-HD MA track), and
// cleared when a new title starts.
static bool s_hd_session_off = false;

surround_mode_t surround_get_mode(void) { return s_mode; }

// Stereo and 5.1 always; 7.1 only where the chain will take 8 channels of
// LPCM -- see surround.h.
const surround_mode_t SURROUND_ORDER[] = {
    SURROUND_OFF, SURROUND_HD, SURROUND_HD_71,
};

int surround_order_count(void) {
    return (audio_out_lpcm_max_channels() >= 8) ? 3 : 2;
}

surround_mode_t surround_sanitize(int v) {
    if (v < SURROUND_OFF || v > SURROUND_HD_71) return SURROUND_OFF;
    // AC-3 is retired: 5.1 makes the same request when a source has no HD
    // track, and copies the HD one when there is.
    if (v == SURROUND_AC3) return SURROUND_HD;
    // 7.1 asked for on a chain that caps at 6 would be a setting the hardware
    // cannot honour, so it lands on 5.1 rather than failing quietly.
    if (v == SURROUND_HD_71 && surround_order_count() < 3) return SURROUND_HD;
    return (surround_mode_t)v;
}

void surround_set_mode(surround_mode_t m) {
    s_mode = surround_sanitize((int)m);
    s_hd_session_off = false;   // an explicit choice clears the session veto
    surround_save();
}

void surround_cycle(void) {
    const int n = surround_order_count();
    int i = 0;
    for (int k = 0; k < n; k++)
        if (SURROUND_ORDER[k] == s_mode) { i = k; break; }
    surround_set_mode(SURROUND_ORDER[(i + 1) % n]);
}

const char *surround_mode_label(void) {
    switch (s_mode) {
    case SURROUND_HD:    return "5.1";
    case SURROUND_HD_71: return "7.1";
    case SURROUND_AC3:   return "5.1";   // retired; sanitised on load
    default:             return "Stereo";
    }
}

// Both surround modes take the copy path -- 7.1 differs only in how wide an
// output it asks the console for, not in which track it wants.
bool surround_hd_preferred(void) {
    return (s_mode == SURROUND_HD || s_mode == SURROUND_HD_71) &&
           !s_hd_session_off;
}

void surround_hd_session_disable(void) { s_hd_session_off = true;  }
void surround_hd_session_reset(void)   { s_hd_session_off = false; }

// Missing file => off (the safe default that matches the stereo ship path).
// The file predates the DTS mode and then held only "0"/"1", which still mean
// exactly what they meant; anything outside 0..2 is treated as off.
void surround_load(void) {
    FILE *f = fopen(jf_data_path(SURROUND_FILE), "r");
    if (!f) return;
    int v = 0;
    if (fscanf(f, "%d", &v) == 1) s_mode = surround_sanitize(v);
    fclose(f);
}

void surround_save(void) {
    FILE *f = fopen(jf_data_path(SURROUND_FILE), "w");
    if (!f) return;
    fprintf(f, "%d\n", (int)s_mode);
    fclose(f);
}
