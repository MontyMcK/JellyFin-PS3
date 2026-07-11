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
        if (cpu_row_clipped(sy)) continue;
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
        if (cpu_row_clipped(sy)) continue;
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

// Blit an item's thumb at w x h (Now Playing album art).  The cache's slot
// buffers are sized for grid cards and thumb_request silently DROPS bigger
// requests — so anything over the slot budget is fetched at the cap and
// nearest-neighbour upscaled here instead of never appearing at all.
// Returns false while the cache is still fetching — the caller draws its
// own placeholder.
bool xmb_cpu_blit_thumb(const char *item_id, int x, int y, int w, int h) {
    int rw = w, rh = h;
    int cap = thumb_max_square();
    if (rw > cap || rh > cap || (size_t)rw * rh > (size_t)cap * cap) {
        rw = rw < cap ? rw : cap;
        rh = rh < cap ? rh : cap;
    }
    thumb_request(item_id, rw, rh);
    const Bitmap *bm = thumb_get(item_id, rw, rh);
    if (!bm) return false;
    if (rw == w && rh == h) cpu_blit_bitmap(bm, x, y);
    else                    cpu_blit_bitmap_scaled(bm, x, y, w, h);
    return true;
}

// Middle-dot separator ("2014 · 2h 49m · Sci-Fi").  \xB7 is U+00B7 in
// Open Sans — drawTTF treats bytes as Latin-1 codepoints, so it just works.
#define META_SEP " \xB7 "

// Draw the meta string "year · duration · genre" at (x, y).
void xmb_draw_meta(u32 x, u32 y, const XMBItem *it, float px) {
    char meta[64] = "";
    if (it->year_str[0])     { snprintf(meta, sizeof(meta), "%s", it->year_str); }
    if (it->duration_str[0]) {
        if (meta[0]) strncat(meta, META_SEP, sizeof(meta)-strlen(meta)-1);
        strncat(meta, it->duration_str, sizeof(meta)-strlen(meta)-1);
    }
    if (it->genre[0]) {
        if (meta[0]) strncat(meta, META_SEP, sizeof(meta)-strlen(meta)-1);
        strncat(meta, it->genre, sizeof(meta)-strlen(meta)-1);
    }
    if (meta[0]) drawTTF(x, y, meta, px, XMB_TEXT_DIM);
}

// Draw text clipped to max_w pixels, ".." appended when truncated.
static void draw_ttf_clipped(u32 x, u32 y, const char *text, float px,
                             u32 color, int max_w, bool bold = false) {
    if (ttf_text_width(text, px, bold) <= max_w) {
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
        if (ttf_text_width(trial, px, bold) <= max_w) {
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
    // Music: square album covers under the sub-tab header, with a taller
    // text band for the title + artist (+ meta on the selected card).
    if (tab == XMB_TAB_MUSIC) {
        gg->portrait = false;
        gg->cols     = XMB_MUSIC_COLS;
        int card = (XMB_GRID_AVAIL_H - XMB_MUSIC_SUBTAB_H) / XMB_GRID_ROWS
                   - XMB_MUSIC_TEXT_H - 6;
        gg->card_w = gg->card_h = card;
        gg->vis    = gg->cols * XMB_GRID_ROWS;
        gg->stride = card + XMB_MUSIC_TEXT_H + 6;
        gg->grid_w = gg->cols * card + (gg->cols - 1) * XMB_CARD_GAP_X;
        gg->x0     = ((int)display_width - gg->grid_w) / 2;
        return;
    }

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

// Colored letter tile: stable per-item hash picks one of eight muted
// panel colors, the first letter of the name sits centered and bold.
// Used as the music placeholder here and by the Now Playing screen for
// the art panel + Up Next thumbs.
void xmb_draw_letter_tile(const char *seed, const char *name,
                          int x, int y, int size) {
    static const u32 TILE_COLORS[8] = {
        0x001E4433UL,   // deep green
        0x006E4A1AUL,   // amber brown
        0x005C1F2AUL,   // dark red
        0x001D2C55UL,   // navy
        0x00174449UL,   // teal
        0x0044224EUL,   // plum
        0x002E3358UL,   // slate
        0x004A441EUL,   // olive
    };
    u32 h = 2166136261u;
    for (const char *p = seed; p && *p; p++) h = (h ^ (u8)*p) * 16777619u;
    drawRect((u32)x, (u32)y, (u32)size, (u32)size, TILE_COLORS[h & 7]);

    char letter[2] = { '?', 0 };
    if (name && name[0]) {
        letter[0] = name[0];
        if (letter[0] >= 'a' && letter[0] <= 'z') letter[0] -= 32;
    }
    float px = (float)size * 0.52f;
    int   lw = ttf_text_width(letter, px, true);
    drawTTF((u32)(x + (size - lw) / 2),
            (u32)(y + (size - (int)px) / 2 - (int)(px * 0.08f)),
            letter, px, 0x00D8DCEBUL, true);
}

// Draw one card at (cx,cy): cached image (or a dim placeholder while it
// loads), the watched-progress strip, and — when selected — a thin white
// frame with a soft 1px halo.  Shared by the grid and the Home shelf.
void xmb_draw_card(const char *item_id, int cx, int cy, int card_w, int card_h,
                   u8 progress_pct, bool selected, const char *tile_name) {
    thumb_request(item_id, card_w, card_h);
    const Bitmap *bm = thumb_get(item_id, card_w, card_h);
    if (bm) {
        cpu_blit_bitmap(bm, cx, cy);
    } else if (tile_name) {
        // Music cards: colored letter tile (matches the Now Playing art).
        xmb_draw_letter_tile(item_id, tile_name, cx, cy,
                             card_w < card_h ? card_w : card_h);
    } else {
        // Loading placeholder: dark panel with a faint image glyph.
        drawRect((u32)cx, (u32)cy, (u32)card_w, (u32)card_h, XMB_THUMB_DIM);
        const float ph_px = 32.0f;
        drawIcon((u32)(cx + (card_w - (int)ph_px) / 2),
                 (u32)(cy + (card_h - (int)ph_px) / 2),
                 0xE3F4, ph_px, 0x00262C4EUL);
    }

    // Watched-progress strip along the bottom edge of the card.
    if (progress_pct > 0) {
        int bar_y = cy + card_h - 4;
        int fill  = (card_w * progress_pct) / 100;
        drawRect((u32)cx, (u32)bar_y, (u32)card_w, 4, XMB_TRACK);
        if (fill > 0)
            drawRect((u32)cx, (u32)bar_y, (u32)fill, 4, XMB_ACCENT);
    }

    if (selected) {
        const int T = 2, G = 2, O = G + T;
        int w = card_w, h = card_h;
        drawRect((u32)(cx - O), (u32)(cy - O), (u32)(w + 2*O), T, XMB_KEY_SEL);
        drawRect((u32)(cx - O), (u32)(cy + h + G), (u32)(w + 2*O), T, XMB_KEY_SEL);
        drawRect((u32)(cx - O), (u32)(cy - G), T, (u32)(h + 2*G), XMB_KEY_SEL);
        drawRect((u32)(cx + w + G), (u32)(cy - G), T, (u32)(h + 2*G), XMB_KEY_SEL);
        drawRectBlend((u32)(cx - O - 1), (u32)(cy - O - 1), (u32)(w + 2*O + 2), 1, XMB_KEY_SEL, 70);
        drawRectBlend((u32)(cx - O - 1), (u32)(cy + h + O), (u32)(w + 2*O + 2), 1, XMB_KEY_SEL, 70);
        drawRectBlend((u32)(cx - O - 1), (u32)(cy - O), 1, (u32)(h + 2*O), XMB_KEY_SEL, 70);
        drawRectBlend((u32)(cx + w + O), (u32)(cy - O), 1, (u32)(h + 2*O), XMB_KEY_SEL, 70);
    }
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
        const char *ty = items[idx].type;
        bool music = strcmp(ty, "MusicAlbum")  == 0 ||
                     strcmp(ty, "Audio")       == 0 ||
                     strcmp(ty, "MusicArtist") == 0 ||
                     strcmp(ty, "MusicGenre")  == 0 ||
                     strcmp(ty, "Playlist")    == 0;
        xmb_draw_card(items[idx].id, cx, cy, gg->card_w, gg->card_h,
                      items[idx].progress_pct, idx == sel,
                      music ? items[idx].name : NULL);
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
// with a meta line), plus a scrollbar showing the window's place in the
// whole library.  abs_start is the server index of items[0] (sliding
// pagination), abs_total the library's full count (0 = just use count).
void xmb_grid_text(const GridGeom *gg, const XMBItem *items, int count,
                   int sel, int scroll, int y0, bool more_below,
                   int abs_start, int abs_total) {
    (void)more_below;
    // Dimmed title under every non-selected card so the user always sees
    // what each card is.
    for (int i = 0; i < gg->vis; i++) {
        int idx = scroll + i;
        if (idx >= count) break;
        if (idx == sel) continue;
        int cx, cy;
        grid_cell_pos(gg, i, y0, &cx, &cy);
        draw_ttf_clipped((u32)cx, (u32)(cy + gg->card_h + 8),
                         items[idx].name, 16, XMB_TEXT_DIM, gg->card_w);
        // Music cards carry an artist credit under the title.
        if (items[idx].artist[0])
            draw_ttf_clipped((u32)cx, (u32)(cy + gg->card_h + 28),
                             items[idx].artist, 13, XMB_TEXT_FAINT,
                             gg->card_w);
    }

    if (sel >= scroll && sel < scroll + gg->vis && sel < count) {
        const XMBItem *it = &items[sel];
        int cx, cy;
        grid_cell_pos(gg, sel - scroll, y0, &cx, &cy);
        int ty = cy + gg->card_h + 7;
        draw_ttf_clipped((u32)cx, (u32)ty, it->name, 20,
                         XMB_WHITE, gg->card_w, true);
        if (it->artist[0]) {
            draw_ttf_clipped((u32)cx, (u32)(ty + 27), it->artist, 15,
                             XMB_TEXT_DIM, gg->card_w);
            ty += 22;   // meta line shifts down to make room
        }
        char meta[96] = "";
        if (it->year_str[0])     snprintf(meta, sizeof(meta), "%s", it->year_str);
        if (it->duration_str[0]) {
            if (meta[0]) strncat(meta, META_SEP, sizeof(meta)-strlen(meta)-1);
            strncat(meta, it->duration_str, sizeof(meta)-strlen(meta)-1);
        }
        if (it->genre[0]) {
            if (meta[0]) strncat(meta, META_SEP, sizeof(meta)-strlen(meta)-1);
            strncat(meta, it->genre, sizeof(meta)-strlen(meta)-1);
        }
        if (it->codec[0]) {
            if (meta[0]) strncat(meta, META_SEP, sizeof(meta)-strlen(meta)-1);
            strncat(meta, it->codec, sizeof(meta)-strlen(meta)-1);
        }
        if (meta[0])
            draw_ttf_clipped((u32)cx, (u32)(ty + 27), meta, 14,
                             XMB_TEXT_DIM, gg->card_w);
    }

    // Scrollbar beside the grid: thumb size/position track the visible
    // window against the whole library, not just the loaded page.
    {
        int total = abs_total > count + abs_start ? abs_total
                                                  : count + abs_start;
        int vis   = gg->vis;
        if (total > vis) {
            int first = abs_start + scroll;
            int bar_x = gg->x0 + gg->grid_w + 14;
            int bar_h = gg->stride + gg->card_h;   // row 1 top -> row 2 card bottom
            drawRect((u32)bar_x, (u32)y0, 3, (u32)bar_h, XMB_TRACK);
            int th = bar_h * vis / total;
            if (th < 24)    th = 24;
            if (th > bar_h) th = bar_h;
            int rng = total - vis;
            int off = rng > 0 ? (bar_h - th) * first / rng : 0;
            if (off < 0)            off = 0;
            if (off > bar_h - th)   off = bar_h - th;
            drawRect((u32)bar_x, (u32)(y0 + off), 3, (u32)th, XMB_ACCENT);
        }
    }
}

