// Settings tab rendering — account card (avatar + identity) and selectable
// action rows: "Log Out" and the "Debug Logging" toggle.

#include <stdio.h>
#include <string.h>
#include <math.h>

#include "ui_visuals.h"
#include "jellyfin_api.h"
#include "update_check.h"
#include "plog.h"
#include "hd1080.h"
#include "surround.h"
#include "centermix.h"
#include "statsovl.h"
#include "subfont.h"
#include "subcolor.h"

static const char *SETTINGS_LABELS[XMB_SETTINGS_COUNT] =
    { "Log Out", "Debug Logging", "Screen Size", "1080p Playback (Alpha)",
      "Audio Output", "Dialogue Boost", "Subtitle Font", "Subtitle Colour",
      "Theme"
#if ENABLE_PLAYER_STATS
    , "Player Stats Overlay"
#endif
    };
// ICON_BUG is reused for the stats row: it is the same diagnostics family as
// Debug Logging, and the Tabler font here is a 20-glyph subset (see
// ui/fonts/tabler_icons.h) — a new glyph would mean regenerating the subset.
// ICON_MUSIC (already in the subset) marks the surround audio row.
static const int   SETTINGS_ICONS[XMB_SETTINGS_COUNT]  =
    { ICON_LOGOUT, ICON_BUG, ICON_TV, ICON_MOVIE, ICON_MUSIC, ICON_MUSIC,
      ICON_TV, ICON_TV, ICON_PHOTO
#if ENABLE_PLAYER_STATS
    , ICON_BUG
#endif
    };

#define SET_PANEL_H UIS_H(96)
#define SET_ROW_H   UIS_H(56)

static int settings_panel_y(void) { return XMB_CONTENT_Y + 16; }

// Y of the first action row, below the account info card.
static int settings_rows_top(void) {
    return settings_panel_y() + SET_PANEL_H + 24;
}

// Keep a stable XMB row rhythm. When the list is longer than the safe area,
// selection scrolling (rather than row compression) moves the visible window.
static int settings_row_pitch(void) {
    return SET_ROW_H + UIS_H(10);
}

static int settings_visible_rows(void) {
    int avail = (int)display_height - XMB_BOTTOM_PAD - settings_rows_top();
    int pitch = settings_row_pitch();
    if (avail < SET_ROW_H) return 1;

    int n = 1 + (avail - SET_ROW_H) / pitch;
    if (n < 1) n = 1;
    if (n > XMB_SETTINGS_COUNT) n = XMB_SETTINGS_COUNT;
    return n;
}

// First row in the visible settings window. The selected row is always kept
// visible by advancing this window; the row height itself never changes.
static int settings_first_row(void) {
    int vis = settings_visible_rows();
    int first = g_settings_sel - vis + 1;
    if (first < 0) first = 0;

    int max_first = XMB_SETTINGS_COUNT - vis;
    if (max_first < 0) max_first = 0;
    if (first > max_first) first = max_first;
    return first;
}

static int settings_row_y(int i) {
    return settings_rows_top()
         + (i - settings_first_row()) * settings_row_pitch();
}

// Centered confirm dialog rect.
static void settings_confirm_rect(int *x, int *y, int *w, int *h) {
    *w = 520; *h = 118;
    *x = ((int)display_width - *w) / 2;
    *y = XMB_CONTENT_Y + 100;
}

// 1px hairline outline around a rect.
static void hairline_frame(int x, int y, int w, int h) {
    drawRect((u32)x, (u32)y, (u32)w, 1, XMB_HAIRLINE);
    drawRect((u32)x, (u32)(y + h - 1), (u32)w, 1, XMB_HAIRLINE);
    drawRect((u32)x, (u32)y, 1, (u32)h, XMB_HAIRLINE);
    drawRect((u32)(x + w - 1), (u32)y, 1, (u32)h, XMB_HAIRLINE);
}

// Filled circle, scanline by scanline (small radii only).
static void fill_circle(int cx, int cy, int r, u32 color) {
    for (int dy = -r; dy <= r; dy++) {
        int hw = (int)(sqrtf((float)(r * r - dy * dy)) + 0.5f);
        drawRect((u32)(cx - hw), (u32)(cy + dy), (u32)(2 * hw + 1), 1, color);
    }
}

// CPU phase: account card, row highlight, confirm dialog panel.
void xmb_cpu_draw_settings(void) {
    int W      = (int)display_width;
    int list_x = (W - XMB_LIST_W) / 2;

    if (g_settings_confirm) {
        int mx, my, mw, mh;
        settings_confirm_rect(&mx, &my, &mw, &mh);
        drawRect((u32)mx, (u32)my, (u32)mw, (u32)mh, XMB_PANEL);
        hairline_frame(mx, my, mw, mh);
        return;
    }

    // Account card with avatar disc.
    int py = settings_panel_y();
    drawRect((u32)list_x, (u32)py, (u32)XMB_LIST_W, SET_PANEL_H, XMB_PANEL);
    hairline_frame(list_x, py, XMB_LIST_W, SET_PANEL_H);
    fill_circle(list_x + 46, py + SET_PANEL_H / 2, 22, XMB_ACCENT_DEEP);

    // Selected action row. The visible window is derived from the selected
    // row, so navigation can reach every setting without putting anything
    // below the safe area.
    {
        int first = settings_first_row();
        int last  = first + settings_visible_rows();
        for (int i = first; i < last; i++) {
            if (i != g_settings_sel) continue;
            int iy = settings_row_y(i);
            drawRect((u32)list_x, (u32)iy, (u32)XMB_LIST_W, SET_ROW_H, XMB_PANEL_HI);
            drawRect((u32)(list_x - UIS_W(4)), (u32)iy, UIS_W(3), SET_ROW_H, XMB_ACCENT);
        }
    }
}

// RSX phase: account text, action labels, confirm prompt.
void xmb_draw_settings(void) {
    int W      = (int)display_width;
    int list_x = (W - XMB_LIST_W) / 2;

    if (g_settings_confirm) {
        int mx, my, mw, mh;
        settings_confirm_rect(&mx, &my, &mw, &mh);
        const char *q = "Log out of this account?";
        int qw = ttf_text_width(q, UIS_TF(21), true);
        drawTTF((u32)(mx + (mw - qw) / 2), (u32)(my + UIS_H(28)), q, UIS_TF(21), XMB_TEXT, true);
        const char *s = "You'll need to sign in again to browse your library.";
        int sw = ttf_text_width(s, UIS_TF(14));
        drawTTF((u32)(mx + (mw - sw) / 2), (u32)(my + UIS_H(66)), s, UIS_TF(14), XMB_TEXT_DIM);
        return;
    }

    int py = settings_panel_y();
    int tx = list_x + UIS_W(84);

    // Avatar initial.
    {
        char ini[2] = { ' ', '\0' };
        if (g_username[0]) {
            ini[0] = g_username[0];
            if (ini[0] >= 'a' && ini[0] <= 'z') ini[0] -= 32;
        }
        int iw = ttf_text_width(ini, UIS_TF(22), true);
        drawTTF((u32)(list_x + UIS_W(46) - iw / 2), (u32)(py + SET_PANEL_H / 2 - UIS_H(12)),
                ini, UIS_TF(22), XMB_WHITE, true);
    }

    // Identity.
    drawTTF((u32)tx, (u32)(py + UIS_H(14)), "Account", UIS_TF(13), XMB_TEXT_FAINT);
    char line[320];
    snprintf(line, sizeof(line), "%s", g_username[0] ? g_username : "(unknown)");
    drawTTF((u32)tx, (u32)(py + UIS_H(34)), line, UIS_TF(21), XMB_TEXT, true);
    snprintf(line, sizeof(line), "%s", g_server[0] ? g_server : "(no server)");
    drawTTF((u32)tx, (u32)(py + UIS_H(64)), line, UIS_TF(14), XMB_TEXT_DIM);

    // Action rows. Draw only the visible window; the input handler still
    // owns the full selection range.
    int first_row = settings_first_row();
    int last_row  = first_row + settings_visible_rows();
    for (int i = first_row; i < last_row; i++) {
        int  iy  = settings_row_y(i);
        bool sel = (i == g_settings_sel);
        u32  clr = sel ? XMB_WHITE : XMB_TEXT_DIM;
        drawIcon((u32)(list_x + UIS_W(20)), (u32)(iy + (SET_ROW_H - UIS_H(20)) / 2),
                 SETTINGS_ICONS[i], UIS_TF(20.0f), clr);
        drawTTF((u32)(list_x + UIS_W(52)), (u32)(iy + (SET_ROW_H - UIS_H(18)) / 2 - UIS_H(2)),
                SETTINGS_LABELS[i], UIS_TF(18), clr, sel);
        if (i == 1) {   // Debug Logging — right-aligned On/Off state
            const char *val = plog_enabled() ? "On" : "Off";
            int vw = ttf_text_width(val, UIS_TF(18), sel);
            drawTTF((u32)(list_x + XMB_LIST_W - UIS_W(24) - vw),
                    (u32)(iy + (SET_ROW_H - UIS_H(18)) / 2 - UIS_H(2)),
                    val, UIS_TF(18), plog_enabled() ? XMB_ACCENT : XMB_TEXT_FAINT, sel);
        }
        if (i == 2) {   // Screen Size — right-aligned overscan percentage
            int pm = (int)(overscan_frac() * 1000.0f + 0.5f);   // permille
            char val[16];
            if (pm == 0) snprintf(val, sizeof(val), "Off");
            else         snprintf(val, sizeof(val), "%d.%d%%", pm / 10, pm % 10);
            int vw = ttf_text_width(val, UIS_TF(18), sel);
            drawTTF((u32)(list_x + XMB_LIST_W - UIS_W(24) - vw),
                    (u32)(iy + (SET_ROW_H - UIS_H(18)) / 2 - UIS_H(2)),
                    val, UIS_TF(18), pm ? XMB_ACCENT : XMB_TEXT_FAINT, sel);
        }
        if (i == 3) {   // 1080p Playback (Alpha) — right-aligned On/Off state
            const char *val = hd1080_enabled() ? "On" : "Off";
            int vw = ttf_text_width(val, UIS_TF(18), sel);
            drawTTF((u32)(list_x + XMB_LIST_W - UIS_W(24) - vw),
                    (u32)(iy + (SET_ROW_H - UIS_H(18)) / 2 - UIS_H(2)),
                    val, UIS_TF(18), hd1080_enabled() ? XMB_ACCENT : XMB_TEXT_FAINT, sel);
        }
        if (i == 4) {   // Audio Output — right-aligned Stereo/5.1/7.1 state
            const char *val = surround_mode_label();
            int vw = ttf_text_width(val, UIS_TF(18), sel);
            drawTTF((u32)(list_x + XMB_LIST_W - UIS_W(24) - vw),
                    (u32)(iy + (SET_ROW_H - UIS_H(18)) / 2 - UIS_H(2)),
                    val, UIS_TF(18), surround_enabled() ? XMB_ACCENT : XMB_TEXT_FAINT, sel);
        }
        if (i == 5) {   // Dialogue Boost — right-aligned gain state
            const char *val = centermix_label();
            int vw = ttf_text_width(val, UIS_TF(18), sel);
            drawTTF((u32)(list_x + XMB_LIST_W - UIS_W(24) - vw),
                    (u32)(iy + (SET_ROW_H - UIS_H(18)) / 2 - UIS_H(2)),
                    val, UIS_TF(18), centermix_active() ? XMB_ACCENT : XMB_TEXT_FAINT, sel);
        }
        if (i == 6) {   // Subtitle Font — right-aligned typeface name
            const char *val = subfont_label();
            int vw = ttf_text_width(val, UIS_TF(18), sel);
            drawTTF((u32)(list_x + XMB_LIST_W - UIS_W(24) - vw),
                    (u32)(iy + (SET_ROW_H - UIS_H(18)) / 2 - UIS_H(2)),
                    val, UIS_TF(18),
                    subfont_get() ? XMB_ACCENT : XMB_TEXT_FAINT, sel);
        }
        if (i == 7) {   // Subtitle Colour — right-aligned look name
            const char *val = subcolor_label();
            int vw = ttf_text_width(val, UIS_TF(18), sel);
            drawTTF((u32)(list_x + XMB_LIST_W - UIS_W(24) - vw),
                    (u32)(iy + (SET_ROW_H - UIS_H(18)) / 2 - UIS_H(2)),
                    val, UIS_TF(18),
                    subcolor_get() ? XMB_ACCENT : XMB_TEXT_FAINT, sel);
        }
        if (i == 8) {   // Theme — right-aligned live theme name
            // Always drawn in the accent, because the accent IS the thing the
            // row changes: the value's colour previews the choice.
            const char *val = theme_current_name();
            int vw = ttf_text_width(val, UIS_TF(18), sel);
            drawTTF((u32)(list_x + XMB_LIST_W - UIS_W(24) - vw),
                    (u32)(iy + (SET_ROW_H - UIS_H(18)) / 2 - UIS_H(2)),
                    val, UIS_TF(18), XMB_ACCENT, sel);
        }
#if ENABLE_PLAYER_STATS
        if (i == 9) {   // Player Stats Overlay — right-aligned On/Off state
            const char *val = statsovl_enabled() ? "On" : "Off";
            int vw = ttf_text_width(val, UIS_TF(18), sel);
            drawTTF((u32)(list_x + XMB_LIST_W - UIS_W(24) - vw),
                    (u32)(iy + (SET_ROW_H - UIS_H(18)) / 2 - UIS_H(2)),
                    val, UIS_TF(18), statsovl_enabled() ? XMB_ACCENT : XMB_TEXT_FAINT, sel);
        }
#endif
    }

    // Version footer — skip it if the visible settings window already
    // occupies the bottom of the safe area.
    {
        int footer_y = (int)display_height - XMB_BOTTOM_PAD - UIS_H(26);
        int last_visible = settings_first_row() + settings_visible_rows() - 1;
        int last_row_bottom = settings_row_y(last_visible) + SET_ROW_H;
        if (footer_y > last_row_bottom + 6) {
            const char *ver = "Jellyfin for PS3 " APP_VERSION " \xC2\xB7 built " __DATE__;
            int vw = ttf_text_width(ver, UIS_TF(13));
            drawTTF((u32)((W - vw) / 2), (u32)footer_y, ver, UIS_TF(13), XMB_TEXT_FAINT);
        }
    }
}

// -------------------------------------------------------
// Overscan calibration screen (Settings > Screen Size)
// -------------------------------------------------------
// A full-screen takeover: a light "safe-area" field inset by the current
// overscan, with a bright border and corner L-brackets the user aligns to the
// visible edges of their CRT (d-pad left/right adjusts).  Because the field is
// drawn at exactly the inset that the whole UI will use, matching the corners
// to the screen edges guarantees nothing else clips.

// DELIBERATELY NOT THEMED.  The overscan calibration screen has to stay
// legible under ANY theme -- including one a user writes badly -- because it is
// what you use to fix a screen you cannot read.  Fixed light field, dark ink.
#define OVL_FIELD   0x00C8CCDAUL   // light safe-area field
#define OVL_SURND   0x00101018UL   // near-black surround (over the wave)
#define OVL_INK     0x00202634UL   // dark title text on the field
#define OVL_INK_DIM 0x003A4256UL   // dark secondary text

void xmb_overscan_calib_cpu(void) {
    int W = (int)display_width, H = (int)display_height;
    int ox = overscan_x(), oy = overscan_y();
    if (W - 2 * ox < 40 || H - 2 * oy < 40) { ox = 0; oy = 0; }

    // Flat surround covers the animated wave so the frame edges read cleanly.
    drawRect(0, 0, (u32)W, (u32)H, OVL_SURND);
    // The light field = the area that survives the inset everywhere else.
    drawRect((u32)ox, (u32)oy, (u32)(W - 2 * ox), (u32)(H - 2 * oy), OVL_FIELD);

    // Thin accent border at the inset edge.
    const int t = UIS_H(4);
    drawRect((u32)ox, (u32)oy,            (u32)(W - 2 * ox), (u32)t, XMB_ACCENT);
    drawRect((u32)ox, (u32)(H - oy - t),  (u32)(W - 2 * ox), (u32)t, XMB_ACCENT);
    drawRect((u32)ox, (u32)oy, (u32)t, (u32)(H - 2 * oy), XMB_ACCENT);
    drawRect((u32)(W - ox - t), (u32)oy, (u32)t, (u32)(H - 2 * oy), XMB_ACCENT);

    // Bolder corner L-brackets — the primary alignment guide.
    const int arm = (W < 1000 ? 40 : 64), ct = 7;
    // top-left
    drawRect((u32)ox, (u32)oy, (u32)arm, (u32)ct, XMB_ACCENT);
    drawRect((u32)ox, (u32)oy, (u32)ct, (u32)arm, XMB_ACCENT);
    // top-right
    drawRect((u32)(W - ox - arm), (u32)oy, (u32)arm, (u32)ct, XMB_ACCENT);
    drawRect((u32)(W - ox - ct),  (u32)oy, (u32)ct,  (u32)arm, XMB_ACCENT);
    // bottom-left
    drawRect((u32)ox, (u32)(H - oy - ct),  (u32)arm, (u32)ct, XMB_ACCENT);
    drawRect((u32)ox, (u32)(H - oy - arm), (u32)ct,  (u32)arm, XMB_ACCENT);
    // bottom-right
    drawRect((u32)(W - ox - arm), (u32)(H - oy - ct),  (u32)arm, (u32)ct, XMB_ACCENT);
    drawRect((u32)(W - ox - ct),  (u32)(H - oy - arm), (u32)ct,  (u32)arm, XMB_ACCENT);
}

void xmb_overscan_calib_text(void) {
    int W = (int)display_width, H = (int)display_height;
    int cy = H / 2;

    const char *title = "Screen Size";
    int tw = ttf_text_width(title, UIS_TF(26), true);
    drawTTF((u32)((W - tw) / 2), (u32)(cy - UIS_H(78)), title, UIS_TF(26), OVL_INK, true);

    const char *l1 = "Match the corners to the edges of your screen";
    int l1w = ttf_text_width(l1, UIS_TF(16));
    drawTTF((u32)((W - l1w) / 2), (u32)(cy - UIS_H(34)), l1, UIS_TF(16), OVL_INK_DIM);

    int pm = (int)(overscan_frac() * 1000.0f + 0.5f);
    char pct[16];
    snprintf(pct, sizeof(pct), "%d.%d%%", pm / 10, pm % 10);
    int pw = ttf_text_width(pct, UIS_TF(30), true);
    drawTTF((u32)((W - pw) / 2), (u32)(cy - UIS_H(2)), pct, UIS_TF(30), XMB_ACCENT_DEEP, true);

    const char *hint = "D-pad Left / Right to adjust";
    int hw = ttf_text_width(hint, UIS_TF(15));
    drawTTF((u32)((W - hw) / 2), (u32)(cy + UIS_H(44)), hint, UIS_TF(15), OVL_INK_DIM);

    const char *keys = "Cross  Save        Circle  Cancel";
    int kw = ttf_text_width(keys, UIS_TF(15));
    drawTTF((u32)((W - kw) / 2), (u32)(cy + UIS_H(70)), keys, UIS_TF(15), OVL_INK_DIM);
}
