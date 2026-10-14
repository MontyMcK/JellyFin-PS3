// Home under the spine: one stage, drawn at every depth.   (#included by
// ui_home.cpp, after the model, so it reads the rows directly.)
//
// The current canvas (design-import-v3, "00 · Depth model") draws Home as
// ONE category at a time, and says the L1 column and the L2 queue are "the
// same items on the same invisible surface, seen from a different angle --
// it swings from vertical to horizontal rather than cross-fading".  So this
// is not two screens with a cut between them.  Every item has a box at L1
// (hanging under the Home icon) and a box at L2 (the queue), and is drawn at
// the depth-weighted blend of the two.
//
// ALL OF THAT IS NOW THE DEPTH ENGINE'S.  The slots, the swing, the category
// glide, skipping empty rows, the fades and the facts layout are in
// render/depth.h (host-tested against spine6's numbers in test_depth.c), and
// the cards are drawn by render/ui_depth.cpp -- the same renderer the library
// tabs use.  What is left here is what is Home's own: which rows exist, what
// an item is called ("S1 E4" and the series poster for an episode), the facts
// column's words, and the input.
//
// Left/Right slides the queue, Up/Down changes category with a vertical
// glide, and nothing here snaps: the queue position, the category and the
// depth are all spine.h approaches, stepped once per frame.
//
// DECIDED HERE, not by the canvas (spine6, unchanged):
//   * Posters, not stills, in every row: an episode shows its SERIES poster.
//     Music albums are square.
//   * The focus halo follows the item nearest the focus slot.
//   * The facts fade out while the queue moves and back in as it settles.
//   * The L1 column is drawn AFTER the wave (the wave's first draw is an
//     opaque gradient, and reordering it is off limits); true opacity keeps
//     the wave visible through the receding items.
//   * Coming back from detail the queue settles 40 px into place.

#include <math.h>
#include "thumbnail_cache.h"
#include "ui_wave.h"
#include "ui_depth.h"
#include "api_facts.h"

// --- per-frame state ----------------------------------------------------------------

static spine_approach s_qrow;                  // category, in rows
static spine_approach s_qpos[HOME_ROWS_N];     // each row's queue position
static int            s_row_col[HOME_ROWS_N];  // each row's focused item
static unsigned       s_q_frame = 0;
static bool           s_q_fresh = true;

// Which rows are worth landing on: loading (it may fill) or not empty.
static void q_visit(unsigned char v[HOME_ROWS_N]) {
    for (int r = 0; r < HOME_ROWS_N; r++)
        v[r] = s_rows[r].kind != HROW_STUB &&
               (row_optional(r) ? s_rows[r].count > 0
                                : (!s_rows[r].loaded || s_rows[r].count > 0));
}

static void q_clamp_cols(void) {
    for (int r = 0; r < HOME_ROWS_N; r++) {
        const int n = s_rows[r].count;
        if (s_row_col[r] >= n) s_row_col[r] = n > 0 ? n - 1 : 0;
        if (s_row_col[r] < 0)  s_row_col[r] = 0;
    }
    s_focus_col = s_row_col[s_focus_row];
}

static void q_tick(void) {
    const unsigned id = spine_frame_id();
    if (id == s_q_frame) return;
    const int snap = s_q_fresh || s_q_frame + 1 < id;
    const unsigned long long dt = spine_frame_dt_us();

    // Landed on a category that turned out empty: move to the nearest one
    // that is not, downward first.
    unsigned char v[HOME_ROWS_N];
    q_visit(v);
    s_focus_row = depth_nearest_visitable(v, HOME_ROWS_N, s_focus_row);
    q_clamp_cols();

    depth_follow(&s_qrow, (float)s_focus_row, snap, SPINE_CATEGORY_MS, dt);
    for (int r = 0; r < HOME_ROWS_N; r++)
        depth_follow(&s_qpos[r], (float)s_row_col[r], snap, SPINE_CATEGORY_MS, dt);
    s_q_frame = id;
    s_q_fresh = false;
}

// Everything the three phases agree on for one frame.
typedef struct {
    float e;          // swing: 0 = L1 column, 1 = L2 queue
    float anchor_x;   // authored x of the column (follows the spine row)
    float near;       // the column's nearness to the spine focus (base layer)
    float settle_dy;  // authored px the stage sits low (coming back from detail)
    float settle_a;   // and its fade
    int   clip_top;   // screen px
} QFrame;

static void q_frame(QFrame *q) {
    const float d = spine_depth();
    q->e = depth_swing(d);
    int cx = 0;
    float near = 1.0f;
    spine_column_anchor(g_active_tab, &cx, &near);
    q->anchor_x = depth_ax(cx);
    q->near     = spine_lerp(near, 1.0f, q->e);
    const float over = depth_over_l2(d);                     // detail above
    q->settle_dy = over * DEPTH_SETTLE_DY;
    q->settle_a  = 1.0f - over;
    // At L2 a category gliding up must not cross the divider (y=214).  At
    // L1 there is no divider and the focus halo may reach up toward the
    // label, so the clip opens as the stage swings down.
    q->clip_top = (int)((float)depth_sy(216.0f) * q->e);
}

static int q_shape(int r) { return s_rows[r].kind == HROW_SQUARE ? DEPTH_SQUARE : DEPTH_POSTER; }

// Which image stands for an item in the queue: an episode's series poster.
static const char *q_img_id(int r, int c) {
    const XMBItem *it = &s_rows[r].items[c];
    if (strcmp(it->type, "Episode") == 0 && s_extra[r][c].series_id[0])
        return s_extra[r][c].series_id;
    return it->id;
}

static void q_src(int r, int *w, int *h) {
    xmb_queue_src(w, h);
    if (q_shape(r) == DEPTH_SQUARE) {
        const int s = (int)sqrtf((float)(*w) * (float)(*h));
        *w = *h = s;
    }
}

// --- the stage callback: row r's item c at this frame's swing ----------------------------

typedef struct {
    int         r;
    float       dy;      // authored: the category glide + the detail settle
    const QFrame *q;
    depth_xform xf;
} QCtx;

static bool q_card(const void *vctx, int c, DepthCard *out) {
    const QCtx *x = (const QCtx *)vctx;
    const HomeRow *row = &s_rows[x->r];
    if (c < 0 || c >= row->count) return false;
    const float k = (float)c - s_qpos[x->r].value;
    depth_box b = { 0, 0, 0, 0, 0, 0, 0 };
    if (!depth_column_queue_box(k, x->q->e, q_shape(x->r), x->q->anchor_x, &b))
        b.op = 0.0f;   // not drawn, but still resolvable: its thumbnail is wanted
    out->box = depth_to_screen(&x->xf, &b, x->dy);
    out->img_id = q_img_id(x->r, c);
    q_src(x->r, &out->src_w, &out->src_h);
    out->img = THUMB_IMG_PRIMARY;
    out->halo = depth_halo(k);
    // The L1 progress strip hands over to the facts column's bar as the
    // stage swings.
    out->progress_pct = row->items[c].progress_pct;
    out->progress_a   = x->q->e < 0.99f ? out->halo * (1.0f - x->q->e) : 0.0f;
    out->selected  = c == s_row_col[x->r] && x->r == s_focus_row;
    out->tile_name = NULL;
    return true;
}

static DepthStage q_stage(const QCtx *ctx, float alpha) {
    DepthStage s;
    const float pos = s_qpos[ctx->r].value;
    s.i0    = (int)floorf(pos) + DEPTH_KMIN;
    s.i1    = (int)floorf(pos) + DEPTH_KMAX + 1;
    s.pos   = pos;
    s.alpha = alpha;
    s.card  = q_card;
    s.ctx   = ctx;
    return s;
}

// --- GPU phase ------------------------------------------------------------------------

// The facts column's layout in SCREEN px: shared by the GPU (chip, progress)
// and the text phase so the two cannot drift apart.  The geometry is
// depth_facts_layout's; the opacity is Home's (settled queue x level).
typedef struct {
    int   x;
    bool  tech;          // this kind of item has streams to describe
    int   y_eye, y_title, y_meta, y_lab, y_val, y_alab, y_chip, chip_h, y_prog, y_watch;
    float a;             // overall opacity
} QFacts;

static void q_facts_layout(int r, float dy, float alpha, float e, QFacts *f) {
    const int c = s_row_col[r];
    f->tech = s_rows[r].count > 0 && !row_local(r) && facts_wanted(s_rows[r].items[c].type);
    const depth_facts L = depth_facts_layout(q_shape(r), f->tech, e, dy);
    f->x       = depth_sx(L.x);
    f->y_eye   = depth_sy(L.y_eye);
    f->y_title = depth_sy(L.y_title);
    f->y_meta  = depth_sy(L.y_meta);
    f->y_lab   = depth_sy(L.y_lab);
    f->y_val   = depth_sy(L.y_val);
    f->y_alab  = depth_sy(L.y_alab);
    f->y_chip  = depth_sy(L.y_chip);
    f->chip_h  = UIS_H((int)L.chip_h);
    f->y_prog  = depth_sy(L.y_prog);
    f->y_watch = depth_sy(L.y_watch);
    f->a = alpha * depth_queue_settle(s_qpos[r].value, c) * depth_l2_facts_alpha(spine_depth());
}

// Watched fraction and runtime for the focused item, or false.
static bool q_progress(const XMBItem *it, const ItemFacts *fx, bool hfx,
                       float *frac, u32 *runtime) {
    if (it->resume_secs < 10) return false;
    u32 rt = hfx ? fx->runtime_secs : 0;
    if (!rt && it->progress_pct > 0) rt = it->resume_secs * 100u / it->progress_pct;
    if (!rt || rt <= it->resume_secs) return false;
    *frac = (float)it->resume_secs / (float)rt;
    *runtime = rt;
    return true;
}

static void q_facts_gpu(int r, float dy, float alpha, float e) {
    if (s_rows[r].count <= 0) return;
    QFacts f;
    q_facts_layout(r, dy, alpha, e, &f);
    if (f.a <= 0.02f) return;
    const XMBItem *it = &s_rows[r].items[s_row_col[r]];
    ItemFacts fx;
    unsigned long long age = 0;
    const bool hfx = facts_get(it->id, &fx, &age);

    // The audio chip: outlined in the accent for lossless, hairline for not.
    if (f.tech && hfx && fx.audio[0]) {
        const float ta = f.a * spine_clampf((float)age / 250000.0f, 0.0f, 1.0f);
        const float px = UIS_TF(11.0f);
        const int cw = ttf_text_width_face(fx.audio, px, UI_FACE_SPEC) + UIS_W(20);
        wave_draw_rrect_outline_gpu(f.x, f.y_chip, cw, f.chip_h, f.chip_h / 2, 1,
                                    fx.lossless ? XMB_ACCENT_ALT : XMB_HAIRLINE, depth_a8(ta),
                                    XMB_BG, XMB_BG, depth_a8(ta));
    }
    // Progress: 320x4, the file's blue (the canvas's L1 and L2 frames).
    float frac;
    u32 rt;
    if (q_progress(it, &fx, hfx, &frac, &rt)) {
        const int pw = UIS_W(320), ph = UIS_H(4) > 2 ? UIS_H(4) : 2;
        ui_rect_gpu_draw(f.x, f.y_prog, pw, ph, XMB_TRACK, depth_a8(f.a));
        const int fw = (int)((float)pw * frac + 0.5f);
        if (fw > 0) ui_rect_gpu_draw(f.x, f.y_prog, fw, ph, XMB_ACCENT_ALT, depth_a8(f.a));
    }
}

void xmb_home_stage_gpu(void) {
    if (!ui_card_gpu_ready()) return;
    home_init_once();
    q_tick();
    QFrame q;
    q_frame(&q);
    ui_card_gpu_clip(q.clip_top, 0);

    depth_row rows[2];
    const int nr = depth_rows(s_qrow.value, HOME_ROWS_N, rows);
    for (int ri = 0; ri < nr; ri++) {
        const int r = rows[ri].r;
        if (s_rows[r].count <= 0) continue;
        QCtx ctx = { r, rows[ri].dy * q.e + q.settle_dy, &q, depth_xf() };
        const DepthStage st = q_stage(&ctx, rows[ri].w * q.near * q.settle_a);
        depth_stage_gpu(&st);
        q_facts_gpu(r, ctx.dy, rows[ri].w * q.near * q.settle_a, q.e);
    }
    ui_card_gpu_clip(0, 0);
}

// --- CPU phase ------------------------------------------------------------------------
//
// Thumbnail requests (the cache is driven by what is asked for each frame)
// and, only when the RSX card path is off, a plain opaque fallback.

void xmb_home_stage_cpu(void) {
    home_init_once();
    home_step_load();
    q_tick();
    if (g_play_gen != s_seen_gen) home_mark_dynamic_stale();

    QFrame q;
    q_frame(&q);

    // Requests: the focused row from one behind to two past what is drawn,
    // and the neighbouring rows' first few, so Up/Down lands on posters.
    // Focused row first: request order is the cache's priority (see
    // home_request_thumbs in ui_home.cpp).
    static const int k_order[3] = { 0, -1, +1 };
    for (int oi = 0; oi < 3; oi++) {
        const int r = s_focus_row + k_order[oi];
        if (r < 0 || r >= HOME_ROWS_N) continue;
        const HomeRow *row = &s_rows[r];
        if (row->count <= 0) continue;
        const int col = s_row_col[r];
        const bool focus = r == s_focus_row;
        QCtx ctx = { r, 0.0f, &q, depth_xf() };
        DepthStage st = q_stage(&ctx, 1.0f);
        st.i0 = col - 1;
        st.i1 = col + (focus ? DEPTH_KMAX + 1 : 2) + 1;
        depth_stage_request(&st);
        if (focus && !row_local(r) && facts_wanted(row->items[col].type) &&
            fabsf(s_qpos[r].value - (float)col) < 0.35f)
            facts_request(row->items[col].id);
    }

    if (ui_card_gpu_ready()) return;

    // The fallback: opaque cards where they are wholly on screen.
    g_cpu_clip_top = q.clip_top;
    depth_row rows[2];
    const int nr = depth_rows(s_qrow.value, HOME_ROWS_N, rows);
    for (int ri = 0; ri < nr; ri++) {
        QCtx ctx = { rows[ri].r, rows[ri].dy * q.e + q.settle_dy, &q, depth_xf() };
        const DepthStage st = q_stage(&ctx, 1.0f);
        depth_stage_cpu(&st, 0.3f);
    }
    g_cpu_clip_top = 0;
}

// --- text -------------------------------------------------------------------------------

// What the item is called on this screen: an episode goes by its series.
static const char *q_title(int r, int c) {
    const XMBItem *it = &s_rows[r].items[c];
    if (strcmp(it->type, "Episode") == 0 && s_extra[r][c].series_name[0])
        return s_extra[r][c].series_name;
    return it->name;
}

// The meta row, "a · b · c", separators at half strength, clipped to max_w by
// dropping trailing parts.
static void q_meta(int x, int y, int r, int c, float px, u32 col, u32 sep_col, int max_w) {
    const XMBItem *it = &s_rows[r].items[c];
    const HomeExtra *x2 = &s_extra[r][c];
    const char *part[6];
    char se[24] = "";
    int n = 0;
    if (strcmp(it->type, "Episode") == 0) {
        if (x2->season || x2->episode) {
            snprintf(se, sizeof se, "S%d E%d", x2->season, x2->episode);
            part[n++] = se;
        }
        if (x2->series_name[0]) part[n++] = it->name;   // the episode's own name
        if (it->duration_str[0]) part[n++] = it->duration_str;
    } else if (strcmp(it->type, "MusicAlbum") == 0) {
        if (it->artist[0])       part[n++] = it->artist;
        if (it->year_str[0])     part[n++] = it->year_str;
        if (it->duration_str[0]) part[n++] = it->duration_str;
    } else {
        if (it->year_str[0])     part[n++] = it->year_str;
        if (it->duration_str[0]) part[n++] = it->duration_str;
        if (x2->rating[0])       part[n++] = x2->rating;
        if (it->genre[0])        part[n++] = it->genre;
    }
    const int gap = UIS_W(10);
    const int sw = ttf_text_width("\xC2\xB7", px);
    int cx = x;
    for (int i = 0; i < n; i++) {
        char buf[132];
        snprintf(buf, sizeof buf, "%s", part[i]);
        int w = ttf_text_width(buf, px);
        if (cx + w > x + max_w) {
            if (i == 0) {   // the first part always shows, cut to fit
                depth_fit_text(buf, sizeof buf, part[i], px, UI_FACE_REGULAR, max_w);
                drawTTF((u32)cx, (u32)y, buf, px, col);
            }
            break;
        }
        if (i) {
            drawTTF((u32)cx, (u32)y, "\xC2\xB7", px, sep_col);
            cx += sw + gap;
        }
        drawTTF((u32)cx, (u32)y, buf, px, col);
        cx += w + gap;
    }
}

static void q_hms(char *out, size_t cap, u32 s) {
    snprintf(out, cap, "%u:%02u:%02u", s / 3600, (s / 60) % 60, s % 60);
}

static void q_facts_text(int r, float dy, float alpha, float e) {
    QFacts f;
    q_facts_layout(r, dy, alpha, e, &f);
    const float lvl = depth_l2_facts_alpha(spine_depth());
    const HomeRow *row = &s_rows[r];
    const int max_w = UIS_W((int)DEPTH_FACTS_W);
    char line[160];

    // The eyebrow is the category and stays up through a queue move; the
    // rest is the focused item's and waits for it to settle.
    const float ea = alpha * lvl;
    if (ea > 0.02f) {
        const int adv = xmb_draw_eyebrow(f.x, f.y_eye, tr(row->title),
                                         depth_mix_q(XMB_BG, XMB_ACCENT_ALT, ea));
        if (row->count > 0) {
            snprintf(line, sizeof line, TR("%d of %d"), s_row_col[r] + 1, row->count);
            drawTTF_face((u32)(f.x + adv + UIS_W(12)), (u32)f.y_eye, line, UIS_TF(11.0f),
                         depth_mix_q(XMB_BG, XMB_TEXT_FAINT, ea), UI_FACE_SPEC);
        }
    }
    if (row->count <= 0) {
        if (ea > 0.02f)
            drawTTF_face((u32)f.x, (u32)f.y_title, row->loaded ? TR("Nothing here yet") : TR("Loading..."),
                         UIS_TF(36.0f), depth_mix_q(XMB_BG, XMB_TEXT_DIM, ea), UI_FACE_DISPLAY);
        return;
    }
    if (f.a <= 0.02f) return;

    const int c = s_row_col[r];
    const XMBItem *it = &row->items[c];
    const float tpx = UIS_TF(36.0f);
    depth_fit_text(line, sizeof line, q_title(r, c), tpx, UI_FACE_DISPLAY, max_w);
    drawTTF_face((u32)f.x, (u32)f.y_title, line, tpx, depth_mix_q(XMB_BG, XMB_TEXT, f.a), UI_FACE_DISPLAY);
    q_meta(f.x, f.y_meta, r, c, UIS_TF(13.5f), depth_mix_q(XMB_BG, XMB_TEXT_DIM, f.a),
           depth_mix_q(XMB_BG, XMB_TEXT_DIM, f.a * 0.5f), max_w);

    ItemFacts fx;
    unsigned long long age = 0;
    const bool hfx = facts_get(it->id, &fx, &age);
    if (f.tech && hfx) {
        const float ta = f.a * spine_clampf((float)age / 250000.0f, 0.0f, 1.0f);
        const float lpx = UIS_TF(9.0f), vpx = UIS_TF(11.0f), trk = lpx * 0.1f;
        const u32 lc = depth_mix_q(XMB_BG, XMB_TEXT_FAINT, ta), vc = depth_mix_q(XMB_BG, XMB_TEXT_DIM, ta);
        const char *lab[3] = { TR("CONTAINER"), TR("VIDEO"), TR("SUBTITLES") };
        const char *val[3] = { fx.container[0] ? fx.container : "-",
                               fx.video[0] ? fx.video : "-", fx.subs };
        int x = f.x;
        for (int i = 0; i < 3; i++) {
            drawTTF_tracked((u32)x, (u32)f.y_lab, lab[i], lpx, lc, UI_FACE_SPEC, trk);
            drawTTF_face((u32)x, (u32)f.y_val, val[i], vpx, vc, UI_FACE_SPEC);
            const int lw = ttf_text_width_tracked(lab[i], lpx, UI_FACE_SPEC, trk);
            const int vw = ttf_text_width_face(val[i], vpx, UI_FACE_SPEC);
            x += (lw > vw ? lw : vw) + UIS_W(40);
        }
        if (fx.audio[0]) {
            drawTTF_tracked((u32)f.x, (u32)f.y_alab, TR("AUDIO"), lpx, lc, UI_FACE_SPEC, trk);
            const int ty = f.y_chip + (f.chip_h - (int)vpx) / 2 - UIS_H(1);
            drawTTF_face((u32)(f.x + UIS_W(10)), (u32)ty, fx.audio, vpx,
                         depth_mix_q(XMB_BG, fx.lossless ? XMB_ACCENT_ALT : XMB_TEXT_DIM, ta),
                         UI_FACE_SPEC);
            if (fx.audio_more[0]) {
                const int cw = ttf_text_width_face(fx.audio, vpx, UI_FACE_SPEC) + UIS_W(20);
                drawTTF_face((u32)(f.x + cw + UIS_W(12)), (u32)ty, fx.audio_more, vpx,
                             depth_mix_q(XMB_BG, XMB_TEXT_FAINT, ta), UI_FACE_SPEC);
            }
        }
    }

    float frac;
    u32 rt;
    if (q_progress(it, &fx, hfx, &frac, &rt)) {
        char el[16];
        q_hms(el, sizeof el, it->resume_secs);
        const u32 left = rt - it->resume_secs;
        if (left >= 3600) snprintf(line, sizeof line, TR("%s WATCHED \xC2\xB7 %uH %02uM LEFT"),
                                   el, left / 3600, (left / 60) % 60);
        else              snprintf(line, sizeof line, TR("%s WATCHED \xC2\xB7 %uM LEFT"),
                                   el, left / 60 > 0 ? left / 60 : 1);
        drawTTF_face((u32)f.x, (u32)f.y_watch, line, UIS_TF(11.0f),
                     depth_mix_q(XMB_BG, XMB_TEXT_FAINT, f.a), UI_FACE_SPEC);
    }
}

// L1's label block beside the column (the engine's depth_l1_label_draw, with
// Home's meta row): eyebrow and position, a 30 px title, one meta line.
typedef struct { int r, c; } QMetaCtx;
static void q_label_meta(const void *vctx, int x, int y, int max_w, float a) {
    const QMetaCtx *m = (const QMetaCtx *)vctx;
    if (m->c < 0) return;
    q_meta(x, y, m->r, m->c, UIS_TF(14.0f), depth_mix_q(XMB_BG, XMB_TEXT_DIM, a),
           depth_mix_q(XMB_BG, XMB_TEXT_DIM, a * 0.5f), max_w);
}

static void q_column_text(const QFrame &q) {
    const float a = depth_l1_label_alpha(spine_depth(), q.near);
    if (a <= 0.02f) return;
    const int r = s_focus_row;
    const HomeRow *row = &s_rows[r];
    char pos[24] = "";
    if (row->count > 0) snprintf(pos, sizeof pos, "%d/%d", s_row_col[r] + 1, row->count);
    const char *title = row->count > 0 ? q_title(r, s_row_col[r])
                      : row->loaded ? TR("Nothing here yet") : TR("Loading...");
    const QMetaCtx m = { r, row->count > 0 ? s_row_col[r] : -1 };
    depth_l1_label_draw(q_shape(r), q.anchor_x, a,
                        row->count > 0 ? tr(row->title) : TR("Home"), pos, title,
                        q_label_meta, &m);
}

void xmb_home_stage_text(void) {
    home_init_once();
    q_tick();
    QFrame q;
    q_frame(&q);
    g_cpu_clip_top = q.clip_top;
    q_column_text(q);
    depth_row rows[2];
    const int nr = depth_rows(s_qrow.value, HOME_ROWS_N, rows);
    for (int ri = 0; ri < nr; ri++) {
        const float dy = rows[ri].dy * q.e + q.settle_dy;
        q_facts_text(rows[ri].r, dy, rows[ri].w * q.settle_a, q.e);
    }
    g_cpu_clip_top = 0;
}

// --- the screensaver's artwork -----------------------------------------------------------

int xmb_home_ambient_art(AmbientArt *out, int max) {
    home_init_once();
    static char meta[16][16];
    static const int order[4] = { HR_CONTINUE, HR_NEXTUP, HR_MOVIES, HR_SHOWS };
    int n = 0;
    for (int oi = 0; oi < 4 && n < max; oi++) {
        const int r = order[oi];
        const HomeRow *row = &s_rows[r];
        if (!row->loaded) continue;
        for (int c = 0; c < row->count && c < 4 && n < max; c++) {
            // One entry per image: Next Up often repeats Continue Watching's
            // series.
            const char *img = q_img_id(r, c);
            bool dup = false;
            for (int k = 0; k < n; k++) if (strcmp(out[k].img_id, img) == 0) dup = true;
            if (dup) continue;
            AmbientArt *a = &out[n];
            a->img_id  = img;
            q_src(r, &a->src_w, &a->src_h);
            a->eyebrow = row->title;
            a->title   = q_title(r, c);
            char *m = meta[n < 16 ? n : 15];
            const HomeExtra *x = &s_extra[r][c];
            if (strcmp(row->items[c].type, "Episode") == 0 && x->episode)
                snprintf(m, 16, "S%u E%u", (unsigned)x->season, (unsigned)x->episode);
            else
                snprintf(m, 16, "%s", row->items[c].year_str);
            a->meta = m;
            n++;
        }
    }
    return n;
}

// --- input ------------------------------------------------------------------------------

bool xmb_home_at_top(void) {
    if (!g_spine_on) return s_focus_row == 0;
    unsigned char v[HOME_ROWS_N];
    q_visit(v);
    return depth_at_top(v, HOME_ROWS_N, s_focus_row) != 0;
}

bool xmb_home_open_focused(void) {
    home_init_once();
    const HomeRow *row = &s_rows[s_focus_row];
    const int c = g_spine_on ? s_row_col[s_focus_row] : s_focus_col;
    if (row->count <= 0 || c < 0 || c >= row->count) return false;
    const XMBItem *it = &row->items[c];
    if (home_play_downloaded(it)) return true;
    if (strcmp(it->type, "Series") == 0) {
        spine_set_level(SPINE_L2);          // its seasons live inside the TV tab
        if (xmb_open_series(it)) init_btns();
        return true;
    }
    if (strcmp(it->type, "MusicAlbum") == 0) {
        music_screen_open_album(it, "Home");
        init_btns();
        return true;
    }
    XMBItem copy = *it;                     // the rows may refetch under detail
    xmb_show_item_info(&copy);
    init_btns();
    return true;
}

static void home_peek_open(void) { xmb_home_open_focused(); }

bool xmb_home_peek_focused(void) {
    home_init_once();
    const HomeRow *row = &s_rows[s_focus_row];
    const int c = g_spine_on ? s_row_col[s_focus_row] : s_focus_col;
    if (row->count <= 0 || c < 0 || c >= row->count) return false;
    const XMBItem *it = &row->items[c];
    if (strncmp(it->id, "offp:", 5) == 0) return false;     // a download has no page to peek
    if (strcmp(it->type, "MusicAlbum") == 0) return xmb_home_open_focused();
    peek_open_item(it, 0, 0, home_peek_open);   // the stage notes the image
    return true;
}

static bool home_queue_input(void) {
    if (BTN_PRESSED(l1)) { xmb_switch_tab(xmb_next_enabled(g_active_tab, -1)); return false; }
    if (BTN_PRESSED(r1)) { xmb_switch_tab(xmb_next_enabled(g_active_tab, +1)); return false; }

    unsigned char v[HOME_ROWS_N];
    q_visit(v);
    if (BTN_REPEAT(up))   s_focus_row = depth_step_visitable(v, HOME_ROWS_N, s_focus_row, -1);
    if (BTN_REPEAT(down)) s_focus_row = depth_step_visitable(v, HOME_ROWS_N, s_focus_row, +1);
    const int n = s_rows[s_focus_row].count;
    int *col = &s_row_col[s_focus_row];
    if (BTN_REPEAT(right) && *col + 1 < n) (*col)++;
    if (BTN_REPEAT(left)  && *col > 0)     (*col)--;
    s_focus_col = *col;

    // X opens (detail); Triangle is the quick-peek.
    if (BTN_PRESSED(cross))         xmb_home_open_focused();
    else if (BTN_PRESSED(triangle)) xmb_home_peek_focused();
    return false;
}
