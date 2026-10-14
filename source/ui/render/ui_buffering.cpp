// The playback buffering screen.  See ui_buffering.h and buffer_anim.h.
//
// LAYERS, back to front (all GPU, in one GPU phase):
//   1. the item's artwork, opaque, full screen: the detail page's backdrop,
//      else its poster cropped to a wide band (LINEAR upscaling softens it,
//      which is what a background wants); with neither, the accent's deep
//      shade as a soft pool of light;
//   2. the veil: a vertical black ramp, darkest at the bottom where the title
//      sits.  It also carries the fade: at ui_a = 0 the veil is opaque black;
//   3. a soft glow behind the mark, in the artwork's glow shade, breathing;
//   4. the ring's faint full-circle track, then the arc in Jellyfin's own
//      colours (#AA5CC3 purple at the transparent tail -> #00A4DC blue at
//      the head; closed, purple -> blue -> purple, no seam);
//   5. the Jellyfin mark -- the graphic only, no wordmark.
// Then rsxSync and the text: BUFFERING / 47% under the ring, the title at the
// lower left, and the O hint.

#include <stdio.h>
#include <string.h>

#include <sysutil/sysutil.h>

#include "ui_buffering.h"
#include "buffer_anim.h"
#include "ui_art.h"
#include "ui_card_gpu.h"
#include "ui_wave.h"
#include "ui_text_gpu.h"
#include "ui.h"
#include "ui_visuals.h"
#include "rsxutil.h"
#include "timing.h"
#include <unistd.h>
#include <sys/thread.h>

static buf_anim    s_anim;
static bool        s_on = false;
static bool        s_flip_pending = false;
static u64         s_last_draw = 0;
static char        s_title[128];
static char        s_label[48] = TRN("BUFFERING");
static bool        s_show_pct = false;
static const char *s_hint = NULL;
static int         s_art = -1;           // GPU_TEX_* slot, or -1
static art_palette s_pal;

// Jellyfin's brand colours: the ring is drawn in them whatever the artwork.
#define JF_PURPLE 0x00AA5CC3
#define JF_BLUE   0x0000A4DC

static int BX(int x) { return XMB_OX + UIS_W(x); }
static int BY(int y) { return XMB_OY + UIS_H(y); }

static inline u8 a8(float a) {
    a = a < 0.0f ? 0.0f : (a > 1.0f ? 1.0f : a);
    return (u8)(a * 255.0f + 0.5f);
}

void buffering_begin(const char *item_id, const char *title) {
    buf_anim_start(&s_anim, timing_get_us());
    s_on = true;
    s_flip_pending = false;
    s_last_draw = 0;
    snprintf(s_title, sizeof s_title, "%s", title ? title : "");
    snprintf(s_label, sizeof s_label, "%s", TRN("PREPARING"));
    s_show_pct = false;
    s_hint = NULL;
    // Only artwork that is known to be THIS item's.  After an auto-advance to
    // the next episode the slots still hold the previous one's: better the
    // plain background than the wrong picture.
    s_art = -1;
    if (item_id && item_id[0] && ui_card_gpu_ready()) {
        if (strcmp(ui_gpu_tex_tag(GPU_TEX_BACKDROP), item_id) == 0)    s_art = GPU_TEX_BACKDROP;
        else if (strcmp(ui_gpu_tex_tag(GPU_TEX_POSTER), item_id) == 0) s_art = GPU_TEX_POSTER;
    }
    ui_art_peek(item_id, &s_pal);
}

void buffering_step(const char *label, bool show_pct, const char *circle_hint) {
    snprintf(s_label, sizeof s_label, "%s", label ? label : "");
    s_show_pct = show_pct;
    s_hint = circle_hint;
}

void buffering_progress(float p) { buf_anim_progress(&s_anim, p); }

bool buffering_active(void) { return s_on; }

static void draw_gpu(const buf_frame &f) {
    const int W = (int)display_width, H = (int)display_height;
    const float ua = f.ui_a;

    // 1. artwork
    bool art = false;
    if (s_art == GPU_TEX_BACKDROP) {
        art = ui_gpu_tex_draw(GPU_TEX_BACKDROP, 0, 0, W, H);
    } else if (s_art == GPU_TEX_POSTER) {
        // A 2:3 poster filling the width shows H / (W * 1.5) of its height;
        // take that band from the middle.
        float band = (float)H / ((float)W * 1.5f);
        if (band > 1.0f) band = 1.0f;
        art = ui_gpu_tex_draw_crop(GPU_TEX_POSTER, 0, 0, W, H,
                                   0.5f - band * 0.5f, 0.5f + band * 0.5f);
    }
    ui_card_gpu_end();

    // 2. the veil (and the fade).  Without artwork the clear colour is
    //    already near black and the accent's deep shade pools behind the
    //    mark instead.
    if (art) {
        static const float vp[4] = { 0.0f, 0.40f, 0.70f, 1.0f };
        const float base[4] = { 0.80f, 0.70f, 0.78f, 0.94f };
        u8 va[4];
        for (int i = 0; i < 4; i++) va[i] = a8(1.0f - ua * (1.0f - base[i]));
        wave_draw_ramp_gpu(0, 0, W, H, 0x00030408, true, 4, vp, va);
    } else {
        wave_draw_glow_gpu(BX(640), BY(330), UIS_W(620), UIS_H(420),
                           (u8)(s_pal.deep >> 16), (u8)(s_pal.deep >> 8), (u8)s_pal.deep,
                           a8(0.9f * ua));
    }

    // 3-5. glow, ring, mark
    const int cx = BX(640), cy = BY(318);
    const float R = (float)UIS_H(64), T = (float)UIS_H(3) < 2.0f ? 2.0f : (float)UIS_H(3);
    const u32 g = s_pal.glow;
    wave_draw_glow_gpu(cx, cy, UIS_H(150), UIS_H(150),
                       (u8)(g >> 16), (u8)(g >> 8), (u8)g, a8(f.glow_a * ua));
    if (f.track_a > 0.0f)
        wave_draw_ring_arc_gpu(cx, cy, R, T * 0.66f, 0.0f, 1.0f, 72,
                               0x00FFFFFF, a8(f.track_a * 0.6f * ua),
                               0x00FFFFFF, a8(f.track_a * 0.6f * ua));
    {
        // The ring is always Jellyfin's own colours, whatever the artwork:
        // purple #AA5CC3 at the (transparent) tail -> blue #00A4DC at the
        // head.  Closed, purple -> blue -> purple round its two halves, so
        // the full circle has no seam.
        const float arc = f.arc > 1.0f ? 1.0f : f.arc;
        const u8 head = a8(f.ring_a * ua);
        if (arc >= 0.999f) {
            const float a0 = f.ring_angle - 1.0f;
            wave_draw_ring_arc_gpu(cx, cy, R, T, a0, 0.5f, 48,
                                   JF_PURPLE, head, JF_BLUE, head);
            wave_draw_ring_arc_gpu(cx, cy, R, T, a0 + 0.5f, 0.5f, 48,
                                   JF_BLUE, head, JF_PURPLE, head);
        } else {
            int segs = (int)(96.0f * arc);
            if (segs < 8) segs = 8;
            wave_draw_ring_arc_gpu(cx, cy, R, T, f.ring_angle - arc, arc, segs,
                                   JF_PURPLE, 0, JF_BLUE, head);
        }
    }
    wave_draw_jf_logo_gpu(cx, cy, (float)UIS_H(34) * f.mark_scale,
                          JF_PURPLE, JF_BLUE, a8(f.mark_a * ua));
}

static void draw_text(const buf_frame &f) {
    // Quantised fades (depth_mix_q's rule): a fading label reuses cached runs.
    const float q = (float)(int)(f.ui_a * 16.0f + 0.5f) / 16.0f;
    const u32 bg = 0x00030408;
    const int W = (int)display_width;

    if (s_label[0]) {
        const int ew = xmb_eyebrow_width(tr(s_label));
        xmb_draw_eyebrow((W - ew) / 2, BY(408), tr(s_label), art_mix(bg, XMB_TEXT_DIM, q));
    }
    if (s_show_pct) {
        char pct[8];
        snprintf(pct, sizeof pct, "%d%%", f.pct);
        const float px = UIS_TF(20.0f);
        const int pw = ttf_text_width_face(pct, px, UI_FACE_DISPLAY);
        drawTTF_face((u32)((W - pw) / 2), (u32)BY(430), pct, px,
                     art_mix(bg, XMB_TEXT, q), UI_FACE_DISPLAY);
    }
    if (s_title[0]) {
        const float px = UIS_TF(22.0f);
        char t[128];
        snprintf(t, sizeof t, "%s", s_title);
        int len = (int)strlen(t);
        const int maxw = BX(1223) - BX(57);
        while (len > 1 && ttf_text_width_face(t, px, UI_FACE_DISPLAY) > maxw) t[--len] = 0;
        drawTTF_face((u32)BX(57), (u32)BY(606), t, px, art_mix(bg, XMB_TEXT, q * 0.92f),
                     UI_FACE_DISPLAY);
    }
    if (s_hint && f.ui_a > 0.5f && s_anim.phase == BUF_LOADING) {
        Hint h[1];
        h[0].glyph = 'C'; h[0].label = s_hint;
        draw_hints_bar(h, 1);
    }
}

void buffering_frame(void) {
    if (!s_on) return;
    if (s_flip_pending) waitflip_timeout(250000);   // bounded: see buffering_frame
    const u64 now = timing_get_us();
    s_last_draw = now;
    buf_frame f;
    buf_anim_eval(&s_anim, now, &f);

    clearScreen(0x00030408);
    draw_gpu(f);
    rsxSync();
    ui_text_gpu_begin();
    draw_text(f);
    ui_text_gpu_flush();
    flip();
    s_flip_pending = true;
#if BUILD_FOR_RPCS3
    // Same rule as player_startup_flip(): the emulator must not have a frame
    // in flight across the long heap reshuffles that follow these screens.
    rsxSync();
#endif
}

void buffering_frame_paced(u64 min_us) {
    if (!s_on) return;
    if (s_last_draw && timing_get_us() - s_last_draw < min_us) return;
    buffering_frame();
}

// Every flip wait in this file is bounded (2026-09-27): the 24p check draws and
// flips its own prompt while this screen is up, and resets the flip status as
// it goes -- so a flip this screen still thinks is pending never completes, and
// an unbounded waitflip() froze playback at "p8" after the user pressed X.
void buffering_finish(bool ready) {
    if (!s_on) return;
    const u64 now = timing_get_us();
    if (ready) buf_anim_ready(&s_anim, now);
    else       buf_anim_cancel(&s_anim, now);
    // The outro runs at the display's own pace (one frame per flip); it is
    // bounded by buffer_anim.h (at most BUF_MIN_SHOW_US + ~0.45 s).
    while (running && !buf_anim_done(&s_anim, timing_get_us())) {
        sysUtilCheckCallback();
        buffering_frame();
    }
    // One last black frame so nothing of the screen lingers under the
    // player's first video frame.
    if (running) {
        if (s_flip_pending) waitflip_timeout(250000);   // bounded: see buffering_frame
        clearScreen(0x00000000);
        flip();
        s_flip_pending = true;
    }
    s_on = false;
}

// --- loading_run -------------------------------------------------------------
//
// 2026-09-25: "add a loading screen for the pause when pressing X to open an
// item details page".  The pause is the page's blocking fetches (details,
// versions, poster, backdrop).  They run on a thread; this draws.
static void (*volatile s_lw_fn)(void *) = NULL;
static void *volatile  s_lw_arg = NULL;
static volatile bool   s_lw_done = false;

static void lw_thread(void *arg)
{
    (void)arg;
    s_lw_fn(s_lw_arg);
    __sync_synchronize();
    s_lw_done = true;
    sysThreadExit(0);
}

bool loading_run(void (*work)(void *), void *arg, const char *label,
                 bool flip_pending)
{
    if (!work) return flip_pending;
    s_lw_fn = work;
    s_lw_arg = arg;
    s_lw_done = false;
    __sync_synchronize();
    sys_ppu_thread_t tid;
    static char name[] = "jf_loading";
    if (sysThreadCreate(&tid, lw_thread, NULL, 1100, 128 * 1024,
                        THREAD_JOINABLE, name) != 0) {
        work(arg);                      // no thread: the old blocking way
        return flip_pending;
    }
    const u64 t0 = timing_get_us();
    while (!s_lw_done) {
        const u64 now = timing_get_us();
        if (now - t0 < 120000ULL || !running) { usleep(4000); continue; }
        sysUtilCheckCallback();
        if (flip_pending) waitflip_timeout(250000);
        const float t  = (float)(now - t0 - 120000ULL) * 1.0e-6f;
        float a = t / 0.25f;
        a = a > 1.0f ? 1.0f : a;
        a = a * a * (3.0f - 2.0f * a);

        clearScreen(XMB_BG);
        wave_draw();
        const int cx = (int)display_width / 2, cy = (int)((float)display_height * 0.46f);
        const float R = (float)UIS_H(40), T = UIS_H(3) < 2 ? 2.0f : (float)UIS_H(3);
        const float ang = t * 0.85f;
        wave_draw_ring_arc_gpu(cx, cy, R, T * 0.66f, 0.0f, 1.0f, 64,
                               0x00FFFFFF, a8(0.12f * a), 0x00FFFFFF, a8(0.12f * a));
        wave_draw_ring_arc_gpu(cx, cy, R, T, ang - 0.32f, 0.32f, 28,
                               JF_PURPLE, 0, JF_BLUE, a8(a));
        wave_draw_jf_logo_gpu(cx, cy, (float)UIS_H(20), JF_PURPLE, JF_BLUE, a8(0.9f * a));
        rsxSync();
        ui_text_gpu_begin();
        if (label && label[0]) {
            const float q = (float)(int)(a * 16.0f + 0.5f) / 16.0f;
            const int ew = xmb_eyebrow_width(label);
            xmb_draw_eyebrow(((int)display_width - ew) / 2, cy + (int)R + UIS_H(22), label,
                             art_mix(XMB_BG, XMB_TEXT_DIM, q));
        }
        ui_text_gpu_flush();
        flip();
        flip_pending = true;
    }
    __sync_synchronize();
    u64 r;
    sysThreadJoin(tid, &r);
    return flip_pending;
}
