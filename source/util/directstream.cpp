// Direct Stream toggle store -- see directstream.h.

#include "directstream.h"
#include "jf_paths.h"     // jf_data_path()
#include <stdio.h>

#define DIRECTSTREAM_FILE "jellyfin_directstream.txt"

static bool s_enabled = true;
static bool s_loaded  = false;

// Loaded on first use: the player asks per request, Settings per frame, and
// neither needs a startup hook.
static void load_once(void) {
    if (s_loaded) return;
    s_loaded = true;
    FILE *f = fopen(jf_data_path(DIRECTSTREAM_FILE), "r");
    if (!f) return;
    int v = 1;
    if (fscanf(f, "%d", &v) == 1) s_enabled = (v != 0);
    fclose(f);
}

bool directstream_enabled(void) { load_once(); return s_enabled; }

void directstream_set_enabled(bool on) {
    load_once();
    s_enabled = on;
    FILE *f = fopen(jf_data_path(DIRECTSTREAM_FILE), "w");
    if (!f) return;
    fprintf(f, "%d\n", s_enabled ? 1 : 0);
    fclose(f);
}
