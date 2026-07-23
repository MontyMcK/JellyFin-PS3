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

static const char *SETTINGS_LABELS[XMB_SETTINGS_COUNT] =
    { "Log Out", "Debug Logging", "Screen Size", "1080p Playback (Alpha)" };
static const int   SETTINGS_ICONS[XMB_SETTINGS_COUNT]  =
    { ICON_LOGOUT, ICON_BUG, ICON_TV, ICON_MOVIE };

#define SET_PANEL_H 96
#define SET_ROW_H   56

static int settings_panel_y(void) { return XMB_CONTENT_Y + 16; }

// Y of the i-th action row, below the account info card.
static int settings_row_y(int i) {
    return settings_panel_y() + SET_PANEL_H + 24 + i * (SET_ROW_H + 10);
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

    // Selected action row.
    for (int i = 0; i < XMB_SETTINGS_COUNT; i++) {
        if (i != g_settings_sel) continue;
        int iy = settings_row_y(i);
        drawRect((u32)list_x, (u32)iy, (u32)XMB_LIST_W, SET_ROW_H, XMB_PANEL_HI);
        drawRect((u32)(list_x - 4), (u32)iy, 3, SET_ROW_H, XMB_ACCENT);
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
        int qw = ttf_text_width(q, 21, true);
        drawTTF((u32)(mx + (mw - qw) / 2), (u32)(my + 28), q, 21, XMB_TEXT, true);
        const char *s = "You'll need to sign in again to browse your library.";
        int sw = ttf_text_width(s, 14);
        drawTTF((u32)(mx + (mw - sw) / 2), (u32)(my + 66), s, 14, XMB_TEXT_DIM);
        return;
    }

    int py = settings_panel_y();
    int tx = list_x + 84;

    // Avatar initial.
    {
        char ini[2] = { ' ', '\0' };
        if (g_username[0]) {
            ini[0] = g_username[0];
            if (ini[0] >= 'a' && ini[0] <= 'z') ini[0] -= 32;
        }
        int iw = ttf_text_width(ini, 22, true);
        drawTTF((u32)(list_x + 46 - iw / 2), (u32)(py + SET_PANEL_H / 2 - 12),
                ini, 22, XMB_WHITE, true);
    }

    // Identity.
    drawTTF((u32)tx, (u32)(py + 14), "Account", 13, XMB_TEXT_FAINT);
    char line[320];
    snprintf(line, sizeof(line), "%s", g_username[0] ? g_username : "(unknown)");
    drawTTF((u32)tx, (u32)(py + 34), line, 21, XMB_TEXT, true);
    snprintf(line, sizeof(line), "%s", g_server[0] ? g_server : "(no server)");
    drawTTF((u32)tx, (u32)(py + 64), line, 14, XMB_TEXT_DIM);

    // Action rows.
    for (int i = 0; i < XMB_SETTINGS_COUNT; i++) {
        int  iy  = settings_row_y(i);
        bool sel = (i == g_settings_sel);
        u32  clr = sel ? XMB_WHITE : XMB_TEXT_DIM;
        drawIcon((u32)(list_x + 20), (u32)(iy + (SET_ROW_H - 20) / 2),
                 SETTINGS_ICONS[i], 20.0f, clr);
        drawTTF((u32)(list_x + 52), (u32)(iy + (SET_ROW_H - 18) / 2 - 2),
                SETTINGS_LABELS[i], 18, clr, sel);
        if (i == 1) {   // Debug Logging — right-aligned On/Off state
            const char *val = plog_enabled() ? "On" : "Off";
            int vw = ttf_text_width(val, 18, sel);
            drawTTF((u32)(list_x + XMB_LIST_W - 24 - vw),
                    (u32)(iy + (SET_ROW_H - 18) / 2 - 2),
                    val, 18, plog_enabled() ? XMB_ACCENT : XMB_TEXT_FAINT, sel);
        }
        if (i == 2) {   // Screen Size — right-aligned overscan percentage
            int pm = (int)(overscan_frac() * 1000.0f + 0.5f);   // permille
            char val[16];
            if (pm == 0) snprintf(val, sizeof(val), "Off");
            else         snprintf(val, sizeof(val), "%d.%d%%", pm / 10, pm % 10);
            int vw = ttf_text_width(val, 18, sel);
            drawTTF((u32)(list_x + XMB_LIST_W - 24 - vw),
                    (u32)(iy + (SET_ROW_H - 18) / 2 - 2),
                    val, 18, pm ? XMB_ACCENT : XMB_TEXT_FAINT, sel);
        }
        if (i == 3) {   // 1080p Playback (Alpha) — right-aligned On/Off state
            const char *val = hd1080_enabled() ? "On" : "Off";
            int vw = ttf_text_width(val, 18, sel);
            drawTTF((u32)(list_x + XMB_LIST_W - 24 - vw),
                    (u32)(iy + (SET_ROW_H - 18) / 2 - 2),
                    val, 18, hd1080_enabled() ? XMB_ACCENT : XMB_TEXT_FAINT, sel);
        }
    }

    // Version footer — skip it if a large overscan inset has squeezed the
    // bottom-anchored footer up into the last (top-anchored) action row.
    {
        int footer_y = (int)display_height - XMB_BOTTOM_PAD - 26;
        int last_row_bottom = settings_row_y(XMB_SETTINGS_COUNT - 1) + SET_ROW_H;
        if (footer_y > last_row_bottom + 6) {
            const char *ver = "Jellyfin for PS3 " APP_VERSION " \xB7 built " __DATE__;
            int vw = ttf_text_width(ver, 13);
            drawTTF((u32)((W - vw) / 2), (u32)footer_y, ver, 13, XMB_TEXT_FAINT);
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
    const int t = 4;
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
    int tw = ttf_text_width(title, 26, true);
    drawTTF((u32)((W - tw) / 2), (u32)(cy - 78), title, 26, OVL_INK, true);

    const char *l1 = "Match the corners to the edges of your screen";
    int l1w = ttf_text_width(l1, 16);
    drawTTF((u32)((W - l1w) / 2), (u32)(cy - 34), l1, 16, OVL_INK_DIM);

    int pm = (int)(overscan_frac() * 1000.0f + 0.5f);
    char pct[16];
    snprintf(pct, sizeof(pct), "%d.%d%%", pm / 10, pm % 10);
    int pw = ttf_text_width(pct, 30, true);
    drawTTF((u32)((W - pw) / 2), (u32)(cy - 2), pct, 30, XMB_ACCENT_DEEP, true);

    const char *hint = "D-pad Left / Right to adjust";
    int hw = ttf_text_width(hint, 15);
    drawTTF((u32)((W - hw) / 2), (u32)(cy + 44), hint, 15, OVL_INK_DIM);

    const char *keys = "Cross  Save        Circle  Cancel";
    int kw = ttf_text_width(keys, 15);
    drawTTF((u32)((W - kw) / 2), (u32)(cy + 70), keys, 15, OVL_INK_DIM);
}
