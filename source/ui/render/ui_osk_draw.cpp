// Search tab rendering — search field, OSK (CPU key cells + RSX labels)
// and the results list below it.

#include <stdio.h>
#include <string.h>

#include "ui_render_internal.h"
#include "thumbnail_cache.h"
#include "timing.h"

#define OSK_FIELD_H 40

// -------------------------------------------------------
// Search layout
// -------------------------------------------------------
// The keyboard is tall enough that on anything below 1080p there was no room
// left under it for results: vis_rows came out 0, the draw loop never ran a
// single iteration, and the list was invisible — while the input handler
// happily kept moving the selection through items nobody could see (the
// "results not visible underneath the keyboard" report).  Measured before the
// fix: 0 rows at 480 NTSC, and 0 at 720p as soon as any overscan was set.
//
// So the keyboard COLLAPSES once focus moves into the results: while typing
// it is drawn in full with however many rows fit beneath it, and the moment
// the user presses down into the list it folds away to just the search field,
// handing the whole content area to the results (3+ rows at every mode down
// to 480i).  Up from the first row, or circle, brings it straight back.

int xmb_search_results_y(void) {
    if (g_search_focus_results)
        return XMB_CONTENT_Y + 8 + OSK_FIELD_H + 16;
    return OSK_Y0 + (OSK_ROWS_N + 1) * OSK_STEP_Y + 20;
}

// Rows that actually fit.  The single source of truth for both the renderer
// and the scroll arithmetic — they used to disagree, the handler assuming a
// fixed 6 while the renderer measured the screen.
int xmb_search_vis_rows(void) {
    int r = ((int)display_height - XMB_BOTTOM_PAD - xmb_search_results_y())
            / XMB_ROW_STRIDE;
    return r < 0 ? 0 : r;
}

void xmb_cpu_draw_osk(void) {
    int W = (int)display_width;
    int total_w = 10 * OSK_STEP_X - OSK_GAP;
    int osk_x0  = (W - total_w) / 2;

    // Search field: dark well with an accent underline.
    drawRect((u32)osk_x0, (u32)(XMB_CONTENT_Y + 8),
             (u32)total_w, OSK_FIELD_H, 0x00131830UL);
    drawRect((u32)osk_x0, (u32)(XMB_CONTENT_Y + 8 + OSK_FIELD_H - 2),
             (u32)total_w, 2, XMB_ACCENT);

    if (g_search_focus_results) return;   // collapsed: field only, no keys

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

// One key label, centered in a key cell, dark when the key is selected
// (selected keys are white).
static void osk_key_label(int kx, int kw, int ry, const char *lbl, bool sel) {
    const float px = 20.0f;
    int lw = ttf_text_width(lbl, px);
    int ty = ry + (OSK_KEY_H - (int)px) / 2 - 2;
    drawTTF((u32)(kx + (kw - lw) / 2), (u32)(ty > 0 ? ty : 0), lbl, px,
            sel ? XMB_KEY_LABEL_SEL : XMB_TEXT);
}

void xmb_rsx_draw_osk(void) {
    int W = (int)display_width;
    int total_w = 10 * OSK_STEP_X - OSK_GAP;
    int osk_x0  = (W - total_w) / 2;

    u64 us = timing_get_us();
    bool cursor = ((us / 500000) & 1) == 0;

    // Search field content: magnifier glyph, then typed text or ghost prompt.
    {
        int fx = osk_x0 + 14;
        int fy = XMB_CONTENT_Y + 8;
        drawIcon((u32)fx, (u32)(fy + (OSK_FIELD_H - 20) / 2), ICON_SEARCH, 20.0f,
                 XMB_TEXT_FAINT);
        int tx = fx + 30;
        int ty = fy + (OSK_FIELD_H - 18) / 2 - 2;
        if (g_search_buf[0]) {
            char disp[68];
            snprintf(disp, sizeof(disp), "%s%s", g_search_buf, cursor ? "_" : "");
            drawTTF((u32)tx, (u32)ty, disp, 18, XMB_TEXT);
        } else {
            if (cursor) drawTTF((u32)tx, (u32)ty, "_", 18, XMB_TEXT);
            drawTTF((u32)(tx + 14), (u32)ty, "Search your library", 17,
                    XMB_TEXT_FAINT);
        }
    }

    // -1 skips the key rows entirely while the results are focused; the
    // search field above and the results below still draw.
    const int key_rows = g_search_focus_results ? -1 : OSK_ROWS_N;
    for (int r = 0; r <= key_rows; r++) {
        int ry = OSK_Y0 + r * OSK_STEP_Y;
        if (r == OSK_ROWS_N) {
            int space_w = 5 * OSK_STEP_X - OSK_GAP;
            int bsx = osk_x0 + space_w + OSK_GAP;
            int clx = bsx + OSK_KEY_W + OSK_GAP;
            osk_key_label(osk_x0, space_w, ry, "Space",
                          r == g_osk_row && g_osk_col == 0);
            {   // Backspace: Material icon, centered in the key.
                bool sel = (r == g_osk_row && g_osk_col == 1);
                drawIcon((u32)(bsx + (OSK_KEY_W - 22) / 2),
                         (u32)(ry + (OSK_KEY_H - 22) / 2), ICON_BACKSPACE, 22.0f,
                         sel ? XMB_KEY_LABEL_SEL : XMB_TEXT);
            }
            osk_key_label(clx, OSK_KEY_W, ry, "Clear",
                          r == g_osk_row && g_osk_col == 2);
        } else {
            const char **rows = g_osk_sym ? OSK_SYMBOLS : OSK_LETTERS;
            const char  *row  = rows[r];
            int base_len = strlen(row);

            for (int c = 0; c < base_len; c++) {
                char label[2] = { row[c], '\0' };
                osk_key_label(osk_x0 + c * OSK_STEP_X, OSK_KEY_W, ry, label,
                              r == g_osk_row && c == g_osk_col);
            }
            if (r == OSK_ROWS_N - 1) {
                osk_key_label(osk_x0 + base_len * OSK_STEP_X, OSK_KEY_W, ry,
                              g_osk_sym ? "ABC" : "#+=",
                              r == g_osk_row && g_osk_col == base_len);
            }
        }
    }

    int results_y = xmb_search_results_y();
    int count     = g_search_results_count;
    int vis_r     = xmb_search_vis_rows();
    {
        int sr_list_x = ((int)display_width - XMB_LIST_W) / 2;
        int sr_tx     = sr_list_x + 16 + XMB_THUMB_W + 16;
        for (int i = 0; i < vis_r; i++) {
            int idx = g_search_scroll + i;
            if (idx >= count) break;
            const XMBItem *it = &g_search_results[idx];
            int iy = results_y + i * XMB_ROW_STRIDE;
            bool sel = g_search_focus_results && idx == g_search_sel;
            drawTTF((u32)sr_tx, (u32)(iy + 18), it->name, 19,
                    sel ? XMB_WHITE : XMB_TEXT, sel);
            xmb_draw_meta((u32)sr_tx, (u32)(iy + 46), it, 14);
        }
    }
    if (count == 0 && g_search_buf[0]) {
        char msg[96];
        snprintf(msg, sizeof(msg), "No results for \"%s\"", g_search_buf);
        int mw = ttf_text_width(msg, 16);
        drawTTF((u32)(((int)display_width - mw) / 2), (u32)(results_y + 10),
                msg, 16, XMB_TEXT_FAINT);
    } else if (count > 0 && vis_r == 0) {
        // Hits exist but the keyboard leaves no room to list them (every mode
        // below 1080p).  Without this the screen looks identical to "no
        // results" and there is nothing to suggest pressing Down — which is
        // exactly how the invisible list went unnoticed.  One line fits in
        // the ~60px under the keyboard even at 480i.
        char msg[64];
        snprintf(msg, sizeof(msg), "%d result%s \xB7 press Down to browse",
                 count, count == 1 ? "" : "s");
        int mw = ttf_text_width(msg, 15);
        drawTTF((u32)(((int)display_width - mw) / 2), (u32)(results_y + 6),
                msg, 15, XMB_ACCENT);
    }
}

// CPU draws for search results list (selection highlight + scaled thumbs).
void xmb_cpu_draw_search_results(void) {
    int results_y = xmb_search_results_y();
    int count     = g_search_results_count;
    int vis_r     = xmb_search_vis_rows();
    int list_x = ((int)display_width - XMB_LIST_W) / 2;
    for (int i = 0; i < vis_r; i++) {
        int idx = g_search_scroll + i;
        if (idx >= count) break;
        int iy = results_y + i * XMB_ROW_STRIDE;
        if (g_search_focus_results && idx == g_search_sel) {
            drawRect((u32)list_x, (u32)iy,
                     (u32)XMB_LIST_W, (u32)XMB_ROW_H, XMB_PANEL_HI);
            drawRect((u32)(list_x - 4), (u32)iy,
                     3, (u32)XMB_ROW_H, XMB_ACCENT);
        }
        xmb_cpu_blit_thumb_scaled(g_search_results[idx].id,
                                  list_x + 16,
                                  iy + (XMB_ROW_H - XMB_THUMB_H) / 2,
                                  XMB_THUMB_W, XMB_THUMB_H);
    }
}
