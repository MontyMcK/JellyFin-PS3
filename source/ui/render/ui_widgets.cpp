// Chrome widgets — top bar (brand + clock), tab bar, divider, alphabetical
// jump bar, controller hints bar, empty states, breadcrumbs.

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "ui_visuals.h"
#include "ui_wave.h"
#include "icons.h"

// -------------------------------------------------------
// Tab icon codepoints (Tabler Icons)
// -------------------------------------------------------

static const int TAB_CODEPOINTS[XMB_TAB_COUNT] = {
    ICON_SEARCH,       // XMB_TAB_SEARCH
    ICON_HOME,         // XMB_TAB_HOME
    ICON_MOVIE,        // XMB_TAB_MOVIES
    ICON_TV,           // XMB_TAB_TV
    ICON_MUSIC,        // XMB_TAB_MUSIC
    ICON_COLLECTIONS,  // XMB_TAB_COLLECTIONS
    ICON_SETTINGS,     // XMB_TAB_SETTINGS
};

// -------------------------------------------------------
// Top bar: brand on the left, clock on the right (XMB style)
// -------------------------------------------------------

void xmb_draw_topbar(void) {
    // Brand: "Jellyfin" + accent "PS3" tag on a shared baseline.
    drawTTF(XMB_ITEM_PAD, 20, "Jellyfin", 22, XMB_TEXT, true);
    drawTTF(XMB_ITEM_PAD + ttf_text_width("Jellyfin", 22, true) + 8, 27, "PS3",
            13, XMB_ACCENT, true);

    // Clock: "13/6  21:34" right-aligned, date dimmer than time.
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    if (!tm) return;
    char t_str[8], d_str[8];
    snprintf(t_str, sizeof(t_str), "%d:%02d", tm->tm_hour, tm->tm_min);
    snprintf(d_str, sizeof(d_str), "%d/%d", tm->tm_mday, tm->tm_mon + 1);
    int tw = ttf_text_width(t_str, 19);
    int dw = ttf_text_width(d_str, 13);
    int tx = (int)display_width - (int)XMB_ITEM_PAD - tw;
    drawTTF((u32)tx, 22, t_str, 19, XMB_TEXT_DIM);
    drawTTF((u32)(tx - dw - 10), 27, d_str, 13, XMB_TEXT_FAINT);
}

// Faded hairline under the tab bar — bright at the center, dissolving
// toward the edges instead of a hard full-width line.
void xmb_draw_divider(void) {
    int W = (int)display_width;
    u32 *row = color_buffer[curr_fb] + (u32)XMB_DIVIDER_Y * display_width;
    const u32 c_r = 0x8A, c_g = 0x93, c_b = 0xC8;
    for (int x = 0; x < W; x++) {
        // Triangular falloff, peak alpha ~72/255 at center.
        int d = x < W / 2 ? x : W - x;
        u32 a = (u32)(72 * d * 2 / W);
        if (a == 0) continue;
        u32 bg = row[x];
        row[x] = (((a*c_r + (255-a)*((bg>>16)&0xFF))/255) << 16) |
                 (((a*c_g + (255-a)*((bg>> 8)&0xFF))/255) <<  8) |
                  ((a*c_b + (255-a)*( bg     &0xFF))/255);
    }
}

// -------------------------------------------------------
// Tab bar
// -------------------------------------------------------

void xmb_draw_tabs(void) {
    const int TAB_SPACING = 72;
    const int icon_cy     = XMB_TOPBAR_H + 30;   // icon centerline

    // Pack only the enabled tabs and center *that* group.  Centering on the
    // full XMB_TAB_COUNT span would leave a gap (and shove the row off to the
    // left) whenever a library is missing — e.g. no Collections boxsets.
    int enabled[XMB_TAB_COUNT];
    int n = 0;
    for (int t = 0; t < XMB_TAB_COUNT; t++)
        if (g_tabs[t].enabled) enabled[n++] = t;
    if (n == 0) return;

    const int group_w      = (n - 1) * TAB_SPACING;
    const int tab_group_x0 = (int)display_width / 2 - group_w / 2;

    // Uniform icon row; the active tab is white with its label and a short
    // accent underline beneath (no size jump, no L1/R1 chips — the bumpers
    // still switch tabs, the hints bar can advertise that where relevant).
    for (int i = 0; i < n; i++) {
        int  t      = enabled[i];
        int  cx     = tab_group_x0 + i * TAB_SPACING;
        bool active = (t == g_active_tab);
        int icon_px = active ? 30 : 26;
        int icon_x  = cx - icon_px / 2;
        int icon_y  = icon_cy - icon_px / 2;

        drawIcon((u32)icon_x, (u32)icon_y, TAB_CODEPOINTS[t], (float)icon_px,
                 active ? XMB_WHITE : XMB_ICON_IDLE);

        if (active) {
            int lw = ttf_text_width(g_tabs[t].label, 14);
            drawTTF((u32)(cx - lw / 2), (u32)(XMB_TOPBAR_H + 52),
                    g_tabs[t].label, 14, XMB_TEXT);
            drawRect((u32)(cx - 13), (u32)(XMB_TOPBAR_H + 74), 26, 3,
                     XMB_ACCENT);
        }
    }
}

// Alphabetical jump bar rendered to the left of the item list.
// Always visible on library tabs at depth 0; letters are dimmed when unfocused,
// the selected entry pops white while g_jumpbar_active is true.
void xmb_draw_jumpbar(int tab) {
    GridGeom gg;
    xmb_grid_geom(tab, &gg);
    int bar_top = XMB_GRID_Y0
                + (tab == XMB_TAB_MUSIC ? XMB_MUSIC_SUBTAB_H : 0);
    int bar_bot = bar_top + XMB_GRID_ROWS * gg.stride - XMB_CARD_TEXT_H;
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
        u32 color = sel ? XMB_WHITE
                  : g_jumpbar_active ? XMB_ICON_IDLE
                  : 0x00363D63UL;
        drawTTF((u32)jbar_x, (u32)ty, jbar_labels[i], font_px, color, sel);
    }
}

// Music sub-tab header — "Albums  Artists  Playlists  Genres  Songs" over
// the grid.  The active sub-tab carries the accent underline; while the
// d-pad focuses the header its label pops white so it's obvious LEFT/RIGHT
// will switch it.
void xmb_draw_music_subtabs(int x, int y, int active, bool focused) {
    static const char *labels[MUSIC_ST_COUNT] =
        { "Albums", "Artists", "Playlists", "Genres", "Songs" };
    const float px = 16.0f;
    for (int i = 0; i < MUSIC_ST_COUNT; i++) {
        bool is_active = (i == active);
        u32 color = is_active ? (focused ? XMB_WHITE : XMB_TEXT)
                              : XMB_TEXT_FAINT;
        drawTTF((u32)x, (u32)y, labels[i], px, color, is_active);
        int w = ttf_text_width(labels[i], px, is_active);
        if (is_active)
            drawRect((u32)x, (u32)(y + 24), (u32)w, 3,
                     focused ? XMB_KEY_SEL : XMB_ACCENT);
        x += w + 34;
    }
}

// Controller-hints bar — intentionally a no-op (2026-07-12 redesign): the
// persistent button legend added chrome without earning it, and the controls
// are discoverable by use.  Call sites keep passing their Hint arrays so the
// per-screen mappings stay documented in code and the bar can come back with
// one edit here if it's ever missed.
void draw_hints_bar(const Hint *hints, int n) {
    (void)hints;
    (void)n;
}

// -------------------------------------------------------
// Empty state — tab icon, dimmed, centered above one line of text
// -------------------------------------------------------

void xmb_draw_empty_state(int tab, const char *msg) {
    int cx = (int)display_width / 2;
    int cy = (XMB_CONTENT_Y + (int)display_height - XMB_BOTTOM_PAD) / 2 - 30;
    const float icon_px = 48.0f;
    drawIcon((u32)(cx - (int)icon_px / 2), (u32)(cy - (int)icon_px),
             TAB_CODEPOINTS[tab], icon_px, 0x002E3458UL);
    int tw = ttf_text_width(msg, 17);
    drawTTF((u32)(cx - tw / 2), (u32)(cy + 8), msg, 17, XMB_TEXT_FAINT);
}

// -------------------------------------------------------
// Breadcrumb — dim parents, chevron separators, bright leaf
// -------------------------------------------------------

void xmb_draw_breadcrumb(int x, int y, const char *a, const char *b,
                         const char *leaf) {
    const float px = 15.0f;
    const char *parts[3] = { a, b, leaf };
    for (int i = 0; i < 3; i++) {
        if (!parts[i]) continue;
        bool is_leaf = (i == 2) || (i == 1 && !parts[2]) || (i == 0 && !parts[1] && !parts[2]);
        drawTTF((u32)x, (u32)y, parts[i], px,
                is_leaf ? 0x00C5CBE3UL : XMB_TEXT_FAINT);
        x += ttf_text_width(parts[i], px);
        // Chevron between segments.
        bool more = (i < 2) && parts[i + 1];
        if (more) {
            drawIcon((u32)(x + 4), (u32)(y - 1), ICON_CHEVRON_RIGHT, 16.0f,
                     XMB_TEXT_FAINT);
            x += 24;
        }
    }
}
