// Library categories on the depth engine: Movies, Shows, Music, Collections,
// Playlists -- every tab whose L2 is the card grid.
//
// spine6 gave these a separate, simpler L1: two opaque posters (always the
// library's FIRST item), dimmed by a background-coloured rect, lifted out and
// cut to the grid at depth 0.5, after which the grid glided up 48 px.  Home
// meanwhile had the real model -- one surface that swings.  This file puts the
// libraries on Home's model, through the same engine:
//
//   L1  the column under the category's icon: the REMEMBERED focus at 128x192
//       with the next two receding under colder veils -- truly translucent,
//       so the wave shows through them -- and the label block beside it.
//   L1 -> L2  every card swings from its column slot into its grid cell
//       (depth_column_grid_box).  The rest of the page unfolds out of the
//       column: the items before the focus from behind it, the ones after
//       from its foot, fading up as they travel.  No cut, no cross-fade.
//   L2  at the hand-over (DEPTH_HANDOFF_E, ~1 px from every cell) the grid
//       takes the screen back and draws exactly as it always has -- titles,
//       scrollbar, A-Z rail, focus ring, paging.  Going back up, the stage
//       takes over again on the first frame the swing leaves 1.
//
// Why the grid and not Home's queue at L2: the canvas keeps "02 · Library tab
// -- paged 2x5 grid" for libraries, and the grid carries what a queue cannot
// (paging a 3,000-item library, the A-Z rail, sort).  The engine does not
// care: the L2 layout is one function (depth_grid_cell here, depth_queue_slot
// on Home), so moving libraries to a queue later is a change of callback, not
// a new screen.
//
// The thumbnails are the grid's own: the column asks the cache for exactly
// the size the grid caches them at and the RSX scales it (LINEAR), so the two
// levels share one slot per item and a poster seen in either shows in the
// other with no fetch (spine3's fix for posters that never loaded at L1).

#include <stdio.h>
#include <string.h>

#include "ui.h"
#include "ui_visuals.h"
#include "ui_internal.h"
#include "ui_render_internal.h"
#include "ui_card_gpu.h"
#include "ui_spine.h"
#include "ui_depth.h"

static bool lib_grid_kind(int tab) {
    const int k = xmb_kind(tab);
    return tab != XMB_TAB_HOME && k != TABKIND_SEARCH && k != TABKIND_SETTINGS;
}

bool depth_stage_tab(int tab) {
    return g_spine_on && (tab == XMB_TAB_HOME || lib_grid_kind(tab));
}

bool depth_lib_owns(int tab) {
    if (!g_spine_on || tab == XMB_TAB_HOME) return false;
    // Search and Settings have no column: their label block shows at base
    // and their own screen takes over at the halfway point, as before.
    if (!lib_grid_kind(tab)) return spine_at_base();
    return !depth_grid_owns(depth_swing(spine_depth()));
}

// --- this frame's view of the category --------------------------------------------

typedef struct {
    int            tab;
    bool           grid;        // false: Search / Settings (label only)
    const XMBItem *items;
    int            count, sel, scroll, y0, abs_start, abs_total;
    GridGeom       gg;
    int            shape;
    float          e, anchor_x, near;
    depth_xform    xf;
    depth_grid     g;
} LibView;

static void lib_view(int tab, LibView *v) {
    memset(v, 0, sizeof *v);
    v->tab   = tab;
    v->e     = depth_swing(spine_depth());
    v->xf    = depth_xf();
    v->shape = DEPTH_POSTER;
    int cx = 0;
    float near = 1.0f;
    spine_column_anchor(tab, &cx, &near);
    v->anchor_x = depth_ax(cx);
    v->near     = spine_lerp(near, 1.0f, v->e);
    if (!lib_grid_kind(tab)) return;

    bool more = false;
    if (!xmb_grid_view(tab, &v->gg, &v->items, &v->count, &v->sel, &v->scroll,
                       &v->y0, &more, &v->abs_start, &v->abs_total))
        return;
    v->grid = true;
    const int k = xmb_kind(tab);
    v->shape = (k == TABKIND_MUSIC || k == TABKIND_PLAYLISTS) ? DEPTH_SQUARE
             : v->gg.portrait ? DEPTH_POSTER : DEPTH_WIDE;
    // The grid exactly as ui_lists.cpp's grid_cell_pos() walks it.  y0 is at
    // rest: spine_content_dy() is 0 for a stage tab, because the swing IS
    // its motion.
    v->g.x0      = (float)v->gg.x0;
    v->g.y0      = (float)v->y0;
    v->g.cell_w  = (float)v->gg.card_w;
    v->g.cell_h  = (float)v->gg.card_h;
    v->g.pitch_x = (float)(v->gg.card_w + XMB_CARD_GAP_X);
    v->g.pitch_y = (float)v->gg.stride;
    v->g.cols    = v->gg.cols;
    v->g.vis     = v->gg.vis;
    v->g.scroll  = v->scroll;
}

static bool lib_music_type(const char *ty) {
    return strcmp(ty, "MusicAlbum") == 0 || strcmp(ty, "Audio") == 0 ||
           strcmp(ty, "MusicArtist") == 0 || strcmp(ty, "MusicGenre") == 0 ||
           strcmp(ty, "Playlist") == 0;
}

static bool lib_card(const void *vctx, int i, DepthCard *out) {
    const LibView *v = (const LibView *)vctx;
    if (i < 0 || i >= v->count) return false;
    const XMBItem *it = &v->items[i];
    depth_column_grid_box(i, v->sel, v->e, v->shape, v->anchor_x, &v->xf, &v->g,
                          &out->box);
    out->img_id = it->id;
    out->src_w  = v->gg.card_w;
    out->src_h  = v->gg.card_h;
    // Same rule the grid uses for which image a card shows.
    out->img = (!v->gg.portrait && !lib_music_type(it->type) && it->has_thumb)
             ? THUMB_IMG_THUMB : THUMB_IMG_PRIMARY;
    // The halo marks the focus in the column and fades as it swings; the
    // grid's own focus ring takes over at the hand-over.
    out->halo = i == v->sel ? 1.0f - v->e : 0.0f;
    out->progress_pct = it->progress_pct;
    out->progress_a   = out->box.op;
    out->selected  = i == v->sel;
    out->tile_name = lib_music_type(it->type) ? it->name : NULL;
    return true;
}

// Everything the column shows and everything the grid page shows: the swing
// needs both ends of every card that is on screen at either.
static DepthStage lib_stage(const LibView *v) {
    DepthStage s;
    int lo = v->sel + DEPTH_KMIN, hi = v->sel + DEPTH_KMAX + 1;
    if (v->scroll < lo)          lo = v->scroll;
    if (v->scroll + v->g.vis > hi) hi = v->scroll + v->g.vis;
    s.i0    = lo < 0 ? 0 : lo;
    s.i1    = hi > v->count ? v->count : hi;
    s.pos   = (float)v->sel;
    s.alpha = v->near;
    s.card  = lib_card;
    s.ctx   = v;
    return s;
}

// Warm another library tab's first posters (2026-09-27): the tabs either side
// of the one showing, so moving to them finds art already decoded instead of a
// blank moment.  Exactly the size and image the tab will draw them at -- any
// other size is a separate cache entry and would not help.  Only tabs whose
// items are already loaded; never fetches a listing.
void depth_lib_prefetch(int tab, int n) {
    if (tab < 0 || tab >= XMB_TAB_COUNT || !lib_grid_kind(tab) || !g_items_loaded[tab]) return;
    GridGeom gg; const XMBItem *items = NULL;
    int count = 0, sel = 0, scroll = 0, y0 = 0, abs_start = 0, abs_total = 0;
    bool more = false;
    if (!xmb_grid_view(tab, &gg, &items, &count, &sel, &scroll, &y0, &more,
                       &abs_start, &abs_total) || !items) return;
    for (int i = 0; i < count && i < n; i++) {
        const XMBItem *it = &items[i];
        const ThumbImg img = (!gg.portrait && !lib_music_type(it->type) && it->has_thumb)
                           ? THUMB_IMG_THUMB : THUMB_IMG_PRIMARY;
        thumb_request(it->id, gg.card_w, gg.card_h, img);
    }
}

void depth_lib_gpu(int tab) {
    LibView v;
    lib_view(tab, &v);
    if (!v.grid || v.count <= 0) return;
    const DepthStage st = lib_stage(&v);
    depth_stage_gpu(&st);
}

void depth_lib_cpu(int tab) {
    LibView v;
    lib_view(tab, &v);
    if (!v.grid || v.count <= 0) return;
    const DepthStage st = lib_stage(&v);

    // Thumbnails.  The column's three first, always.  The rest of the page
    // (which the swing unfolds) only once this category is where the row has
    // settled, or the swing has started: walking the spine at L1 should cost
    // three fetches per library, not a page of them.  The cache ignores a
    // request it already holds or has in flight, so asking again is free.
    DepthStage col = st;
    col.i0 = v.sel > 0 ? v.sel : 0;
    col.i1 = v.sel + 3 < v.count ? v.sel + 3 : v.count;
    depth_stage_request(&col);
    if (v.e > 0.0f || v.near >= 0.99f) {
        depth_stage_request(&st);
        // Preload: a few cards either side of the page, so moving along
        // finds them decoded.  Asked after the visible ones.
        DepthStage pre = st;
        pre.i0 = st.i1;
        pre.i1 = st.i1 + 4 < v.count ? st.i1 + 4 : v.count;
        if (pre.i0 < pre.i1) depth_stage_request(&pre);
        pre.i1 = st.i0;
        pre.i0 = st.i0 - 2 > 0 ? st.i0 - 2 : 0;
        if (pre.i0 < pre.i1) depth_stage_request(&pre);
    }

    depth_stage_cpu(&st, 0.3f);
}

// The label block's meta line: the grid's own meta row, in its own fixed
// colours -- so it waits for the column to arrive rather than popping in at
// full strength mid-slide.
static void lib_meta(const void *vctx, int x, int y, int max_w, float a) {
    (void)max_w;
    const XMBItem *it = (const XMBItem *)vctx;
    if (it && a > 0.95f) xmb_draw_meta((u32)x, (u32)y, it, UIS_TF(14.0f));
}

void depth_lib_text(int tab) {
    LibView v;
    lib_view(tab, &v);
    const float a = depth_l1_label_alpha(spine_depth(), v.near);
    if (a <= 0.02f) return;

    const XMBItem *it = (v.grid && v.count > 0 && v.sel >= 0 && v.sel < v.count)
                      ? &v.items[v.sel] : NULL;
    const char *title;
    switch (xmb_kind(tab)) {
    case TABKIND_SEARCH:   title = "Search";   break;
    case TABKIND_SETTINGS: title = "Settings"; break;
    default:
        title = it ? it->name
              : !g_items_loaded[tab] ? "Loading..." : "Nothing here yet";
        break;
    }
    // The tab's name is already on the spine row: no eyebrow repeating it.
    depth_l1_label_draw(v.shape, v.anchor_x, a, NULL, NULL, title, lib_meta, it);
}
