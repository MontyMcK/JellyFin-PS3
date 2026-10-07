// The depth engine: the reusable layout half of the spine's three levels.
//
//   spine.h        the spine row: per-level triples, easing, approach motion
//   depth.h        everything that hangs off it -- item boxes, swings,
//                  category glides, remembered focus, fades   (this file)
//   ui_depth.cpp   the draw half: one stage renderer for every category
//
// WHAT IT IS FOR
//
// README 2.9 / the canvas's "00 · Depth model" says every category is ONE
// composition seen from three depths:
//
//   L1  the category's items hang in a column under its spine icon;
//   L2  the same items, "on the same invisible surface, seen from a
//       different angle": the column swings into the category's own layout;
//   L3  the item detail.
//
// spine6 built that for Home only, with its geometry as file statics inside
// xmb/ui_home_stage.inc, and gave the libraries a separate two-poster column
// that cut to the grid half-way through the move.  This file is the part that
// is the same for every category, so Home, Movies, Shows, Music and the rest
// are one engine with different L2 layouts plugged in -- not four screens.
//
// THE MODEL, IN ONE PARAGRAPH
//
// An item is placed by its distance k from the category's focus (k = 0 is
// the focused item; k may be fractional while the focus glides).  Every
// layout is a function k -> depth_box, where a box carries position, size,
// opacity, a "colder veil" and a reflection height.  L1 always uses the
// COLUMN layout.  L2 uses the category's own: the QUEUE (Home: the focused
// item forward, the rest receding right) or the GRID (libraries: the paged
// 5x2 grid, which the canvas keeps for them in "02 · Library tab").  Between
// the two, an item's box is the lerp of its two endpoint boxes at the eased
// depth -- the swing.  Nothing re-lays-out; nothing cross-fades.
//
// WHY PURE C
//
// Same reason as spine.h and wave_gel.h: it is all arithmetic on authored
// numbers, so tests/test_depth.c can check it exactly -- that the swing lands
// on the grid cell to the pixel, that nothing leaves the screen at 720p and
// 1080p, that focus memory clamps -- before any of it reaches a TV.  No
// display_width, no theme, no RSX, no allocation.  Values are 1280x720
// authoring units unless a function says "screen px"; depth_xform maps one to
// the other exactly as UIS_W / UIS_H do, plus the overscan inset.

#ifndef JF_DEPTH_H
#define JF_DEPTH_H

#include <math.h>
#include "spine.h"

#ifdef __cplusplus
extern "C" {
#endif

// --- item shapes -------------------------------------------------------------

#define DEPTH_POSTER    0       // 2:3
#define DEPTH_WIDE      1       // 16:9 stills
#define DEPTH_SQUARE    2       // albums

// The layouts are authored for posters.  A square item is 1.2x the poster's
// width and as tall as it is wide (spine6's decision for music rows); a wide
// one is 1.75x the width at 16:9 -- the library column's 224x126 at k = 0.
static inline void depth_shape_size(float pw, float ph, int shape,
                                    float *w, float *h)
{
    switch (shape) {
    case DEPTH_SQUARE: *w = pw * 1.2f;  *h = *w;                 break;
    case DEPTH_WIDE:   *w = pw * 1.75f; *h = *w * 9.0f / 16.0f;  break;
    default:           *w = pw;         *h = ph;                 break;
    }
}
static inline float depth_shape_w(float pw, int shape)
{
    float w, h;
    depth_shape_size(pw, pw * 1.5f, shape, &w, &h);
    return w;
}

// --- boxes -------------------------------------------------------------------

typedef struct {
    float x, top, w, h;     // position and size
    float op;               // opacity, 0 = not drawn
    float veil;             // the "colder veil" over the image, 0..1
    float refl;             // reflection height (same units as h)
} depth_box;

static inline depth_box depth_box_lerp(const depth_box *a, const depth_box *b,
                                       float t)
{
    depth_box o;
    o.x    = spine_lerp(a->x,    b->x,    t);
    o.top  = spine_lerp(a->top,  b->top,  t);
    o.w    = spine_lerp(a->w,    b->w,    t);
    o.h    = spine_lerp(a->h,    b->h,    t);
    o.op   = spine_lerp(a->op,   b->op,   t);
    o.veil = spine_lerp(a->veil, b->veil, t);
    o.refl = spine_lerp(a->refl, b->refl, t);
    return o;
}

// Authored units -> screen px, exactly as UIS_W / UIS_H + XMB_OX / XMB_OY
// scale: the canvas maps onto the overscan-SAFE rect (display minus the
// inset on both sides), so kx = (disp_w - 2 ox) / 1280, or the override
// percentage shrunk by the same safe fraction.  See ui_visuals.h, "THE SAFE
// AREA".
typedef struct { float ox, oy, kx, ky; } depth_xform;

static inline depth_xform depth_xform_make(int ox, int oy, int disp_w,
                                           int disp_h, int uis_pct)
{
    depth_xform xf;
    const float sw = (float)(disp_w - 2 * ox), sh = (float)(disp_h - 2 * oy);
    xf.ox = (float)ox;
    xf.oy = (float)oy;
    xf.kx = uis_pct ? (float)uis_pct / 100.0f * sw / (float)disp_w : sw / 1280.0f;
    xf.ky = uis_pct ? (float)uis_pct / 100.0f * sh / (float)disp_h : sh / 720.0f;
    return xf;
}

// An authored box in screen px (floats; opacity and veil carried through).
// `dy` is an extra authored vertical offset (a category gliding past).
static inline depth_box depth_to_screen(const depth_xform *xf,
                                        const depth_box *b, float dy)
{
    depth_box o = *b;
    o.x    = xf->ox + b->x * xf->kx;
    o.top  = xf->oy + (b->top + dy) * xf->ky;
    o.w    = b->w * xf->kx;
    o.h    = b->h * xf->ky;
    o.refl = b->refl * xf->ky;
    return o;
}

// Round a screen-px box to the integer rect every draw call takes.  Position
// and size are rounded separately (floor + 0.5), which is what the Home
// stage has always done, so a settled box is stable to the pixel.
static inline void depth_rect(const depth_box *s, int *x, int *y, int *w, int *h)
{
    *x = (int)floorf(s->x   + 0.5f);
    *y = (int)floorf(s->top + 0.5f);
    *w = (int)floorf(s->w   + 0.5f);
    *h = (int)floorf(s->h   + 0.5f);
}

// --- slot range ----------------------------------------------------------------
//
// Distances the layouts define: -1 (just left the focus) .. 5 (about to
// enter).  Beyond them an item is not drawn.
#define DEPTH_KMIN   (-1)
#define DEPTH_KMAX     5
#define DEPTH_NK     (DEPTH_KMAX - DEPTH_KMIN + 1)

static inline int depth_kclamp(int k)
{
    return k < DEPTH_KMIN ? DEPTH_KMIN : (k > DEPTH_KMAX ? DEPTH_KMAX : k);
}

// --- L1: the column --------------------------------------------------------------
//
// Canvas "L1 · Home": under the active slot the category's items hang in a
// vertical column -- 128x192, then 106x159, then 86x129 -- each under a
// colder veil, sinking toward the bottom.  The column top is y = 318, not the
// mockup's 300: under the README-size spine the L1 underline ends at y ~ 303
// (test_spine).  Items past the third are transparent; k = -1 sits on the
// focus at opacity 0, so an item leaving upward dissolves into it.
//
// NOT DONE, deliberately: the canvas tips the 2nd and 3rd items back 11 and
// 19 degrees and draws the column BEFORE the wave so the ribbons pass over
// it.  A tipped card is a trapezoid, which two affine triangles warp with a
// visible seam, and drawing under the wave means reordering the JellyWave
// pass, which is off limits while its strobe work is open.  Scale and veil
// carry the recession instead (SPINE-PLAN D8).
#define DEPTH_COL_TOP   318.0f
#define DEPTH_COL_GAP    14.0f

static const float DEPTH_COL_W[DEPTH_NK]    = { 128, 128, 106,  86,  70,  60,  50 };
static const float DEPTH_COL_H[DEPTH_NK]    = { 192, 192, 159, 129, 105,  90,  75 };
static const float DEPTH_COL_OP[DEPTH_NK]   = { 0.0f, 1.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0.0f };
static const float DEPTH_COL_VEIL[DEPTH_NK] = { 0.0f, 0.0f, 0.34f, 0.58f, 0.7f, 0.7f, 0.7f };

// Item k's box in the column hanging at authored x = anchor_x (its centre).
static inline depth_box depth_column_slot(int k, int shape, float anchor_x)
{
    depth_box b;
    float top = DEPTH_COL_TOP;
    int   i, j;
    k = depth_kclamp(k);
    i = k - DEPTH_KMIN;
    depth_shape_size(DEPTH_COL_W[i], DEPTH_COL_H[i], shape, &b.w, &b.h);
    b.op   = DEPTH_COL_OP[i];
    b.veil = DEPTH_COL_VEIL[i];
    b.refl = 0.0f;
    b.x    = anchor_x - b.w * 0.5f;
    for (j = 0; j < k; j++) {
        float wj, hj;
        depth_shape_size(DEPTH_COL_W[j - DEPTH_KMIN], DEPTH_COL_H[j - DEPTH_KMIN],
                         shape, &wj, &hj);
        top += hj + DEPTH_COL_GAP;
    }
    b.top = top;
    return b;
}

// --- L2: the queue (Home) ----------------------------------------------------------
//
// Canvas "L2 · Category": the focused item forward at 200x300, x = 64, with a
// reflection fading over 46 px.  The rest bottom-align on one invisible
// surface at y ~ 552 and recede right, each smaller, dimmer and cooler --
// 104x156 @ .62, 92x138 @ .48, 80x120 @ .36, 70x105 @ .26 -- with their own
// short reflections (18% of their height).  The facts column floats between.
#define DEPTH_Q_FOCUS_X   64.0f
#define DEPTH_Q_FIRST_X  836.0f    // the first receding item, for posters
#define DEPTH_Q_GAP       24.0f    // between receding items
#define DEPTH_Q_LEAVE     40.0f    // gap the item leaving to the left keeps
#define DEPTH_Q_REFL      46.0f    // the focus's reflection
#define DEPTH_Q_REFL_K     0.18f   // the others', as a fraction of height
#define DEPTH_VEIL_RGB   0x121E3Cu // rgba(18,30,60): the canvas's colder veil

static const float DEPTH_Q_W[DEPTH_NK]    = { 200, 200, 104,  92,  80,  70,  62 };
static const float DEPTH_Q_H[DEPTH_NK]    = { 300, 300, 156, 138, 120, 105,  93 };
static const float DEPTH_Q_BOT[DEPTH_NK]  = { 566, 566, 552, 550, 548, 545, 543 };
static const float DEPTH_Q_OP[DEPTH_NK]   = { 0.0f, 1.0f, 0.62f, 0.48f, 0.36f, 0.26f, 0.0f };
static const float DEPTH_Q_VEIL[DEPTH_NK] = { 0.0f, 0.0f, 0.13f, 0.18f, 0.22f, 0.26f, 0.26f };

// How much further right everything right of the focus sits for this shape:
// a wider focused item pushes the facts column, and the queue with it, so
// the 520 px column never runs under the focus.
static inline float depth_queue_shift(int shape)
{
    return depth_shape_w(DEPTH_Q_W[0 - DEPTH_KMIN], shape) - DEPTH_Q_W[0 - DEPTH_KMIN];
}

static inline depth_box depth_queue_slot(int k, int shape)
{
    depth_box b;
    int i, j;
    k = depth_kclamp(k);
    i = k - DEPTH_KMIN;
    depth_shape_size(DEPTH_Q_W[i], DEPTH_Q_H[i], shape, &b.w, &b.h);
    b.top  = DEPTH_Q_BOT[i] - b.h;
    b.op   = DEPTH_Q_OP[i];
    b.veil = DEPTH_Q_VEIL[i];
    b.refl = k <= 0 ? DEPTH_Q_REFL : DEPTH_Q_REFL_K * b.h;
    if (k == 0) {
        b.x = DEPTH_Q_FOCUS_X;
    } else if (k < 0) {
        b.x = DEPTH_Q_FOCUS_X - b.w - DEPTH_Q_LEAVE;
    } else {
        float x = DEPTH_Q_FIRST_X + depth_queue_shift(shape);
        for (j = 1; j < k; j++)
            x += depth_shape_w(DEPTH_Q_W[j - DEPTH_KMIN], shape) + DEPTH_Q_GAP;
        b.x = x;
    }
    return b;
}

// --- L2: the grid (libraries) -------------------------------------------------------
//
// The library grid in SCREEN px, described exactly as ui_lists.cpp's
// grid_cell_pos() walks it, so a swing that ends on depth_grid_cell() ends
// on the pixel the grid itself then draws.
typedef struct {
    float x0, y0;           // first cell, screen px (the grid's y0)
    float cell_w, cell_h;   // the card, not its text band
    float pitch_x;          // card_w + XMB_CARD_GAP_X
    float pitch_y;          // the row stride
    int   cols, vis;        // columns; visible cells (cols * rows)
    int   scroll;           // first visible index (a multiple of cols)
} depth_grid;

// Item idx's cell.  Opaque and unveiled inside the visible window; outside
// it the box is still where the cell would be (above or below the window),
// at opacity 0, so an item the column shows but the grid scrolls past fades
// out on its way instead of popping.
static inline depth_box depth_grid_cell(const depth_grid *g, int idx)
{
    depth_box b;
    const int C   = g->cols > 0 ? g->cols : 1;
    const int rel = idx - g->scroll;
    const int row = rel >= 0 ? rel / C : -((-rel + C - 1) / C);
    const int col = rel - row * C;
    b.x    = g->x0 + (float)col * g->pitch_x;
    b.top  = g->y0 + (float)row * g->pitch_y;
    b.w    = g->cell_w;
    b.h    = g->cell_h;
    b.op   = (rel >= 0 && rel < g->vis) ? 1.0f : 0.0f;
    b.veil = 0.0f;
    b.refl = 0.0f;
    return b;
}

// --- the swing ----------------------------------------------------------------------
//
// e is the eased swing, 0 = L1 column, 1 = L2 layout.  From the depth the
// spine is at: the move is slow-in / slow-out over the first level only.
static inline float depth_swing(float depth)
{
    return spine_ease(spine_clampf(depth, 0.0f, 1.0f));
}

// Home's item at FRACTIONAL queue distance k: the column->queue blend at
// both neighbouring integer slots, then between them.  Returns 0 when it is
// out of range or transparent.  (spine6's q_box, unchanged.)
static inline int depth_column_queue_box(float k, float e, int shape,
                                         float anchor_x, depth_box *out)
{
    int       k0;
    float     f;
    depth_box a1, a2, b1, b2, a, b;
    if (k <= (float)DEPTH_KMIN || k >= (float)DEPTH_KMAX) return 0;
    k0 = (int)floorf(k);
    f  = k - (float)k0;
    a1 = depth_column_slot(k0, shape, anchor_x);
    a2 = depth_queue_slot(k0, shape);
    b1 = depth_column_slot(k0 + 1, shape, anchor_x);
    b2 = depth_queue_slot(k0 + 1, shape);
    a  = depth_box_lerp(&a1, &a2, e);
    b  = depth_box_lerp(&b1, &b2, e);
    *out = depth_box_lerp(&a, &b, f);
    return out->op > 0.004f;
}

// A library item idx, focus `focus`, in SCREEN px: its column slot (through
// xf) swinging into its grid cell.  Items the column does not show start on
// the column's end slots at opacity 0 -- the ones before the focus come out
// from behind it, the ones after from the foot of the column -- so the grid
// unfolds out of the column rather than fading up around it.
static inline int depth_column_grid_box(int idx, int focus, float e, int shape,
                                        float anchor_x, const depth_xform *xf,
                                        const depth_grid *g, depth_box *out)
{
    const int k  = idx - focus;
    depth_box c  = depth_column_slot(depth_kclamp(k), shape, anchor_x);
    depth_box cs, gc;
    if (k < DEPTH_KMIN || k > DEPTH_KMAX) c.op = 0.0f;
    cs = depth_to_screen(xf, &c, 0.0f);
    gc = depth_grid_cell(g, idx);
    *out = depth_box_lerp(&cs, &gc, e);
    return out->op > 0.004f;
}

// When the grid takes the screen back from the swing, in e.  A far cell
// travels ~1000 px at 1080p, so 0.995 still left 5 px to jump (test_depth);
// at 0.999 every card is within ~1 px of its cell and the hand-over does not
// show.  With the slow-out that is depth ~0.98, ~0.4 s into the level move.
#define DEPTH_HANDOFF_E  0.999f

static inline int depth_grid_owns(float e) { return e >= DEPTH_HANDOFF_E; }

// --- category glide ---------------------------------------------------------------
//
// Up / Down between Home's categories: the row position f approaches the
// focused row, and the (at most) two rows either side of it are drawn with
// a weight (1 at rest) and a vertical offset of Q_ROW_DY per row.
#define DEPTH_ROW_DY  110.0f

typedef struct { int r; float w, dy; } depth_row;

static inline int depth_rows(float f, int nrows, depth_row out[2])
{
    int r0 = (int)floorf(f), n = 0, r;
    for (r = r0; r <= r0 + 1; r++) {
        float w;
        if (r < 0 || r >= nrows) continue;
        w = 1.0f - fabsf((float)r - f);
        if (w <= 0.01f) continue;
        out[n].r  = r;
        out[n].w  = w;
        out[n].dy = ((float)r - f) * DEPTH_ROW_DY;
        n++;
    }
    return n;
}

// --- empty categories are skipped ---------------------------------------------------
//
// visit[i] says whether category i is worth landing on (loading counts: it
// may fill).  Step dir (+1 / -1) to the next visitable one; stay put if none.
static inline int depth_step_visitable(const unsigned char *visit, int n,
                                       int cur, int dir)
{
    int i;
    for (i = cur + dir; i >= 0 && i < n; i += dir)
        if (visit[i]) return i;
    return cur;
}

// Landed on a category that turned out empty: the nearest one that is not,
// downward first.  Unchanged if it is visitable or nothing is.
static inline int depth_nearest_visitable(const unsigned char *visit, int n,
                                          int cur)
{
    int d;
    if (cur >= 0 && cur < n && visit[cur]) return cur;
    for (d = 1; d < n; d++) {
        if (cur + d < n && cur + d >= 0 && visit[cur + d]) return cur + d;
        if (cur - d >= 0 && cur - d < n && visit[cur - d]) return cur - d;
    }
    return cur;
}

// True when nothing above `cur` is visitable -- Up from here leaves the level.
static inline int depth_at_top(const unsigned char *visit, int n, int cur)
{
    int i;
    for (i = cur - 1; i >= 0 && i < n; i--)
        if (visit[i]) return 0;
    return 1;
}

// --- remembered focus ------------------------------------------------------------------
//
// "Each category remembers focus."  One fixed slot per category (no
// allocation): the selection and the grid's first visible index.  Restoring
// clamps to what is loaded NOW, and keeps the selection inside the visible
// window -- a library that shrank, or one whose loaded page was dropped, can
// never be handed an index it does not have.
#define DEPTH_MAX_CATS  16

typedef struct {
    int           sel[DEPTH_MAX_CATS];
    int           scroll[DEPTH_MAX_CATS];
    unsigned char have[DEPTH_MAX_CATS];
} depth_focus_mem;

static inline void depth_focus_init(depth_focus_mem *m)
{
    int i;
    for (i = 0; i < DEPTH_MAX_CATS; i++) {
        m->sel[i] = 0; m->scroll[i] = 0; m->have[i] = 0;
    }
}

static inline void depth_focus_save(depth_focus_mem *m, int cat, int sel,
                                    int scroll)
{
    if (cat < 0 || cat >= DEPTH_MAX_CATS) return;
    m->sel[cat]    = sel < 0 ? 0 : sel;
    m->scroll[cat] = scroll < 0 ? 0 : scroll;
    m->have[cat]   = 1;
}

static inline void depth_focus_forget(depth_focus_mem *m, int cat)
{
    if (cat < 0 || cat >= DEPTH_MAX_CATS) return;
    m->sel[cat] = 0; m->scroll[cat] = 0; m->have[cat] = 0;
}

// Restore category cat into *sel / *scroll for `count` loaded items in a grid
// of `cols` columns showing `vis` cells.  Returns 1 if a remembered position
// was applied, 0 (and 0 / 0) if there was none or nothing is loaded.
static inline int depth_focus_restore(const depth_focus_mem *m, int cat,
                                      int count, int cols, int vis,
                                      int *sel, int *scroll)
{
    int s, sc;
    *sel = 0; *scroll = 0;
    if (cat < 0 || cat >= DEPTH_MAX_CATS || !m->have[cat] || count <= 0)
        return 0;
    if (cols < 1) cols = 1;
    if (vis < cols) vis = cols;
    s  = m->sel[cat] < count ? m->sel[cat] : count - 1;
    sc = m->scroll[cat] - m->scroll[cat] % cols;
    if (s < sc || s >= sc + vis) sc = (s / cols) * cols;
    *sel = s;
    *scroll = sc;
    return 1;
}

// --- fades by level ---------------------------------------------------------------------
//
// Every one of these is a function of the spine's depth alone, so the GPU,
// CPU and text phases of a frame always agree.

// The L1 label block beside the column: gone by depth ~0.45, so it has left
// before the L2 facts arrive.
static inline float depth_l1_label_alpha(float depth, float near)
{
    return near * spine_clampf(1.0f - depth * 2.2f, 0.0f, 1.0f);
}

// The L2 facts column: in over the second half of the move.
static inline float depth_l2_facts_alpha(float depth)
{
    return spine_clampf((depth - 0.55f) / 0.45f, 0.0f, 1.0f);
}

// How far toward L3 the spine is (0 at L2, 1 in detail).  Coming back from
// detail the stage settles DEPTH_SETTLE_DY into place as it fades up.
#define DEPTH_SETTLE_DY  40.0f
static inline float depth_over_l2(float depth)
{
    return spine_clampf(depth - 1.0f, 0.0f, 1.0f);
}

// A category's detail text waits until its queue has settled on the focus:
// text under a poster flying 770 px reads as noise.
static inline float depth_queue_settle(float pos, int focus)
{
    return spine_clampf(1.0f - fabsf(pos - (float)focus) * 3.0f, 0.0f, 1.0f);
}

// The focus treatment (glow, accent bands, frame) follows whichever item is
// nearest the focus slot, so it travels with the poster.
static inline float depth_halo(float k)
{
    float h = 1.0f - fabsf(k) * 1.6f;
    return h > 0.0f ? h : 0.0f;
}

// --- the L1 label block --------------------------------------------------------------
//
// "The label goes beside.  XMB never captions under the icon."  Eyebrow with
// the count in the spec face, a 30 px title, one meta line, beside the
// focused artwork.  Authored units.
#define DEPTH_L1_TEXT_GAP  36.0f
typedef struct { float x, y_eye, y_title, y_meta, title_px; } depth_l1_label;

static inline depth_l1_label depth_l1_label_layout(int shape, float anchor_x)
{
    depth_l1_label L;
    const spine_level *l1 = &SPINE_LEVEL[SPINE_L1];
    L.title_px = l1->title_px;
    L.x        = anchor_x + depth_shape_w(l1->art_w, shape) * 0.5f
               + DEPTH_L1_TEXT_GAP;
    L.y_eye    = DEPTH_COL_TOP + 22.0f;
    L.y_title  = L.y_eye + 24.0f;
    L.y_meta   = L.y_title + L.title_px * 1.35f;
    return L;
}

// --- the L2 facts column -------------------------------------------------------------
//
// Canvas "L2 describes": 520 px at x = 304 (for a poster focus), floating
// between the focus and the queue.  It drifts 20 px in from the right as the
// swing completes.  Authored units; `dy` is the category's glide offset.
#define DEPTH_FACTS_W   520.0f
#define DEPTH_FACTS_TOP 266.0f

typedef struct {
    float x;
    float y_eye, y_title, y_meta, y_lab, y_val, y_alab, y_chip, chip_h;
    float y_prog, y_watch;
} depth_facts;

static inline depth_facts depth_facts_layout(int shape, int tech, float e,
                                             float dy)
{
    depth_facts f;
    const float drift = (1.0f - spine_clampf((e - 0.5f) * 2.0f, 0.0f, 1.0f)) * 20.0f;
    const float T = DEPTH_FACTS_TOP + dy;
    f.x       = DEPTH_Q_FOCUS_X + depth_shape_w(DEPTH_Q_W[0 - DEPTH_KMIN], shape)
              + 40.0f + drift;
    f.y_eye   = T;
    f.y_title = T + 29.0f;
    f.y_meta  = T + 80.0f;
    f.y_lab   = T + 124.0f;
    f.y_val   = T + 142.0f;
    f.y_alab  = T + 174.0f;
    f.y_chip  = T + 189.0f;
    f.chip_h  = 25.0f;
    f.y_prog  = T + (tech ? 236.0f : 120.0f);
    f.y_watch = T + (tech ? 249.0f : 133.0f);
    return f;
}

// --- the floor --------------------------------------------------------------------------
//
// A reflection is decoration; the hints bar is not.  Under CRT overscan the
// stage is pushed down by the inset while the hints bar is pushed up by it,
// and at 720p / 480p with 8% overscan the queue focus's 46 px reflection ran
// up to 30 px into the hints (found by test_layout's depth pass).  So a
// reflection is shortened to end at `floor` (screen px): never negative, and
// untouched wherever it already fits -- which is every screen without
// overscan.
static inline float depth_refl_clamp(float top, float h, float refl, float gap,
                                     float floor_y)
{
    const float room = floor_y - (top + h + gap);
    if (refl > room) refl = room;
    return refl > 0.0f ? refl : 0.0f;
}

// --- colour ----------------------------------------------------------------------------
//
// Mix two 0x00RRGGBB colours, t in 0..1, QUANTISED to sixteenths.  Every
// distinct colour is a separate cached text run, so a continuous fade would
// upload a new run for every string on every frame of a move; sixteen steps
// is below what a fade on a TV resolves, and repeated moves hit the cache.
static inline unsigned depth_mix_q(unsigned a, unsigned b, float t)
{
    int k = (int)(spine_clampf(t, 0.0f, 1.0f) * 16.0f + 0.5f), sh;
    unsigned out = 0;
    for (sh = 0; sh <= 16; sh += 8) {
        int ca = (int)((a >> sh) & 0xFF), cb = (int)((b >> sh) & 0xFF);
        out |= (unsigned)(ca + (cb - ca) * k / 16) << sh;
    }
    return out;
}

static inline unsigned char depth_a8(float a)
{
    return (unsigned char)(spine_clampf(a, 0.0f, 1.0f) * 255.0f + 0.5f);
}

// --- per-frame following --------------------------------------------------------------
//
// Chase `target` with the XMB's approach, once per frame: snap on the first
// frame (or after a missed one, so a returning screen starts still instead of
// sliding from wherever it was left), otherwise step by the real dt.
static inline void depth_follow(spine_approach *a, float target, int snap,
                                int ms, unsigned long long dt_us)
{
    if (snap) spine_approach_init(a, target, ms);
    spine_approach_set(a, target);
    if (!snap) spine_approach_step(a, dt_us);
}

#ifdef __cplusplus
}
#endif

#endif // JF_DEPTH_H
