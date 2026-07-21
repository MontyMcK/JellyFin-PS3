// CRT overscan inset store — see overscan.h.

#include "overscan.h"
#include "jf_paths.h"     // jf_data_path()
#include "rsxutil.h"      // display_width / display_height
#include <stdio.h>

#define OVERSCAN_FILE "jellyfin_overscan.txt"

static float s_frac = 0.0f;

float overscan_frac(void) { return s_frac; }

void overscan_set_frac(float f) {
    if (f < 0.0f)              f = 0.0f;
    if (f > OVERSCAN_MAX_FRAC) f = OVERSCAN_MAX_FRAC;
    s_frac = f;
}

// Pixel inset per edge at the current display resolution.  display_width/height
// are fixed at init, so these are cheap to recompute per call.
int overscan_x(void) { return (int)(s_frac * (float)display_width  + 0.5f); }
int overscan_y(void) { return (int)(s_frac * (float)display_height + 0.5f); }

// Persisted as integer permille (e.g. 35 = 3.5%) in the app data dir, next to
// the login/settings files.  Missing file => 0 (no inset), the safe default.
void overscan_load(void) {
    FILE *f = fopen(jf_data_path(OVERSCAN_FILE), "r");
    if (!f) return;
    int permille = 0;
    if (fscanf(f, "%d", &permille) == 1)
        overscan_set_frac((float)permille / 1000.0f);
    fclose(f);
}

void overscan_save(void) {
    FILE *f = fopen(jf_data_path(OVERSCAN_FILE), "w");
    if (!f) return;
    fprintf(f, "%d\n", (int)(s_frac * 1000.0f + 0.5f));
    fclose(f);
}
