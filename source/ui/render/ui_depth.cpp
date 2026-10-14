// The depth engine's draw half -- see ui_depth.h for the model and depth.h
// for every number.  This file decides no geometry; it draws boxes.
//
// It is spine6's Home stage renderer (xmb/ui_home_stage.inc: q_card_gpu,
// q_halo_glow, q_halo_bands, the CPU fallback, q_column_text) lifted out of
// Home and driven by a callback, so the libraries draw through the same code.
// The drawing is unchanged: same primitives, same alphas, same order.
//
// COST.  Per visible card: one constant-alpha textured quad, at most one
// veil rect, and for a reflecting card six more quads of the same texture.
// The halo is one 14-vertex fan and a handful of rects.  No VRAM reads, no
// per-frame allocation (the per-stage card list is a fixed array on the
// stack), and every text colour goes through depth_mix_q so a fade hits the
// run cache.

#include <math.h>
#include <string.h>
#include <stdio.h>

#include "ui.h"
#include "ui_visuals.h"
#include "ui_render_internal.h"
#include "ui_card_gpu.h"
#include "ui_wave.h"
#include "ui_spine.h"
#include "ui_depth.h"

// A stage never has more than the queue's seven slots plus a screen of grid;
// the library swing draws the column and a 5x2 page at once.
#define DEPTH_STAGE_MAX 40

depth_xform depth_xf(void)
{
    return depth_xform_make(XMB_OX, XMB_OY, (int)display_width,
                            (int)display_height, g_uis_pct);
}

int depth_sx(float ax)
{
    const depth_xform xf = depth_xf();
    return (int)xf.ox + (int)floorf(ax * xf.kx + 0.5f);
}

int depth_sy(float ay)
{
    const depth_xform xf = depth_xf();
    return (int)xf.oy + (int)floorf(ay * xf.ky + 0.5f);
}

float depth_ax(int sx)
{
    const depth_xform xf = depth_xf();
    return ((float)sx - xf.ox) / xf.kx;
}

// The top of the hints bar's glyphs (draw_hints_bar: 24 px icons centred on
// H - OY - 22), less a 6 px breath.
int depth_floor_y(void)
{
    return (int)display_height - XMB_OY - UIS_H(22) - UIS_H(12) - UIS_H(6);
}

// --- the focus rect, for detail's arrival -------------------------------------

static int      s_fr_x, s_fr_y, s_fr_w, s_fr_h;
static unsigned s_fr_frame = 0;
static char     s_fc_id[64];
static int      s_fc_w, s_fc_h;
static ThumbImg s_fc_img = THUMB_IMG_PRIMARY;
static unsigned s_fc_frame = 0;

void depth_note_focus_rect(int x, int y, int w, int h)
{
    s_fr_x = x; s_fr_y = y; s_fr_w = w; s_fr_h = h;
    s_fr_frame = spine_frame_id();
    s_fc_frame = 0;
}

void depth_note_focus_card(const char *img_id, int src_w, int src_h, ThumbImg img)
{
    if (!img_id || !img_id[0] || src_w <= 0 || src_h <= 0) return;
    snprintf(s_fc_id, sizeof(s_fc_id), "%s", img_id);
    s_fc_w = src_w; s_fc_h = src_h; s_fc_img = img;
    s_fc_frame = spine_frame_id();
}

bool depth_last_focus_card(char *img_id, int id_sz, int *src_w, int *src_h, ThumbImg *img)
{
    if (!s_fc_frame || s_fc_frame + 2 < spine_frame_id()) return false;
    snprintf(img_id, (size_t)id_sz, "%s", s_fc_id);
    *src_w = s_fc_w; *src_h = s_fc_h; *img = s_fc_img;
    return true;
}

bool depth_last_focus_rect(int *x, int *y, int *w, int *h)
{
    if (!s_fr_frame || s_fr_w <= 0 || s_fr_h <= 0 ||
        s_fr_frame + 2 < spine_frame_id())
        return false;
    *x = s_fr_x; *y = s_fr_y; *w = s_fr_w; *h = s_fr_h;
    return true;
}

// --- one frame's cards, resolved and ordered --------------------------------------

typedef struct {
    DepthCard c[DEPTH_STAGE_MAX];
    float     dist[DEPTH_STAGE_MAX];
    int       n;
    int       nearest;     // index into c[] of the card nearest the focus
} StageList;

// Resolve every card in range and sort FAR TO NEAR.  Insertion sort: never
// more than a few dozen entries, already nearly ordered frame to frame.
static void stage_collect(const DepthStage *s, StageList *L)
{
    L->n = 0;
    L->nearest = -1;
    for (int i = s->i0; i < s->i1 && L->n < DEPTH_STAGE_MAX; i++) {
        DepthCard c;
        if (!s->card(s->ctx, i, &c)) continue;
        if (c.box.op * s->alpha <= 0.004f) continue;
        const float d = fabsf((float)i - s->pos);
        int j = L->n++;
        while (j > 0 && L->dist[j - 1] < d) {
            L->c[j] = L->c[j - 1];
            L->dist[j] = L->dist[j - 1];
            j--;
        }
        L->c[j] = c;
        L->dist[j] = d;
    }
    if (L->n) L->nearest = L->n - 1;
}

static void card_rect(const DepthCard *c, int *x, int *y, int *w, int *h)
{
    depth_rect(&c->box, x, y, w, h);
}

// --- GPU ------------------------------------------------------------------------------

// The canvas's focus treatment: a soft accent glow behind (drawn first), then
// three accent bands fading outward, a faint white line and the 2 px white
// frame.  `a` scales all of it.
static void halo_glow(int x, int y, int w, int h, float a)
{
    if (!wave_gpu_blend_ready() || a <= 0.02f) return;
    const u32 c = XMB_ACCENT;
    wave_draw_glow_gpu(x + w / 2, y + h / 2,
                       (int)(0.7f * (w / 2 + UIS_W(100))),
                       (int)(0.7f * (h / 2 + UIS_H(80))),
                       (u8)((c >> 16) & 0xFF), (u8)((c >> 8) & 0xFF), (u8)(c & 0xFF),
                       depth_a8(0.2f * a));
}

static void ring(int x, int y, int w, int h, int out, int t, u32 c, float a)
{
    const u8 al = depth_a8(a);
    if (!al || t <= 0) return;
    const int X = x - out, Y = y - out, W = w + 2 * out, H = h + 2 * out;
    ui_rect_gpu_draw(X, Y, W, t, c, al);
    ui_rect_gpu_draw(X, Y + H - t, W, t, c, al);
    ui_rect_gpu_draw(X, Y + t, t, H - 2 * t, c, al);
    ui_rect_gpu_draw(X + W - t, Y + t, t, H - 2 * t, c, al);
}

static void halo_bands(int x, int y, int w, int h, float a)
{
    if (a <= 0.02f) return;
    const u32 acc = XMB_ACCENT;
    ring(x, y, w, h, UIS_W(24), UIS_W(8), acc, 0.07f * a);
    ring(x, y, w, h, UIS_W(16), UIS_W(6), acc, 0.14f * a);
    ring(x, y, w, h, UIS_W(9),  UIS_W(6), acc, 0.24f * a);
    ring(x, y, w, h, UIS_W(3),  1,        XMB_WHITE, 0.27f * a);
    ring(x, y, w, h, 0,         UIS_W(2) > 2 ? UIS_W(2) : 2, XMB_WHITE, a);
}

// One card: the image at its opacity (or the dim placeholder while it loads),
// the veil, and the reflection -- the image's bottom rows mirrored, fading out
// in six strips from 0.34 x the card's opacity (the canvas's 46 px gradient).
static void card_gpu(const DepthCard *c, float alpha)
{
    int x, y, w, h;
    card_rect(c, &x, &y, &w, &h);
    if (w <= 0 || h <= 0 || x >= (int)display_width || x + w <= 0) return;
    const float op = c->box.op * alpha;
    u32 off = 0, pitch = 0;
    const bool have = c->img_id && c->src_w > 0 && c->src_h > 0 &&
                      thumb_gpu_texture(c->img_id, c->src_w, c->src_h,
                                        &off, &pitch, c->img);
    if (have)
        ui_card_gpu_draw_ex(off, (u32)c->src_w, (u32)c->src_h, pitch,
                            x, y, w, h, 0.0f, 1.0f, depth_a8(op));
    else
        ui_rect_gpu_draw(x, y, w, h, XMB_THUMB_DIM, depth_a8(op));
    if (c->box.veil > 0.01f)
        ui_rect_gpu_draw(x, y, w, h, DEPTH_VEIL_RGB, depth_a8(c->box.veil * alpha));

    const int gap = UIS_H(4) > 0 ? UIS_H(4) : 1;
    const int rh  = (int)floorf(depth_refl_clamp((float)y, (float)h, c->box.refl,
                                                 (float)gap, (float)depth_floor_y()) + 0.5f);
    if (have && rh >= 4) {
        const int N = 6;
        const float span = (float)rh / (float)h;       // texture rows covered
        for (int i = 0; i < N; i++) {
            const int y0 = y + h + gap + rh * i / N, y1 = y + h + gap + rh * (i + 1) / N;
            if (y1 <= y0) continue;
            const float v0 = 1.0f - span * (float)i / N;
            const float v1 = 1.0f - span * (float)(i + 1) / N;
            const float ra = 0.34f * op * (1.0f - ((float)i + 0.5f) / N);
            ui_card_gpu_draw_ex(off, (u32)c->src_w, (u32)c->src_h, pitch,
                                x, y0, w, y1 - y0, v0, v1, depth_a8(ra));
        }
    }
}

void depth_stage_gpu(const DepthStage *s)
{
    if (!ui_card_gpu_ready() || s->alpha <= 0.004f) return;
    StageList L;
    stage_collect(s, &L);
    if (!L.n) return;

    for (int i = 0; i < L.n; i++) {                   // glows, behind
        const DepthCard *c = &L.c[i];
        if (c->halo <= 0.0f) continue;
        int x, y, w, h;
        card_rect(c, &x, &y, &w, &h);
        halo_glow(x, y, w, h, c->halo * s->alpha);
    }
    for (int i = 0; i < L.n; i++)                     // cards, far to near
        card_gpu(&L.c[i], s->alpha);
    for (int i = 0; i < L.n; i++) {                   // bands + progress, over
        const DepthCard *c = &L.c[i];
        int x, y, w, h;
        card_rect(c, &x, &y, &w, &h);
        if (c->halo > 0.0f) halo_bands(x, y, w, h, c->halo * s->alpha);
        if (c->progress_pct > 0 && c->progress_a > 0.004f) {
            const int sh = UIS_H(4) > 2 ? UIS_H(4) : 2;
            const u8  sa = depth_a8(c->progress_a * s->alpha);
            ui_rect_gpu_draw(x, y + h - sh, w, sh, XMB_TRACK, sa);
            ui_rect_gpu_draw(x, y + h - sh, w * c->progress_pct / 100, sh,
                             XMB_ACCENT_ALT, sa);
        }
    }

    // Where the focus is, for item detail to fly out of -- from the stage that
    // dominates the screen only, so a neighbouring row passing through a
    // category glide cannot claim it.
    if (L.nearest >= 0 && L.dist[L.nearest] < 0.5f && s->alpha >= 0.5f) {
        int x, y, w, h;
        card_rect(&L.c[L.nearest], &x, &y, &w, &h);
        depth_note_focus_rect(x, y, w, h);
        const DepthCard *fc = &L.c[L.nearest];
        depth_note_focus_card(fc->img_id, fc->src_w, fc->src_h, fc->img);
    }
}

// --- CPU ------------------------------------------------------------------------------

void depth_stage_cpu(const DepthStage *s, float min_op)
{
    if (ui_card_gpu_ready() || s->alpha <= 0.004f) return;
    StageList L;
    stage_collect(s, &L);
    for (int i = 0; i < L.n; i++) {
        const DepthCard *c = &L.c[i];
        if (c->box.op < min_op) continue;
        int x, y, w, h;
        card_rect(c, &x, &y, &w, &h);
        if (x < 0 || y < 0 || x + w > (int)display_width || y + h > (int)display_height)
            continue;
        xmb_draw_card_src(c->img_id, c->src_w, c->src_h, x, y, w, h, 0,
                          c->selected, c->tile_name, c->img);
    }
}

void depth_stage_request(const DepthStage *s)
{
    for (int i = s->i0; i < s->i1; i++) {
        DepthCard c;
        if (!s->card(s->ctx, i, &c)) continue;
        if (c.img_id && c.src_w > 0 && c.src_h > 0)
            thumb_request(c.img_id, c.src_w, c.src_h, c.img);
    }
}

// --- text -----------------------------------------------------------------------------

void depth_fit_text(char *out, size_t cap, const char *s, float px, int face,
                    int max_w)
{
    snprintf(out, cap, "%s", s);
    if (ttf_text_width_face(out, px, face) <= max_w) return;
    size_t len = strlen(out);
    while (len > 0) {
        do { len--; } while (len > 0 && ((unsigned char)out[len] & 0xC0) == 0x80);
        if (len + 4 > cap) continue;
        memcpy(out + len, "...", 4);
        if (ttf_text_width_face(out, px, face) <= max_w) return;
    }
}

void depth_l1_label_draw(int shape, float anchor_x, float a,
                         const char *eyebrow, const char *pos,
                         const char *title, DepthMetaFn meta, const void *ctx)
{
    if (a <= 0.02f) return;
    const depth_l1_label L = depth_l1_label_layout(shape, anchor_x);
    const int tx    = depth_sx(L.x);
    const int max_w = (int)display_width - XMB_ITEM_PAD - tx;
    if (max_w <= 0) return;
    int ty = depth_sy(L.y_eye);

    char line[160];
    // No eyebrow (NULL): the title alone, on the line it always sits on.
    if (eyebrow) {
        const int adv = xmb_draw_eyebrow(tx, ty, eyebrow,
                                         depth_mix_q(XMB_BG, XMB_ACCENT_ALT, a));
        if (pos && pos[0])
            drawTTF_face((u32)(tx + adv + UIS_W(12)), (u32)ty, pos, UIS_TF(11.0f),
                         depth_mix_q(XMB_BG, XMB_TEXT_FAINT, a), UI_FACE_SPEC);
    }
    ty += UIS_H(24);

    const float tpx = UIS_TF(L.title_px);
    depth_fit_text(line, sizeof line, title ? title : "", tpx, UI_FACE_DISPLAY, max_w);
    drawTTF_face((u32)tx, (u32)ty, line, tpx, depth_mix_q(XMB_BG, XMB_TEXT, a),
                 UI_FACE_DISPLAY);
    ty += (int)(tpx * 1.35f);
    if (meta) meta(ctx, tx, ty, max_w, a);
}
