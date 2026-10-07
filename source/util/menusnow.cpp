// Menu Particles toggle store -- see menusnow.h.

#include "menusnow.h"
#include "jf_paths.h"     // jf_data_path()
#include <stdio.h>

#define MENUSNOW_FILE "jellyfin_menusnow.txt"

static bool s_enabled = false;

static void menusnow_save(void) {
    FILE *f = fopen(jf_data_path(MENUSNOW_FILE), "w");
    if (!f) return;
    fprintf(f, "%d\n", s_enabled ? 1 : 0);
    fclose(f);
}

bool menusnow_enabled(void) { return s_enabled; }

void menusnow_set_enabled(bool on) {
    s_enabled = on;
    menusnow_save();
}

void menusnow_load(void) {
    FILE *f = fopen(jf_data_path(MENUSNOW_FILE), "r");
    if (!f) return;
    int v = 0;
    if (fscanf(f, "%d", &v) == 1) s_enabled = (v != 0);
    fclose(f);
}
