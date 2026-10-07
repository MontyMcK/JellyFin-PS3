// The ambient screensaver ("film-reel" mode).  State and motion are
// render/experience.h (amb_*), host-tested; this file draws them.
//
// After a few idle minutes on the XMB the interface dissolves under a dark
// cover, the cover lifts part way so the JellyWave shows again -- dimmed,
// otherwise exactly as it always draws -- and one poster at a time drifts
// slowly across the middle of the screen, with a soft glow in its own colour
// (render/art_colour.h), a faint reflection, the lockup and clock, and one
// line of metadata.  Any button dissolves it back; that press does nothing
// else.
//
// COST.  While it runs, the XMB's own phases are not drawn at all, so the
// frame is cheaper than a normal one: the wave, one full-screen blended quad,
// one glow fan, two textured quads and a few cached text runs.  The artwork is
// what Home already has in its thumbnail cache, at the size Home caches it,
// so nothing new is fetched or decoded.  No allocation, no framebuffer read,
// no extra rsxSync.  The JellyWave renderer is not touched.
//
// Only with the spine gate on.  jellyfin_ambient.txt sets the idle time in
// minutes (0 = never); missing = 3.

#include <stdio.h>
#include <string.h>

#include "ui_internal.h"
#include "ui_render_internal.h"
#include "ui_card_gpu.h"
#include "ui_wave.h"
#include "ui_art.h"
#include "experience.h"
#include "depth.h"          // depth_mix_q
#include "ui_depth.h"       // depth_fit_text
#include "thumbnail_cache.h"
#include "timing.h"
#include "plog.h"
#include "jf_paths.h"

#define AMB_FILE  "jellyfin_ambient.txt"
#define AMB_MAX   12

static amb_state  s_amb;
static bool       s_amb_ready = false;
static AmbientArt s_art[AMB_MAX];
static int        s_n = 0;
static int        s_last_phase = AMB_OFF;
static amb_frame  s_f;

static void ambient_init_once(void) {
    if (s_amb_ready) return;
    s_amb_ready = true;
    int minutes = 3;
    FILE *fp = fopen(jf_data_path(AMB_FILE), "r");
    if (fp) {
        if (fscanf(fp, "%d", &minutes) != 1) minutes = 3;
        fclose(fp);
    }
    if (minutes < 0) minutes = 0;
    if (minutes > 120) minutes = 120;
    amb_init(&s_amb, timing_get_us(), (unsigned long long)minutes * 60000000ull);
    char b[64];
    snprintf(b, sizeof b, "ambient: idle %d min%s", minutes, minutes ? "" : " (off)");
    plog(b);
}

static bool any_button(void) {
    return btn_cur.up || btn_cur.down || btn_cur.left || btn_cur.right ||
           btn_cur.cross || btn_cur.circle || btn_cur.square || btn_cur.triangle ||
           btn_cur.start || btn_cur.select ||
           btn_cur.l1 || btn_cur.r1 || btn_cur.l2 || btn_cur.r2;
}

bool ambient_update(bool allowed) {
    if (!g_spine_on) return false;
    ambient_init_once();
    const unsigned long long now = timing_get_us();
    // A modal (the update popup, overscan calibration) counts as activity:
    // the screensaver never starts over one.
    // A gap since the last XMB frame means another screen had the display
    // (detail, music, a film): that was activity, not idleness.
    static unsigned long long s_prev = 0;
    const bool away = s_prev && now - s_prev > 1000000ull;
    s_prev = now;
    const int swallow = amb_tick(&s_amb, now, any_button() || !allowed || away);
    if (s_amb.phase != s_last_phase) {
        if (s_amb.phase == AMB_ON) {
            s_n = xmb_home_ambient_art(s_art, AMB_MAX);
            char b[48];
            snprintf(b, sizeof b, "ambient: on, %d posters", s_n);
            plog(b);
        } else if (s_amb.phase == AMB_OFF) {
            plog("ambient: off");
        }
        s_last_phase = s_amb.phase;
    }
    amb_eval(&s_amb, now, &s_f);
    return swallow != 0;
}

bool ambient_draw_ui(void) { return !g_spine_on || s_f.draw_ui; }

static inline u8 a8(float a) {
    a = a < 0.0f ? 0.0f : (a > 1.0f ? 1.0f : a);
    return (u8)(a * 255.0f + 0.5f);
}

// Where the current slide's poster sits this frame, screen px.
typedef struct { int x, y, w, h; float a; int idx; } AmbPoster;

static bool ambient_poster(AmbPoster *p) {
    if (s_n <= 0) return false;
    amb_slide sl;
    amb_slide_at(timing_get_us() - s_amb.t_on, s_n, &sl);
    if (sl.idx < 0) return false;
    const float bw = 232.0f * sl.scale, bh = 348.0f * sl.scale;
    const float cx = 430.0f + sl.dx, cy = 330.0f + sl.dy;
    p->x = XMB_OX + UIS_W((int)(cx - bw * 0.5f));
    p->y = XMB_OY + UIS_H((int)(cy - bh * 0.5f));
    p->w = UIS_W((int)bw);
    p->h = UIS_H((int)bh);
    p->a = sl.a * s_f.art_a;
    p->idx = sl.idx;
    return true;
}

// GPU phase, in place of the XMB's own (only when !ambient_draw_ui()).
void ambient_gpu(void) {
    if (!g_spine_on || !s_f.draw_ambient) return;
    const int W = (int)display_width, H = (int)display_height;
    ui_rect_gpu_draw(0, 0, W, H, 0x00000000, a8(s_f.cover));

    AmbPoster p;
    if (ambient_poster(&p)) {
        const AmbientArt *a = &s_art[p.idx];
        // Ask for this slide and the next so the change never waits on a
        // decode; the cache ages out anything no longer asked for.
        thumb_request(a->img_id, a->src_w, a->src_h);
        const AmbientArt *nx = &s_art[(p.idx + 1) % s_n];
        thumb_request(nx->img_id, nx->src_w, nx->src_h);

        const art_palette pal =
            ui_art_palette(a->img_id, thumb_get(a->img_id, a->src_w, a->src_h));
        const u32 g = pal.glow;
        wave_draw_glow_gpu(p.x + p.w / 2, p.y + p.h / 2, p.w, (p.h * 3) / 4,
                           (u8)(g >> 16), (u8)(g >> 8), (u8)g, a8(0.30f * p.a));
        u32 off = 0, pitch = 0;
        if (thumb_gpu_texture(a->img_id, a->src_w, a->src_h, &off, &pitch)) {
            const u8 pa = a8(p.a);
            ui_card_gpu_draw_ex(off, (u32)a->src_w, (u32)a->src_h, pitch,
                                p.x, p.y, p.w, p.h, 0.0f, 1.0f, pa);
            // A faint reflection: the bottom sixth, mirrored.
            const int rh = p.h / 6;
            ui_card_gpu_draw_ex(off, (u32)a->src_w, (u32)a->src_h, pitch,
                                p.x, p.y + p.h + UIS_H(4), p.w, rh,
                                1.0f, 1.0f - (float)rh / (float)p.h, a8(0.14f * p.a));
        }
    }
    ui_card_gpu_end();
}

// Text phase (inside the text-run window).
void ambient_text(void) {
    if (!g_spine_on || !s_f.draw_ambient) return;
    xmb_draw_topbar();     // the lockup and the clock stay: it is still the XMB
    AmbPoster p;
    if (!ambient_poster(&p)) return;
    const AmbientArt *a = &s_art[p.idx];
    const float t = p.a;
    const int x = XMB_OX + UIS_W(640);
    const int max_w = (int)display_width - XMB_ITEM_PAD - x;
    if (max_w <= 0 || t < 0.04f) return;
    xmb_draw_eyebrow(x, XMB_OY + UIS_H(282), a->eyebrow ? a->eyebrow : "",
                     depth_mix_q(0x00000000, XMB_ACCENT_ALT, t * 0.9f));
    char line[160];
    const float tpx = UIS_TF(30.0f);
    depth_fit_text(line, sizeof line, a->title ? a->title : "", tpx, UI_FACE_DISPLAY, max_w);
    drawTTF_face((u32)x, (u32)(XMB_OY + UIS_H(306)), line, tpx,
                 depth_mix_q(0x00000000, XMB_TEXT, t), UI_FACE_DISPLAY);
    if (a->meta && a->meta[0])
        drawTTF((u32)x, (u32)(XMB_OY + UIS_H(352)), a->meta, UIS_TF(14.0f),
                depth_mix_q(0x00000000, XMB_TEXT_DIM, t));
}

// After the frame's text flush, while the UI is dissolving in or out: one
// blended quad over everything, FIFO-ordered after the text runs, so no
// fence is needed.
void ambient_cover_over_ui(void) {
    if (!g_spine_on || !s_f.draw_ui || s_f.cover <= 0.002f) return;
    ui_rect_gpu_draw(0, 0, (int)display_width, (int)display_height,
                     0x00000000, a8(s_f.cover));
    ui_card_gpu_end();
}
