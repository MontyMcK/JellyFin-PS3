// Chrome widgets — tab bar, alphabetical jump bar, controller hints bar,
// top-bar L1/R1 indicators.

#include "ui_visuals.h"

// -------------------------------------------------------
// Tab icon codepoints (Material Icons)
// -------------------------------------------------------

static const int TAB_CODEPOINTS[XMB_TAB_COUNT] = {
    0xE8B6,  // XMB_TAB_SEARCH      (search/magnifier)
    0xE889,  // XMB_TAB_RESUME      (history/clock-arrow)
    0xE54D,  // XMB_TAB_MOVIES      (movie/film)
    0xE333,  // XMB_TAB_TV          (TV)
    0xE3A1,  // XMB_TAB_MUSIC       (music note)
    0xE8EF,  // XMB_TAB_COLLECTIONS (collections/four squares)
    0xE8B8,  // XMB_TAB_SETTINGS    (settings/gear)
};

void xmb_draw_tabs(void) {
    const int TAB_SPACING  = 96;
    const int group_half_w = ((XMB_TAB_COUNT - 1) * TAB_SPACING) / 2;
    const int tab_group_x0 = (int)display_width / 2 - group_half_w;

    for (int t = 0; t < XMB_TAB_COUNT; t++) {
        if (!g_tabs[t].enabled) continue;

        int cx      = tab_group_x0 + t * TAB_SPACING;
        bool active = (t == g_active_tab);
        int icon_px = active ? 96 : 32;
        int icon_x  = cx - icon_px / 2;
        int icon_y  = XMB_TOPBAR_H + (XMB_TABBAR_H - icon_px) / 2 - 8;

        drawIcon((u32)icon_x, (u32)icon_y, TAB_CODEPOINTS[t], (float)icon_px, 0x00ae99d6);
    }
}

// Alphabetical jump bar rendered to the left of the item list.
// Always visible on library tabs at depth 0; letters are dimmed when unfocused,
// accent-coloured on the selected entry when g_jumpbar_active is true.
void xmb_draw_jumpbar(int tab) {
    GridGeom gg;
    xmb_grid_geom(tab, &gg);
    int bar_top = XMB_GRID_Y0;
    int bar_bot = XMB_GRID_Y0 + XMB_GRID_ROWS * gg.stride - XMB_CARD_TEXT_H;
    int bar_h   = bar_bot - bar_top;
    int jbar_x  = gg.x0 - JBAR_GAP * 3 - JBAR_W;
    if (jbar_x < 0) jbar_x = 0;

    // Step height evenly divides the bar; font fills each slot (1.2× gives glyph ascender
    // room without adjacent letters visually overlapping on TV at viewing distance).
    float entry_h = (float)bar_h / (float)JBAR_ENTRIES;
    float font_px = entry_h * 1.2f;
    if (font_px < 12.0f) font_px = 12.0f;
    if (font_px > 28.0f) font_px = 28.0f;

    static const char * const jbar_labels[JBAR_ENTRIES] = {
        "#","A","B","C","D","E","F","G","H","I","J","K","L","M",
        "N","O","P","Q","R","S","T","U","V","W","X","Y","Z"
    };

    for (int i = 0; i < JBAR_ENTRIES; i++) {
        int ey = bar_top + (int)(i * entry_h);
        int ty = ey + (int)((entry_h - font_px) * 0.5f);
        if (ty < 0) ty = 0;
        if ((u32)ty >= display_height) continue;
        bool sel = g_jumpbar_active && (i == g_jumpbar_sel);
        u32 color = sel ? 0x00FFFFFFUL : 0x00554477UL;
        drawTTF((u32)jbar_x, (u32)ty, jbar_labels[i], font_px, color);
    }
}

// Render an array of (glyph, label) hint pairs as a centered horizontal row
// at the bottom of the screen.  Glyphs drawn in tab-icon colour; labels in white.
void draw_hints_bar(const Hint *hints, int n) {
    if (n <= 0) return;

    const float icon_px = 33.0f;
    const float text_px = 24.0f;
    const int   gap_it  =  8;   // pixels between icon and its label
    const int   gap_sep = 28;   // pixels between hint pairs

    // Measure total width
    int total_w = 0;
    for (int i = 0; i < n; i++) {
        total_w += iconic_adv_px(hints[i].glyph, icon_px);
        total_w += gap_it;
        total_w += ttf_text_width(hints[i].label, text_px);
        if (i < n - 1) total_w += gap_sep;
    }

    int x = ((int)display_width - total_w) / 2;
    if (x < (int)XMB_ITEM_PAD) x = (int)XMB_ITEM_PAD;
    int y = (int)display_height - (int)XMB_BOTTOM_PAD + 18;
    if (y < 0 || (u32)y >= display_height) return;

    for (int i = 0; i < n; i++) {
        draw_iconic_glyph((u32)x, (u32)y, hints[i].glyph, icon_px, 0x00ae99d6);
        x += iconic_adv_px(hints[i].glyph, icon_px) + gap_it;
        drawTTF((u32)x, (u32)(y + 5), hints[i].label, text_px, 0x00FFFFFF);
        x += ttf_text_width(hints[i].label, text_px);
        if (i < n - 1) x += gap_sep;
    }
}

// Render L1 + R1 Iconic PSx glyphs right-aligned in the top bar.
void draw_topbar_lr(void) {
    const float icon_px = 40.0f;
    int w_l     = iconic_adv_px('l', icon_px);
    int w_r     = iconic_adv_px('r', icon_px);
    int total_w = w_l + 4 + w_r;
    int x = ((int)display_width - total_w) / 2;
    if (x < 0) x = 0;
    int y = (XMB_TOPBAR_H - (int)icon_px) / 2;
    if (y < 0) y = 0;
    draw_iconic_glyph((u32)x, (u32)y, 'l', icon_px, 0x00ae99d6);
    x += w_l + 4;
    draw_iconic_glyph((u32)x, (u32)y, 'r', icon_px, 0x00ae99d6);
}
