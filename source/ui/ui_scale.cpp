// UI scale store — see the UIS_W/UIS_H/UIS_TF block in ui_visuals.h for what
// the value means and why it exists.
//
// Same shape as util/overscan.cpp: one small text file in the app data dir,
// missing file means the safe default.  The default here is 0 = AUTO, i.e.
// scale with the framebuffer, so a console that has never seen this file gets
// the proportional layout.
//
// Written by hand rather than by the UI: there is no Settings row for it,
// because the point of the file is to be editable from outside a build that
// might be laying out its own Settings screen wrongly.  jellyfin_uiscale.txt
// containing "100" restores the pre-2026-09-20 look on any build.

#include "ui_scale.h"
#include "jf_paths.h"     // jf_data_path()
#include "plog.h"
#include <stdio.h>

#define UISCALE_FILE "jellyfin_uiscale.txt"

// 0 = auto.  Defined here rather than in ui_visuals.h because the inline
// accessors there are compiled into every UI translation unit.
int g_uis_pct = 0;

// Clamped hard.  A typo that halves or quadruples every anchor produces a UI
// that cannot be navigated back to a Settings screen, and the recovery path
// would be the same FTP edit that caused it -- so the range stays somewhere a
// mistake is still usable.
#define UISCALE_MIN  50
#define UISCALE_MAX 250

void ui_scale_load(void) {
    FILE *f = fopen(jf_data_path(UISCALE_FILE), "r");
    if (!f) return;
    int pct = 0;
    bool got = (fscanf(f, "%d", &pct) == 1);
    fclose(f);
    if (!got) return;

    if (pct != 0) {
        if (pct < UISCALE_MIN) pct = UISCALE_MIN;
        if (pct > UISCALE_MAX) pct = UISCALE_MAX;
    }
    g_uis_pct = pct;
    char msg[80];
    snprintf(msg, sizeof msg, "uiscale: %d%% (%s)", pct,
             pct ? "from jellyfin_uiscale.txt"
                 : "auto, tracking the framebuffer");
    plog(msg);
}

int ui_scale_pct(void) { return g_uis_pct; }
