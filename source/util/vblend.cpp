// Temporal blend switch -- see vblend.h.

#include "vblend.h"
#include "jf_paths.h"     // jf_data_path()
#include <stdio.h>

static bool s_enabled = false;
static bool s_loaded  = false;

bool vblend_enabled(void) {
    if (!s_loaded) {
        s_loaded = true;
        FILE *f = fopen(jf_data_path("jellyfin_blend.txt"), "r");
        if (f) {
            int v = 0;
            if (fscanf(f, "%d", &v) == 1) s_enabled = (v != 0);
            fclose(f);
        }
    }
    return s_enabled;
}
