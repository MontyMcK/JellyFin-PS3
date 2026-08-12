// Surround 5.1 (Alpha) toggle store — see surround.h.

#include "surround.h"
#include "jf_paths.h"     // jf_data_path()
#include <stdio.h>

#define SURROUND_FILE "jellyfin_surround.txt"

static bool s_enabled = false;

bool surround_enabled(void) { return s_enabled; }

void surround_set_enabled(bool on) {
    s_enabled = on;
    surround_save();
}

// Missing file => disabled (the safe default that matches the stereo ship path).
void surround_load(void) {
    FILE *f = fopen(jf_data_path(SURROUND_FILE), "r");
    if (!f) return;
    int v = 0;
    if (fscanf(f, "%d", &v) == 1) s_enabled = (v != 0);
    fclose(f);
}

void surround_save(void) {
    FILE *f = fopen(jf_data_path(SURROUND_FILE), "w");
    if (!f) return;
    fprintf(f, "%d\n", s_enabled ? 1 : 0);
    fclose(f);
}
