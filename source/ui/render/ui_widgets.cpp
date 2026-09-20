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
#include "jfmark_png.h"
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
// (handoff section 8's Phase 2 block).  Scales with the UI scale so it stays
// proportional inside the chrome band above the tab strip.
//
// This is the design's lockup now: the rasterised mark (gfx/jfmark_png.h) and
// "JELLYFIN" in Mata Bold running the theme's wordmark ramp.  v1.0's sizes --
// mark 21px, wordmark 14px, gap 6 -- and no "PS3" tag, because the mark
// carries the PS3 icon itself.
//
// Two deviations from the canvas, both deliberate:
//
//   * NO skewX(-7deg) scaleX(0.88).  Shearing would mean rasterising the word
//     into a scratch buffer and blitting it row-shifted, which is real work
//     every frame for a 1.7px lean at 14px.  Revisit it if it reads as wrong
//     on a TV beside the mark, which is where this has to be judged anyway.
//   * ONE shadow pass, not the canvas's stack of five.  They exist so the
//     wordmark survives the wave moving behind it; at 14px they resolve to
//     about a single hard 1px drop, which is what this draws.
void xmb_draw_topbar(void) {
    const int oy    = XMB_OY;   // shift the top bar down out of the CRT overscan
    const int row_y = oy + UIS_H(20);            // the shared 24px row
    const int cy    = row_y + UIS_H(24) / 2;

    const int mark_px = UIS_H(21);
    xmb_draw_mark((int)XMB_ITEM_PAD, cy - mark_px / 2, mark_px);

    // The wordmark's ink is centred on the same line as the mark rather than
    // being given a y of its own: 14px type against a 21px mark is exactly the
    // case where an eyeballed offset stops being right at the next scale.
    const float word_px = UIS_TF(14.0f);
    const float track   = word_px * 0.02f;       // the design's 0.02em
    const u32   ramp[4] = { XMB_LK_WORD_A, XMB_LK_WORD_B,
                            XMB_LK_WORD_C, XMB_LK_WORD_D };
    const int   wx = (int)XMB_ITEM_PAD + mark_px + UIS_H(6);
    int wy;
    if (ttf_center_y("JELLYFIN", word_px, UI_FACE_LOCKUP, cy, &wy)) {
        drawTTF_tracked((u32)wx, (u32)(wy + UIS_H(1)), "JELLYFIN", word_px,
                        0x00000000, UI_FACE_LOCKUP, track);
        drawTTF_ramp((u32)wx, (u32)wy, "JELLYFIN", word_px,
                     ramp, 4, UI_FACE_LOCKUP, track);
    }

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
// One 1px rule of the divider stack: x 40 -> 1240, flat across the middle and
// fading to nothing over the outer 12% at each end.
//
// The canvas spells it as a gradient stop list --
//   linear-gradient(90deg, transparent, hair 12%, hair 88%, transparent)
// -- which is a flat line with two ramps, NOT the triangular falloff this used
// to draw.  The old version was also full-bleed and a hardcoded 8A93C8, which
// is within a shade of `hairline` composited over the shipping background but
// would have stayed blue on a gold screen.
static void divider_rule(int y, u32 color, u32 alpha) {
    if (y < 0 || (u32)y >= display_height || !alpha) return;
    const int x0 = (int)XMB_ITEM_PAD;
    const int x1 = (int)display_width - (int)XMB_ITEM_PAD;
    const int w  = x1 - x0;
    if (w <= 0) return;
    const int fade = w * 12 / 100;               // the outer 12%, each end

    u32 *row = color_buffer[curr_fb] + (u32)y * display_width;
    const u32 c_r = (color >> 16) & 0xFF, c_g = (color >> 8) & 0xFF, c_b = color & 0xFF;
    for (int x = x0; x < x1; x++) {
        int d = x - x0 < x1 - 1 - x ? x - x0 : x1 - 1 - x;
        u32 a = (fade > 0 && d < fade) ? alpha * (u32)d / (u32)fade : alpha;
        if (!a) continue;
        u32 bg = row[x];
        row[x] = (((a*c_r + (255-a)*((bg>>16)&0xFF))/255) << 16) |
                 (((a*c_g + (255-a)*((bg>> 8)&0xFF))/255) <<  8) |
                  ((a*c_b + (255-a)*( bg     &0xFF))/255);
    }
}

// The divider under the tab bar: the hairline, and the trim rule 4px below it
// that the canvas draws with `--jf-trim2`.  That second line is transparent
// under the shipping theme (trim_alpha 0) and brass under Golden Age, which is
// the first time the `trim` token has been drawn by anything.
void xmb_draw_divider(void) {
    divider_rule(XMB_DIVIDER_Y, XMB_HAIRLINE, 255);
    if (g_theme.trim_alpha)
        divider_rule(XMB_DIVIDER_Y + UIS_H(4), XMB_TRIM, g_theme.trim_alpha);
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

// Blit an ARGB master into the current CPU draw target, scaled to dw x dh with
// an area-average filter.  bright scales RGB (255 = as-authored) so the HUD can
// dim unfocused controls the way ctrl_color() dims its vector glyphs.
//
// Shared by the button sprites and the brand mark: both are small ARGB images
// that are authored once at a master size and drawn at whatever the current UI
// scale asks for, and neither wants a per-size cache for it.
static void blit_argb_scaled(const u32 *m, int mw, int mh,
                             int dx0, int dy0, int dw, int dh, u32 bright) {
    if (!m || mw <= 0 || mh <= 0 || dw <= 0 || dh <= 0) return;
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

void draw_ps_button_vcentered(u32 x, int cy, char glyph, int h, u32 bright) {
    ps_sprites_load();
    int i = ps_tile_idx(glyph);
    if (s_ps_state != 1 || i < 0 || !s_ps_px[i] || h <= 0) return;
    int mw = s_ps_w[i], mh = s_ps_h[i];
    int dw = mw * h / mh;
    blit_argb_scaled(s_ps_px[i], mw, mh, (int)x, cy - h / 2, dw, h, bright);
}

// -------------------------------------------------------
// Brand mark — the lockup's jellyfish, from gfx/jfmark_png.h
// -------------------------------------------------------
// The design's mark is an SVG with a bevel, an edge stroke and a clipped
// image, so it arrives here already rasterised (see that header for why, and
// for the geometry constants used below).  Two variants exist because the
// gradient is baked in; the loaded theme picks between them.
//
// Each decodes on first use and stays: 80x80x4 is 25 KB, and a theme that is
// never selected never costs its variant.

#define JFMARK_COOL 0
#define JFMARK_GOLD 1

static u32 *s_mark_px[2];
static int  s_mark_state[2];          // 0 = untried, 1 = ok, -1 = failed

static void mark_load(int v) {
    if (s_mark_state[v]) return;
    s_mark_state[v] = -1;
    int w, h, comp;
    unsigned char *img = stbi_load_from_memory(
        v == JFMARK_GOLD ? jfmark_gold_png : jfmark_cool_png,
        (int)(v == JFMARK_GOLD ? jfmark_gold_png_len : jfmark_cool_png_len),
        &w, &h, &comp, 4);
    if (!img) return;
    if (w != JFMARK_MASTER || h != JFMARK_MASTER) { stbi_image_free(img); return; }
    u32 *m = (u32 *)malloc((size_t)w * h * 4);
    if (!m) { stbi_image_free(img); return; }
    for (int i = 0; i < w * h; i++)
        m[i] = ((u32)img[i*4+3] << 24) | ((u32)img[i*4] << 16) |
               ((u32)img[i*4+1] << 8) | img[i*4+2];
    stbi_image_free(img);
    s_mark_px[v] = m;
    s_mark_state[v] = 1;
}

// Which raster this theme gets.  The ramp is baked in, so the only question a
// theme can answer is which of the two it is NEARER: a warm mark colour (Golden
// Age's E8B45C) takes the gold PS logo, a cool one (XMB wave's AA5CC3) takes
// the soft one.  Red against blue is the whole test -- a hue angle would be
// more words for the same two answers.
static int mark_variant(void) {
    u32 c = XMB_LK_MARK_A;
    return (((c >> 16) & 0xFF) > (c & 0xFF)) ? JFMARK_GOLD : JFMARK_COOL;
}

// Draw the mark with its BELL that many pixels wide, its top-left at (x,y).
// The raster is wider than the bell -- it carries the design's drop shadow in
// a padded box -- so both the size and the origin are adjusted here rather
// than at the call site.
void xmb_draw_mark(int x, int y, int bell_px) {
    if (bell_px <= 0) return;
    int v = mark_variant();
    mark_load(v);
    if (s_mark_state[v] != 1) return;
    int box = bell_px * JFMARK_SPAN_U / JFMARK_BELL_U;
    int off = bell_px * JFMARK_PAD_U  / JFMARK_BELL_U;
    blit_argb_scaled(s_mark_px[v], JFMARK_MASTER, JFMARK_MASTER,
                     x - off, y - off, box, box, 255);
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
