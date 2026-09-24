// Search tab rendering — search field, OSK (CPU key cells + RSX labels)
// and the results list below it.

#include <stdio.h>
#include <string.h>

#include "ui_render_internal.h"
#include "thumbnail_cache.h"
#include "timing.h"
#include "ui_tab_anim.h"

#define OSK_FIELD_H UIS_H(40)

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

// The key highlight glides between keys (focus_glide) instead of the selected
// key simply changing colour.  Keys are drawn plain, then one highlight block
// over the selection; the rect drawn is kept here so the label pass can darken
// each label by how much of its key the highlight is covering right now --
// otherwise the new key's label would go dark before the block arrived.
static int s_hl_x, s_hl_y, s_hl_w, s_hl_h;
static bool s_hl_on = false;

// Fraction of the key [kx,ky,kw,kh] under the highlight, 0..1.
static float osk_hl_cover(int kx, int ky, int kw, int kh) {
    if (!s_hl_on || kw <= 0 || kh <= 0) return 0.0f;
    int x0 = kx > s_hl_x ? kx : s_hl_x;
    int y0 = ky > s_hl_y ? ky : s_hl_y;
    int x1 = (kx + kw) < (s_hl_x + s_hl_w) ? (kx + kw) : (s_hl_x + s_hl_w);
    int y1 = (ky + kh) < (s_hl_y + s_hl_h) ? (ky + kh) : (s_hl_y + s_hl_h);
    if (x1 <= x0 || y1 <= y0) return 0.0f;
    return (float)((x1 - x0) * (y1 - y0)) / (float)(kw * kh);
}

static u32 osk_mix(u32 a, u32 b, float t) {
    if (t <= 0.0f) return a;
    if (t >= 1.0f) return b;
    u32 out = 0;
    for (int sh = 0; sh <= 16; sh += 8) {
        const float ca = (float)((a >> sh) & 0xFF), cb = (float)((b >> sh) & 0xFF);
        out |= (u32)(ca + (cb - ca) * t + 0.5f) << sh;
    }
    return out;
}

void xmb_cpu_draw_osk(void) {
    int W = (int)display_width;
    int total_w = 10 * OSK_STEP_X - OSK_GAP;
    int osk_x0  = (W - total_w) / 2 + tab_anim_content_dx();

    // Search field: dark well with an accent underline.
    drawRect((u32)osk_x0, (u32)(XMB_CONTENT_Y + UIS_H(8)),
             (u32)total_w, OSK_FIELD_H, XMB_TRACK);
    drawRect((u32)osk_x0, (u32)(XMB_CONTENT_Y + UIS_H(8) + OSK_FIELD_H - UIS_H(2)),
             (u32)total_w, UIS_H(2), XMB_ACCENT);

    s_hl_on = false;
    if (g_search_focus_results) return;   // collapsed: field only, no keys

    int hx = 0, hy = 0, hw = 0, hh = 0;
    for (int r = 0; r <= OSK_ROWS_N; r++) {
        if (r == OSK_ROWS_N) {
            int space_w = 5 * OSK_STEP_X - OSK_GAP;
            int sy = OSK_Y0 + r * OSK_STEP_Y;
            drawRect((u32)osk_x0, (u32)sy, (u32)space_w, OSK_KEY_H, XMB_KEY_NORMAL);
            int bsx = osk_x0 + space_w + OSK_GAP;
            drawRect((u32)bsx, (u32)sy, OSK_KEY_W, OSK_KEY_H, XMB_KEY_NORMAL);
            int clx = bsx + OSK_KEY_W + OSK_GAP;
            drawRect((u32)clx, (u32)sy, OSK_KEY_W, OSK_KEY_H, XMB_KEY_NORMAL);
            if (r == g_osk_row) {
                hy = sy; hh = OSK_KEY_H;
                if      (g_osk_col == 0) { hx = osk_x0; hw = space_w; }
                else if (g_osk_col == 1) { hx = bsx;    hw = OSK_KEY_W; }
                else                     { hx = clx;    hw = OSK_KEY_W; }
            }
        } else {
            const char **rows = g_osk_sym ? OSK_SYMBOLS : OSK_LETTERS;
            int rlen = (int)strlen(rows[r]);
            if (r == OSK_ROWS_N - 1) rlen++;
            int ry = OSK_Y0 + r * OSK_STEP_Y;
            for (int c = 0; c < rlen; c++) {
                drawRect((u32)(osk_x0 + c * OSK_STEP_X), (u32)ry,
                         OSK_KEY_W, OSK_KEY_H, XMB_KEY_NORMAL);
                if (r == g_osk_row && c == g_osk_col) {
                    hx = osk_x0 + c * OSK_STEP_X; hy = ry;
                    hw = OSK_KEY_W; hh = OSK_KEY_H;
                }
            }
        }
    }

    if (hw > 0) {
        // Snaps when the layout changes (letters <-> symbols).
        focus_glide(0x7EEE0000 | (g_osk_sym ? 1 : 0), &hx, &hy, &hw, &hh);
        drawRect((u32)hx, (u32)hy, (u32)hw, (u32)hh, XMB_KEY_SEL);
        s_hl_x = hx; s_hl_y = hy; s_hl_w = hw; s_hl_h = hh; s_hl_on = true;
    }
}

// One key label, centered in a key cell, darkened by how much of the key the
// (gliding) highlight covers -- selected keys are white.
static void osk_key_label(int kx, int kw, int ry, const char *lbl) {
    const float px = UIS_TF(20.0f);
    int lw = ttf_text_width(lbl, px);
    int ty = ry + (OSK_KEY_H - (int)px) / 2 - UIS_H(2);
    drawTTF((u32)(kx + (kw - lw) / 2), (u32)(ty > 0 ? ty : 0), lbl, px,
            osk_mix(XMB_TEXT, XMB_KEY_LABEL_SEL, osk_hl_cover(kx, ry, kw, OSK_KEY_H)));
}

void xmb_rsx_draw_osk(void) {
    int W = (int)display_width;
    int total_w = 10 * OSK_STEP_X - OSK_GAP;
    int osk_x0  = (W - total_w) / 2 + tab_anim_content_dx();

    u64 us = timing_get_us();
    bool cursor = ((us / 500000) & 1) == 0;

    // Search field content: magnifier glyph, then typed text or ghost prompt.
    {
        int fx = osk_x0 + UIS_W(14);
        int fy = XMB_CONTENT_Y + UIS_H(8);
        drawIcon((u32)fx, (u32)(fy + (OSK_FIELD_H - UIS_H(20)) / 2), ICON_SEARCH, UIS_TF(20.0f),
                 XMB_TEXT_FAINT);
        int tx = fx + UIS_W(30);
        int ty = fy + (OSK_FIELD_H - UIS_H(18)) / 2 - UIS_H(2);
        if (g_search_buf[0]) {
            char disp[68];
            snprintf(disp, sizeof(disp), "%s%s", g_search_buf, cursor ? "_" : "");
            drawTTF((u32)tx, (u32)ty, disp, UIS_TF(18), XMB_TEXT);
        } else {
            if (cursor) drawTTF((u32)tx, (u32)ty, "_", UIS_TF(18), XMB_TEXT);
            drawTTF((u32)(tx + UIS_W(14)), (u32)ty, "Search your library", UIS_TF(17),
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
            osk_key_label(osk_x0, space_w, ry, "Space");
            {   // Backspace: Material icon, centered in the key.
                drawIcon((u32)(bsx + (OSK_KEY_W - UIS_W(22)) / 2),
                         (u32)(ry + (OSK_KEY_H - UIS_H(22)) / 2), ICON_BACKSPACE, UIS_TF(22.0f),
                         osk_mix(XMB_TEXT, XMB_KEY_LABEL_SEL,
                                 osk_hl_cover(bsx, ry, OSK_KEY_W, OSK_KEY_H)));
            }
            osk_key_label(clx, OSK_KEY_W, ry, "Clear");
        } else {
            const char **rows = g_osk_sym ? OSK_SYMBOLS : OSK_LETTERS;
            const char  *row  = rows[r];
            int base_len = strlen(row);

            for (int c = 0; c < base_len; c++) {
                char label[2] = { row[c], '\0' };
                osk_key_label(osk_x0 + c * OSK_STEP_X, OSK_KEY_W, ry, label);
            }
            if (r == OSK_ROWS_N - 1) {
                osk_key_label(osk_x0 + base_len * OSK_STEP_X, OSK_KEY_W, ry,
                              g_osk_sym ? "ABC" : "#+=");
            }
        }
    }

    int results_y = xmb_search_results_y();
    int count     = g_search_results_count;
    int vis_r     = xmb_search_vis_rows();
    {
        int sr_list_x = ((int)display_width - XMB_LIST_W) / 2 + tab_anim_content_dx();
        int sr_tx     = sr_list_x + UIS_W(16) + XMB_THUMB_W + UIS_W(16);
        for (int i = 0; i < vis_r; i++) {
            int idx = g_search_scroll + i;
            if (idx >= count) break;
            const XMBItem *it = &g_search_results[idx];
            int iy = results_y + i * XMB_ROW_STRIDE;
            bool sel = g_search_focus_results && idx == g_search_sel;
            drawTTF((u32)sr_tx, (u32)(iy + UIS_H(18)), it->name, UIS_TF(19),
                    sel ? XMB_WHITE : XMB_TEXT, sel);
            xmb_draw_meta((u32)sr_tx, (u32)(iy + UIS_H(46)), it, UIS_TF(14));
        }
    }
    // Only a real query gets to say "No results".  A term that is too short
    // was never sent, and one inside the typing pause or out on the search
    // worker (this server takes 2.5-8.4 s to answer) is about to be; both
    // used to print "No results" and made a working search look dead.
    if (count == 0 && g_search_buf[0] && g_search_state == SEARCH_TOO_SHORT) {
        char msg[64];
        snprintf(msg, sizeof(msg), "Type at least %d letters", SEARCH_MIN_CHARS);
        int mw = ttf_text_width(msg, UIS_TF(16));
        drawTTF((u32)(((int)display_width - mw) / 2), (u32)(results_y + UIS_H(10)),
                msg, UIS_TF(16), XMB_TEXT_DIM);
    } else if (count == 0 && g_search_buf[0] &&
               (g_search_state == SEARCH_PENDING ||
                g_search_state == SEARCH_QUERYING || xmb_search_in_flight())) {
        char msg[96];
        snprintf(msg, sizeof(msg), "Searching for \"%s\"...", g_search_buf);
        int mw = ttf_text_width(msg, UIS_TF(16));
        drawTTF((u32)(((int)display_width - mw) / 2), (u32)(results_y + UIS_H(10)),
                msg, UIS_TF(16), XMB_TEXT_DIM);
    } else if (count == 0 && g_search_buf[0] && g_search_state == SEARCH_DONE) {
        char msg[96];
        snprintf(msg, sizeof(msg), "No results for \"%s\"", g_search_buf);
        int mw = ttf_text_width(msg, UIS_TF(16));
        drawTTF((u32)(((int)display_width - mw) / 2), (u32)(results_y + UIS_H(10)),
                msg, UIS_TF(16), XMB_TEXT_FAINT);
    } else if (count > 0 && vis_r == 0) {
        // Hits exist but the keyboard leaves no room to list them (every mode
        // below 1080p).  Without this the screen looks identical to "no
        // results" and there is nothing to suggest pressing Down — which is
        // exactly how the invisible list went unnoticed.  One line fits in
        // the ~60px under the keyboard even at 480i.
        char msg[64];
        snprintf(msg, sizeof(msg), "%d result%s \xC2\xB7 press Down to browse",
                 count, count == 1 ? "" : "s");
        int mw = ttf_text_width(msg, UIS_TF(15));
        drawTTF((u32)(((int)display_width - mw) / 2), (u32)(results_y + UIS_H(6)),
                msg, UIS_TF(15), XMB_ACCENT);
    }
}

// CPU draws for search results list (selection highlight + scaled thumbs).
void xmb_cpu_draw_search_results(void) {
    int results_y = xmb_search_results_y();
    int count     = g_search_results_count;
    int vis_r     = xmb_search_vis_rows();
    int list_x = ((int)display_width - XMB_LIST_W) / 2 + tab_anim_content_dx();
    for (int i = 0; i < vis_r; i++) {
        int idx = g_search_scroll + i;
        if (idx >= count) break;
        int iy = results_y + i * XMB_ROW_STRIDE;
        if (g_search_focus_results && idx == g_search_sel) {
            drawRect((u32)list_x, (u32)iy,
                     (u32)XMB_LIST_W, (u32)XMB_ROW_H, XMB_PANEL_HI);
            drawRect((u32)(list_x - UIS_W(4)), (u32)iy,
                     UIS_W(3), (u32)XMB_ROW_H, XMB_ACCENT);
        }
        xmb_cpu_blit_thumb_scaled(g_search_results[idx].id,
                                  list_x + UIS_W(16),
                                  iy + (XMB_ROW_H - XMB_THUMB_H) / 2,
                                  XMB_THUMB_W, XMB_THUMB_H);
    }
}
