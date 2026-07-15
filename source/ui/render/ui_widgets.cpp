// Chrome widgets — top bar (brand + clock), tab bar, divider, alphabetical
// jump bar, controller hints bar, empty states, breadcrumbs.

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

#include "ui.h"
#include "ui_visuals.h"
#include "ui_wave.h"
#include "icons.h"
#include "stb_image.h"
#include "ps_buttons_png.h"
#include "plog.h"

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
    const int icon_cy     = XMB_TOPBAR_H + UIS_H(30);   // icon centerline

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
            drawTTF((u32)(cx - lw / 2), (u32)(XMB_TOPBAR_H + UIS_H(52)),
                    g_tabs[t].label, 14, XMB_TEXT);
            drawRect((u32)(cx - 13), (u32)(XMB_TOPBAR_H + UIS_H(74)), 26, 3,
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

// -------------------------------------------------------
// PS button sprites (Kenney "PlayStation Series" sheet)
// -------------------------------------------------------
// The sheet is a 12x12 grid of 128px tiles embedded as a PNG in
// ps_buttons_png.h.  On first use it's decoded with stb_image, the tiles
// we need are trimmed to their alpha bounding box and kept as small ARGB
// masters, and the 9MB decode buffer is freed.  Drawing scales a master
// to the requested height with an area-average filter — cheap at hint
// sizes and clean at any scale, so no per-size caching is needed.

#define PS_SHEET_DIM  1536
#define PS_TILE       128

// Hint glyph -> sheet tile.  All plain white solid variants — no coloured
// face buttons, no red d-pad highlights.
static const struct { char glyph; int row, col; } PS_TILES[] = {
    { 'X', 10, 5 },   // Cross (white solid)
    { 'C', 11, 7 },   // Circle (white solid)
    { 'S', 10, 11 },  // Square (white solid)
    { 'T',  9, 1 },   // Triangle (white solid)
    { 'A',  5, 8 },   // START
    { 'B',  5, 6 },   // SELECT
    { 'D',  9, 3 },   // D-pad, plain white (Switch)
    { 'E',  9, 3 },   // D-pad, plain white (Jump)
    { 'L',  6, 7 },   // L2
    { 'R',  5, 3 },   // R2
};
#define PS_NTILES ((int)(sizeof PS_TILES / sizeof PS_TILES[0]))

static u32 *s_ps_px[PS_NTILES];             // trimmed ARGB masters
static int  s_ps_w[PS_NTILES], s_ps_h[PS_NTILES];
static int  s_ps_state = 0;                 // 0=not loaded, 1=ok, -1=failed

static int ps_tile_idx(char glyph) {
    for (int i = 0; i < PS_NTILES; i++)
        if (PS_TILES[i].glyph == glyph) return i;
    return -1;
}

static void ps_sprites_load(void) {
    if (s_ps_state) return;
    s_ps_state = -1;
    int w, h, comp;
    unsigned char *img = stbi_load_from_memory(
        ps_buttons_png, (int)ps_buttons_png_len, &w, &h, &comp, 4);
    if (!img) return;
    if (w != PS_SHEET_DIM || h != PS_SHEET_DIM) { stbi_image_free(img); return; }

    for (int i = 0; i < PS_NTILES; i++) {
        int tx = PS_TILES[i].col * PS_TILE, ty = PS_TILES[i].row * PS_TILE;
        // Alpha bounding box inside the tile.
        int x0 = PS_TILE, y0 = PS_TILE, x1 = -1, y1 = -1;
        for (int y = 0; y < PS_TILE; y++) {
            const unsigned char *p = img + (((ty + y) * w + tx) * 4);
            for (int x = 0; x < PS_TILE; x++) {
                if (p[x * 4 + 3]) {
                    if (x < x0) x0 = x;
                    if (x > x1) x1 = x;
                    if (y < y0) y0 = y;
                    if (y > y1) y1 = y;
                }
            }
        }
        if (x1 < 0) continue;               // empty tile — leave master NULL
        int mw = x1 - x0 + 1, mh = y1 - y0 + 1;
        u32 *m = (u32 *)malloc((size_t)mw * mh * 4);
        if (!m) continue;
        for (int y = 0; y < mh; y++) {
            const unsigned char *p = img + (((ty + y0 + y) * w + tx + x0) * 4);
            for (int x = 0; x < mw; x++)
                m[y * mw + x] = ((u32)p[x*4+3] << 24) | ((u32)p[x*4] << 16) |
                                ((u32)p[x*4+1] << 8) | p[x*4+2];
        }
        s_ps_px[i] = m; s_ps_w[i] = mw; s_ps_h[i] = mh;
    }
    stbi_image_free(img);
    s_ps_state = 1;
}

void ps_sprites_preload(void) {
    ps_sprites_load();
    plog(s_ps_state == 1 ? "ps_sprites: sheet ok" : "ps_sprites: sheet FAILED");
}

int ps_btn_width(char glyph, int h) {
    ps_sprites_load();
    int i = ps_tile_idx(glyph);
    if (s_ps_state != 1 || i < 0 || !s_ps_px[i] || h <= 0) return h;
    return s_ps_w[i] * h / s_ps_h[i];
}

// Blit one button sprite scaled to height h, vertically centred on cy.
// bright scales the sprite's RGB (255 = as-authored) so the HUD can dim
// unfocused controls the way ctrl_color() dims its vector glyphs.
void draw_ps_button_vcentered(u32 x, int cy, char glyph, int h, u32 bright) {
    ps_sprites_load();
    int i = ps_tile_idx(glyph);
    if (s_ps_state != 1 || i < 0 || !s_ps_px[i] || h <= 0) return;
    const u32 *m = s_ps_px[i];
    int mw = s_ps_w[i], mh = s_ps_h[i];
    int dw = mw * h / mh, dh = h;
    if (dw <= 0) return;
    int dx0 = (int)x, dy0 = cy - dh / 2;
    bool rt  = cpu_rt_on();
    u32  tw_ = cpu_draw_w();
    for (int oy = 0; oy < dh; oy++) {
        int sy = dy0 + oy;
        if (cpu_row_clipped(sy)) continue;
        u32 *row = cpu_draw_row((u32)sy);
        int my0 = oy * mh / dh, my1 = (oy + 1) * mh / dh;
        if (my1 <= my0) my1 = my0 + 1;
        for (int ox = 0; ox < dw; ox++) {
            int sx = dx0 + ox;
            if (sx < 0 || (u32)sx >= tw_) continue;
            int mx0 = ox * mw / dw, mx1 = (ox + 1) * mw / dw;
            if (mx1 <= mx0) mx1 = mx0 + 1;
            // Area-average the source box (alpha-weighted colour).
            u32 ar = 0, ag = 0, ab = 0, aa = 0, np = 0;
            for (int my = my0; my < my1; my++) {
                const u32 *src = m + my * mw;
                for (int mx = mx0; mx < mx1; mx++) {
                    u32 s = src[mx], a = s >> 24;
                    ar += ((s >> 16) & 0xFF) * a;
                    ag += ((s >>  8) & 0xFF) * a;
                    ab += ( s        & 0xFF) * a;
                    aa += a; np++;
                }
            }
            if (!aa) continue;
            u32 a = aa / np;
            u32 r = (ar / aa) * bright / 255;
            u32 g = (ag / aa) * bright / 255;
            u32 b = (ab / aa) * bright / 255;
            u32 c = (r << 16) | (g << 8) | b;
            if (rt) { row[sx] = argb_over(row[sx], c, a); continue; }
            if (a == 255) { row[sx] = c; continue; }
            u32 bg = row[sx];
            u32 ro = (a * r + (255 - a) * ((bg >> 16) & 0xFF)) / 255;
            u32 go = (a * g + (255 - a) * ((bg >>  8) & 0xFF)) / 255;
            u32 bo = (a * b + (255 - a) * ( bg        & 0xFF)) / 255;
            row[sx] = (ro << 16) | (go << 8) | bo;
        }
    }
}

// -------------------------------------------------------
// Controller-hints bar — sprite buttons + labels, bottom-right
// -------------------------------------------------------

void draw_hints_bar(const Hint *hints, int n) {
    if (n <= 0) return;
    ps_sprites_load();
    if (s_ps_state != 1) return;

    const int   icon_h  = UIS_H(24);
    const float text_px = (float)UIS_H(15);
    const int   gap_it  = UIS_W(8);    // icon to its label
    const int   gap_sep = UIS_W(26);   // between hint pairs

    int total_w = 0;
    for (int i = 0; i < n; i++) {
        total_w += ps_btn_width(hints[i].glyph, icon_h);
        total_w += gap_it;
        total_w += ttf_text_width(hints[i].label, text_px);
        if (i < n - 1) total_w += gap_sep;
    }

    int x = (int)display_width - XMB_ITEM_PAD - total_w;
    if (x < (int)XMB_ITEM_PAD) x = (int)XMB_ITEM_PAD;
    int cy = (int)display_height - XMB_BOTTOM_PAD + UIS_H(34);
    if (cy < 0 || (u32)cy >= display_height) return;

    for (int i = 0; i < n; i++) {
        draw_ps_button_vcentered((u32)x, cy, hints[i].glyph, icon_h, 255);
        x += ps_btn_width(hints[i].glyph, icon_h) + gap_it;
        drawTTF((u32)x, (u32)(cy - (int)(text_px * 0.55f)), hints[i].label,
                text_px, XMB_TEXT_DIM);
        x += ttf_text_width(hints[i].label, text_px);
        if (i < n - 1) x += gap_sep;
    }
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
