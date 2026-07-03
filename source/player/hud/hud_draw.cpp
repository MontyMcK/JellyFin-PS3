// HUD rendering — paused title, dim strip, transport controls, seek bar,
// time labels, AUDIO/CC buttons, and the track-selection popup menu.
//
// The HUD is NOT drawn into the framebuffer.  It is composed with the CPU
// into a main-RAM ARGB staging buffer only when its content changes (the
// time string ticks once a second; everything else is input-driven),
// uploaded to an RSX texture, and drawn every frame as a single
// alpha-blended GPU quad after the video (rsx_draw_hud_overlay).  Per-frame
// cost is a few FIFO words — no rsxSync stall, no CPU writes into VRAM —
// so a visible seek bar can no longer push the display loop past one
// vblank (the "HUD splits the FPS with the video" lag).

#include <stdio.h>
#include <string.h>
#include <malloc.h>

#include <ppu-types.h>
#include <rsx/rsx.h>

#include "player_hud.h"
#include "player_hud_internal.h"
#include "player_rsx.h"
#include "ui.h"
#include "ui_visuals.h"
#include "rsxutil.h"
#include "plog.h"

// -------------------------------------------------------
// Overlay buffers + compose-on-change state
// -------------------------------------------------------

static u32 *s_ovl_stage   = NULL;   // main-RAM compose target (straight alpha)
static u32 *s_ovl_tex     = NULL;   // RSX-local texture the GPU samples
static u32  s_ovl_tex_off = 0;
static u32  s_ovl_w = 0, s_ovl_h = 0;
static int  s_ovl_prev_y0 = 0, s_ovl_prev_y1 = 0;  // rows drawn by last compose
static int  s_ovl_y0 = 0, s_ovl_y1 = 0;            // rows drawn by this compose

// Everything the overlay's pixels depend on.  Compared memberwise per frame;
// a change triggers one recompose + upload.  Zero the struct before filling
// so padding compares clean.
struct OvlKey {
    u32  elapsed_secs;
    u32  total_secs;
    s32  focus, incr_idx;
    s32  menu_sel, menu_cur, menu_n;
    u8   paused, cc, menu_vis, title_on;
    char audio[64];
};
static OvlKey s_ovl_key;
static bool   s_ovl_key_valid = false;

void hud_overlay_alloc(void) {
    s_ovl_w = display_width;
    s_ovl_h = display_height;
    u32 bytes = s_ovl_w * s_ovl_h * 4;
    s_ovl_stage = (u32*)memalign(128, bytes);
    s_ovl_tex   = (u32*)rsxMemalign(64, bytes);
    if (s_ovl_stage) memset(s_ovl_stage, 0, bytes);
    if (s_ovl_tex) {
        memset((void*)s_ovl_tex, 0, bytes);
        rsxAddressToOffset(s_ovl_tex, &s_ovl_tex_off);
    }
    if (!s_ovl_stage || !s_ovl_tex) plog("hud_ovl: alloc FAILED");
    s_ovl_prev_y0 = s_ovl_prev_y1 = 0;
    s_ovl_key_valid = false;
}

void hud_overlay_free(void) {
    if (s_ovl_stage) { free(s_ovl_stage);   s_ovl_stage = NULL; }
    if (s_ovl_tex)   { rsxFree(s_ovl_tex);  s_ovl_tex   = NULL; }
}

// Extend the dirty row span of the current compose.
static void ovl_span(int y0, int y1) {
    if (y0 < 0) y0 = 0;
    if (y1 > (int)s_ovl_h) y1 = (int)s_ovl_h;
    if (y1 <= y0) return;
    if (y0 < s_ovl_y0) s_ovl_y0 = y0;
    if (y1 > s_ovl_y1) s_ovl_y1 = y1;
}

// Translucent black backdrop written straight into the staging buffer —
// the bottom layer of each HUD region, so no compositing needed.
static void ovl_dim(u32 rx, u32 ry, u32 rw, u32 rh, u8 alpha) {
    if (rx >= s_ovl_w || ry >= s_ovl_h) return;
    u32 x2 = (rx + rw > s_ovl_w) ? s_ovl_w : rx + rw;
    u32 y2 = (ry + rh > s_ovl_h) ? s_ovl_h : ry + rh;
    u32 px = (u32)alpha << 24;
    for (u32 y = ry; y < y2; y++) {
        u32 *row = s_ovl_stage + y * s_ovl_w;
        for (u32 x = rx; x < x2; x++) row[x] = px;
    }
    ovl_span((int)ry, (int)y2);
}

// -------------------------------------------------------
// CPU-drawn primitives (render into the compose target)
// -------------------------------------------------------

static void draw_circle(int cx, int cy, int r, u32 color) {
    int r2 = r * r;
    u32 tw_ = cpu_draw_w(), th_ = cpu_draw_h();
    if (cpu_rt_on()) color |= 0xFF000000u;
    for (int dy = -r; dy <= r; dy++) {
        int sy = cy + dy;
        if (sy < 0 || (u32)sy >= th_) continue;
        u32 *rowp = cpu_draw_row((u32)sy);
        for (int dx = -r; dx <= r; dx++) {
            if (dx * dx + dy * dy > r2) continue;
            int sx = cx + dx;
            if (sx < 0 || (u32)sx >= tw_) continue;
            rowp[sx] = color;
        }
    }
}

// Draw ▶ (paused=true) or ⏸ (paused=false) centred at (cx, cy).
static void draw_pp_symbol(int cx, int cy, bool paused, u32 color) {
    if (paused) {
        // Right-pointing filled triangle: tip at (cx + PP_W/2, cy).
        int half_h = PP_H / 2;
        int half_w = PP_W / 2;
        u32 tw_ = cpu_draw_w(), th_ = cpu_draw_h();
        if (cpu_rt_on()) color |= 0xFF000000u;
        for (int dy = -half_h; dy <= half_h; dy++) {
            int sy = cy + dy;
            if (sy < 0 || (u32)sy >= th_) continue;
            int abs_dy = dy < 0 ? -dy : dy;
            int span = (half_h - abs_dy) * (2 * half_w) / (half_h > 0 ? half_h : 1);
            int x0 = cx - half_w;
            int x1 = x0 + span;
            u32 *line = cpu_draw_row((u32)sy);
            for (int sx = x0; sx <= x1; sx++) {
                if (sx >= 0 && (u32)sx < tw_)
                    line[sx] = color;
            }
        }
    } else {
        // Two vertical bars (pause symbol).
        int bar_w = 5;
        int gap   = 6;
        int x0 = cx - gap / 2 - bar_w;
        int x1 = cx + gap / 2;
        int y0 = cy - PP_H / 2;
        if (x0 < 0) x0 = 0;
        if (x1 < 0) x1 = 0;
        if (y0 < 0) y0 = 0;
        drawRect((u32)x0, (u32)y0, (u32)bar_w, (u32)PP_H, color);
        drawRect((u32)x1, (u32)y0, (u32)bar_w, (u32)PP_H, color);
    }
}

static void fmt_time(char *buf, int sz, u32 secs) {
    u32 h = secs / 3600;
    u32 m = (secs % 3600) / 60;
    u32 s = secs % 60;
    if (h > 0) snprintf(buf, sz, "%u:%02u:%02u", h, m, s);
    else        snprintf(buf, sz, "%u:%02u",      m, s);
}

// Colour for a focusable slot: white=focused, dimmed=another is focused, accent=no focus.
static u32 ctrl_color(int slot) {
    if (g_hud.focus < 0)     return HUD_ACCENT;
    if (g_hud.focus == slot) return HUD_FOCUSED;
    return HUD_DIMMED;
}

// -------------------------------------------------------
// Popup menu (track selection), bottom-right above the strip
// -------------------------------------------------------

static void draw_menu(int dw, int dh) {
    int max_w = ttf_text_width(g_hud.menu_title, MENU_TITLE_PX);
    for (int i = 0; i < g_hud.menu_n; i++) {
        int tw = MENU_DOT_COL + ttf_text_width(g_hud.menu_items[i], ROW_TEXT_PX);
        if (tw > max_w) max_w = tw;
    }
    int mw = max_w + 2 * MENU_PAD;
    if (mw > dw - 2 * RIGHT_PAD) mw = dw - 2 * RIGHT_PAD;
    int mh  = MENU_TITLE_H + g_hud.menu_n * MENU_ROW_H + MENU_PAD;
    int mx1 = dw - RIGHT_PAD;
    int mx0 = mx1 - mw;
    int my1 = dh - HUD_STRIP_H - 6;
    int my0 = my1 - mh;
    if (my0 < 6) my0 = 6;

    ovl_dim((u32)mx0, (u32)my0, (u32)mw, (u32)(my1 - my0), 225);

    drawTTF((u32)(mx0 + MENU_PAD),
            (u32)(my0 + (MENU_TITLE_H - (int)MENU_TITLE_PX) / 2),
            g_hud.menu_title, MENU_TITLE_PX, HUD_ACCENT, /*bold=*/true);

    for (int i = 0; i < g_hud.menu_n; i++) {
        int row_y0 = my0 + MENU_TITLE_H + i * MENU_ROW_H;
        int row_cy = row_y0 + MENU_ROW_H / 2;
        if (i == g_hud.menu_sel)
            drawRect((u32)(mx0 + 4), (u32)row_y0,
                     (u32)(mw - 8), MENU_ROW_H, HUD_ACCENT_DIM);
        if (i == g_hud.menu_cur)
            draw_circle(mx0 + MENU_PAD + 5, row_cy, 4, HUD_ACCENT);
        drawTTF((u32)(mx0 + MENU_PAD + MENU_DOT_COL),
                (u32)(row_cy - (int)(ROW_TEXT_PX * 0.5f)),
                g_hud.menu_items[i], ROW_TEXT_PX,
                (i == g_hud.menu_sel) ? HUD_FOCUSED : 0x0099A0BCUL);
    }
}

// -------------------------------------------------------
// hud_compose — render the full overlay into the staging buffer
// -------------------------------------------------------

static void hud_compose(u64 elapsed_us, bool paused) {
    // Wipe the rows the previous compose drew, then track this one's span.
    if (s_ovl_prev_y1 > s_ovl_prev_y0)
        memset(s_ovl_stage + (u32)s_ovl_prev_y0 * s_ovl_w, 0,
               (u32)(s_ovl_prev_y1 - s_ovl_prev_y0) * s_ovl_w * 4);
    s_ovl_y0 = (int)s_ovl_h;
    s_ovl_y1 = 0;

    cpu_rt_begin(s_ovl_stage, s_ovl_w, s_ovl_h);

    int dw = (int)s_ovl_w;
    int dh = (int)s_ovl_h;

    // ---- Title (top-left, only while paused) ----
    if (paused && g_hud.title[0]) {
        int tw = ttf_text_width(g_hud.title, TITLE_PX);
        if (tw > dw - 2 * LEFT_PAD) tw = dw - 2 * LEFT_PAD;
        ovl_dim((u32)(LEFT_PAD - 12), (u32)(TITLE_TOP_PAD - 8),
                (u32)(tw + 24), (u32)((int)TITLE_PX + 16), 185);
        drawTTF((u32)LEFT_PAD, (u32)TITLE_TOP_PAD, g_hud.title, TITLE_PX,
                HUD_FOCUSED);
    }

    // ---- Background strip ----
    int strip_y = dh - HUD_STRIP_H;
    if (strip_y < 0) strip_y = 0;
    ovl_dim(0, (u32)strip_y, (u32)dw, (u32)(dh - strip_y), 185);

    // ---- Row centre ----
    // Everything — transport, seek bar, times, AUDIO, CC — shares one row.
    int ctrl_cy  = dh - CTRL_Y_OFF;
    int audio_cy = ctrl_cy;

    // ---- Time strings (needed for seek bar layout) ----
    u32 elapsed_secs = (u32)(elapsed_us / 1000000ULL);
    char elapsed_str[16];
    fmt_time(elapsed_str, sizeof(elapsed_str), elapsed_secs);
    int elapsed_w = ttf_text_width(elapsed_str, ROW_TEXT_PX);

    char rem_str[20] = "";
    int  rem_w = 0;
    if (g_hud.total_secs > 0) {
        u32 rem = (g_hud.total_secs > elapsed_secs) ? g_hud.total_secs - elapsed_secs : 0;
        char t[16]; fmt_time(t, sizeof(t), rem);
        snprintf(rem_str, sizeof(rem_str), "-%s", t);
        rem_w = ttf_text_width(rem_str, ROW_TEXT_PX);
    }

    // ---- Right controls block: audio + CC ----
    int w_music  = (int)MUSIC_ICON_PX;
    int w_alabel = ttf_text_width(g_hud.audio_label, ROW_TEXT_PX);
    int w_cc     = ttf_text_width("CC", CC_TEXT_PX);
    int rctrl_w  = w_music + ICON_LABEL_GAP + w_alabel + AUDIO_SEP + w_cc;
    int rctrl_x0 = dw - RIGHT_PAD - rctrl_w;   // left edge of right controls

    // ---- Left controls block: rewind + play/pause + fast-forward ----
    int w_rew    = iconic_adv_px('L', ROW_ICON_PX);
    int w_ff     = iconic_adv_px('R', ROW_ICON_PX);
    int lctrl_x1 = LEFT_PAD + w_rew + CTRL_GAP + PP_W + CTRL_GAP + w_ff;

    // ---- Seek bar (between elapsed time and remaining time) ----
    int track_x0 = lctrl_x1 + TIME_GAP + elapsed_w + TIME_GAP;
    int rem_x    = rctrl_x0  - RCTRL_GAP - rem_w;
    int track_x1 = rem_x     - TIME_GAP;
    int track_w  = track_x1  - track_x0;
    if (track_w < 20) { track_w = 20; track_x1 = track_x0 + 20; }

    int track_y = ctrl_cy - TRACK_H / 2;

    float progress = 0.0f;
    if (g_hud.total_secs > 0) {
        progress = (float)elapsed_secs / (float)g_hud.total_secs;
        if (progress > 1.0f) progress = 1.0f;
    }
    int fill_w = (int)(progress * (float)track_w);

    drawRect((u32)track_x0,            (u32)track_y, (u32)fill_w,             TRACK_H, HUD_ACCENT);
    drawRect((u32)(track_x0 + fill_w), (u32)track_y, (u32)(track_w - fill_w), TRACK_H, HUD_ACCENT_DIM);
    draw_circle(track_x0 + fill_w, track_y + TRACK_H / 2, SCRUB_R, HUD_ACCENT);

    // ---- Time labels (centred vertically on ctrl row) ----
    // drawTTF y ≈ top of glyph; shift up by half px to visually centre.
    int time_y = ctrl_cy - (int)(ROW_TEXT_PX * 0.5f);
    drawTTF((u32)(lctrl_x1 + TIME_GAP), (u32)time_y, elapsed_str, ROW_TEXT_PX, HUD_ACCENT);
    if (rem_w > 0)
        drawTTF((u32)rem_x, (u32)time_y, rem_str, ROW_TEXT_PX, HUD_ACCENT);

    // ---- Left transport controls ----
    // Iconic glyphs: ink vertically centred on the control row.
    int lx = LEFT_PAD;

    draw_iconic_glyph_vcentered((u32)lx, ctrl_cy, 'L', ROW_ICON_PX,
                                ctrl_color(FOCUS_REW));
    lx += w_rew + CTRL_GAP;

    draw_pp_symbol(lx + PP_W / 2, ctrl_cy, paused, ctrl_color(FOCUS_PP));
    lx += PP_W + CTRL_GAP;

    draw_iconic_glyph_vcentered((u32)lx, ctrl_cy, 'R', ROW_ICON_PX,
                                ctrl_color(FOCUS_FF));

    // ---- Audio / CC (right of the seek bar, same row) ----
    int audio_icon_y = audio_cy - (int)(MUSIC_ICON_PX * 0.5f);
    int audio_text_y = audio_cy - (int)(ROW_TEXT_PX   * 0.5f);
    int cc_text_y    = audio_cy - (int)(CC_TEXT_PX    * 0.5f);
    int rx = rctrl_x0;

    drawIcon((u32)rx, (u32)audio_icon_y, MATERIAL_MUSIC_NOTE, MUSIC_ICON_PX,
             ctrl_color(FOCUS_AUDIO));
    rx += w_music + ICON_LABEL_GAP;
    drawTTF((u32)rx, (u32)audio_text_y, g_hud.audio_label, ROW_TEXT_PX,
            ctrl_color(FOCUS_AUDIO));
    rx += w_alabel + AUDIO_SEP;

    drawTTF((u32)rx, (u32)cc_text_y, "CC", CC_TEXT_PX,
            ctrl_color(FOCUS_CC), /*bold=*/true);
    // Subtitles on: accent underline beneath the CC button.
    if (g_hud.cc_active)
        drawRect((u32)rx, (u32)(cc_text_y + (int)CC_TEXT_PX + 3),
                 (u32)w_cc, 2, HUD_ACCENT);

    if (g_hud.menu_visible && g_hud.menu_n > 0)
        draw_menu(dw, dh);

    cpu_rt_end();

    // Upload the union of the previous and current spans so rows the HUD no
    // longer covers (e.g. a closed menu) are cleared on the texture too.
    int up_y0 = (s_ovl_prev_y0 < s_ovl_y0) ? s_ovl_prev_y0 : s_ovl_y0;
    int up_y1 = (s_ovl_prev_y1 > s_ovl_y1) ? s_ovl_prev_y1 : s_ovl_y1;
    if (up_y1 > up_y0)
        memcpy((void*)(s_ovl_tex + (u32)up_y0 * s_ovl_w),
               s_ovl_stage + (u32)up_y0 * s_ovl_w,
               (u32)(up_y1 - up_y0) * s_ovl_w * 4);
    s_ovl_prev_y0 = (s_ovl_y1 > s_ovl_y0) ? s_ovl_y0 : 0;
    s_ovl_prev_y1 = (s_ovl_y1 > s_ovl_y0) ? s_ovl_y1 : 0;

    {
        static int s_cl = 0;
        if (s_cl < 12) {
            s_cl++;
            char buf[80];
            snprintf(buf, sizeof(buf), "hud_ovl: compose#%d rows=%d..%d up=%d..%d",
                     s_cl, s_ovl_y0, s_ovl_y1, up_y0, up_y1);
            plog(buf);
        }
    }
}

// -------------------------------------------------------
// hud_draw — recompose the overlay if its content changed, then queue the
// GPU quad.  Called every frame the HUD is visible; the compose branch runs
// about once a second (when the time string ticks) or on input.
// -------------------------------------------------------

void hud_draw(u64 elapsed_us, bool paused) {
    if (!g_hud.visible) return;
    if (!s_ovl_stage || !s_ovl_tex) return;   // alloc failed: no HUD

    OvlKey key;
    memset(&key, 0, sizeof(key));
    key.elapsed_secs = (u32)(elapsed_us / 1000000ULL);
    key.total_secs   = g_hud.total_secs;
    key.focus        = g_hud.focus;
    key.incr_idx     = g_hud.incr_idx;
    key.menu_sel     = g_hud.menu_sel;
    key.menu_cur     = g_hud.menu_cur;
    key.menu_n       = g_hud.menu_n;
    key.paused       = paused ? 1 : 0;
    key.cc           = g_hud.cc_active ? 1 : 0;
    key.menu_vis     = g_hud.menu_visible ? 1 : 0;
    key.title_on     = (paused && g_hud.title[0]) ? 1 : 0;
    snprintf(key.audio, sizeof(key.audio), "%s", g_hud.audio_label);

    if (!s_ovl_key_valid || memcmp(&key, &s_ovl_key, sizeof(key)) != 0) {
        s_ovl_key       = key;
        s_ovl_key_valid = true;
        hud_compose(elapsed_us, paused);
    }

    rsx_draw_hud_overlay(s_ovl_tex_off, s_ovl_w, s_ovl_h);
}
