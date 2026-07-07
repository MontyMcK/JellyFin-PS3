// Settings tab rendering — account card (avatar + identity) and selectable
// action rows: "Log Out" and the "Debug Logging" toggle.

#include <stdio.h>
#include <string.h>
#include <math.h>

#include "ui_visuals.h"
#include "jellyfin_api.h"
#include "update_check.h"
#include "plog.h"

static const char *SETTINGS_LABELS[XMB_SETTINGS_COUNT] = { "Log Out", "Debug Logging" };
static const int   SETTINGS_ICONS[XMB_SETTINGS_COUNT]  = { 0xE879, 0xE868 };  // exit_to_app, bug_report

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
    }

    // Version footer.
    {
        const char *ver = "Jellyfin for PS3 " APP_VERSION " \xB7 built " __DATE__;
        int vw = ttf_text_width(ver, 13);
        drawTTF((u32)((W - vw) / 2),
                (u32)((int)display_height - XMB_BOTTOM_PAD - 26),
                ver, 13, XMB_TEXT_FAINT);
    }
}
