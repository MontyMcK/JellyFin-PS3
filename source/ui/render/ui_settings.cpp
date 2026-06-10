// Settings tab rendering — account info card + selectable action rows.
// Only "Log Out" exists for now (XMB_SETTINGS_COUNT == 1).

#include <stdio.h>

#include "ui_visuals.h"
#include "jellyfin_api.h"

static const char *SETTINGS_LABELS[XMB_SETTINGS_COUNT] = { "Log Out" };

// Y of the i-th action row, below the account info card.
static int settings_row_y(int i) {
    return XMB_CONTENT_Y + 120 + i * XMB_ROW_STRIDE;
}

// CPU phase: account card background + selection highlight (or confirm dim).
void xmb_cpu_draw_settings(void) {
    int W      = (int)display_width;
    int list_x = (W - XMB_LIST_W) / 2;

    if (g_settings_confirm) {
        // Solid backdrop behind the confirmation prompt.
        drawRect(0, (u32)XMB_CONTENT_Y, display_width,
                 display_height - XMB_CONTENT_Y - XMB_BOTTOM_PAD, XMB_HIGHLIGHT);
        return;
    }

    // Account info card.
    drawRect((u32)list_x, (u32)XMB_CONTENT_Y, (u32)XMB_LIST_W, 96, XMB_THUMB_DIM);

    // Highlight the selected action row.
    for (int i = 0; i < XMB_SETTINGS_COUNT; i++) {
        if (i != g_settings_sel) continue;
        int iy = settings_row_y(i);
        drawRect((u32)list_x, (u32)iy, (u32)XMB_LIST_W, (u32)XMB_ROW_H, XMB_HIGHLIGHT);
        drawRect((u32)(list_x - 5), (u32)iy, 4, (u32)XMB_ROW_H, XMB_ACCENT);
    }
}

// RSX phase: account text, action labels, confirm prompt.
void xmb_draw_settings(void) {
    int W      = (int)display_width;
    int list_x = (W - XMB_LIST_W) / 2;
    int tx     = list_x + 20;

    // Account info.
    drawTTF((u32)tx, (u32)(XMB_CONTENT_Y + 22), "ACCOUNT", 14, 0x00A090C0);
    char line[320];
    snprintf(line, sizeof(line), "Signed in as %s",
             g_username[0] ? g_username : "(unknown)");
    drawTTF((u32)tx, (u32)(XMB_CONTENT_Y + 44), line, 20, 0x00FFFFFF);
    snprintf(line, sizeof(line), "Server  %s",
             g_server[0] ? g_server : "(none)");
    drawTTF((u32)tx, (u32)(XMB_CONTENT_Y + 72), line, 14, 0x00A0A0A0);

    // Action rows.
    for (int i = 0; i < XMB_SETTINGS_COUNT; i++) {
        int iy = settings_row_y(i);
        drawTTF((u32)tx, (u32)(iy + 26), SETTINGS_LABELS[i], 22, 0x00FFFFFF);
    }

    // Confirmation prompt.
    if (g_settings_confirm) {
        int cy = XMB_CONTENT_Y + 140;
        const char *q = "Log out of this account?";
        int qw = ttf_text_width(q, 24);
        drawTTF((u32)((W - qw) / 2), (u32)cy, q, 24, 0x00FFFFFF);
        const char *h = "X  Log Out        O  Cancel";
        int hw = ttf_text_width(h, 18);
        drawTTF((u32)((W - hw) / 2), (u32)(cy + 44), h, 18, 0x00C0B0E0);
    }
}
