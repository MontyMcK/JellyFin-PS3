// Card-grid rendering for the library tabs (Continue Watching, Movies, TV,
// Collections, and their sub-screens), plus thumbnail blits for the search
// results list.  Card images live in MAIN memory and are blitted to the
// framebuffer with the CPU after rsxSync — fast both ways (cached reads,
// write-combined framebuffer writes), and nothing for the GPU to wedge on.

#include <stdio.h>
#include <string.h>

#include "ui_render_internal.h"
#include "bitmap.h"
#include "thumbnail_cache.h"

// -------------------------------------------------------
// CPU blits: main-memory bitmap -> framebuffer
// -------------------------------------------------------

// 1:1 copy, clipped to the display.
static void cpu_blit_bitmap(const Bitmap *bm, int dx, int dy) {
    if (!bm || !bm->pixels) return;
    int w = (int)bm->width, h = (int)bm->height;
    for (int row = 0; row < h; row++) {
        int sy = dy + row;
        if (sy < 0 || sy >= (int)display_height) continue;
        int sx0 = dx, copy_w = w, src_x = 0;
        if (sx0 < 0) { src_x = -sx0; copy_w += sx0; sx0 = 0; }
        if (sx0 + copy_w > (int)display_width) copy_w = (int)display_width - sx0;
        if (copy_w <= 0) continue;
        memcpy(color_buffer[curr_fb] + (u32)sy * display_width + sx0,
               bm->pixels + (u32)row * bm->width + src_x,
               (size_t)copy_w * 4);
    }
}

// Nearest-neighbour downscale into a dw x dh rect (search thumbs).
static void cpu_blit_bitmap_scaled(const Bitmap *bm, int dx, int dy,
                                   int dw, int dh) {
    if (!bm || !bm->pixels || dw <= 0 || dh <= 0) return;
    for (int row = 0; row < dh; row++) {
        int sy = dy + row;
        if (sy < 0 || sy >= (int)display_height) continue;
        const u32 *src = bm->pixels +
            ((u32)((u64)row * bm->height / dh)) * bm->width;
        u32 *dst = color_buffer[curr_fb] + (u32)sy * display_width;
        for (int col = 0; col < dw; col++) {
            int sx = dx + col;
            if (sx < 0 || sx >= (int)display_width) continue;
            dst[sx] = src[(u32)((u64)col * bm->width / dw)];
        }
    }
}

// Search results: small scaled thumb, dim placeholder while loading.
// Reuses the Movies-tab poster size so a thumb cached from browsing is
// shared with the search list.
void xmb_cpu_blit_thumb_scaled(const char *item_id, int x, int y,
                               int w, int h) {
    GridGeom gg;
    xmb_grid_geom(XMB_TAB_MOVIES, &gg);
    thumb_request(item_id, gg.card_w, gg.card_h);
    const Bitmap *bm = thumb_get(item_id, gg.card_w, gg.card_h);
    if (bm)
        cpu_blit_bitmap_scaled(bm, x, y, w, h);
    else
        drawRect((u32)x, (u32)y, (u32)w, (u32)h, XMB_THUMB_DIM);
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

// Draw text clipped to max_w pixels, ".." appended when truncated.
static void draw_ttf_clipped(u32 x, u32 y, const char *text, float px,
                             u32 color, int max_w, bool bold = false) {
    if (ttf_text_width(text, px) <= max_w) {
        drawTTF(x, y, text, px, color, bold);
        return;
    }
    char buf[128];
    snprintf(buf, sizeof(buf), "%s", text);
    int len = (int)strlen(buf);
    while (len > 1) {
        buf[--len] = '\0';
        char trial[132];
        snprintf(trial, sizeof(trial), "%s..", buf);
        if (ttf_text_width(trial, px) <= max_w) {
            drawTTF(x, y, trial, px, color, bold);
            return;
        }
    }
}

// -------------------------------------------------------
// Card grid
// -------------------------------------------------------

// Poster screens (Movies, TV series/seasons, Collections) get portrait
// cards; Continue Watching and episode lists get landscape stills.
bool xmb_tab_uses_portrait(int tab) {
    if (tab == XMB_TAB_RESUME) return false;
    if (tab == XMB_TAB_TV && g_tv_depth == 2) return false;   // episodes
    return true;
}

void xmb_grid_geom(int tab, GridGeom *gg) {
    gg->portrait = xmb_tab_uses_portrait(tab);
    int card_h = XMB_CARD_H_FIT;
    if (gg->portrait) {
        gg->cols   = XMB_PORTRAIT_COLS;
        gg->card_h = card_h;
        gg->card_w = card_h * 2 / 3;
    } else {
        gg->cols = XMB_LANDSCAPE_COLS;
        int w = card_h * 16 / 9;
        if (w > XMB_CARD_W_CAP) w = XMB_CARD_W_CAP;
        gg->card_w = w;
        gg->card_h = w * 9 / 16;
    }
    gg->vis    = gg->cols * XMB_GRID_ROWS;
    gg->stride = gg->card_h + XMB_CARD_TEXT_H + 6;
    gg->grid_w = gg->cols * gg->card_w + (gg->cols - 1) * XMB_CARD_GAP_X;
    gg->x0     = ((int)display_width - gg->grid_w) / 2;
}

static void grid_cell_pos(const GridGeom *gg, int vis_idx, int y0,
                          int *cx, int *cy) {
    *cx = gg->x0 + (vis_idx % gg->cols) * (gg->card_w + XMB_CARD_GAP_X);
    *cy = y0     + (vis_idx / gg->cols) * gg->stride;
}

// CPU phase (after rsxSync): card images, placeholders, selection border,
// progress strips.
void xmb_grid_cpu(const GridGeom *gg, const XMBItem *items, int count,
                  int sel, int scroll, int y0) {
    for (int i = 0; i < gg->vis; i++) {
        int idx = scroll + i;
        if (idx >= count) break;
        int cx, cy;
        grid_cell_pos(gg, i, y0, &cx, &cy);

        thumb_request(items[idx].id, gg->card_w, gg->card_h);
        const Bitmap *bm = thumb_get(items[idx].id, gg->card_w, gg->card_h);
        if (bm)
            cpu_blit_bitmap(bm, cx, cy);
        else
            drawRect((u32)cx, (u32)cy, (u32)gg->card_w, (u32)gg->card_h,
                     XMB_THUMB_DIM);

        // Watched-progress strip along the bottom edge of the card.
        if (items[idx].progress_pct > 0) {
            int bar_y = cy + gg->card_h - 5;
            int fill  = (gg->card_w * items[idx].progress_pct) / 100;
            drawRect((u32)cx, (u32)bar_y, (u32)gg->card_w, 5, XMB_BADGE_BG);
            if (fill > 0)
                drawRect((u32)cx, (u32)bar_y, (u32)fill, 5, XMB_ACCENT);
        }

        // Selection: accent frame around the card.
        if (idx == sel) {
            const int T = 4;   // frame thickness
            drawRect((u32)(cx - T), (u32)(cy - T),
                     (u32)(gg->card_w + 2*T), (u32)T, XMB_ACCENT);
            drawRect((u32)(cx - T), (u32)(cy + gg->card_h),
                     (u32)(gg->card_w + 2*T), (u32)T, XMB_ACCENT);
            drawRect((u32)(cx - T), (u32)cy,
                     (u32)T, (u32)gg->card_h, XMB_ACCENT);
            drawRect((u32)(cx + gg->card_w), (u32)cy,
                     (u32)T, (u32)gg->card_h, XMB_ACCENT);
        }
    }

    // Prefetch the next page of thumbs past the visible window so paging
    // down shows images instead of placeholders.  Queued after the visible
    // requests, so they never delay what's on screen.
    for (int i = 0; i < gg->vis; i++) {
        int idx = scroll + gg->vis + i;
        if (idx >= count) break;
        thumb_request(items[idx].id, gg->card_w, gg->card_h);
    }
}

// Phase 3: titles under every visible card (selected one bigger/bold,
// with a meta line), scroll arrows.
void xmb_grid_text(const GridGeom *gg, const XMBItem *items, int count,
                   int sel, int scroll, int y0, bool more_below) {
    // Dimmed title under every non-selected card so the user always sees
    // what each card is.
    for (int i = 0; i < gg->vis; i++) {
        int idx = scroll + i;
        if (idx >= count) break;
        if (idx == sel) continue;
        int cx, cy;
        grid_cell_pos(gg, i, y0, &cx, &cy);
        draw_ttf_clipped((u32)cx, (u32)(cy + gg->card_h + 8),
                         items[idx].name, 17, 0x009999AAUL, gg->card_w);
    }

    if (sel >= scroll && sel < scroll + gg->vis && sel < count) {
        const XMBItem *it = &items[sel];
        int cx, cy;
        grid_cell_pos(gg, sel - scroll, y0, &cx, &cy);
        int ty = cy + gg->card_h + 6;
        draw_ttf_clipped((u32)cx, (u32)ty, it->name, 21,
                         0x00FFFFFF, gg->card_w, true);
        char meta[96] = "";
        if (it->year_str[0])     snprintf(meta, sizeof(meta), "%s", it->year_str);
        if (it->duration_str[0]) {
            if (meta[0]) strncat(meta, " . ", sizeof(meta)-strlen(meta)-1);
            strncat(meta, it->duration_str, sizeof(meta)-strlen(meta)-1);
        }
        if (it->genre[0]) {
            if (meta[0]) strncat(meta, " . ", sizeof(meta)-strlen(meta)-1);
            strncat(meta, it->genre, sizeof(meta)-strlen(meta)-1);
        }
        if (it->codec[0]) {
            if (meta[0]) strncat(meta, " . ", sizeof(meta)-strlen(meta)-1);
            strncat(meta, it->codec, sizeof(meta)-strlen(meta)-1);
        }
        if (meta[0])
            draw_ttf_clipped((u32)cx, (u32)(ty + 28), meta, 14,
                             0x00AAAAAAUL, gg->card_w);
    }

    int ax = gg->x0 + gg->grid_w + 12;
    if (scroll > 0)
        drawTTF((u32)ax, (u32)y0, "^", 12, 0x00FFFFFF);
    if (more_below)
        drawTTF((u32)ax, (u32)(y0 + XMB_GRID_ROWS * gg->stride - 40),
                "v", 12, 0x00FFFFFF);
}

