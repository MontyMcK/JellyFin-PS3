// Item list rendering — main library lists plus the TV (seasons/episodes)
// and Collections sub-lists.  Each list draws in two phases: CPU rects and
// thumbnails first (after rsxSync), then TTF text on top.

#include <stdio.h>
#include <string.h>

#include "ui_render_internal.h"
#include "bitmap.h"
#include "thumbnail_cache.h"

// Blit one cached thumbnail into the framebuffer at (x, y); dim placeholder
// rectangle while the cache fills.
void xmb_blit_thumb(const char *item_id, int x, int y) {
    thumb_request(item_id);
    const Bitmap *th = thumb_get(item_id);
    if (!th) {
        drawRect((u32)x, (u32)y, XMB_THUMB_W, XMB_THUMB_H, XMB_THUMB_DIM);
        return;
    }
    u32 *fb = color_buffer[curr_fb];
    int stride = (int)display_width;
    for (int row = 0; row < (int)XMB_THUMB_H; row++) {
        int sy = y + row;
        if (sy < 0 || sy >= (int)display_height) continue;
        for (int col = 0; col < (int)XMB_THUMB_W; col++) {
            int sx = x + col;
            if (sx < 0 || sx >= (int)display_width) continue;
            fb[sy * stride + sx] = th->pixels[row * XMB_THUMB_W + col];
        }
    }
}

// Draw the meta string "year . duration . genre" at (x, y).
void xmb_draw_meta(u32 x, u32 y, const XMBItem *it, float px) {
    char meta[64] = "";
    if (it->year_str[0])     { snprintf(meta, sizeof(meta), "%s", it->year_str); }
    if (it->duration_str[0]) {
        if (meta[0]) strncat(meta, " . ", sizeof(meta)-strlen(meta)-1);
        strncat(meta, it->duration_str, sizeof(meta)-strlen(meta)-1);
    }
    if (it->genre[0]) {
        if (meta[0]) strncat(meta, " . ", sizeof(meta)-strlen(meta)-1);
        strncat(meta, it->genre, sizeof(meta)-strlen(meta)-1);
    }
    if (meta[0]) drawTTF(x, y, meta, px, 0x00FFFFFF);
}

// -------------------------------------------------------
// Shared row helpers.  Every list view is the same centered narrow
// list; only the backing array, scroll state, and origin differ.
// -------------------------------------------------------

// CPU phase for one row: selection highlight, thumbnail, codec badge bg.
static void xmb_cpu_draw_row(const XMBItem *it, int list_x, int iy, bool selected) {
    if (selected) {
        drawRect((u32)list_x, (u32)iy,
                 (u32)XMB_LIST_W, (u32)XMB_ROW_H, XMB_HIGHLIGHT);
        drawRect((u32)(list_x - 5), (u32)iy, 4, (u32)XMB_ROW_H, XMB_ACCENT);
    }
    xmb_blit_thumb(it->id, list_x + 16, iy + (XMB_ROW_H - XMB_THUMB_H) / 2);

    int badge_x = list_x + XMB_LIST_W - 80;
    int badge_y = iy + (XMB_ROW_H - 22) / 2;
    drawRect((u32)badge_x, (u32)badge_y, 66, 22, XMB_BADGE_BG);
}

// TTF phase for one row: title, meta line, codec badge text.
static void xmb_text_draw_row(const XMBItem *it, int list_x, int iy) {
    int tx = list_x + 16 + XMB_THUMB_W + 16;
    drawTTF((u32)tx, (u32)(iy + 20), it->name, 22, 0x00FFFFFF);
    xmb_draw_meta((u32)tx, (u32)(iy + 50), it);
    if (it->codec[0]) {
        int bx = list_x + XMB_LIST_W - 75;
        int by = iy + (XMB_ROW_H - 22) / 2 + 4;
        drawTTF((u32)bx, (u32)by, it->codec, 13, 0x00FFFFFF);
    }
}

// More-content arrows: "^" on the top row when scrolled, "v" on the bottom
// row when more rows (or more pages) exist below.
static void xmb_draw_scroll_arrows(int i, int vis, int iy,
                                   int scroll, int count, bool more_pages) {
    if (i == 0 && scroll > 0)
        drawTTF(display_width - 20, (u32)iy, "^", 8, 0x00FFFFFF);
    if (i == vis-1 && (scroll + vis < count || more_pages))
        drawTTF(display_width - 20, (u32)iy, "v", 8, 0x00FFFFFF);
}

// -------------------------------------------------------
// Main library list (Movies / TV series / Collections)
// -------------------------------------------------------

void xmb_cpu_draw_items(int tab) {
    int count  = g_item_count[tab];
    int vis    = XMB_ITEMS_VIS;
    int list_x = ((int)display_width - XMB_LIST_W) / 2;

    for (int i = 0; i < vis; i++) {
        int idx = g_scroll_top + i;
        if (idx >= count) break;
        int iy = XMB_CONTENT_Y + i * XMB_ROW_STRIDE;
        xmb_cpu_draw_row(&g_items[tab][idx], list_x, iy, idx == g_sel);
    }
}

void xmb_draw_item_list(int tab) {
    int count  = g_item_count[tab];
    int vis    = XMB_ITEMS_VIS;
    int list_x = ((int)display_width - XMB_LIST_W) / 2;

    for (int i = 0; i < vis; i++) {
        int idx = g_scroll_top + i;
        if (idx >= count) break;
        int iy = XMB_CONTENT_Y + i * XMB_ROW_STRIDE;
        xmb_text_draw_row(&g_items[tab][idx], list_x, iy);
        xmb_draw_scroll_arrows(i, vis, iy, g_scroll_top, count,
                               g_tab_start[tab] + count < g_tab_total[tab]);
    }
}

// -------------------------------------------------------
// TV sub-list (seasons or episodes)
// -------------------------------------------------------

void xmb_cpu_draw_sub(void) {
    int vis    = XMB_ITEMS_VIS;
    int list_x = ((int)display_width - XMB_LIST_W) / 2;
    int y0     = XMB_CONTENT_Y + 26;
    for (int i = 0; i < vis; i++) {
        int idx = g_tv_sub_scroll + i;
        if (idx >= g_tv_sub_count) break;
        int iy = y0 + i * XMB_ROW_STRIDE;
        xmb_cpu_draw_row(&g_tv_sub_items[idx], list_x, iy, idx == g_tv_sub_sel);
    }
}

void xmb_draw_sub_list(void) {
    int vis    = XMB_ITEMS_VIS;
    int list_x = ((int)display_width - XMB_LIST_W) / 2;
    int y0     = XMB_CONTENT_Y + 26;
    for (int i = 0; i < vis; i++) {
        int idx = g_tv_sub_scroll + i;
        if (idx >= g_tv_sub_count) break;
        int iy = y0 + i * XMB_ROW_STRIDE;
        xmb_text_draw_row(&g_tv_sub_items[idx], list_x, iy);
        xmb_draw_scroll_arrows(i, vis, iy, g_tv_sub_scroll, g_tv_sub_count,
                               g_tv_sub_start + g_tv_sub_count < g_tv_sub_total);
    }
}

// -------------------------------------------------------
// Collections sub-list (movies inside a collection)
// -------------------------------------------------------

void xmb_cpu_draw_col_sub(void) {
    int vis    = XMB_ITEMS_VIS;
    int list_x = ((int)display_width - XMB_LIST_W) / 2;
    int y0     = XMB_CONTENT_Y + 26;
    for (int i = 0; i < vis; i++) {
        int idx = g_col_sub_scroll + i;
        if (idx >= g_col_sub_count) break;
        int iy = y0 + i * XMB_ROW_STRIDE;
        xmb_cpu_draw_row(&g_col_sub_items[idx], list_x, iy, idx == g_col_sub_sel);
    }
}

void xmb_draw_col_sub_list(void) {
    int vis    = XMB_ITEMS_VIS;
    int list_x = ((int)display_width - XMB_LIST_W) / 2;
    int y0     = XMB_CONTENT_Y + 26;
    for (int i = 0; i < vis; i++) {
        int idx = g_col_sub_scroll + i;
        if (idx >= g_col_sub_count) break;
        int iy = y0 + i * XMB_ROW_STRIDE;
        xmb_text_draw_row(&g_col_sub_items[idx], list_x, iy);
        xmb_draw_scroll_arrows(i, vis, iy, g_col_sub_scroll, g_col_sub_count,
                               g_col_sub_start + g_col_sub_count < g_col_sub_total);
    }
}
