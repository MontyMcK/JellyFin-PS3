// Search tab rendering — search-bar OSK (CPU key cells + RSX labels)
// and the results list below it.

#include <stdio.h>
#include <string.h>

#include "ui_render_internal.h"
#include "timing.h"

void xmb_cpu_draw_osk(void) {
    int W = (int)display_width;
    int total_w = 10 * OSK_STEP_X - OSK_GAP;
    int osk_x0  = (W - total_w) / 2;

    drawRect((u32)osk_x0, (u32)(XMB_CONTENT_Y + 8),
             (u32)total_w, 40, 0x001A1040);

    for (int r = 0; r <= OSK_ROWS_N; r++) {
        if (r == OSK_ROWS_N) {
            int space_w = 5 * OSK_STEP_X - OSK_GAP;
            int sy = OSK_Y0 + r * OSK_STEP_Y;
            bool sp_sel = (r == g_osk_row && g_osk_col == 0);
            drawRect((u32)osk_x0, (u32)sy, (u32)space_w, OSK_KEY_H,
                     sp_sel ? XMB_KEY_SEL : XMB_KEY_NORMAL);
            int bsx = osk_x0 + space_w + OSK_GAP;
            bool bs_sel = (r == g_osk_row && g_osk_col == 1);
            drawRect((u32)bsx, (u32)sy, OSK_KEY_W, OSK_KEY_H,
                     bs_sel ? XMB_KEY_SEL : XMB_KEY_NORMAL);
            int clx = bsx + OSK_KEY_W + OSK_GAP;
            bool cl_sel = (r == g_osk_row && g_osk_col == 2);
            drawRect((u32)clx, (u32)sy, OSK_KEY_W, OSK_KEY_H,
                     cl_sel ? XMB_KEY_SEL : XMB_KEY_NORMAL);
        } else {
            const char **rows = g_osk_sym ? OSK_SYMBOLS : OSK_LETTERS;
            int rlen = (int)strlen(rows[r]);
            if (r == OSK_ROWS_N - 1) rlen++;
            int ry = OSK_Y0 + r * OSK_STEP_Y;
            for (int c = 0; c < rlen; c++) {
                bool sel = (r == g_osk_row && c == g_osk_col);
                drawRect((u32)(osk_x0 + c * OSK_STEP_X), (u32)ry,
                         OSK_KEY_W, OSK_KEY_H,
                         sel ? XMB_KEY_SEL : XMB_KEY_NORMAL);
            }
        }
    }
}

void xmb_rsx_draw_osk(void) {
    int W = (int)display_width;
    int total_w = 10 * OSK_STEP_X - OSK_GAP;
    int osk_x0  = (W - total_w) / 2;

    u64 us = timing_get_us();
    bool cursor = ((us / 500000) & 1) == 0;

    char disp[68];
    snprintf(disp, sizeof(disp), "%s%s", g_search_buf, cursor ? "_" : " ");
    {
        int bar_cx = (int)display_width / 2;
        int text_w = (int)strlen(disp) * 18;
        int tx = bar_cx - text_w / 2;
        if (tx < (int)XMB_ITEM_PAD + 4) tx = (int)XMB_ITEM_PAD + 4;
        drawTTF((u32)tx, (u32)(XMB_CONTENT_Y + 18), disp, 18, 0x00FFFFFF);
    }

    for (int r = 0; r <= OSK_ROWS_N; r++) {
        if (r == OSK_ROWS_N) {
            int space_w = 5 * OSK_STEP_X - OSK_GAP;
            int sy = OSK_Y0 + r * OSK_STEP_Y + (OSK_KEY_H - 8) / 2;
            int cx_space = osk_x0 + space_w / 2 - 20;
            drawTTF((u32)(cx_space > 0 ? cx_space : 0), (u32)sy, "SPACE", 21, 0x00FFFFFF);

            int bsx = osk_x0 + space_w + OSK_GAP;
            drawTTF((u32)(bsx + (OSK_KEY_W - 8) / 2), (u32)sy, "<", 21, 0x00FFFFFF);

            int clx = bsx + OSK_KEY_W + OSK_GAP;
            drawTTF((u32)(clx + (OSK_KEY_W - 40) / 2), (u32)sy, "CLEAR", 21, 0x00FFFFFF);
        } else {
            const char **rows = g_osk_sym ? OSK_SYMBOLS : OSK_LETTERS;
            const char  *row  = rows[r];
            int base_len = strlen(row);
            int ry = OSK_Y0 + r * OSK_STEP_Y + (OSK_KEY_H - 8) / 2;

            for (int c = 0; c < base_len; c++) {
                char label[3] = { row[c], '\0', '\0' };
                int kx = osk_x0 + c * OSK_STEP_X + (OSK_KEY_W - 8) / 2;
                drawTTF((u32)kx, (u32)ry, label, 21, 0x00FFFFFF);
            }
            if (r == OSK_ROWS_N - 1) {
                const char *toggle_lbl = g_osk_sym ? "ABC" : "#+=";
                int kx = osk_x0 + base_len * OSK_STEP_X + (OSK_KEY_W - 24) / 2;
                drawTTF((u32)(kx > 0 ? kx : 0), (u32)ry, toggle_lbl, 21, 0x00FFFFFF);
            }
        }
    }

    int kb_bottom = OSK_Y0 + (OSK_ROWS_N + 1) * OSK_STEP_Y + 20;
    int results_y = kb_bottom;
    int count = g_search_results_count;
    int vis_r = ((int)display_height - XMB_BOTTOM_PAD - results_y) / XMB_ROW_STRIDE;
    if (vis_r < 0) vis_r = 0;
    {
        int sr_list_x = ((int)display_width - XMB_LIST_W) / 2;
        int sr_tx     = sr_list_x + 16 + XMB_THUMB_W + 16;
        for (int i = 0; i < vis_r; i++) {
            int idx = g_search_scroll + i;
            if (idx >= count) break;
            const XMBItem *it = &g_search_results[idx];
            int iy = results_y + i * XMB_ROW_STRIDE;
            drawTTF((u32)sr_tx, (u32)(iy + 20), it->name, 18, 0x00FFFFFF);
            xmb_draw_meta((u32)sr_tx, (u32)(iy + 48), it, 18);
        }
    }
    if (count == 0 && g_search_buf[0]) {
        drawTTF((u32)XMB_ITEM_PAD, (u32)results_y, "No results.", 8, 0x00FFFFFF);
    }
}

// CPU draws for search results list (thumbs + selection highlight)
void xmb_cpu_draw_search_results(void) {
    int results_y = OSK_Y0 + (OSK_ROWS_N + 1) * OSK_STEP_Y + 20;
    int count     = g_search_results_count;
    int vis_r     = ((int)display_height - XMB_BOTTOM_PAD - results_y) / XMB_ROW_STRIDE;
    if (vis_r < 0) vis_r = 0;
    int list_x = ((int)display_width - XMB_LIST_W) / 2;
    for (int i = 0; i < vis_r; i++) {
        int idx = g_search_scroll + i;
        if (idx >= count) break;
        int iy = results_y + i * XMB_ROW_STRIDE;
        if (g_search_focus_results && idx == g_search_sel) {
            drawRect((u32)list_x, (u32)iy,
                     (u32)XMB_LIST_W, (u32)XMB_ROW_H, XMB_HIGHLIGHT);
            drawRect((u32)(list_x - 5), (u32)iy,
                     4, (u32)XMB_ROW_H, XMB_ACCENT);
        }
        xmb_blit_thumb(g_search_results[idx].id,
                       list_x + 16, iy + (XMB_ROW_H - XMB_THUMB_H) / 2);
    }
}
