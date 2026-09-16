// Surround 5.1 (Alpha) setting store — see surround.h.

#include "surround.h"
#include "jf_paths.h"     // jf_data_path()
#include <stdio.h>

#define SURROUND_FILE "jellyfin_surround.txt"

static surround_mode_t s_mode = SURROUND_OFF;
// Session-scoped veto on the copy path (not persisted): set when the copied
// track turns out to be undecodable here (a coreless DTS-HD MA track), and
// cleared when a new title starts.
static bool s_hd_session_off = false;

surround_mode_t surround_get_mode(void) { return s_mode; }

void surround_set_mode(surround_mode_t m) {
    if (m < SURROUND_OFF || m > SURROUND_HD) m = SURROUND_OFF;
    s_mode = m;
    s_hd_session_off = false;   // an explicit choice clears the session veto
    surround_save();
}

void surround_cycle(void) {
    switch (s_mode) {
    case SURROUND_OFF: surround_set_mode(SURROUND_AC3); break;
    case SURROUND_AC3: surround_set_mode(SURROUND_HD); break;
    default:           surround_set_mode(SURROUND_OFF); break;
    }
}

const char *surround_mode_label(void) {
    switch (s_mode) {
    case SURROUND_AC3: return "AC-3";
    case SURROUND_HD:  return "HD";
    default:           return "Off";
    }
}

bool surround_hd_preferred(void) {
    return s_mode == SURROUND_HD && !s_hd_session_off;
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
    if (fscanf(f, "%d", &v) == 1) {
        s_mode = (v == 1) ? SURROUND_AC3
               : (v == 2) ? SURROUND_HD
                          : SURROUND_OFF;
    }
    fclose(f);
}

void surround_save(void) {
    FILE *f = fopen(jf_data_path(SURROUND_FILE), "w");
    if (!f) return;
    fprintf(f, "%d\n", (int)s_mode);
    fclose(f);
}
