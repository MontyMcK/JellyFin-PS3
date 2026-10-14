// Auto Skip toggle store -- see autoskip.h.

#include "autoskip.h"
#include "jf_paths.h"     // jf_data_path()
#include <stdio.h>

#define AUTOSKIP_FILE "jellyfin_autoskip.txt"

static bool s_enabled = false;

bool autoskip_enabled(void) { return s_enabled; }

void autoskip_set_enabled(bool on) {
    s_enabled = on;
    FILE *f = fopen(jf_data_path(AUTOSKIP_FILE), "w");
    if (!f) return;
    fprintf(f, "%d\n", s_enabled ? 1 : 0);
    fclose(f);
}

void autoskip_load(void) {
    FILE *f = fopen(jf_data_path(AUTOSKIP_FILE), "r");
    if (!f) return;
    int v = 0;
    if (fscanf(f, "%d", &v) == 1) s_enabled = (v != 0);
    fclose(f);
}
