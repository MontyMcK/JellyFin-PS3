// Update-available popup — modal panel shown over the whole XMB screen once
// the background GitHub check (net/update_check.cpp) reports a newer release.
// The frame is drawn normally first; this dims it with the GPU quad and lays
// the panel on top, so it sits above everything (grid, thumbnails, chrome).
// Cross dismisses it for the session and hands input back untouched.

#include <stdio.h>
#include <string.h>

#include "ui_visuals.h"
#include "ui_wave.h"
#include "update_check.h"

static bool s_dismissed = false;
static bool s_have_ver  = false;
static char s_version[64];

bool xmb_update_popup_active(void) {
    if (s_dismissed) return false;
    if (!s_have_ver) {
        if (!update_check_result(s_version, sizeof(s_version))) return false;
        s_have_ver = true;
    }
    return true;
}

void xmb_update_popup_input(void) {
    if (BTN_PRESSED(cross)) {
        s_dismissed = true;
        init_btns();   // don't leak the X press into the screen underneath
    }
}

// 1px hairline outline around a rect (same as the settings card / confirm).
static void hairline_frame(int x, int y, int w, int h) {
    drawRect((u32)x, (u32)y, (u32)w, 1, XMB_HAIRLINE);
    drawRect((u32)x, (u32)(y + h - 1), (u32)w, 1, XMB_HAIRLINE);
    drawRect((u32)x, (u32)y, 1, (u32)h, XMB_HAIRLINE);
    drawRect((u32)(x + w - 1), (u32)y, 1, (u32)h, XMB_HAIRLINE);
}

void xmb_update_popup_draw(void) {
    // GPU dim over the fully drawn frame; returns fenced, so the CPU panel
    // writes below land on top of it.
    wave_dim_screen(110);

    const int mw = 480, mh = 170;
    int mx = ((int)display_width  - mw) / 2;
    int my = ((int)display_height - mh) / 2 - 20;

    drawRect((u32)mx, (u32)my, (u32)mw, (u32)mh, XMB_PANEL);
    hairline_frame(mx, my, mw, mh);

    const char *title = "New version detected";
    int tw = ttf_text_width(title, 21, true);
    drawTTF((u32)(mx + (mw - tw) / 2), (u32)(my + 28), title, 21, XMB_TEXT, true);

    // Release tag, shown without a leading v/V.
    {
        const char *v = s_version;
        if (*v == 'v' || *v == 'V') v++;
        char line[80];
        snprintf(line, sizeof(line), "Version %s", v);
        int lw = ttf_text_width(line, 14);
        drawTTF((u32)(mx + (mw - lw) / 2), (u32)(my + 64), line, 14, XMB_TEXT_DIM);
    }

    // OK button — always focused, so it uses the OSK selected-key style
    // (white key, dark label).
    {
        const int   bw = 140, bh = 36;
        const float px = 18.0f;
        int bx = mx + (mw - bw) / 2;
        int by = my + mh - bh - 22;
        drawRect((u32)bx, (u32)by, (u32)bw, (u32)bh, XMB_KEY_SEL);
        int lw = ttf_text_width("OK", px, true);
        drawTTF((u32)(bx + (bw - lw) / 2),
                (u32)(by + (bh - (int)px) / 2 - 2), "OK", px,
                XMB_KEY_LABEL_SEL, true);
    }

    static const Hint h[] = {{'X', "OK"}};
    draw_hints_bar(h, 1);
}
