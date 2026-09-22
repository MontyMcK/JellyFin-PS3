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
#include "circle_blit.h"
#include "ui_card_gpu.h"
#include "ui_strobe_test.h"

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

// Nearest-neighbour rescale into a dw x dh rect.
//
// The obvious form of this -- `src[col * bm->width / dw]` -- puts a 64-bit
// integer divide in the INNER PIXEL LOOP.  ppu-gcc emits `divdu` there, and on
// the PPE that is a microcoded instruction: tens of cycles, not pipelined, and
// it serialises around itself.  At one card's worth of pixels that is already
// several milliseconds; the moment focus-scale animation puts every visible
// card on this path every frame it is the whole frame budget.
//
// Replaced with exact Bresenham stepping.  With q = W/dw and r = W%dw computed
// once, stepping src by q and carrying r reproduces floor(col*W/dw) bit for
// bit for every col -- this is not an approximation, the output pixels are
// identical -- with no division inside either loop.
//
// The horizontal clip is hoisted out of the loop for the same reason: the old
// per-pixel `if (sx < 0 || sx >= display_width) continue;` cost a branch on
// every pixel to handle an edge case that is decided once per call.
static void cpu_blit_bitmap_scaled(const Bitmap *bm, int dx, int dy,
                                   int dw, int dh) {
    if (!bm || !bm->pixels || dw <= 0 || dh <= 0) return;
    if (bm->width == 0 || bm->height == 0) return;

    // Visible column span [c0, c1) of the destination rect.
    int c0 = 0, c1 = dw;
    if (dx < 0)                          c0 = -dx;
    if (dx + c1 > (int)display_width)    c1 = (int)display_width - dx;
    if (c0 >= c1) return;

    const u32 qx = bm->width  / (u32)dw, rx = bm->width  % (u32)dw;
    const u32 qy = bm->height / (u32)dh, ry = bm->height % (u32)dh;

    // Advance the source column to c0 without stepping through the skipped
    // pixels: one divide, outside the loop, only when the rect is clipped.
    u32 src_x0  = (u32)(((u64)c0 * bm->width) / (u32)dw);
    u32 accx0   = (u32)(((u64)c0 * rx) % (u32)dw);

    u32 src_y = 0, accy = 0;
    for (int row = 0; row < dh; row++) {
        int sy = dy + row;
        if (!cpu_row_clipped(sy)) {
            const u32 *src = bm->pixels + (size_t)src_y * bm->width;
            u32 *dst = color_buffer[curr_fb] + (u32)sy * display_width + dx;
            u32 sx = src_x0, accx = accx0;
            for (int col = c0; col < c1; col++) {
                dst[col] = src[sx];
                sx += qx;
                accx += rx;
                if (accx >= (u32)dw) { accx -= (u32)dw; sx++; }
            }
        }
        src_y += qy;
        accy  += ry;
        if (accy >= (u32)dh) { accy -= (u32)dh; src_y++; }
    }
}

// The circle rasterisers live in render/circle_blit.h so a host test can drive
// the same code -- see the header for why the anti-aliasing never touches the
// framebuffer.  These two wrap them onto the live surface.
static CbSurface cb_surface(void)
{
    CbSurface s;
    s.px       = color_buffer[curr_fb];
    s.w        = (int)display_width;
    s.h        = (int)display_height;
    s.pitch    = (int)display_width;
    s.clip_top = g_cpu_clip_top;
    s.clip_bot = g_cpu_clip_bot;
    return s;
}

// An item's image as a circle of diameter d.  Falls back to a flat disc while
// the fetch is in flight, so the row's rhythm is there from the first frame.
void xmb_cpu_blit_thumb_circle(const char *item_id, int x, int y, int d,
                               u32 rim)
{
    GridGeom gg;
    xmb_grid_geom_portrait(&gg);
    thumb_request(item_id, gg.card_w, gg.card_h);
    const Bitmap *bm = thumb_get(item_id, gg.card_w, gg.card_h);
    CbSurface s = cb_surface();
    if (bm && bm->pixels && bm->width && bm->height)
        cb_blit_circle(&s, bm->pixels, (int)bm->width, (int)bm->height,
                       x, y, d, rim);
    else
        cb_fill_circle(&s, x, y, d, XMB_THUMB_DIM, rim);
}
// Search results: small scaled thumb, dim placeholder while loading.
// Reuses the Movies-tab poster size so a thumb cached from browsing is
// shared with the search list.
void xmb_cpu_blit_thumb_scaled(const char *item_id, int x, int y,
                               int w, int h) {
    GridGeom gg;
    xmb_grid_geom_portrait(&gg);
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

// Middle-dot separator ("2014 - 2h 49m - Sci-Fi"), written as real UTF-8.
//
// It used to be a bare \xB7, which worked only because drawTTF read bytes as
// Latin-1.  It still renders -- utf8_next() falls back to the lead byte when a
// sequence is malformed, and a lone 0xB7 is malformed -- but relying on the
// fallback for a string this file OWNS is backwards.  Every display literal in
// this tree is UTF-8 now; the fallback is there for what the server sends.
#define META_SEP " \xC2\xB7 "

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
// face < 0 keeps the old behaviour (regular or bold system face); pass a
// UI_FACE_* to put the string on one of the handoff's narrow-purpose faces --
// section 3.0 gives media titles to UI_FACE_DISPLAY and nothing else on a card
// goes with them.
static void draw_ttf_clipped(u32 x, u32 y, const char *text, float px,
                             u32 color, int max_w, bool bold = false,
                             int face = -1) {
    if (face >= 0) {
        if (ttf_text_width_face(text, px, face) <= max_w) {
            drawTTF_face(x, y, text, px, color, face);
            return;
        }
        // Clip against the SAME face the text will be drawn in, or the ellipsis
        // lands in the wrong place.
        char buf[192];
        snprintf(buf, sizeof(buf), "%s", text);
        int len = (int)strlen(buf);
        while (len > 1) {
            buf[--len] = '\0';
            char trial[196];
            snprintf(trial, sizeof(trial), "%s..", buf);
            if (ttf_text_width_face(trial, px, face) <= max_w) {
                drawTTF_face(x, y, trial, px, color, face);
                return;
            }
        }
        return;
    }
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
    if (xmb_kind(tab) == TABKIND_TV && g_tv_depth == 2) return false;   // episodes
    return true;
}

// Core geometry, keyed on what the tab IS rather than which tab it is, so
// callers that just want "a poster grid" (the search list, the thumbnail
// prefetcher) can ask for it without naming a library that may not exist.
static void grid_geom_core(XMBTabKind kind, bool portrait, GridGeom *gg) {
    // Music AND playlist art is square (album covers, playlist art), so both
    // get square cards with a taller text band for title + artist (+ meta on
    // the selected card).  A Playlists library was previously falling through
    // to the 2:3 portrait branch below, which stretched square covers into
    // poster-shaped boxes.
    //
    // Only a real music library reserves height for the
    // Albums/Artists/Playlists/... sub-tab header above the grid — a
    // Playlists library has no sub-tabs, so subtracting that there would
    // shrink the cards to make room for a header that never draws.
    if (kind == TABKIND_MUSIC || kind == TABKIND_PLAYLISTS) {
        const int header = (kind == TABKIND_MUSIC) ? XMB_MUSIC_SUBTAB_H : 0;
        gg->portrait = false;
        gg->cols     = XMB_MUSIC_COLS;
        int card = (XMB_GRID_AVAIL_H - header) / XMB_GRID_ROWS
                   - XMB_MUSIC_TEXT_H - 6;
        gg->card_w = gg->card_h = card;
        gg->vis    = gg->cols * XMB_GRID_ROWS;
        gg->stride = card + XMB_MUSIC_TEXT_H + 6;
        gg->grid_w = gg->cols * card + (gg->cols - 1) * XMB_CARD_GAP_X;
        gg->x0     = ((int)display_width - gg->grid_w) / 2;
        return;
    }

    gg->portrait = portrait;
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

void xmb_grid_geom(int tab, GridGeom *gg) {
    grid_geom_core(xmb_kind(tab), xmb_tab_uses_portrait(tab), gg);
}

// Poster-grid geometry with no tab behind it.  TABKIND_GENERIC keeps it off
// the music branch; portrait=true gives the same card size the Movies-style
// grids use, so a thumb cached while browsing is reused by the search list.
void xmb_grid_geom_portrait(GridGeom *gg) {
    grid_geom_core(TABKIND_GENERIC, true, gg);
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
    // DELIBERATELY NOT THEMED.  These eight are a decorative HUE SET, not
    // palette colours: the tile picks one by hashing the item id so adjacent
    // placeholders differ from each other.  Routing them through theme tokens
    // would collapse that variety to one colour.  Everything else in this file
    // follows g_theme.
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
            letter, px, XMB_TEXT, true);
}

// Draw one card at (cx,cy): cached image (or a dim placeholder while it
// loads), the watched-progress strip, and — when selected — a thin white
// frame with a soft 1px halo.  Shared by the grid and the Home shelf.
// NOTE on the `cards` bucket: it times xmb_draw_cpu_phase(), which runs AFTER
// rsxSync() -- the card IMAGES go through xmb_draw_gpu_phase() before the
// fence and are NOT in it.  Profiled 2026-09-19: with the GPU card path live
// this function is ~5 us for 11 cards (three cache lookups and a return).
// The 2,489 us that bucket used to carry was the tab-bar divider; see
// wave_draw_divider_gpu().

void xmb_draw_card(const char *item_id, int cx, int cy, int card_w, int card_h,
                   u8 progress_pct, bool selected, const char *tile_name,
                   ThumbImg img) {
    thumb_request(item_id, card_w, card_h, img);
    const Bitmap *bm = thumb_get(item_id, card_w, card_h, img);
    if (bm) {
        // The GPU pass (xmb_card_gpu_one, run before this frame's rsxSync)
        // has already drawn this image straight from the slot's VRAM mirror.
        // Blitting it again here would cost the 6.4 ms this change exists to
        // remove -- and would draw the identical pixels on top of themselves.
        // Only fall back when the GPU path could not take it.
        u32 dummy_off, dummy_pitch;
        if (!ui_card_gpu_ready() ||
            !thumb_gpu_texture(item_id, card_w, card_h,
                               &dummy_off, &dummy_pitch, img))
            cpu_blit_bitmap(bm, cx, cy);
    } else if (tile_name) {
        // Music cards: colored letter tile (matches the Now Playing art).
        xmb_draw_letter_tile(item_id, tile_name, cx, cy,
                             card_w < card_h ? card_w : card_h);
    } else {
        // Loading placeholder: dark panel with a faint image glyph.
        drawRect((u32)cx, (u32)cy, (u32)card_w, (u32)card_h, XMB_THUMB_DIM);
        const float ph_px = UIS_TF(32.0f);
        drawIcon((u32)(cx + (card_w - (int)ph_px) / 2),
                 (u32)(cy + (card_h - (int)ph_px) / 2),
                 ICON_PHOTO, ph_px, XMB_HAIRLINE);
    }

    // Watched-progress strip along the bottom edge of the card.
    if (progress_pct > 0) {
        int bar_y = cy + card_h - UIS_H(4);
        int fill  = (card_w * progress_pct) / 100;
        drawRect((u32)cx, (u32)bar_y, (u32)card_w, UIS_H(4), XMB_TRACK);
        if (fill > 0)
            drawRect((u32)cx, (u32)bar_y, (u32)fill, UIS_H(4), XMB_ACCENT);
    }

    // The selection border is eight rects, four of them drawRectBlend -- the
    // VRAM read-modify-write path, ~1,200 pixels at the measured ~700 ns each.
    // When the GPU pass is live it has already drawn the identical geometry
    // with blended quads, so skip it here rather than paying for it twice.
    if (selected && !ui_card_gpu_ready() &&
        !strobe_test_disable_card_cpu_fallback()) {
        const int T = 2, G = 2, O = G + T;
        int w = card_w, h = card_h;
        drawRect((u32)(cx - O), (u32)(cy - O), (u32)(w + 2*O), T, XMB_FOCUS_RING);
        drawRect((u32)(cx - O), (u32)(cy + h + G), (u32)(w + 2*O), T, XMB_FOCUS_RING);
        drawRect((u32)(cx - O), (u32)(cy - G), T, (u32)(h + 2*G), XMB_FOCUS_RING);
        drawRect((u32)(cx + w + G), (u32)(cy - G), T, (u32)(h + 2*G), XMB_FOCUS_RING);
        drawRectBlend((u32)(cx - O - 1), (u32)(cy - O - 1), (u32)(w + 2*O + UIS_W(2)), 1, XMB_FOCUS_RING, 70);
        drawRectBlend((u32)(cx - O - 1), (u32)(cy + h + O), (u32)(w + 2*O + UIS_W(2)), 1, XMB_FOCUS_RING, 70);
        drawRectBlend((u32)(cx - O - 1), (u32)(cy - O), 1, (u32)(h + 2*O), XMB_FOCUS_RING, 70);
        drawRectBlend((u32)(cx + w + O), (u32)(cy - O), 1, (u32)(h + 2*O), XMB_FOCUS_RING, 70);
    }
}

// GPU phase (BEFORE rsxSync): draw one card's image from its VRAM mirror.
// Returns true if it drew, so callers can tell a hit from a miss.  Cards that
// are still loading draw nothing here -- their placeholder is a CPU rect and
// belongs in the CPU phase with the rest of the chrome.
bool xmb_card_gpu_one(const char *item_id, int cx, int cy,
                      int card_w, int card_h, ThumbImg img) {
    if (!ui_card_gpu_ready()) return false;
    u32 off = 0, pitch = 0;
    if (!thumb_gpu_texture(item_id, card_w, card_h, &off, &pitch, img))
        return false;
    ui_card_gpu_draw(off, (u32)card_w, (u32)card_h, pitch,
                     cx, cy, card_w, card_h);
    return true;
}

// GPU phase for a card grid: the same walk xmb_grid_cpu does, images only.
void xmb_grid_gpu(const GridGeom *gg, const XMBItem *items, int count,
                  int sel, int scroll, int y0) {
    if (!ui_card_gpu_ready()) return;
    for (int i = 0; i < gg->vis; i++) {
        int idx = scroll + i;
        if (idx >= count) break;
        int cx, cy;
        grid_cell_pos(gg, i, y0, &cx, &cy);
        const char *ty = items[idx].type;
        bool music = strcmp(ty, "MusicAlbum")  == 0 ||
                     strcmp(ty, "Audio")       == 0 ||
                     strcmp(ty, "MusicArtist") == 0 ||
                     strcmp(ty, "Playlist")    == 0;
        ThumbImg img = (!gg->portrait && !music && items[idx].has_thumb)
                     ? THUMB_IMG_THUMB : THUMB_IMG_PRIMARY;
        xmb_card_gpu_one(items[idx].id, cx, cy, gg->card_w, gg->card_h, img);
        if (idx == sel)
            ui_card_gpu_selection(cx, cy, gg->card_w, gg->card_h);
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
        // Media title -- section 3.0 gives these to the display face.
        draw_ttf_clipped((u32)cx, (u32)(cy + gg->card_h + UIS_H(8)),
                         items[idx].name, UIS_TF(16), XMB_TEXT_DIM, gg->card_w,
                         false, UI_FACE_DISPLAY);
        // Music cards carry an artist credit under the title.
        if (items[idx].artist[0])
            draw_ttf_clipped((u32)cx, (u32)(cy + gg->card_h + UIS_H(28)),
                             items[idx].artist, UIS_TF(13), XMB_TEXT_FAINT,
                             gg->card_w);
    }

    if (sel >= scroll && sel < scroll + gg->vis && sel < count) {
        const XMBItem *it = &items[sel];
        int cx, cy;
        grid_cell_pos(gg, sel - scroll, y0, &cx, &cy);
        int ty = cy + gg->card_h + UIS_H(7);
        draw_ttf_clipped((u32)cx, (u32)ty, it->name, UIS_TF(20),
                         XMB_WHITE, gg->card_w, false, UI_FACE_DISPLAY);
        if (it->artist[0]) {
            draw_ttf_clipped((u32)cx, (u32)(ty + UIS_H(27)), it->artist, UIS_TF(15),
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
            draw_ttf_clipped((u32)cx, (u32)(ty + UIS_H(27)), meta, UIS_TF(14),
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
            int bar_x = gg->x0 + gg->grid_w + UIS_W(14);
            int bar_h = gg->stride + gg->card_h;   // row 1 top -> row 2 card bottom
            drawRect((u32)bar_x, (u32)y0, UIS_W(3), (u32)bar_h, XMB_TRACK);
            int th = bar_h * vis / total;
            if (th < 24)    th = 24;
            if (th > bar_h) th = bar_h;
            int rng = total - vis;
            int off = rng > 0 ? (bar_h - th) * first / rng : 0;
            if (off < 0)            off = 0;
            if (off > bar_h - th)   off = bar_h - th;
            drawRect((u32)bar_x, (u32)(y0 + off), UIS_W(3), (u32)th, XMB_ACCENT);
        }
    }
}
