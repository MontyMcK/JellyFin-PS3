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

// Icon per tab KIND, not per tab index — several tabs can share a kind now
// that every library gets its own.  A library with an unrecognised
// CollectionType falls back to the generic collections glyph.
static int tab_icon(int tab) {
    switch (xmb_kind(tab)) {
    case TABKIND_SEARCH:   return ICON_SEARCH;
    case TABKIND_HOME:     return ICON_HOME;
    case TABKIND_MOVIES:   return ICON_MOVIE;
    case TABKIND_TV:       return ICON_TV;
    case TABKIND_MUSIC:    return ICON_MUSIC;
    case TABKIND_PLAYLISTS:return ICON_MUSIC;
    case TABKIND_BOXSETS:  return ICON_COLLECTIONS;
    case TABKIND_SETTINGS: return ICON_SETTINGS;
    // Every custom / untyped library shares ONE icon, so they read as a
    // family rather than masquerading as a Collections library.
    //
    // ICON_PHOTO is the only sensible glyph left: the bundled Tabler font is
    // a 20-glyph SUBSET (ui/fonts/tabler_icons.h) and all 20 are already
    // spoken for, so nothing is truly free.  This one at least never appears
    // in the tab bar — its only other use is a list placeholder in
    // ui_lists.cpp — so it collides with nothing the user sees up here.
    // A dedicated folder glyph means regenerating the subset (fontTools on
    // tabler-icons.ttf with the ICON_* codepoints, per that file's header).
    default:               return ICON_PHOTO;
    }
}

// -------------------------------------------------------
// Top bar: brand on the left, clock on the right (XMB style)
// -------------------------------------------------------

// Top bar: lockup left, clock right, both on the same 24px row at y=20
// (handoff section 8's Phase 2 block).  Scales with UIS_T so it stays
// proportional inside the chrome band above the tab strip.
//
// NOT YET the design's lockup.  Section 3.1 asks for a 24x24 mark with a 16px
// wordmark in the display face at x=71, and section 3.0 names that face as
// Microgramma Bold Extended.  Neither the mark nor the three handoff typefaces
// are embedded in this build yet, so this keeps the text lockup and only fixes
// its geometry and scale.  The "PS3" tag stays until the mark replaces it --
// the revision removed it because the new mark carries the PS3 icon itself.
void xmb_draw_topbar(void) {
    const int oy = XMB_OY;   // shift the top bar down out of the CRT overscan
    const int row_y = oy + UIS_H(20);            // the shared 24px row

    // Brand lockup.  v1.0 shrank it: the mark 24 -> 21px, the wordmark 16 ->
    // 14px, the gap 7 -> 6.  The "PS3" tag stays until the mark asset replaces
    // it -- the design removed it because the new mark carries PS3 itself.
    const float brand_px = UIS_TF(21.0f);
    const float tag_px   = UIS_TF(12.0f);
    drawTTF(XMB_ITEM_PAD, (u32)row_y, "Jellyfin", brand_px, XMB_TEXT, true);
    drawTTF(XMB_ITEM_PAD + ttf_text_width("Jellyfin", brand_px, true) + UIS_H(6),
            (u32)(row_y + UIS_H(7)), "PS3", tag_px, XMB_ACCENT, true);

    // Clock, right-aligned, with the date dimmer beside it.
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    if (!tm) return;
    char t_str[8], d_str[8];
    snprintf(t_str, sizeof(t_str), "%d:%02d", tm->tm_hour, tm->tm_min);
    snprintf(d_str, sizeof(d_str), "%d/%d", tm->tm_mday, tm->tm_mon + 1);
    // v1.0 moved BOTH of these onto --font-tab (Satoshi).  The clock used to be
    // one of two things the display face was allowed; it is not any more, which
    // matters now that the display face is a subset with no colon -- a clock on
    // it would have rendered "1334" and nobody would have known why.
    const float time_px = UIS_TF(16.0f);         // Satoshi 700
    const float date_px = UIS_TF(11.0f);         // Satoshi 500
    int tw = ttf_text_width_face(t_str, time_px, UI_FACE_TAB);
    int dw = ttf_text_width_face(d_str, date_px, UI_FACE_TAB_REG);
    int tx = (int)display_width - (int)XMB_ITEM_PAD - tw;
    drawTTF_face((u32)tx, (u32)(row_y + UIS_H(2)), t_str, time_px,
                 XMB_TEXT, UI_FACE_TAB);
    drawTTF_face((u32)(tx - dw - UIS_H(10)), (u32)(row_y + UIS_H(7)),
                 d_str, date_px, XMB_TEXT_FAINT, UI_FACE_TAB_REG);
}


// Section eyebrow — the small uppercase label above a row.
//
// v1.0 gives these their own token, `--font-eyebrow`: Microgramma at 11px,
// weight 400, UPPERCASE, 0.18em tracking.  In the canvas they are "Continue
// watching", "Recently added · Movies", "Search library", "Results", "Up next";
// this client has a few more of its own ("Cast & Crew", "More Like This") and
// they get the same treatment, because the design's rule is about what the
// label IS, not which screen it happens to be on.
//
// Microgramma carried the DISPLAY role before v1.0 and was already embedded, so
// the new role costs nothing.  Uppercasing is ASCII-only (ui_upper_ascii) — the
// text can be a server row title.
//
// Returns the advance, so a caller can put a count or a chevron after it.
int xmb_draw_eyebrow(int x, int y, const char *text, u32 color)
{
    char up[96];
    snprintf(up, sizeof up, "%s", text);
    ui_upper_ascii(up);
    const float px    = UIS_TF(11.0f);
    const float track = px * 0.18f;
    drawTTF_tracked((u32)x, (u32)y, up, px, color, UI_FACE_EYEBROW, track);
    return ttf_text_width_tracked(up, px, UI_FACE_EYEBROW, track);
}

int xmb_eyebrow_width(const char *text)
{
    char up[96];
    snprintf(up, sizeof up, "%s", text);
    ui_upper_ascii(up);
    const float px = UIS_TF(11.0f);
    return ttf_text_width_tracked(up, px, UI_FACE_EYEBROW, px * 0.18f);
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

// Tab strip geometry is MEASURED, from handoff section 3.1 and the Phase 2
// block at the end of section 8.  The handoff states outright that those
// numbers win over any revision note, because they were read off the rendered
// JFChrome frame rather than projected:
//
//   7 items, width 70, gap 40, stride 110, row starts x=275
//   icon 26x26 at y=61 · label y=96 (active only) · underline 2px y=110
//   divider y=144
//
// The "nav band 80->68px" note is NOT a stride and was never one: it describes
// the flex container holding the row. The row still starts at y=61 and the
// divider still sits at y=144, so XMB_TABBAR_H stays as it was and nothing
// below the chrome moves.
//
// The numbers are authored at 1280x720 and go through UIS_T, which scales both
// ways -- see ui_visuals.h.  At 1080p that is 1.5x, which is what keeps the
// strip proportional and consistent with the hints bar.  The vertical anchors
// stay RELATIVE to XMB_TOPBAR_H so the strip cannot drift away from the bar
// above it if that ever changes.
#define TAB_ICON_PX     26   // uniform; the active tab reads by colour, not size
#define TAB_ITEM_W      70
#define TAB_GAP         40
#define TAB_STRIDE      (TAB_ITEM_W + TAB_GAP)   // 110
#define TAB_ICON_Y      61   // from the top of the frame, 720p
#define TAB_LABEL_Y     96
#define TAB_RULE_Y     110
#define TAB_RULE_H       2

void xmb_draw_tabs(void) {
    const int oy = XMB_OY;                       // overscan shift

    // Display order (Search, Home, libraries, Settings) — NOT array order,
    // since Settings keeps a low index but renders last.
    int enabled[XMB_TAB_COUNT];
    int n = xmb_tab_order(enabled);
    if (n == 0) return;

    // Stride adapts, because the tab count follows the server's library count
    // rather than the design's fixed 7.  The measured 110 is kept whenever the
    // row fits; past that it shrinks to whatever divides the usable width,
    // floored so icons never overlap.
    const int avail = (int)display_width - 2 * XMB_ITEM_PAD;
    int spacing = UIS_H(TAB_STRIDE);
    if (n > 1 && (n - 1) * spacing > avail) {
        spacing = avail / (n - 1);
        const int floor_px = UIS_H(TAB_ICON_PX) + UIS_H(8);
        if (spacing < floor_px) spacing = floor_px;
    }

    const int group_w      = (n - 1) * spacing;
    const int tab_group_x0 = (int)display_width / 2 - group_w / 2;

    const int icon_px = UIS_H(TAB_ICON_PX);
    const int icon_y  = oy + UIS_H(TAB_ICON_Y);
    const int label_y = oy + UIS_H(TAB_LABEL_Y);
    const int rule_y  = oy + UIS_H(TAB_RULE_Y);

    // Uniform icon row: the active tab reads by COLOUR plus its label and the
    // accent underline, never by growing.  Section 3.0: "Focused items change
    // scale, ring and brightness — never weight", and the strip's own table
    // gives one icon size for every state.
    for (int i = 0; i < n; i++) {
        int  t      = enabled[i];
        int  cx     = tab_group_x0 + i * spacing;
        bool active = (t == g_active_tab);

        drawIcon((u32)(cx - icon_px / 2), (u32)icon_y, tab_icon(t),
                 (float)icon_px, active ? XMB_WHITE : XMB_ICON_IDLE);

        if (active) {
            // v1.0: Satoshi Bold 11.5px, UPPERCASE, 0.04em tracking.
            //
            // Labels are server library names, so they can be long and the
            // active tab can sit at either end of the row.  Centre under the
            // icon, then clamp into the safe area so a wide name is never
            // pushed off-screen -- measured WITH the tracking, or the clamp
            // would be computed against a narrower string than gets drawn.
            const float px    = UIS_TF(11.5f);
            const float track = px * 0.04f;      // 0.04em
            char label[sizeof(g_tabs[t].label)];
            snprintf(label, sizeof label, "%s", g_tabs[t].label);
            ui_upper_ascii(label);

            int lw = ttf_text_width_tracked(label, px, UI_FACE_TAB, track);
            int lx = cx - lw / 2;
            int lo = XMB_ITEM_PAD;
            int hi = (int)display_width - XMB_ITEM_PAD - lw;
            if (lx < lo) lx = lo;
            if (hi >= lo && lx > hi) lx = hi;
            drawTTF_tracked((u32)lx, (u32)label_y, label, px, XMB_TEXT,
                            UI_FACE_TAB, track);
            const int rw = UIS_H(TAB_ICON_PX);   // underline matches the icon
            drawRect((u32)(cx - rw / 2), (u32)rule_y,
                     (u32)rw, (u32)UIS_H(TAB_RULE_H), XMB_ACCENT);
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
                + (xmb_kind(tab) == TABKIND_MUSIC ? XMB_MUSIC_SUBTAB_H : 0);
    int bar_bot = bar_top + XMB_GRID_ROWS * gg.stride - XMB_CARD_TEXT_H;
    int bar_h   = bar_bot - bar_top;
    int jbar_x  = gg.x0 - JBAR_GAP * 3 - JBAR_W;
    if (jbar_x < 0) jbar_x = 0;
    // Step height evenly divides the bar; the font fills each slot (1.2x gives
    // glyph ascender room without adjacent letters visually overlapping on a TV
    // at viewing distance).
    float entry_h = (float)bar_h / (float)JBAR_ENTRIES;
    float font_px = entry_h * 1.2f;
    if (font_px < UIS_TF(12)) font_px = UIS_TF(12);
    if (font_px > UIS_TF(28)) font_px = UIS_TF(28);

    // THE COLUMN IS THE HARD CONSTRAINT, and it wins over both clamps above.
    //
    // The size above comes from the GRID's height, but the letters are drawn
    // into a JBAR_W-wide strip, and nothing tied the two together: when the
    // chrome band was rescaled the grid moved, font_px went to its cap, and the
    // A-Z strip drew far too large in a column that had not changed at all.
    //
    // Measured rather than guessed at a ratio -- "W" is the widest label, the
    // advance scales linearly with px so one division is exact, and the system
    // face is Rodin now, whose proportions are not the ones the old 1.2x was
    // eyeballed against.
    float wide = (float)ttf_text_width("W", font_px, false);
    if (wide > (float)JBAR_W) font_px *= (float)JBAR_W / wide;

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
                  : XMB_HAIRLINE;
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
    const float px = UIS_TF(16.0f);
    for (int i = 0; i < MUSIC_ST_COUNT; i++) {
        bool is_active = (i == active);
        u32 color = is_active ? (focused ? XMB_WHITE : XMB_TEXT)
                              : XMB_TEXT_FAINT;
        drawTTF((u32)x, (u32)y, labels[i], px, color, is_active);
        int w = ttf_text_width(labels[i], px, is_active);
        if (is_active)
            drawRect((u32)x, (u32)(y + UIS_H(24)), (u32)w, UIS_H(3),
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

// Shoulder-button badge — handoff section 3.1: "26x22 rounded rects (radius 6,
// 1.5px text_dim border, mono 10.5 label)".
//
// Outline only, drawn as four OPAQUE rects.  A real radius-6 corner needs
// coverage blending, and blended CPU rects read video memory at ~700ns/pixel
// (see UI-BRIEF.md) -- one hint bar of them would cost more than the entire
// frame budget.  Instead each edge is inset by the corner radius so the four
// strokes stop short of meeting, which reads as a rounded box at TV distance
// and costs nothing but writes.  bpx stays 0.
#define HINT_BADGE_W    26
#define HINT_BADGE_H    22
#define HINT_BADGE_R     3   // corner inset, px at 720p (visual radius ~6)

// The whole bar scales with UIS_T rather than UIS_W/UIS_H: it is a text-led
// cluster, and at 1080p the pass-through forms left the badges and their labels
// at two-thirds the handoff's intended size.  Scaling only the type would leave
// 19px labels inside a 26px box, so icons, badges, gaps and text all move
// together and the bar keeps the proportions section 3.1 draws.
static int hint_badge_w(void) { return UIS_H(HINT_BADGE_W); }

static void draw_hint_badge(int x, int cy, const char *label) {
    const int w = UIS_H(HINT_BADGE_W);
    const int h = UIS_H(HINT_BADGE_H);
    const int t = UIS_H(2) < 1 ? 1 : UIS_H(2);   // 1.5px spec, on the pixel grid
    const int r = UIS_H(HINT_BADGE_R);
    const int y = cy - h / 2;

    if (x < 0 || y < 0 || (u32)(x + w) > display_width ||
        (u32)(y + h) > display_height) return;

    const u32 c = XMB_TEXT_DIM;
    drawRect((u32)(x + r),         (u32)y,               (u32)(w - 2 * r), (u32)t, c);
    drawRect((u32)(x + r),         (u32)(y + h - t),     (u32)(w - 2 * r), (u32)t, c);
    drawRect((u32)x,               (u32)(y + r),         (u32)t, (u32)(h - 2 * r), c);
    drawRect((u32)(x + w - t),     (u32)(y + r),         (u32)t, (u32)(h - 2 * r), c);

    // Centred label.  The design asks for a mono face; this build has none
    // (UI_FACE_REGULAR/BOLD/NOTO/ROBOCOND), so regular stands in at the spec
    // size -- "L1"/"R1" are two glyphs and do not need the alignment a mono
    // face buys for the date and the tech strip.
    const float px = UIS_TF(10.5f);      // section 3.1: mono 10.5 badge label
    const int   tw = ttf_text_width(label, px);
    drawTTF((u32)(x + (w - tw) / 2), (u32)(cy - (int)(px * 0.55f)), label, px, c);
}

// A hint is a shoulder badge when its glyph is lowercase 'l'/'r' (L1/R1).
// Uppercase 'L'/'R' stay the L2/R2 sprites the sheet already carries.
static bool hint_is_shoulder(char g) { return g == 'l' || g == 'r'; }

void draw_hints_bar(const Hint *hints, int n) {
    if (n <= 0) return;
    ps_sprites_load();
    if (s_ps_state != 1) return;

    const int   icon_h  = UIS_H(24);
    const float text_px = UIS_TF(12.5f);      // section 3.1: 12.5px label
    const int   gap_it  = UIS_H(8);           // icon to its label
    const int   gap_sep = UIS_H(22);          // section 3.1: items gap 22

    // An EMPTY label pairs a hint with the one after it, so "L1/R1 Tab" is a
    // single cluster ({'l',""},{'r',"Tab"}) rather than two hints with a full
    // separator between them.
    const int gap_pair = UIS_H(4);

    int total_w = 0;
    for (int i = 0; i < n; i++) {
        total_w += hint_is_shoulder(hints[i].glyph)
                     ? hint_badge_w()
                     : ps_btn_width(hints[i].glyph, icon_h);
        if (hints[i].label && hints[i].label[0]) {
            total_w += gap_it + ttf_text_width(hints[i].label, text_px);
            if (i < n - 1) total_w += gap_sep;
        } else {
            total_w += gap_pair;
        }
    }

    int x = (int)display_width - XMB_ITEM_PAD - total_w;
    if (x < (int)XMB_ITEM_PAD) x = (int)XMB_ITEM_PAD;

    // Section 3.1 puts the hint baseline at y=698 of the 720-tall canvas.  The
    // overscan inset stays folded in on top of that, so a calibrated CRT still
    // lifts the whole bar clear of the bezel.
    int cy = (int)display_height - XMB_OY - UIS_H(22);
    if (cy < 0 || (u32)cy >= display_height) return;

    for (int i = 0; i < n; i++) {
        if (hint_is_shoulder(hints[i].glyph)) {
            draw_hint_badge(x, cy, hints[i].glyph == 'l' ? "L1" : "R1");
            x += hint_badge_w();
        } else {
            draw_ps_button_vcentered((u32)x, cy, hints[i].glyph, icon_h, 255);
            x += ps_btn_width(hints[i].glyph, icon_h);
        }
        if (hints[i].label && hints[i].label[0]) {
            x += gap_it;
            drawTTF((u32)x, (u32)(cy - (int)(text_px * 0.55f)), hints[i].label,
                    text_px, XMB_TEXT_DIM);
            x += ttf_text_width(hints[i].label, text_px);
            if (i < n - 1) x += gap_sep;
        } else {
            x += gap_pair;
        }
    }
}

// -------------------------------------------------------
// Empty state — tab icon, dimmed, centered above one line of text
// -------------------------------------------------------

void xmb_draw_empty_state(int tab, const char *msg) {
    int cx = (int)display_width / 2;
    int cy = (XMB_CONTENT_Y + (int)display_height - XMB_BOTTOM_PAD) / 2 - UIS_H(30);
    const float icon_px = UIS_TF(48.0f);
    drawIcon((u32)(cx - (int)icon_px / 2), (u32)(cy - (int)icon_px),
             tab_icon(tab), icon_px, XMB_HAIRLINE);
    int tw = ttf_text_width(msg, UIS_TF(17));
    drawTTF((u32)(cx - tw / 2), (u32)(cy + UIS_H(8)), msg, UIS_TF(17), XMB_TEXT_FAINT);
}

// -------------------------------------------------------
// Breadcrumb — dim parents, chevron separators, bright leaf
// -------------------------------------------------------

void xmb_draw_breadcrumb(int x, int y, const char *a, const char *b,
                         const char *leaf) {
    const float px = UIS_TF(15.0f);
    const char *parts[3] = { a, b, leaf };
    for (int i = 0; i < 3; i++) {
        if (!parts[i]) continue;
        bool is_leaf = (i == 2) || (i == 1 && !parts[2]) || (i == 0 && !parts[1] && !parts[2]);
        drawTTF((u32)x, (u32)y, parts[i], px,
                is_leaf ? XMB_TEXT : XMB_TEXT_FAINT);
        x += ttf_text_width(parts[i], px);
        // Chevron between segments.
        bool more = (i < 2) && parts[i + 1];
        if (more) {
            drawIcon((u32)(x + UIS_W(4)), (u32)(y - 1), ICON_CHEVRON_RIGHT, UIS_TF(16.0f),
                     XMB_TEXT_FAINT);
            x += UIS_W(24);
        }
    }
}
