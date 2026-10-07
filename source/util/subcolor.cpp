// Subtitle colour — see subcolor.h.

#include "subcolor.h"
#include "jf_paths.h"
#include <stdio.h>

#define SUBCOLOR_FILE "jellyfin_subcolor.txt"

static subcolor_t s_col = SUBCOLOR_WHITE;

subcolor_t subcolor_get(void) { return s_col; }

const char *subcolor_label(void) {
    switch (s_col) {
    case SUBCOLOR_YELLOW: return "Soft Yellow";
    case SUBCOLOR_GREY:   return "Soft Grey";
    default:              return "White";
    }
}

u32 subcolor_fill(void) {
    switch (s_col) {
    // Warm, not saturated. Pure yellow (#FFFF00) is what old DVDs used and it
    // glares on a modern panel; pulling the blue up and the red down slightly
    // keeps the separation from the picture without the harshness.
    case SUBCOLOR_YELLOW: return 0xFFE79AFFUL;
    // Off-white at ~85% alpha. Not pure white, and not fully opaque, so it
    // reads as part of the frame rather than sitting on top of it.
    case SUBCOLOR_GREY:   return 0xE6E8EAD9UL;
    default:              return 0xFFFFFFFFUL;
    }
}

u32 subcolor_outline(void) {
    // Black throughout. The outline exists to separate the fill from whatever
    // is behind it, and anything other than black does that less well.
    // The translucent look gets a slightly softer outline so the edge does
    // not end up more solid than the letters it is outlining.
    return (s_col == SUBCOLOR_GREY) ? 0x000000B4UL : 0x000000E6UL;
}

int subcolor_outline_px(u32 display_height) {
    // Soft Grey is specified as a THIN outline at any size: it is the quiet
    // option, and a 2px black edge around a translucent fill is not quiet.
    if (s_col == SUBCOLOR_GREY) return 1;
    return (display_height >= 720) ? 2 : 1;
}

static void subcolor_save(void) {
    FILE *f = fopen(jf_data_path(SUBCOLOR_FILE), "w");
    if (!f) return;
    fprintf(f, "%d\n", (int)s_col);
    fclose(f);
}

void subcolor_cycle(void) {
    s_col = (subcolor_t)((s_col + 1) % SUBCOLOR_COUNT);
    subcolor_save();
}

// Missing file => White, the broadcast default.
void subcolor_load(void) {
    FILE *f = fopen(jf_data_path(SUBCOLOR_FILE), "r");
    if (!f) return;
    int v = 0;
    if (fscanf(f, "%d", &v) == 1 && v >= 0 && v < SUBCOLOR_COUNT)
        s_col = (subcolor_t)v;
    fclose(f);
}
