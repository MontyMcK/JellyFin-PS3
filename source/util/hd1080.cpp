// 1080p playback (Alpha) toggle store — see hd1080.h.

#include "hd1080.h"
#include "jf_paths.h"     // jf_data_path()
#include <stdio.h>

#define HD1080_FILE "jellyfin_1080p.txt"

static bool s_enabled = false;

bool hd1080_enabled(void) { return s_enabled; }

void hd1080_set_enabled(bool on) {
    s_enabled = on;
    hd1080_save();
}

// Missing file => disabled (the safe default that matches the 720p ship path).
void hd1080_load(void) {
    FILE *f = fopen(jf_data_path(HD1080_FILE), "r");
    if (!f) return;
    int v = 0;
    if (fscanf(f, "%d", &v) == 1) s_enabled = (v != 0);
    fclose(f);
}

void hd1080_save(void) {
    FILE *f = fopen(jf_data_path(HD1080_FILE), "w");
    if (!f) return;
    fprintf(f, "%d\n", s_enabled ? 1 : 0);
    fclose(f);
}
