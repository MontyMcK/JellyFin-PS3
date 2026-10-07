// Now Playing — the music player screen (see the mockup this implements):
// big glowing album art on the left, the audio-reactive bar visualizer
// above the track title, artist in accent violet, album/year and source
// info lines, an UP NEXT queue on the right, transport icons, and a seek
// bar.  SELECT opens a queue overlay for jumping straight to a track.
//
// Runs its own blocking frame loop like the video player, but keeps the
// XMB's animated wave background so the screen still feels like the rest
// of the app.  All drawing here is CPU-phase (after rsxSync).

#include <stdio.h>
#include <string.h>
#include <math.h>

#include <sysutil/sysutil.h>

#include "ui_internal.h"
#include "ui_wave.h"
#include "ui_sfx.h"
#include "music_screen.h"
#include "music_player.h"
#include "music_fft.h"
#include "plog.h"
#include "timing.h"
#include "thumbnail_cache.h"
#include "ui_art.h"
#include "ui_card_gpu.h"   // ui_card_gpu_ready
#include "ui_text_gpu.h"
#include "ui_buffering.h"
#include "ui_canyon.h"       // Canyon visualizer (L2 / R2, Square)
#include "ui_depth.h"        // depth_last_focus_rect: where the album was

// The album art's accent (render/art_colour.h, sampled once per album from the
// thumbnail already in main memory) tints the visualiser, the artist line and
// the seek bar.  With the spine gate off it is the theme accent, as before.
static art_palette s_mpal;

// The size the cover is fetched at -- the same rule as xmb_cpu_blit_thumb, so
// this reads the very bitmap that is on screen and fetches nothing new.
static const Bitmap *music_art_bitmap(const char *art_id, int A) {
    int rw = A, rh = A, cap = thumb_max_square();
    if (rw > cap || rh > cap || (size_t)rw * rh > (size_t)cap * cap) {
        rw = rw < cap ? rw : cap;
        rh = rh < cap ? rh : cap;
    }
    return thumb_get(art_id, rw, rh);
}

// The cover on the RSX, in the GPU phase (after the wave, before the fence),
// from the thumbnail's VRAM mirror.  2026-09-24: the screen ran at ~25 fps --
// everything on it was drawn by the PPU, and the 453 px cover alone is a
// scaled blit of ~205k pixels into video memory every frame.  False (and the
// CPU blit in draw_now_playing takes over) until the texture is up.
static bool s_cover_gpu = false;

// --- presentation fades (2026-09-24) ----------------------------------------
//
// s_scr_a   the whole screen's content; 1 -> 0 over the outro when leaving,
//           so the screen dissolves to the wave instead of cutting.
// s_up_a    Up Next; fades out in focus mode.
// s_ctl_a   transport, seek bar and times; all fade away in focus mode.
//
// Focus mode: after FOCUS_AFTER_US of playback with no button pressed, the
// screen settles -- Up Next fades away and the controls go translucent --
// until the next press brings them back.  Both directions are eased.
//
// Shapes and text fade by mixing their colour toward the background, in
// sixteenths (so a fading label re-uses its cached GPU text runs, the same
// rule as depth_mix_q); images fade with real alpha on the RSX.
#define FOCUS_AFTER_US   4000000ULL
#define FOCUS_IN_US       650000.0f
#define FOCUS_OUT_US      220000.0f
#define FOCUS_CTL_A          0.00f   // 2026-09-25: the controls fade away too
#define OUTRO_US          480000.0f
#define INTRO_US          420000.0f
static float s_scr_a = 1.0f, s_up_a = 1.0f, s_ctl_a = 1.0f;
static float s_focus_p = 0.0f;          // 0 = normal .. 1 = focused
static bool  s_outro = false;
static u64   s_outro_t0 = 0, s_fade_us = 0, s_last_input_us = 0;
static u64   s_intro_t0 = 0;
static bool  s_entry_flipped = false;   // the opener already finished the XMB frame
static u64   s_viz_toast_us = 0;        // "Visualizer: ..." label shown until then

// --- the choreography (2026-09-25) ------------------------------------------
//
// The screen assembles itself instead of fading in as one sheet, and comes
// apart the other way when you leave:
//
//   cover    flies out of the album tile you pressed (or, from Home, Songs or
//            a playlist, rises out of depth at the centre), eased with a
//            touch of overshoot, into its place on the left
//   text     title, artist, album and meta slide out from BEHIND the cover
//            (clipped at its right edge) and fade up
//   panel    Up Next slides in from the right edge, an XMB-style side panel
//   controls transport and seek rise from below
//
// Leaving reverses it: the text slides back behind the cover, the panel out,
// the controls down, then the cover recedes into depth.  Each value below is
// 0 (not there) .. 1 (in place); the cover's can overshoot 1 briefly.
static float s_e_cover = 1.0f, s_e_text = 1.0f, s_e_panel = 1.0f, s_e_ctl = 1.0f;

// Canyon's layout, like the stock PS3 player's: while the Canyon visualizer
// is up the cover and its text shrink into the bottom-left corner and Up
// Next steps aside, so the landscape has the screen.  0 = normal .. 1 =
// mini; s_mini_e is the eased value everything reads.  It is also the
// canyon's fade: the canyon comes in over the wave as the cover goes down.
static float s_mini = 0.0f, s_mini_e = 0.0f;
#define MINI_MS 650.0f
static bool  s_have_origin = false;     // the album tile's rect is known

// ---- the visualiser on Square (2026-09-27) ----------------------------------
// One cycle here: JellyWave -> JellyDrop -> Canyon -> Off -> JellyWave.
// JellyWave / JellyDrop / Off are the global wave mode (ui_wave.h, which the
// menus cycle too, without Canyon); Canyon is this screen's viz mode, drawn
// over whatever the wave mode is (JellyDrop, the step before it).  Holding Square while Canyon is up still picks its preset.
static const char *const MVIS_NAME[4] = { "JellyWave", "JellyDrop", "Canyon", "Off" };
static int music_vis_cur(void) {
    if (viz_mode() == VIZ_CANYON) return 2;
    const int w = wave_vis_mode();
    return w == WAVE_VIS_JELLYDROP ? 1 : (w == WAVE_VIS_OFF ? 3 : 0);
}
static void music_vis_apply(int idx) {
    switch (idx) {
    case 0: viz_set_mode(VIZ_WAVE);   wave_vis_set(WAVE_VIS_JELLYWAVE); break;
    case 1: viz_set_mode(VIZ_WAVE);   wave_vis_set(WAVE_VIS_JELLYDROP); break;
    // Canyon leaves the wave mode as it was: its fade (s_mini_e) draws the
    // wave underneath, so it must fade in over the JellyDrop it came from --
    // forcing JellyWave here flashed JellyWave for the half second.  And the
    // step on to Off sets Off before the fade out, so it fades over nothing.
    case 2: viz_set_mode(VIZ_CANYON); break;
    default: viz_set_mode(VIZ_WAVE);  wave_vis_set(WAVE_VIS_OFF);       break;
    }
}
static const char *music_vis_next_label(void) { return MVIS_NAME[(music_vis_cur() + 1) % 4]; }

// Into or out of Canyon (2026-09-27): no shrink animation any more -- it
// resized the cover and re-set the text every frame and was laggy.  The whole
// screen dips to the background, the mode changes while it is hidden, and it
// comes back in the new layout.  s_swap_a is that dip: 1 normal .. 0 hidden.
#define SWF_HALF_US 280000.0f
static u64   s_swf_t0   = 0;
static int   s_swf_next = -1;       // the mode to apply at the bottom of the dip
static bool  s_swf_on   = false;
static float s_swap_a   = 1.0f;
static void music_vis_cycle(void) {
    const int cur = music_vis_cur(), nx = (cur + 1) % 4;
    if (nx == 2 || cur == 2) {
        if (!s_swf_on) { s_swf_on = true; s_swf_t0 = timing_get_us(); s_swf_next = nx; }
        return;
    }
    music_vis_apply(nx);
}
static int   s_ox = 0, s_oy = 0, s_os = 0;
static bool  s_origin_pending = false;  // set by the opener for the next open

// The text lines as drawn (x, y, w, h px), recorded by draw_now_playing() so
// the particles can flow BETWEEN the lines instead of round one big box.
// One frame late when read, which no particle can notice.
static int   s_txt_box[4][4];
static int   s_txt_n = 0;

static inline float ease_cubic(float t) {          // ease-out
    t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
    const float u = 1.0f - t;
    return 1.0f - u * u * u;
}
static inline float ease_back(float t) {           // ease-out, ~4% overshoot
    t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
    const float c1 = 1.20f, c3 = c1 + 1.0f, u = t - 1.0f;
    return 1.0f + c3 * u * u * u + c1 * u * u;
}
static inline float stage_t(float ms, float delay, float dur) {
    return (ms - delay) / dur;
}

static inline float fade_q(float a) {
    a = a < 0.0f ? 0.0f : (a > 1.0f ? 1.0f : a);
    return (float)(int)(a * 16.0f + 0.5f) / 16.0f;
}
static inline u32 fa(u32 col, float a) { return art_mix(XMB_BG, col, fade_q(a)); }
static inline u8  fa8(float a) {
    a = a < 0.0f ? 0.0f : (a > 1.0f ? 1.0f : a);
    return (u8)(a * 255.0f + 0.5f);
}
static inline float fade_ease(float t) {           // smoothstep
    t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
    return t * t * (3.0f - 2.0f * t);
}

static void music_fades_reset(void) {
    s_scr_a = s_up_a = s_ctl_a = 1.0f;
    s_focus_p = 0.0f;
    s_outro = false;
    s_fade_us = s_last_input_us = s_intro_t0 = timing_get_us();
    s_scr_a = 0.0f;                       // the screen fades in, as it fades out
}

static bool music_any_press(void) {
    // Not Square: it changes the visualiser and leaves focus mode alone.
    return BTN_PRESSED(cross) || BTN_PRESSED(circle) ||
           BTN_PRESSED(triangle) || BTN_PRESSED(up) || BTN_PRESSED(down) ||
           BTN_PRESSED(left) || BTN_PRESSED(right) || BTN_PRESSED(l1) ||
           BTN_PRESSED(r1) || BTN_PRESSED(l2) || BTN_PRESSED(r2) ||
           BTN_PRESSED(start) || BTN_PRESSED(select);
}

// Once per frame, after input.  `focus_ok`: the screen may settle now
// (playing, transport zone, no overlay).
static void music_fades_tick(bool focus_ok) {
    const u64 now = timing_get_us();
    float dt = (float)(now - s_fade_us);
    if (dt > 100000.0f) dt = 100000.0f;
    s_fade_us = now;
    {
        float a = 1.0f;
        if (s_swf_on) {
            const float t = (float)(now - s_swf_t0) / SWF_HALF_US;
            if (t < 1.0f) a = 1.0f - t;
            else {
                if (s_swf_next >= 0) { music_vis_apply(s_swf_next); s_swf_next = -1; }
                a = t - 1.0f;
                if (t >= 2.0f) { a = 1.0f; s_swf_on = false; }
            }
            a = a * a * (3.0f - 2.0f * a);
        }
        s_swap_a = a;
        // The layout snaps (it changes while the screen is hidden).
        s_mini = s_mini_e = viz_mode() == VIZ_CANYON ? 1.0f : 0.0f;
        (void)MINI_MS;
    }

    const bool focus = focus_ok && !s_outro && now - s_last_input_us > FOCUS_AFTER_US;
    if (focus) s_focus_p += dt / FOCUS_IN_US;
    else       s_focus_p -= dt / FOCUS_OUT_US;
    s_focus_p = s_focus_p < 0.0f ? 0.0f : (s_focus_p > 1.0f ? 1.0f : s_focus_p);
    const float e = fade_ease(s_focus_p);
    s_up_a  = 1.0f - e;
    s_ctl_a = 1.0f - (1.0f - FOCUS_CTL_A) * e;
    // Focus mode takes the background gradient to near black: the only
    // light left is the wave / JellyDrop, its glow and the particles.
    wave_set_bg_dim(0.92f * e);

    // the choreography, see above
    {
        const float in_ms = (float)(now - s_intro_t0) * 0.001f;
        float cover = ease_back (stage_t(in_ms,   0.0f, 560.0f));
        float text  = ease_cubic(stage_t(in_ms, 200.0f, 460.0f));
        float panel = ease_cubic(stage_t(in_ms, 280.0f, 540.0f));
        float ctl   = ease_cubic(stage_t(in_ms, 360.0f, 480.0f));
        if (s_outro) {
            const float out_ms = (float)(now - s_outro_t0) * 0.001f;
            text  *= 1.0f - ease_cubic(stage_t(out_ms,   0.0f, 240.0f));
            panel *= 1.0f - ease_cubic(stage_t(out_ms,   0.0f, 280.0f));
            ctl   *= 1.0f - ease_cubic(stage_t(out_ms,   0.0f, 240.0f));
            cover *= 1.0f - ease_cubic(stage_t(out_ms, 150.0f, 330.0f));
        }
        s_e_cover = cover; s_e_text = text; s_e_panel = panel; s_e_ctl = ctl;
    }

    if (s_outro) {
        const float t = (float)(now - s_outro_t0) / OUTRO_US;
        s_scr_a = t >= 1.0f ? 0.0f : 1.0f - fade_ease(t);
    } else {
        const float t = (float)(now - s_intro_t0) / INTRO_US;
        s_scr_a = t >= 1.0f ? 1.0f : fade_ease(t);
    }
    s_scr_a *= s_swap_a;                // the Canyon dip
}

// The cover's place when settled.  cover_A1_full() is the size the TEXTURE
// and the palette are requested at -- never the animated size, or the
// thumbnail cache would be asked for a new size every frame of the shrink.
static inline int cover_A1_full(void) { return (int)(display_height * 0.42f); }
static inline int cover_A1_mini(void) { return (int)(display_height * 0.17f); }
static inline int cover_A1(void) {
    const float f = (float)cover_A1_full(), m = (float)cover_A1_mini();
    return (int)(f + (m - f) * s_mini_e + 0.5f);
}
static inline int cover_x1(void) { return UIS_W(40); }
static inline int cover_y1(void) {
    // mini: its bottom edge above the transport (centred at 0.815 H)
    const float f = display_height * 0.225f;
    const float m = display_height * 0.735f - (float)cover_A1_mini();
    return (int)(f + (m - f) * s_mini_e + 0.5f);
}

// Where the cover is this frame, and how opaque (the choreography).
static void cover_geom(int *x, int *y, int *A, float *alpha) {
    const int W = (int)display_width, H = (int)display_height;
    const float A1 = (float)cover_A1(), x1 = (float)cover_x1(), y1 = (float)cover_y1();
    const float e = s_e_cover;
    const bool from_tile = s_have_origin && !s_outro;
    float oA, ox, oy;
    if (from_tile) { oA = (float)s_os; ox = (float)s_ox; oy = (float)s_oy; }
    else {                                   // out of (or back into) depth
        oA = A1 * 0.64f;
        ox = (float)W * 0.46f - oA * 0.5f;
        oy = (float)H * 0.36f;
    }
    *A = (int)(oA + (A1 - oA) * e + 0.5f);
    *x = (int)(ox + (x1 - ox) * e + 0.5f);
    *y = (int)(oy + (y1 - oy) * e + 0.5f);
    float a = from_tile ? 1.0f : e * 1.5f;
    *alpha = a < 0.0f ? 0.0f : (a > 1.0f ? 1.0f : a);
}

static void music_cover_gpu(const char *art_id, int ax, int ay, int A, float alpha) {
    s_cover_gpu = false;
    if (!art_id || !art_id[0] || !ui_card_gpu_ready()) return;
    // The TEXTURE is always requested at the settled size: the fly-in only
    // scales the quad, it must not ask the thumbnail cache for a new size
    // every frame.
    const int A1 = cover_A1_full();
    int rw = A1, rh = A1, cap = thumb_max_square();
    if (rw > cap || rh > cap || (size_t)rw * rh > (size_t)cap * cap) {
        rw = rw < cap ? rw : cap;
        rh = rh < cap ? rh : cap;
    }
    thumb_request(art_id, rw, rh);
    u32 off = 0, pitch = 0;
    if (!thumb_gpu_texture(art_id, rw, rh, &off, &pitch)) return;
    ui_card_gpu_draw_ex(off, (u32)rw, (u32)rh, pitch, ax, ay, A, A, 0.0f, 1.0f,
                        fa8(alpha));
    ui_card_gpu_end();
    s_cover_gpu = true;
}

static void music_accent_update(const char *art_id, int A) {
    s_mpal = g_spine_on ? ui_art_palette(art_id, music_art_bitmap(art_id, A))
                        : ui_art_fallback();
    // ... and so does the canyon, a little: each song keeps its own preset
    canyon_set_tint(s_mpal.valid ? s_mpal.accent : 0u, 0.35f);
    // the ribbons take a hint of the album's colour
    wave_set_album_tint(s_mpal.valid ? s_mpal.accent : 0u, 0.70f);   // peak of the cycle
}

// -------------------------------------------------------
// Small local helpers
// -------------------------------------------------------

static void fmt_mmss(char *out, int cap, u32 secs) {
    snprintf(out, cap, "%u:%02u", secs / 60, secs % 60);
}

// Text clipped to max_w with ".." (local copy of the grid's helper).
static void draw_clipped(u32 x, u32 y, const char *text, float px,
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

// Anti-aliased filled circle: opaque row spans inside, per-pixel coverage
// blending along the rim (only the ~2px edge band reads the framebuffer, so
// this stays cheap on real hardware too).
static void fill_circle(int cx, int cy, int r, u32 color) {
    float rf = (float)r;
    for (int dy = -r - 1; dy <= r + 1; dy++) {
        float yy = (float)dy;
        float in2 = (rf - 1.0f) * (rf - 1.0f) - yy * yy;
        int hw_in = in2 > 0.0f ? (int)sqrtf(in2) : -1;
        if (hw_in >= 0)
            drawRect((u32)(cx - hw_in), (u32)(cy + dy),
                     (u32)(hw_in * 2 + 1), 1, color);
        float out2 = (rf + 1.0f) * (rf + 1.0f) - yy * yy;
        if (out2 <= 0.0f) continue;
        int hw_out = (int)sqrtf(out2) + 1;
        for (int dx = hw_in + 1; dx <= hw_out; dx++) {
            float d   = sqrtf((float)dx * (float)dx + yy * yy);
            float cov = rf + 0.5f - d;
            if (cov <= 0.0f) continue;
            if (cov > 1.0f) cov = 1.0f;
            u8 a = (u8)(cov * 255.0f);
            drawRectBlend((u32)(cx + dx), (u32)(cy + dy), 1, 1, color, a);
            if (dx > 0)
                drawRectBlend((u32)(cx - dx), (u32)(cy + dy), 1, 1, color, a);
        }
    }
}

// Anti-aliased circle outline of thickness t, alpha-blended — a continuous
// band (unlike the old 64-sample ring, which rendered as dots).
static void ring_aa(int cx, int cy, float r, float t, u32 color, u8 alpha) {
    float half = t * 0.5f;
    int   R    = (int)(r + half) + 2;
    for (int dy = -R; dy <= R; dy++) {
        float yy   = (float)dy;
        float lo2  = (r - half - 1.0f) * (r - half - 1.0f) - yy * yy;
        float hi2  = (r + half + 1.0f) * (r + half + 1.0f) - yy * yy;
        if (hi2 <= 0.0f) continue;
        int x_hi = (int)sqrtf(hi2) + 1;
        int x_lo = lo2 > 0.0f ? (int)sqrtf(lo2) : 0;
        for (int dx = x_lo; dx <= x_hi; dx++) {
            float d   = sqrtf((float)dx * (float)dx + yy * yy);
            float cov = half + 0.5f - fabsf(d - r);
            if (cov <= 0.0f) continue;
            if (cov > 1.0f) cov = 1.0f;
            u8 a = (u8)((float)alpha * cov);
            if (a == 0) continue;
            drawRectBlend((u32)(cx + dx), (u32)(cy + dy), 1, 1, color, a);
            if (dx > 0)
                drawRectBlend((u32)(cx - dx), (u32)(cy + dy), 1, 1, color, a);
        }
    }
}

// Four-segment breadcrumb (the shared helper caps at three).
static __attribute__((unused)) void draw_breadcrumb4(int x, int y, const char *a, const char *b,
                             const char *c, const char *leaf) {
    const float px = UIS_TF(15.0f);
    const char *parts[4] = { a, b, c, leaf };
    for (int i = 0; i < 4; i++) {
        if (!parts[i]) continue;
        bool is_leaf = (i == 3);
        drawTTF((u32)x, (u32)y, parts[i], px,
                is_leaf ? XMB_TEXT : XMB_TEXT_FAINT);
        x += ttf_text_width(parts[i], px);
        if (!is_leaf) {
            drawIcon((u32)(x + UIS_W(4)), (u32)(y - 1), ICON_CHEVRON_RIGHT, UIS_TF(16.0f), XMB_TEXT_FAINT);
            x += UIS_W(24);
        }
    }
}

// -------------------------------------------------------
// The visualizer — the circled part of the mockup, with more and longer
// bars as requested: MUSIC_VIZ_BANDS columns, up to VIZ_H px tall, violet
// with a lighter cap so the peaks read at TV distance.
// -------------------------------------------------------

static void draw_visualizer(int x, int baseline, int width, int max_h, float a, int clip_x) {
    float bands[MUSIC_VIZ_BANDS];
    music_viz_bands(bands);

    // Geometry from the design's JFBars component (gap 4, 2px floor).  Its bar
    // COUNT and motion are not copied: JFBars drives its bars from a sine
    // stand-in, while this client has real FFT band levels from music_fft.cpp,
    // which is strictly better data.  Only the presentation is taken across.
    int gap = UIS_W(4);
    int bw  = (width - gap * (MUSIC_VIZ_BANDS - 1)) / MUSIC_VIZ_BANDS;
    if (bw < 4) bw = 4;

    // Gel bars (2026-09-27, "more jelly XMB-like"): each band a translucent
    // capsule in the album's accent -- a brighter core down its middle, a
    // glowing rounded cap riding the level, a soft halo over the loud ones
    // and a faint reflection under the baseline -- blended on the RSX like
    // the rest of JellyWave.  The flat CPU rectangles below are the
    // fallback where GPU blending is not up.
    if (wave_gpu_blend_ready()) {
        const u32 acc = s_mpal.accent, glo = s_mpal.glow;
        const u32 alt = XMB_ACCENT_ALT;
        const float A = a < 0.0f ? 0.0f : (a > 1.0f ? 1.0f : a);
        const int   r = bw / 2;
        for (int i = 0; i < MUSIC_VIZ_BANDS; i++) {
            const float lv = bands[i] < 0.0f ? 0.0f : (bands[i] > 1.0f ? 1.0f : bands[i]);
            int h = bw + (int)(lv * (float)(max_h - bw));
            const int bx = x + i * (bw + gap);
            const int by = baseline - h;
            if (bx < clip_x) continue;               // still behind the cover
            if (lv > 0.45f)                           // the loud ones glow
                wave_draw_glow_gpu(bx + bw / 2, by + r, bw * 2, bw * 2,
                                   (u8)(glo >> 16), (u8)(glo >> 8), (u8)glo,
                                   (u8)(A * 110.0f * (lv - 0.45f) / 0.55f));
            // the gel body, see-through
            wave_draw_rrect_gpu(bx, by, bw, h, r, acc, glo, (u8)(A * 120.0f));
            // the core: narrower, brighter
            if (bw >= 6 && h > bw)
                wave_draw_rrect_gpu(bx + bw / 4, by + r, bw - 2 * (bw / 4), h - r, bw / 4,
                                    alt, acc, (u8)(A * (70.0f + 90.0f * lv)));
            // the cap: a lit, rounded head on the level
            wave_draw_rrect_gpu(bx, by, bw, bw, r, alt, 0x00FFFFFF,
                                (u8)(A * (150.0f + 100.0f * lv)));
            // the reflection
            const int rh = h / 3;
            if (rh > 2)
                wave_draw_rrect_gpu(bx, baseline + UIS_H(3), bw, rh, r, acc, glo, (u8)(A * 26.0f));
        }
        return;
    }

    for (int i = 0; i < MUSIC_VIZ_BANDS; i++) {
        int h = UIS_H(2) + (int)(bands[i] * (float)(max_h - 2));
        int bx = x + i * (bw + gap);
        int by = baseline - h;
        if (bx < clip_x) continue;               // still behind the cover
        drawRect((u32)bx, (u32)by, (u32)bw, (u32)h, fa(s_mpal.accent, a));
        // Lighter 2px cap on any bar with real energy.  This was a hardcoded
        // lilac (0x00C4B5F7) that ignored the theme entirely -- harmless under
        // XMB wave, plainly wrong under Golden Age, where it put a purple cap
        // on gold bars.  XMB_TEXT is the theme's light ink and tracks it.
        if (h > 6)
            drawRect((u32)bx, (u32)by, (u32)bw, UIS_H(2), fa(XMB_TEXT, a));
    }
}

// -------------------------------------------------------
// Playback context — breadcrumb + meta lines for the queue's origin
// (album, artist/genre album, playlist, or the flat Songs list).
// -------------------------------------------------------

typedef struct {
    char parent[64];    // breadcrumb: "Albums" / "Songs" / artist / genre...
    char title[128];    // album or playlist name ("" for the Songs list)
    char year[8];       // shown after the title when known
    char genre[32];     // meta-line genre
} MusicCtx;

// -------------------------------------------------------
// Queue overlay (SELECT) — the whole queue in PLAY order (so a shuffle
// re-orders it live), full height, X jumps to the highlighted position.
// -------------------------------------------------------

static bool s_q_open   = false;
static int  s_q_sel    = 0;      // play-order position
static int  s_q_scroll = 0;

// D-pad focus: two zones.  TRANSPORT is the control row (0 rewind, 1 prev,
// 2 play/pause, 3 next, 4 ffwd, 5 shuffle) — left/right move, X activates.
// UP from there enters QUEUE — the Up Next list on the right — where
// up/down scroll the full remaining queue, X plays the highlighted track,
// LEFT/circle drop back to the transport.
enum { FZ_TRANSPORT = 0, FZ_QUEUE = 1 };
static int s_fzone   = FZ_TRANSPORT;
static int s_t_focus = 2;
static int s_u_sel    = 0;   // QUEUE zone: selected play-order position
static int s_u_scroll = 0;   // QUEUE zone: first visible position
static bool s_swallow_left = false;   // eat the held LEFT that exited QUEUE

// Up Next row metrics.
//
// The cover is MQ_ART square and the row pitch is that plus MQ_ROW_GAP, so the
// artworks are always separated by the gap and never by whatever is left over.
// They used to be: the pitch was a RAW 54 while the cover was UIS_H(40), so at
// 1080p the cover grew to 60 and the pitch did not -- the artworks overlapped
// by 6px and the column read as one continuous block. The scrollbar was worse
// again, sized from UIS_H(54) while the rows advanced by the raw one, so the
// two disagreed about how tall the list was.
#define MQ_ART      UIS_H(40)
#define MQ_ROW_GAP  UIS_H(6)
#define MQ_ROW_H    (MQ_ART + MQ_ROW_GAP)

// Rows that fit the Up Next list (MQ_ROW_H per entry, stopping above the
// seek bar's time labels).
static int uq_vis_rows(void) {
    int ey0 = (int)(display_height * 0.18f) + UIS_H(34);
    int bot = (int)(display_height * 0.895f) - UIS_H(16);
    int n   = (bot - ey0) / MQ_ROW_H;
    return n < 1 ? 1 : n;
}

// -------------------------------------------------------
// Seek tap batching — the video player's model: each REW/FF tap moves the
// bar immediately and stretches a ~0.9 s gate; the stream only reopens
// once, after the taps stop.  While the restart is in flight the bar keeps
// showing the target instead of snapping back to the stale position.
// -------------------------------------------------------

static int s_seek_pend       = 0;    // accumulated ±10 s, not yet committed
static u64 s_seek_gate_us    = 0;    // commit when now passes this
static int s_seek_hold       = -1;   // committed target shown until caught
static u64 s_seek_hold_until = 0;
static int s_last_pos        = -1;   // track-change detector

static void seek_tap(int dir) {
    // Pin at the track edges so a long hold can't wind up a huge pending
    // jump past 0:00 or the end.
    int target = (int)music_elapsed_secs() + s_seek_pend + dir * 10;
    u32 dur    = music_duration_secs();
    if (target < -5) return;
    if (dur > 0 && target > (int)dur + 5) return;
    s_seek_pend   += dir * 10;
    s_seek_gate_us = timing_get_us() + 900000ULL;
}

// Hold-to-scrub on L2/R2: fires on the initial press, then ~4x/sec while
// held (the shared BTN_REPEAT machine only covers the d-pad).  The commit
// gate keeps stretching while ticks arrive, so the whole hold still costs
// a single stream reopen after release.
static bool seek_hold_tick(bool held, int idx) {
    static u64 s_next_us[2] = { 0, 0 };
    u64 now = timing_get_us();
    if (!held) { s_next_us[idx] = 0; return false; }
    if (s_next_us[idx] == 0) {                    // fresh press
        s_next_us[idx] = now + 350000ULL;
        return true;
    }
    if (now >= s_next_us[idx]) {                  // held: steady ticks
        s_next_us[idx] = now + 240000ULL;
        return true;
    }
    return false;
}

// Runs every frame: commit quiet taps, drop stale display state.
static void seek_update(void) {
    int pos = music_current_pos();
    if (pos != s_last_pos) {
        s_last_pos  = pos;
        s_seek_pend = 0;
        s_seek_hold = -1;
    }
    u64 now = timing_get_us();
    if (s_seek_pend != 0 && now >= s_seek_gate_us) {
        int tgt = (int)music_elapsed_secs() + s_seek_pend;
        u32 dur = music_duration_secs();
        if (tgt < 0) tgt = 0;
        if (dur > 0 && tgt > (int)dur - 1) tgt = (int)dur - 1;
        music_seek(s_seek_pend);
        s_seek_pend       = 0;
        s_seek_hold       = tgt;
        s_seek_hold_until = now + 5000000ULL;
    }
    if (s_seek_hold >= 0) {
        int d = (int)music_elapsed_secs() - s_seek_hold;
        if (d < 0) d = -d;
        if (d <= 2 || now >= s_seek_hold_until) s_seek_hold = -1;
    }
}

// Position the seek bar / time label should display right now.
static int seek_display_secs(void) {
    int el  = (int)music_elapsed_secs();
    u32 dur = music_duration_secs();
    int disp = el;
    if (s_seek_pend != 0)     disp = el + s_seek_pend;
    else if (s_seek_hold >= 0) disp = s_seek_hold;
    if (disp < 0) disp = 0;
    if (dur > 0 && disp > (int)dur) disp = (int)dur;
    return disp;
}

#define Q_ROW_H UIS_H(42)

// Rows that fit the full-height panel at the current resolution.
static int q_vis_rows(void) {
    int top = XMB_TOPBAR_H + UIS_H(26);
    int bot = (int)display_height - XMB_BOTTOM_PAD - UIS_H(6);
    int n   = (bot - top - UIS_H(56) - UIS_H(14)) / Q_ROW_H;
    return n < 3 ? 3 : n;
}

static void draw_queue_overlay(const MusicTrack *tracks, int count,
                               const char *ctx_title) {
    wave_dim_screen(130);   // GPU dim, fenced — CPU panel lands on top

    const int rows_fit = q_vis_rows();
    const int rows     = count < rows_fit ? count : rows_fit;
    const int mw       = UIS_W(640);
    const int mh       = UIS_H(56) + rows * Q_ROW_H + UIS_H(14);
    int mx = ((int)display_width - mw) / 2;
    int my = XMB_TOPBAR_H + UIS_H(26);

    drawRect((u32)mx, (u32)my, (u32)mw, (u32)mh, XMB_PANEL);
    drawRect((u32)mx, (u32)my, (u32)mw, 1, XMB_HAIRLINE);
    drawRect((u32)mx, (u32)(my + mh - 1), (u32)mw, 1, XMB_HAIRLINE);
    drawRect((u32)mx, (u32)my, 1, (u32)mh, XMB_HAIRLINE);
    drawRect((u32)(mx + mw - 1), (u32)my, 1, (u32)mh, XMB_HAIRLINE);

    drawTTF((u32)(mx + UIS_W(26)), (u32)(my + UIS_H(18)), "QUEUE", UIS_TF(14), XMB_TEXT_FAINT, true);
    if (music_is_shuffle())
        drawIcon((u32)(mx + UIS_W(92)), (u32)(my + UIS_H(16)), ICON_SHUFFLE, UIS_TF(18.0f), XMB_ACCENT);
    if (ctx_title[0])
        draw_clipped((u32)(mx + UIS_W(124)), (u32)(my + UIS_H(18)), ctx_title, UIS_TF(14),
                     XMB_TEXT_DIM, mw - UIS_W(240));
    {
        char pos_str[24];
        snprintf(pos_str, sizeof(pos_str), "%d / %d", s_q_sel + 1, count);
        int pw = ttf_text_width(pos_str, UIS_TF(13));
        drawTTF((u32)(mx + mw - UIS_W(26) - pw), (u32)(my + UIS_H(19)), pos_str, UIS_TF(13),
                XMB_TEXT_DIM);
    }

    int cur_pos = music_current_pos();
    int list_y  = my + UIS_H(48);
    for (int i = 0; i < rows; i++) {
        int p = s_q_scroll + i;
        if (p >= count) break;
        int orig = music_track_at(p);
        if (orig < 0) break;
        const MusicTrack *t = &tracks[orig];
        int  ry  = list_y + i * Q_ROW_H;
        bool sel = (p == s_q_sel);
        if (sel)
            drawRect((u32)(mx + UIS_W(12)), (u32)ry, (u32)(mw - UIS_W(24)), Q_ROW_H,
                     XMB_PANEL_HI);
        // Playing marker / play-order number (track # when unshuffled).
        if (p == cur_pos)
            drawIcon((u32)(mx + UIS_W(24)), (u32)(ry + UIS_H(10)), ICON_MUSIC, UIS_TF(20.0f),
                     XMB_ACCENT);
        else {
            int shown = (!music_is_shuffle() && t->track_num > 0)
                            ? t->track_num : p + 1;
            char num[8];
            snprintf(num, sizeof(num), "%d", shown);
            drawTTF((u32)(mx + UIS_W(26)), (u32)(ry + UIS_H(12)), num, UIS_TF(15), XMB_TEXT_FAINT);
        }
        // Album-art thumb (letter tile while it loads).
        if (!xmb_cpu_blit_thumb(t->art_id, mx + UIS_W(56), ry + UIS_H(3), UIS_W(36), UIS_H(36)))
            xmb_draw_letter_tile(t->id, t->name, mx + UIS_W(56), ry + UIS_H(3), UIS_H(36));
        draw_clipped((u32)(mx + UIS_W(104)), (u32)(ry + UIS_H(11)), t->name, UIS_TF(16),
                     sel ? XMB_WHITE : XMB_TEXT, mw - UIS_W(214), sel);
        if (t->duration_secs > 0) {
            char d[16];
            fmt_mmss(d, sizeof(d), t->duration_secs);
            int dw = ttf_text_width(d, UIS_TF(14));
            drawTTF((u32)(mx + mw - UIS_W(30) - dw), (u32)(ry + UIS_H(12)), d, UIS_TF(14),
                    XMB_TEXT_DIM);
        }
    }

    // Scrollbar along the right edge when the queue outruns the panel.
    if (count > rows) {
        int bar_x   = mx + mw - UIS_W(12);
        int track_h = rows * Q_ROW_H - UIS_H(8);
        drawRect((u32)bar_x, (u32)list_y, UIS_W(3), (u32)track_h, XMB_TRACK);
        int th = track_h * rows / count;
        if (th < 18) th = 18;
        int rng = count - rows;
        int off = rng > 0 ? (track_h - th) * s_q_scroll / rng : 0;
        drawRect((u32)bar_x, (u32)(list_y + off), UIS_W(3), (u32)th, XMB_ACCENT);
    }

    static const Hint h[] = {{'X', "Play"}, {'T', "Shuffle"}, {'C', "Close"}};
    draw_hints_bar(h, 3);
}

// -------------------------------------------------------
// Main frame draw
// -------------------------------------------------------

// Up Next's thumbnails on the RSX (GPU phase), so they can fade with real
// alpha in focus mode.  Same layout as draw_now_playing's list; a row whose
// texture is not up yet falls back to the CPU blit / letter tile there.
#define UQ_ROWS_MAX 32
static bool s_up_gpu[UQ_ROWS_MAX];
static void music_upnext_gpu(const MusicTrack *tracks, int count, float a) {
    for (int i = 0; i < UQ_ROWS_MAX; i++) s_up_gpu[i] = false;
    if (a <= 0.01f || !ui_card_gpu_ready()) return;
    const int W = (int)display_width, H = (int)display_height;
    const int up_x  = W - (int)(W * 0.27f);
    const int ey0   = (int)(H * 0.18f) + UIS_H(34);
    const int n_vis = uq_vis_rows();
    const int first = music_current_pos() + 1;
    if (count - first <= 0) return;
    int scroll = (s_fzone == FZ_QUEUE) ? s_u_scroll : first;
    if (scroll > count - n_vis) scroll = count - n_vis;
    if (scroll < first)         scroll = first;
    int ey = ey0;
    bool any = false;
    for (int p = scroll, r = 0; p < count && p < scroll + n_vis && r < UQ_ROWS_MAX; p++, r++) {
        const int orig = music_track_at(p);
        if (orig < 0) break;
        const MusicTrack *u = &tracks[orig];
        thumb_request(u->art_id, MQ_ART, MQ_ART);
        u32 off = 0, pitch = 0;
        if (thumb_gpu_texture(u->art_id, MQ_ART, MQ_ART, &off, &pitch)) {
            ui_card_gpu_draw_ex(off, (u32)MQ_ART, (u32)MQ_ART, pitch, up_x, ey,
                                MQ_ART, MQ_ART, 0.0f, 1.0f, fa8(a));
            s_up_gpu[r] = true;
            any = true;
        }
        ey += MQ_ROW_H;
    }
    if (any) ui_card_gpu_end();
}

static void draw_now_playing(const MusicCtx *ctx, const MusicTrack *tracks,
                             int count) {
    int W = (int)display_width;
    int H = (int)display_height;

    // No breadcrumb (Music > Albums > ... > Now Playing): everything it said
    // is on the screen already.  The lockup stays -- it is the XMB's too.
    g_topbar_logo_a = 1.0f - fade_ease(s_focus_p);   // the Jellyfin lockup fades in focus mode
    xmb_draw_topbar();
    g_topbar_logo_a = 1.0f;
    const float ta = s_e_text;                      // the text column
    float ua;                                       // Up Next (the panel's own)
    const float ca = s_ctl_a * s_e_ctl;             // transport
    const float ka = s_ctl_a * s_e_ctl;             // seek bar + times: fade with the controls
    const int   rise = (int)((1.0f - s_e_ctl) * (float)UIS_H(46));   // controls rise into place

    int cur = music_current_index();
    if (cur >= count) cur = count - 1;
    const MusicTrack *t = &tracks[cur];

    // ---- art (follows the current track's album) + accent frame/glow ----
    int A, ax, ay; float cal;
    cover_geom(&ax, &ay, &A, &cal);
    const int A1 = cover_A1();
    // CPU fallback only once settled: a CPU blit at a changing size would ask
    // the thumbnail cache for a new size every frame of the fly-in.
    if (!s_cover_gpu && cal > 0.5f && A == A1 && (s_mini <= 0.0f || s_mini >= 1.0f)
        && !xmb_cpu_blit_thumb(t->art_id, ax, ay, A, A))
        xmb_draw_letter_tile(t->art_id,
                             ctx->title[0] ? ctx->title : t->name,
                             ax, ay, A);
    {
        // The artwork sits IN the layout, not in a frame.
        //
        // This was a 2px solid accent border on all four sides plus a second
        // 1px pass at alpha 80, with a 2px gap -- a card, and at 1080p a loud
        // one. What is left is a single hairline at alpha 32 (12.5%), drawn
        // tight against the art: enough to stop a dark album cover dissolving
        // into a dark background, not enough to read as an edge. Deliberately
        // NOT replaced with a shadow or a glow.
        const u8 EDGE_A = (u8)(32.0f * cal);
        drawRectBlend((u32)(ax - 1),     (u32)(ay - 1),     (u32)(A + 2), 1, XMB_ACCENT, EDGE_A);
        drawRectBlend((u32)(ax - 1),     (u32)(ay + A),     (u32)(A + 2), 1, XMB_ACCENT, EDGE_A);
        drawRectBlend((u32)(ax - 1),     (u32)(ay - 1),     1, (u32)(A + 2), XMB_ACCENT, EDGE_A);
        drawRectBlend((u32)(ax + A),     (u32)(ay - 1),     1, (u32)(A + 2), XMB_ACCENT, EDGE_A);
    }

    // ---- text column: visualizer, title, artist, album, meta ----
    // Settled layout, then the slide: the column starts tucked behind the
    // cover and is clipped at its right edge until it is clear of it.
    const int tx1       = cover_x1() + A1 + UIS_W(56);
    ua = s_e_panel * s_up_a * (1.0f - s_mini_e);                                   // fades with the screen and focus mode
    const int up_x      = W - (int)(W * 0.27f);
    const int col_w     = up_x - UIS_W(30) - tx1;
    const int tuck      = (ax + A / 2) - tx1;                  // < 0: how far behind the cover it starts
    const int tx        = tx1 + (int)((1.0f - ta) * (float)tuck);
    // The column scales with the cover (k = 1 normal .. ~0.6 mini) and sits
    // at the same fraction of its height: 0.15 H of a 0.42 H cover.
    // Quantised to 0.05 steps: every distinct px is a new set of glyphs in
    // the text atlas, and a smooth k would mint a new size every frame.
    const float k       = (float)(int)((1.0f - 0.40f * s_mini_e) * 20.0f + 0.5f) / 20.0f;
    const int title_top = cover_y1() + (int)((float)cover_A1() * 0.357f);
    const int clip_x    = ta < 0.999f ? ax + A : 0;
    g_text_clip_left = clip_x;

    if (s_mini_e < 0.999f)   // the bars step aside for the canyon
        draw_visualizer(tx, title_top - 18, (int)(W * 0.265f), (int)(H * 0.11f),
                        ta * (1.0f - s_mini_e), clip_x);

    draw_clipped((u32)tx, (u32)title_top, t->name, UIS_TF(32) * k, fa(XMB_WHITE, ta), col_w, true);
    s_txt_n = 0;
#define TXT_BOX(y_, str_, px_, bold_, h_) do {                                     \
        int w_ = ttf_text_width((str_), (px_), (bold_));                         \
        if (w_ > col_w) w_ = col_w;                                              \
        if (s_txt_n < 4 && w_ > 0) {                                             \
            s_txt_box[s_txt_n][0] = tx1; s_txt_box[s_txt_n][1] = (y_);           \
            s_txt_box[s_txt_n][2] = w_;  s_txt_box[s_txt_n][3] = (h_);           \
            s_txt_n++;                                                           \
        }                                                                        \
    } while (0)
    TXT_BOX(title_top, t->name, UIS_TF(32) * k, true, (int)(UIS_H(36) * k));
    if (t->artist[0])
    {
        draw_clipped((u32)tx, (u32)(title_top + (int)(UIS_H(48) * k)), t->artist, UIS_TF(20) * k,
                     fa(s_mpal.accent, ta), col_w);
        TXT_BOX(title_top + (int)(UIS_H(48) * k), t->artist, UIS_TF(20) * k, false, (int)(UIS_H(22) * k));
    }
    {
        char line[160] = "";
        if (ctx->title[0]) snprintf(line, sizeof(line), "%s", ctx->title);
        if (ctx->year[0]) {
            int l = (int)strlen(line);
            snprintf(line + l, sizeof(line) - l, "%s%s",
                     line[0] ? " \xC2\xB7 " : "", ctx->year);
        }
        if (line[0])
        {
            draw_clipped((u32)tx, (u32)(title_top + (int)(UIS_H(80) * k)), line, UIS_TF(15) * k,
                         fa(XMB_TEXT_DIM, ta), col_w);
            TXT_BOX(title_top + (int)(UIS_H(80) * k), line, UIS_TF(15) * k, false, (int)(UIS_H(17) * k));
        }
    }
    {
        // "Track 7 of 13 · Electronic · 320 kbps FLAC" (play-order position)
        char meta[160];
        int  n = snprintf(meta, sizeof(meta), "Track %d of %d",
                          music_current_pos() + 1, count);
        if (ctx->genre[0])
            n += snprintf(meta + n, sizeof(meta) - n, " \xC2\xB7 %s", ctx->genre);
        const char *src = music_source_info();
        if (src[0])
            n += snprintf(meta + n, sizeof(meta) - n, " \xC2\xB7 %s", src);
        draw_clipped((u32)tx, (u32)(title_top + (int)(UIS_H(128) * k)), meta, UIS_TF(14) * k,
                     fa(XMB_TEXT_FAINT, ta), col_w);
        TXT_BOX(title_top + (int)(UIS_H(128) * k), meta, UIS_TF(14) * k, false, (int)(UIS_H(16) * k));
#undef TXT_BOX
    }
    g_text_clip_left = 0;

    // ---- UP NEXT — the full remaining queue: album-art thumbs (letter
    //      tile while loading), d-pad navigable, scrollbar when it
    //      outruns the window ----
    if (ua > 0.01f) {
        int uy    = (int)(H * 0.18f);
        int ey0   = uy + UIS_H(34);
        int n_vis = uq_vis_rows();
        int pos   = music_current_pos();
        int first = pos + 1;              // first upcoming position
        int n_up  = count - first;

        drawTTF((u32)up_x, (u32)uy, "UP NEXT", UIS_TF(13),
                fa(s_fzone == FZ_QUEUE ? XMB_TEXT : XMB_TEXT_FAINT, ua), true);

        if (n_up > 0) {
            // Window origin: follow the d-pad in QUEUE zone, playback
            // otherwise.
            int scroll = (s_fzone == FZ_QUEUE) ? s_u_scroll : first;
            if (scroll > count - n_vis) scroll = count - n_vis;
            if (scroll < first)         scroll = first;

            int text_w = W - UIS_W(46) - (up_x + UIS_W(56));
            int ey     = ey0;
            int row    = 0;
            for (int p = scroll; p < count && p < scroll + n_vis; p++, row++) {
                int orig = music_track_at(p);
                if (orig < 0) break;
                const MusicTrack *u = &tracks[orig];
                bool selq = (s_fzone == FZ_QUEUE && p == s_u_sel);
                const bool on_gpu = row < UQ_ROWS_MAX && s_up_gpu[row];
                if (selq) {
                    // The GPU drew this row's art before the CPU phase, so the
                    // highlight goes AROUND it -- a full-row fill painted over
                    // the cover and it vanished whenever the row was selected.
                    const u32 hc = fa(XMB_PANEL_HI, ua);
                    const int x0 = up_x - UIS_W(8), y0 = ey - MQ_ROW_GAP / 2;
                    const int x1 = W - UIS_W(34), y1 = y0 + MQ_ROW_H;
                    if (on_gpu) {
                        drawRect((u32)x0, (u32)y0, (u32)(up_x - x0), (u32)(y1 - y0), hc);
                        drawRect((u32)(up_x + MQ_ART), (u32)y0, (u32)(x1 - up_x - MQ_ART), (u32)(y1 - y0), hc);
                        if (ey > y0) drawRect((u32)up_x, (u32)y0, MQ_ART, (u32)(ey - y0), hc);
                        if (y1 > ey + MQ_ART)
                            drawRect((u32)up_x, (u32)(ey + MQ_ART), MQ_ART, (u32)(y1 - ey - MQ_ART), hc);
                    } else {
                        drawRect((u32)x0, (u32)y0, (u32)(x1 - x0), (u32)(y1 - y0), hc);
                    }
                }
                if (!on_gpu && ua > 0.5f &&
                    !xmb_cpu_blit_thumb(u->art_id, up_x, ey, MQ_ART, MQ_ART))
                    xmb_draw_letter_tile(u->id, u->name, up_x, ey, MQ_ART);
                draw_clipped((u32)(up_x + UIS_W(56)), (u32)(ey + 1), u->name, UIS_TF(15),
                             fa(selq ? XMB_WHITE : XMB_TEXT, ua), text_w, selq);
                if (u->artist[0])
                    draw_clipped((u32)(up_x + UIS_W(56)), (u32)(ey + UIS_H(22)), u->artist,
                                 UIS_TF(12), fa(XMB_TEXT_FAINT, ua), text_w);
                ey += MQ_ROW_H;
            }

            if (n_up > n_vis) {
                int bar_x   = W - UIS_W(26);
                int track_h = n_vis * MQ_ROW_H - MQ_ROW_GAP;
                drawRect((u32)bar_x, (u32)ey0, UIS_W(3), (u32)track_h, fa(XMB_TRACK, ua));
                int th = track_h * n_vis / n_up;
                if (th < 18) th = 18;
                int rng = n_up - n_vis;
                int off = rng > 0 ? (track_h - th) * (scroll - first) / rng
                                  : 0;
                drawRect((u32)bar_x, (u32)(ey0 + off), UIS_W(3), (u32)th,
                         fa(XMB_ACCENT, ua));
            }
        }
    }

    // ---- transport row (d-pad moves focus, X activates — same model as
    //      the video player's HUD).  Focused control pops white with an
    //      accent tick under it; the play disc gets a bright ring.
    //      Shapes fade by mixing toward the background colour, so at zero
    //      they would still be drawn -- as background-coloured shapes over
    //      the wave.  Faded out means not drawn at all. ----
    if (fade_q(ca) > 0.0f) {
        int cx = W / 2;
        int cy = (int)(H * 0.815f) + rise;
        bool paused = music_is_paused();

        static const int T_OFF[5] = { -130, -72, 0, 72, 130 };
        static const int T_CP[5]  = { ICON_SEEK_BACK, ICON_SKIP_BACK, 0, ICON_SKIP_FWD, ICON_SEEK_FWD };
        static const float T_PX[5] = { 22.0f, 26.0f, 0.0f, 26.0f, 22.0f };

        for (int i = 0; i < 5; i++) {
            bool fc = (i == s_t_focus);
            int  ix = cx + UIS_W(T_OFF[i]);
            if (i == 2) {
                // Play/pause disc — clean two-tone AA fill; focus is a
                // single crisp ring instead of the old dotted glow.
                if (fc) ring_aa(cx, cy, 30.5f, 2.0f, XMB_TEXT, fa8(ca * 235.0f / 255.0f));
                fill_circle(cx, cy, 26, fa(XMB_ACCENT_DEEP, ca));
                fill_circle(cx, cy, 24, fa(XMB_ACCENT, ca));
                drawIcon((u32)(cx - UIS_W(14)), (u32)(cy - UIS_H(14)),
                         paused ? ICON_PLAY : ICON_PAUSE, UIS_TF(28.0f), fa(XMB_WHITE, ca));
            } else {
                int half = (int)(UIS_TF(T_PX[i]) * 0.5f);
                u32 col  = fc ? XMB_WHITE
                              : (i == 1 || i == 3) ? XMB_TEXT_DIM
                                                   : XMB_TEXT_FAINT;
                drawIcon((u32)(ix - half), (u32)(cy - half), T_CP[i],
                         UIS_TF(T_PX[i]), fa(col, ca));
            }
            if (fc)
                drawRect((u32)(ix - UIS_W(8)), (u32)(cy + UIS_H(38)), UIS_W(16), UIS_H(3),
                         fa(XMB_KEY_SEL, ca));
        }

        // Shuffle — the 6th focusable control (triangle also toggles it).
        {
            bool fc = (s_t_focus == 5);
            bool on = music_is_shuffle();
            u32 col = on ? XMB_ACCENT
                         : fc ? XMB_TEXT_DIM : XMB_HAIRLINE;
            drawIcon((u32)(cx + UIS_W(182) - UIS_W(10)), (u32)(cy - UIS_H(10)), ICON_SHUFFLE, UIS_TF(20.0f),
                     fa(col, ca));
            if (fc)
                drawRect((u32)(cx + UIS_W(182) - UIS_W(8)), (u32)(cy + UIS_H(38)), UIS_W(16), UIS_H(3),
                         fa(XMB_KEY_SEL, ca));
        }
    }

    // ---- seek bar (shows the pending/committed seek target while a
    //      batched seek is in flight, so taps feel instant) ----
    if (fade_q(ka) > 0.0f) {
        bool seeking  = (s_seek_pend != 0 || s_seek_hold >= 0);
        u32  shown    = (u32)seek_display_secs();
        u32  duration = music_duration_secs();
        if (duration > 0 && shown > duration) shown = duration;

        // v1.0 moves this block from a centred fixed width to the full content
        // width: the container goes `left:40px; right:40px` and the row holding
        // it becomes `width:100%`, so the elapsed/remaining labels sit INSIDE
        // the margins with the bar flexing to fill what is left between them
        // (`gap:16px`).  It used to be a bar from 30% to 70% with the labels
        // hanging outside it, which is why the times sat so far apart.
        //
        // The transport icons above are unaffected: their row keeps
        // `justify-content:center` inside the now-full-width container, and
        // 40px of margin on both sides leaves the centre where it was.
        char ts[16], td[16];
        fmt_mmss(ts, sizeof(ts), shown);
        td[0] = 0;
        if (duration > 0) fmt_mmss(td, sizeof(td), duration);

        const int gap16 = UIS_W(16);
        int tw = ttf_text_width(ts, UIS_TF(14));
        int dw = td[0] ? ttf_text_width(td, UIS_TF(14)) : 0;

        int rx0 = XMB_ITEM_PAD;
        int rx1 = (int)W - XMB_ITEM_PAD;
        int bx0 = rx0 + tw + gap16;
        int bx1 = rx1 - (td[0] ? dw + gap16 : 0);
        int by  = (int)(H * 0.895f) + rise;
        int bw  = bx1 - bx0;
        if (bw < UIS_W(40)) bw = UIS_W(40);          // degenerate width guard

        drawRect((u32)bx0, (u32)by, (u32)bw, UIS_H(4), fa(XMB_TRACK, ka));
        int fill = (duration > 0) ? (int)((u64)bw * shown / duration) : 0;
        if (fill > bw) fill = bw;
        if (fill > 0)
            drawRect((u32)bx0, (u32)by, (u32)fill, UIS_H(4), fa(s_mpal.accent, ka));
        fill_circle(bx0 + fill, by + UIS_H(2), UIS_H(6), fa(XMB_WHITE, ka));

        drawTTF((u32)rx0, (u32)(by - UIS_H(6)), ts, UIS_TF(14),
                fa(seeking ? XMB_TEXT : XMB_TEXT_DIM, ka));
        if (td[0])
            drawTTF((u32)(rx1 - dw), (u32)(by - UIS_H(6)), td, UIS_TF(14),
                    fa(XMB_TEXT_DIM, ka));
    }

    // The hints go with the controls (they would pop, not fade, so they leave
    // as soon as the screen starts to settle and return with the first press).
    if (!s_q_open && s_ctl_a > 0.9f && s_scr_a > 0.9f && s_e_ctl > 0.95f) {
        if (s_fzone == FZ_QUEUE) {
            // Select names the visualiser it switches TO (JellyWave ->
            // JellyDrop -> Off -> JellyWave).
            // Square names the visualiser it switches TO, in the corner.
            const Hint h[6] = {{'l', ""}, {'r', "Skip"},   // L1/R1: prev/next track
                               {'X', "Play"},
                               {'T', "Shuffle"},
                               {'C', "Back"},
                               {'S', music_vis_next_label()}};
            draw_hints_bar(h, 6);
        } else {
            // Select names the visualiser it switches TO (JellyWave ->
            // JellyDrop -> Off -> JellyWave).
            // Square names the visualiser it switches TO, in the corner.
            const Hint h[6] = {{'l', ""}, {'r', "Skip"},   // L1/R1: prev/next track
                               {'X', "Select"},
                               {'T', "Shuffle"},
                               {'C', "Back"},
                               {'S', music_vis_next_label()}};
            draw_hints_bar(h, 6);
        }
    }
}

// -------------------------------------------------------
// Input — returns true when the screen should close.
// -------------------------------------------------------

static bool music_screen_input(const MusicTrack *tracks, int count) {
    (void)tracks;

    seek_update();   // commit batched seek taps once they go quiet

    // Visualizer, on Square -- the one button this screen leaves free (L2 / R2
    // seek, L1 / R1 skip, Triangle / Select / X / O / Start and the d-pad are
    // all taken).  TAP: Wave <-> Canyon (persisted).  HOLD (0.45 s) while
    // Canyon is up: its next preset.  The tap acts on RELEASE, so a hold
    // never also switches the visualizer.
    if (!s_q_open) {
        static u64  s_sq_down = 0;
        static bool s_sq_held = false;
        const u64 now = timing_get_us();
        if (BTN_PRESSED(square)) { s_sq_down = now; s_sq_held = false; }
        if (btn_cur.square && s_sq_down && !s_sq_held && now - s_sq_down >= 450000ULL) {
            s_sq_held = true;
            if (viz_mode() == VIZ_CANYON) {
                canyon_next_preset();
                s_viz_toast_us = now + 2500000ULL;
            }
        }
        if (!btn_cur.square && s_sq_down) {
            if (!s_sq_held) {
                music_vis_cycle();
                ui_sfx_play(SFX_OPTION);
                s_viz_toast_us = now + 2500000ULL;
            }
            s_sq_down = 0;
        }
    }

    if (s_q_open) {
        int vis = q_vis_rows();
        if (BTN_REPEAT(up)   && s_q_sel > 0)         s_q_sel--;
        if (BTN_REPEAT(down) && s_q_sel < count - 1) s_q_sel++;
        if (s_q_sel < s_q_scroll)        s_q_scroll = s_q_sel;
        if (s_q_sel >= s_q_scroll + vis) s_q_scroll = s_q_sel - vis + 1;
        if (BTN_PRESSED(cross)) {
            music_jump(s_q_sel);         // s_q_sel is a play-order position
            s_q_open = false;
        }
        if (BTN_PRESSED(triangle)) {     // reshuffles the list live
            music_set_shuffle(!music_is_shuffle());
            // The order just changed under the cursor — follow the
            // playing track so the highlight stays meaningful.
            s_q_sel    = music_current_pos();
            s_q_scroll = s_q_sel - vis / 2;
            if (s_q_scroll > count - vis) s_q_scroll = count - vis;
            if (s_q_scroll < 0)           s_q_scroll = 0;
        }
        if (BTN_PRESSED(circle) || BTN_PRESSED(select))
            s_q_open = false;
        return false;
    }

    // ---- QUEUE zone: d-pad drives the Up Next list ----
    if (s_fzone == FZ_QUEUE) {
        int pos   = music_current_pos();
        int first = pos + 1;
        if (first >= count) { s_fzone = FZ_TRANSPORT; return false; }
        // Track advance / shuffle can move the queue under the cursor.
        if (s_u_sel < first)  s_u_sel = first;
        if (s_u_sel >= count) s_u_sel = count - 1;

        if (BTN_PRESSED(start)) return true;
        if (BTN_PRESSED(circle) || BTN_PRESSED(left)) {
            s_fzone   = FZ_TRANSPORT;
            s_t_focus = 5;            // land back on shuffle (entry point)
            // The exit press is usually still held next frame; without this
            // the transport's left-repeat sees it as a fresh press and
            // knocks focus straight from shuffle onto fast-forward.
            s_swallow_left = true;
            return false;
        }
        if (BTN_REPEAT(up)   && s_u_sel > first)     s_u_sel--;
        if (BTN_REPEAT(down) && s_u_sel < count - 1) s_u_sel++;
        int vis = uq_vis_rows();
        if (s_u_sel < s_u_scroll)        s_u_scroll = s_u_sel;
        if (s_u_sel >= s_u_scroll + vis) s_u_scroll = s_u_sel - vis + 1;
        if (s_u_scroll < first)          s_u_scroll = first;

        if (BTN_PRESSED(cross)) {
            music_jump(s_u_sel);
            s_fzone   = FZ_TRANSPORT;  // list re-centers on the new track
            s_t_focus = 2;
        }
        if (BTN_PRESSED(triangle)) {
            music_set_shuffle(!music_is_shuffle());
            s_u_sel = s_u_scroll = music_current_pos() + 1;
        }
        if (seek_hold_tick(btn_cur.l2 != 0, 0)) seek_tap(-1);
        if (seek_hold_tick(btn_cur.r2 != 0, 1)) seek_tap(+1);
        return false;
    }

    // ---- TRANSPORT zone ----
    if (BTN_PRESSED(circle) || BTN_PRESSED(start)) return true;

    // Media keys (Blu-ray remote, keyboard).
    if (BTN_PRESSED(playpause) || BTN_PRESSED(play) || BTN_PRESSED(pause))
        music_toggle_pause();
    if (BTN_PRESSED(stop)) return true;
    if (BTN_PRESSED(next)) music_next();
    if (BTN_PRESSED(prev)) music_prev();

    // D-pad = control focus, X = activate (video-player HUD model);
    // RIGHT past shuffle moves into the Up Next queue.
    if (s_swallow_left) {
        if (!btn_cur.left) s_swallow_left = false;   // wait for release
    } else if (BTN_REPEAT(left) && s_t_focus > 0) s_t_focus--;
    if (BTN_REPEAT(right)) {
        if (s_t_focus < 5) {
            s_t_focus++;
        } else if (music_current_pos() + 1 < count) {
            s_fzone    = FZ_QUEUE;
            s_u_sel    = music_current_pos() + 1;
            s_u_scroll = s_u_sel;
            return false;
        }
    }
    if (BTN_PRESSED(cross)) {
        switch (s_t_focus) {
        case 0: seek_tap(-1);         break;
        case 1: music_prev();         break;
        case 2: music_toggle_pause(); break;
        case 3: music_next();         break;
        case 4: seek_tap(+1);         break;
        case 5: music_set_shuffle(!music_is_shuffle()); break;
        }
    }

    if (BTN_PRESSED(triangle))
        music_set_shuffle(!music_is_shuffle());

    if (false) {   // (Select used to open the full queue overlay; the "Up next" zone shows it)
        int vis    = q_vis_rows();
        s_q_open   = true;
        s_q_sel    = music_current_pos();
        s_q_scroll = s_q_sel - vis / 2;
        if (s_q_scroll > count - vis) s_q_scroll = count - vis;
        if (s_q_scroll < 0)           s_q_scroll = 0;
    }

    // Shoulder shortcuts, same as the video player: L1/R1 track skip,
    // L2/R2 seek — tap for ±10 s, hold to keep scrubbing.
    if (BTN_PRESSED(l1)) music_prev();
    if (BTN_PRESSED(r1)) music_next();
    if (seek_hold_tick(btn_cur.l2 || btn_cur.rew,  0)) seek_tap(-1);
    if (seek_hold_tick(btn_cur.r2 || btn_cur.ffwd, 1)) seek_tap(+1);
    return false;
}

// -------------------------------------------------------
// Entry points
// -------------------------------------------------------

static MusicTrack s_tracks[MUSIC_QUEUE_MAX];

static void music_screen_run(const MusicCtx *ctx, int count, int start_idx) {
    {
        char buf[112];
        snprintf(buf, sizeof(buf), "music_screen: %.24s/%.32s tracks=%d at=%d",
                 ctx->parent, ctx->title, count, start_idx);
        plog(buf);
    }
    if (count <= 0) return;
    if (!music_start(s_tracks, count, start_idx)) return;

    // Entered mid-frame from the XMB input handler: that frame has RSX work
    // queued (clear + wave) but its flip hasn't been issued yet.  Finish it
    // first or the loop's waitflip() below spins forever on a flip that
    // never comes — the same entry dance as the info overlay and the video
    // player (ui_info.cpp / player.cpp).
    if (!s_entry_flipped) {
        rsxSync();
        flip();
    }
    s_entry_flipped = false;

    s_q_open    = false;
    s_fzone     = FZ_TRANSPORT;
    s_t_focus   = 2;
    s_seek_pend = 0;
    s_seek_hold = -1;
    s_last_pos  = -1;
    init_btns();
    music_fades_reset();

    // Render-thread proof of life.  When the app died on hardware there was no
    // way to tell whether this loop was still turning, because the only thing
    // it logged was a wave trace that had already hit its cap.  One line every
    // five seconds, carrying the frame count and the paused flag, dates the
    // last frame the music screen ever drew.
    u64 hb_us    = 0;
    u32 hb_frame = 0;
    // Per-frame cost since the last heartbeat (us): the GPU phase (wave +
    // cover), the fence, and the draw (CPU shapes + queued text).
    u64 c_gpu = 0, c_sync = 0, c_draw = 0; u32 c_n = 0;

    while (running) {
        waitflip();
        sysUtilCheckCallback();
        const u64 t_gpu0 = timing_get_us();
        clearScreen(XMB_BG);
        int cvx, cvy, cvA; float cval;
        cover_geom(&cvx, &cvy, &cvA, &cval);
        {
            // The particles treat the screen's solid things as solid
            // (wave_snow.h): the cover, EACH text line (tight to its glyphs,
            // so particles drift through the gaps between them), and each
            // transport control.
            const int W = (int)display_width, H = (int)display_height;
            int ob[4 * 12], n = 0;
            #define OB(x_, y_, w_, h_) do { if (n < 12) { ob[4*n] = (x_); ob[4*n+1] = (y_); \
                                             ob[4*n+2] = (w_); ob[4*n+3] = (h_); n++; } } while (0)
            if (cval > 0.5f) OB(cvx, cvy, cvA, cvA);
            if (s_e_text > 0.5f)
                for (int i = 0; i < s_txt_n; i++)
                    OB(s_txt_box[i][0], s_txt_box[i][1], s_txt_box[i][2], s_txt_box[i][3]);
            if (s_ctl_a * s_e_ctl > 0.5f) {
                static const int T_OFF[6] = { -130, -72, 0, 72, 130, 182 };
                static const int T_HALF[6] = { 12, 14, 30, 14, 12, 11 };
                const int cx = W / 2, cy = (int)(H * 0.815f);
                for (int i = 0; i < 6; i++) {
                    const int hx = i == 2 ? T_HALF[i] : UIS_W(T_HALF[i]);
                    const int hy = i == 2 ? T_HALF[i] : UIS_H(T_HALF[i]);
                    OB(cx + UIS_W(T_OFF[i]) - hx, cy - hy, 2 * hx, 2 * hy);
                }
            }
            #undef OB
            wave_snow_obstacles(ob, n);
        }
        // The visualizer: Canyon in place of the wave when selected (and
        // able to start), dimmed while the UI is up so the text stays
        // readable, full strength in focus mode.
        // It fades: in over the wave on the way in, out over it on the way
        // back (s_mini_e is the fade), and the wave is only skipped once the
        // canyon fully covers it.
        bool canyon = false;
        if (viz_mode() == VIZ_CANYON || s_mini_e > 0.001f) {
            if (s_mini_e < 0.999f) wave_draw();
            int cur_t = music_current_index();
            canyon_track(cur_t < 0 ? 0 : cur_t);
            const float bright = s_scr_a * (0.58f + 0.42f * fade_ease(s_focus_p));
            canyon = canyon_draw(bright, music_is_paused(), s_mini_e);
            if (!canyon && s_mini_e >= 0.999f) wave_draw();
            canyon = canyon && s_mini_e >= 0.999f;   // 'the wave is not on screen'
        } else {
            wave_draw();
        }
        if (s_swap_a < 0.999f)          // the Canyon dip: the visualiser goes too
            ui_rect_gpu_draw(0, 0, (int)display_width, (int)display_height, XMB_BG,
                             (u8)((1.0f - s_swap_a) * 255.0f + 0.5f));
        {
            // The accent follows the current track's album.  Same cover size
            // draw_now_playing() uses, so it reads the bitmap on screen.
            int cur = music_current_index();
            if (cur >= count) cur = count - 1;
            if (cur < 0) cur = 0;
            music_accent_update(s_tracks[cur].art_id, cover_A1_full());
            music_cover_gpu(s_tracks[cur].art_id, cvx, cvy, cvA, cval);
            // Depth: the near ribbon passes IN FRONT of the cover wherever it
            // rises across it (the same geometry drawn again, clipped).
            // (Not while Canyon is up: there is no ribbon this frame.)
            if (!canyon && s_cover_gpu && cval > 0.9f) wave_draw_front(cvx, cvy, cvA, cvA);
            music_upnext_gpu(s_tracks, count, s_e_panel * s_up_a * (1.0f - s_mini_e));
        }
        c_gpu += timing_get_us() - t_gpu0;

        hb_frame++;
        {
            u64 now = timing_get_us();
            if (hb_us == 0 || now - hb_us >= 5000000ULL) {
                hb_us = now;
                char b[160];
                const u32 n = c_n ? c_n : 1;
                snprintf(b, sizeof b, "music_screen: f=%u %s pos=%us gpu=%llu sync=%llu draw=%llu us/frame",
                         (unsigned)hb_frame,
                         music_is_paused() ? "PAUSED" : "playing",
                         (unsigned)music_elapsed_secs(),
                         (unsigned long long)(c_gpu / n), (unsigned long long)(c_sync / n),
                         (unsigned long long)(c_draw / n));
                plog(b);
                c_gpu = c_sync = c_draw = 0; c_n = 0;
            }
        }

        poll_buttons();
        if (!s_outro) {
            if (music_any_press()) s_last_input_us = timing_get_us();
            // Leaving (O / START, or the queue finished) starts the outro: the
            // screen dissolves to the wave over OUTRO_US, then closes.
            if (music_screen_input(s_tracks, count) || !music_is_active()) {
                s_outro    = true;
                s_outro_t0 = timing_get_us();
            }
        }
        music_fades_tick(!music_is_paused() && s_fzone == FZ_TRANSPORT && !s_q_open);
        if (s_outro && s_scr_a <= 0.0f) break;

        const u64 t_s0 = timing_get_us();
        rsxSync();
        const u64 t_d0 = timing_get_us();
        c_sync += t_d0 - t_s0;

        // Text through the RSX, as the XMB does (render/ui_text_gpu.h): the
        // CPU path composites every glyph by READING video memory, ~100x
        // slower than writing it.
        ui_text_gpu_begin();
        draw_now_playing(ctx, s_tracks, count);
        if (s_viz_toast_us && timing_get_us() < s_viz_toast_us) {
            char vb[64];
            if (viz_mode() == VIZ_CANYON)
                snprintf(vb, sizeof vb, "Visualizer: Canyon - %s", canyon_preset_name());
            else
                snprintf(vb, sizeof vb, "Visualizer: %s", MVIS_NAME[music_vis_cur()]);
            draw_clipped((u32)UIS_W(40), (u32)(display_height * 0.12f), vb, UIS_TF(16),
                         fa(XMB_WHITE, s_scr_a), (int)(display_width * 0.5f));
        }
        if (s_q_open) {
            // The overlay dims the screen and lays a panel over it: the
            // now-playing text has to be on the framebuffer first.
            ui_text_gpu_flush_fenced();
            draw_queue_overlay(s_tracks, count, ctx->title);
        }
        ui_text_gpu_flush();
        c_draw += timing_get_us() - t_d0;
        c_n++;

        flip();
        sysUtilCheckCallback();
    }

    music_stop();
    wave_set_album_tint(0u, 0.0f);            // back to the plain palette
    wave_set_bg_dim(0.0f);                    // the menus get their gradient back
    // Home's posters went blank after an album was played (they stayed fine
    // in the TV tab): nothing cached before or during this screen is trusted
    // on the way out.  See thumb_cache_verify_and_flush().
    thumb_cache_verify_and_flush("music");
    plog("music_screen: exit");
}

void music_screen_origin_from_focus(void) { s_origin_pending = true; }

// The tile the album was opened from, as a square the cover can fly out of.
static void take_origin(void) {
    int x, y, w, h;
    s_have_origin = false;
    if (s_origin_pending && depth_last_focus_rect(&x, &y, &w, &h) && w > 8 && h > 8) {
        const int side = w < h ? w : h;
        s_ox = x + (w - side) / 2;
        s_oy = y + (h - side) / 2;
        s_os = side;
        s_have_origin = true;
    }
    s_origin_pending = false;
}

void music_screen_open_album(const XMBItem *album, const char *parent) {
    take_origin();
    MusicCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    snprintf(ctx.parent, sizeof(ctx.parent), "%s",
             parent && parent[0] ? parent : "Albums");
    snprintf(ctx.title,  sizeof(ctx.title),  "%s", album->name);
    snprintf(ctx.year,   sizeof(ctx.year),   "%s", album->year_str);
    snprintf(ctx.genre,  sizeof(ctx.genre),  "%s", album->genre);
    // Finish the XMB frame this was opened from, then fetch the tracks behind
    // the LOADING screen; the music screen then fades in.
    rsxSync();
    flip();
    struct { const char *id; int count; } ld = { album->id, 0 };
    loading_run([](void *a) {
                    auto *l = (decltype(ld) *)a;
                    l->count = music_fetch_album_tracks(l->id, s_tracks, MUSIC_QUEUE_MAX);
                }, &ld, "Loading", true);
    s_entry_flipped = true;
    music_screen_run(&ctx, ld.count, 0);
}

void music_screen_open_songs(const XMBItem *items, int count, int start_idx) {
    s_origin_pending = false; take_origin();
    if (count > MUSIC_QUEUE_MAX) count = MUSIC_QUEUE_MAX;
    if (start_idx >= count) start_idx = count - 1;
    for (int i = 0; i < count; i++) {
        MusicTrack *t = &s_tracks[i];
        memset(t, 0, sizeof(*t));
        snprintf(t->id,     sizeof(t->id),     "%s", items[i].id);
        snprintf(t->name,   sizeof(t->name),   "%s", items[i].name);
        snprintf(t->artist, sizeof(t->artist), "%s", items[i].artist);
        snprintf(t->art_id, sizeof(t->art_id), "%s",
                 items[i].album_id[0] ? items[i].album_id : items[i].id);
        t->duration_secs = items[i].dur_secs;
    }
    MusicCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    snprintf(ctx.parent, sizeof(ctx.parent), "Songs");
    snprintf(ctx.genre,  sizeof(ctx.genre), "%s", items[start_idx].genre);
    music_screen_run(&ctx, count, start_idx);
}

void music_screen_open_playlist(const XMBItem *playlist) {
    s_origin_pending = false; take_origin();
    MusicCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    snprintf(ctx.parent, sizeof(ctx.parent), "Playlists");
    snprintf(ctx.title,  sizeof(ctx.title),  "%s", playlist->name);
    int count = music_fetch_playlist_tracks(playlist->id, s_tracks,
                                            MUSIC_QUEUE_MAX);
    music_screen_run(&ctx, count, 0);
}
