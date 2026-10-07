// Subtitle typeface setting — see subfont.h.

#include "subfont.h"
#include "jf_paths.h"
#include "ui_visuals.h"     // UI_FACE_*
#include <stdio.h>

#define SUBFONT_FILE "jellyfin_subfont.txt"

static subfont_t s_font = SUBFONT_OPENSANS;

subfont_t subfont_get(void) { return s_font; }

const char *subfont_label(void) {
    switch (s_font) {
    case SUBFONT_NOTO:     return "Noto Sans";
    case SUBFONT_ROBOCOND: return "Roboto Cond.";
    default:               return "Open Sans";
    }
}

int subfont_face(void) {
    switch (s_font) {
    case SUBFONT_NOTO:     return UI_FACE_NOTO;
    case SUBFONT_ROBOCOND: return UI_FACE_ROBOCOND;
    default:               return UI_FACE_BOLD;   // Open Sans Bold
    }
}

static void subfont_save(void) {
    FILE *f = fopen(jf_data_path(SUBFONT_FILE), "w");
    if (!f) return;
    fprintf(f, "%d\n", (int)s_font);
    fclose(f);
}

void subfont_cycle(void) {
    s_font = (subfont_t)((s_font + 1) % SUBFONT_COUNT);
    subfont_save();
}

// Missing file => Open Sans, which is what the UI already draws in.
void subfont_load(void) {
    FILE *f = fopen(jf_data_path(SUBFONT_FILE), "r");
    if (!f) return;
    int v = 0;
    if (fscanf(f, "%d", &v) == 1 && v >= 0 && v < SUBFONT_COUNT)
        s_font = (subfont_t)v;
    fclose(f);
}
