#include <stdio.h>
#include <string.h>

#include <ppu-types.h>
#include <rsx/rsx.h>
#include <rsx/gcm_sys.h>

#include "player_hud.h"
#include "hud_dim_shaders.h"
#include "ui.h"
#include "ui_visuals.h"
#include "rsxutil.h"
#include "plog.h"
#include "plog.h"
#include "timing.h"

extern void crash_log(const char *msg);

// Dim-strip path selector (mutually exclusive; define at most one):
//   HUD_DIM_CPU       -- CPU pixel blend (slow, bulletproof)
//   HUD_DIM_GPU_ARRAY -- original array-fetch GPU quad (freezes; testing only)
//   (neither)         -- inline GPU quad (default; fast, no array fetch wedge)

// -------------------------------------------------------
// Visual constants
// -------------------------------------------------------
#define HUD_ACCENT          0x007C3CEAUL
#define HUD_ACCENT_DIM      0x00261260UL
#define HUD_DIMMED          0x00402070UL   // unfocused when another slot is active
#define HUD_FOCUSED         0x00FFFFFFUL   // white for focused control
#define HUD_SHOW_US         4000000ULL

#define HUD_STRIP_H         130
#define CTRL_Y_OFF           68            // transport row centre from screen bottom
#define AUDIO_Y_OFF          98            // audio/CC row centre from screen bottom
#define INCR_Y_OFF          116            // seek-increment label bottom from screen bottom
#define TRACK_H               4
#define SCRUB_R               6
#define LEFT_PAD             50
#define RIGHT_PAD            50
#define CTRL_GAP             10            // gap between transport control glyphs
#define TIME_GAP             12            // gap between time label edge and seek bar
#define RCTRL_GAP            12            // gap between remaining time and right controls

#define ROW_ICON_PX          24.0f         // L/R iconic glyph size
#define ROW_TEXT_PX          18.0f         // time + audio label text size
#define MUSIC_ICON_PX        24.0f
#define CC_TEXT_PX           20.0f
#define INCR_PX              13.0f
#define ICON_LABEL_GAP        6
#define AUDIO_SEP            16            // horizontal gap between audio btn and CC btn

#define PP_H                 22            // play/pause primitive bounding height (px)
#define PP_W                 22            // play/pause primitive bounding width  (px)

#define MATERIAL_MUSIC_NOTE  0xE405

// Focus slot indices
#define FOCUS_REW    0
#define FOCUS_PP     1
#define FOCUS_FF     2
#define FOCUS_AUDIO  3
#define FOCUS_CC     4
#define FOCUS_COUNT  5

// -------------------------------------------------------
// State
// -------------------------------------------------------
static u32  s_total_secs = 0;
static char s_audio_label[64];
static bool s_visible    = false;
static u64  s_show_us    = 0;
static int  s_seek_delta = 0;
static int  s_focus      = -1;   // -1=none, 0..FOCUS_COUNT-1=focused slot
static int  s_incr_idx   = 0;    // 0=10s  1=30s  2=5min

// Auto-repeat for held FF/REW: after holding past the delay, emit a seek every
// interval until released.  The player's debounce accumulates these, so holding
// scrubs the bar forward quickly and commits to one reopen on release.
#define SEEK_REPEAT_DELAY_US    500000ULL   // hold 0.5s before repeat kicks in
#define SEEK_REPEAT_INTERVAL_US 250000ULL   // then repeat 4x/sec
#define SEEK_RELEASE_GRACE_US   150000ULL   // ignore analog-trigger flicker dropouts
static int s_seek_repeat_dir    = 0;        // -1=rew, +1=ff, 0=none held
static u64 s_seek_hold_start_us = 0;        // when the current hold began
static u64 s_seek_last_fire_us  = 0;        // last auto-repeat emission
static u64 s_seek_last_active_us = 0;       // last frame the button read as held

// RSX resources for the GPU-drawn dim quad
static u8  *s_hud_vbuf    = NULL;
static u32  s_hud_vbuf_off = 0;
static u32 *s_hud_fp_buf  = NULL;
static u32  s_hud_fp_off  = 0;

static const int  s_incr_vals[3]  = { 10, 30, 300 };
static const char *s_incr_strs[3] = { "10s", "30s", "5min" };

// -------------------------------------------------------
// Private helpers
// -------------------------------------------------------

static void show_hud(void) {
    s_visible = true;
    s_show_us = timing_get_us();
}

#if defined(HUD_DIM_CPU)
// Slow fallback: darken strip in-place on color_buffer[curr_fb].
// Precondition: rsxSync() called by caller before hud_draw().
static void dim_rect_cpu(u32 rx, u32 ry, u32 rw, u32 rh, u8 alpha) {
    crash_log("dr1 enter");
    u32 x_end = rx + rw;
    u32 y_end = ry + rh;
    if (x_end > display_width)  x_end = display_width;
    if (y_end > display_height) y_end = display_height;
    u32 inv = 255u - (u32)alpha;
    for (u32 y = ry; y < y_end; y++) {
        u32 *row = color_buffer[curr_fb] + y * display_width;
        for (u32 x = rx; x < x_end; x++) {
            u32 p = row[x];
            u32 r = (((p >> 16) & 0xFFu) * inv) >> 8;
            u32 g = (((p >>  8) & 0xFFu) * inv) >> 8;
            u32 b = (((p      ) & 0xFFu) * inv) >> 8;
            row[x] = (p & 0xFF000000u) | (r << 16) | (g << 8) | b;
        }
    }
    crash_log("dr6 return");
}

#elif defined(HUD_DIM_GPU_ARRAY)
// Array-fetch GPU quad. Known root cause of the original freeze: stale TEX0
// array binding from the video path wedges the RSX vertex fetch unit.
// Kept for reference testing only; do not use in production.
// Caller must call rsxSync() after this returns and before any CPU pixel writes.
static void draw_dim_rect(u32 rx, u32 ry, u32 rw, u32 rh, u8 alpha) {
    crash_log("dr1 enter");
    if (!s_hud_vbuf || !s_hud_fp_buf) { crash_log("dr1 null rsrc"); return; }

    rsxSync();
    crash_log("dr2 synced");

    float W = (float)display_width;
    float H = (float)display_height;
    float x0 = (float)rx         / W * 2.0f - 1.0f;
    float x1 = (float)(rx + rw)  / W * 2.0f - 1.0f;
    float y0 = 1.0f - (float)ry        / H * 2.0f;
    float y1 = 1.0f - (float)(ry + rh) / H * 2.0f;

    typedef struct { float x, y, z, w; u8 r, g, b, a; } Vert;
    Vert *v = (Vert*)s_hud_vbuf;
    v[0].x=x0; v[0].y=y0; v[0].z=0.f; v[0].w=1.f; v[0].r=0; v[0].g=0; v[0].b=0; v[0].a=alpha;
    v[1].x=x1; v[1].y=y0; v[1].z=0.f; v[1].w=1.f; v[1].r=0; v[1].g=0; v[1].b=0; v[1].a=alpha;
    v[2].x=x0; v[2].y=y1; v[2].z=0.f; v[2].w=1.f; v[2].r=0; v[2].g=0; v[2].b=0; v[2].a=alpha;
    v[3].x=x1; v[3].y=y1; v[3].z=0.f; v[3].w=1.f; v[3].r=0; v[3].g=0; v[3].b=0; v[3].a=alpha;
    crash_log("dr2 verts written");

    rsxVertexProgram  *vpo = (rsxVertexProgram*) hud_dim_vp_data;
    rsxFragmentProgram *fpo = (rsxFragmentProgram*) hud_dim_fp_data;

    void *vp_ucode; u32 vp_size;
    rsxVertexProgramGetUCode(vpo, &vp_ucode, &vp_size);
    rsxLoadVertexProgram(context, vpo, vp_ucode);
    rsxSetVertexAttribOutputMask(context, vpo->output_mask);
    rsxLoadFragmentProgramLocation(context, fpo, s_hud_fp_off, GCM_LOCATION_RSX);
    rsxTextureControl(context, 0, GCM_FALSE, 0, 12 << 8, GCM_TEXTURE_MAX_ANISO_1);
    crash_log("dr3 progs loaded");

    rsxSetDepthTestEnable(context, GCM_FALSE);
    rsxSetDepthWriteEnable(context, GCM_FALSE);
    rsxSetColorMask(context,
        GCM_COLOR_MASK_R | GCM_COLOR_MASK_G |
        GCM_COLOR_MASK_B | GCM_COLOR_MASK_A);
    rsxSetBlendFunc(context,
        GCM_SRC_ALPHA, GCM_ONE_MINUS_SRC_ALPHA,
        GCM_SRC_ALPHA, GCM_ONE_MINUS_SRC_ALPHA);
    rsxSetBlendEquation(context, GCM_FUNC_ADD, GCM_FUNC_ADD);
    rsxSetBlendEnable(context, GCM_TRUE);

    rsxBindVertexArrayAttrib(context, GCM_VERTEX_ATTRIB_POS, 0,
        s_hud_vbuf_off, 20, 4, GCM_VERTEX_DATA_TYPE_F32, GCM_LOCATION_RSX);
    rsxBindVertexArrayAttrib(context, GCM_VERTEX_ATTRIB_COLOR0, 0,
        s_hud_vbuf_off + 16, 20, 4, GCM_VERTEX_DATA_TYPE_U8, GCM_LOCATION_RSX);
    rsxBindVertexArrayAttrib(context, GCM_VERTEX_ATTRIB_TEX0, 0,
        0, 0, 0, GCM_VERTEX_DATA_TYPE_F32, GCM_LOCATION_RSX);
    crash_log("dr4 attribs bound");

    rsxInvalidateVertexCache(context);
    rsxDrawVertexArray(context, GCM_TYPE_TRIANGLE_STRIP, 0, 4);
    crash_log("dr5 draw issued");

    rsxSetBlendEnable(context, GCM_FALSE);
    rsxBindVertexArrayAttrib(context, GCM_VERTEX_ATTRIB_COLOR0, 0,
        s_hud_vbuf_off + 16, 0, 4, GCM_VERTEX_DATA_TYPE_U8, GCM_LOCATION_RSX);
    crash_log("dr6 return");
}

#else
// Default: inline GPU quad -- all 4 vertices submitted directly into the RSX
// command FIFO. No vertex-array fetch step: no stale array binding can wedge
// the GPU. Runs on the GPU so frame time stays at ~16ms with HUD visible.
// Caller must call rsxSync() after this returns and before any CPU pixel writes.
static void draw_dim_rect_inline(u32 rx, u32 ry, u32 rw, u32 rh, u8 alpha) {
    crash_log("dr1 enter");
    if (!s_hud_fp_buf) { crash_log("dr1 null rsrc"); return; }

    float W = (float)display_width;
    float H = (float)display_height;
    float x0 = (float)rx        / W * 2.0f - 1.0f;
    float x1 = (float)(rx + rw) / W * 2.0f - 1.0f;
    float y0 = 1.0f - (float)ry        / H * 2.0f;
    float y1 = 1.0f - (float)(ry + rh) / H * 2.0f;

    rsxVertexProgram  *vpo = (rsxVertexProgram*)  hud_dim_vp_data;
    rsxFragmentProgram *fpo = (rsxFragmentProgram*) hud_dim_fp_data;

    void *vp_ucode; u32 vp_size;
    rsxVertexProgramGetUCode(vpo, &vp_ucode, &vp_size);
    rsxLoadVertexProgram(context, vpo, vp_ucode);
    rsxSetVertexAttribOutputMask(context, vpo->output_mask);
    rsxLoadFragmentProgramLocation(context, fpo, s_hud_fp_off, GCM_LOCATION_RSX);
    rsxTextureControl(context, 0, GCM_FALSE, 0, 12 << 8, GCM_TEXTURE_MAX_ANISO_1);
    rsxSetDepthTestEnable(context, GCM_FALSE);
    rsxSetDepthWriteEnable(context, GCM_FALSE);
    rsxSetColorMask(context,
        GCM_COLOR_MASK_R | GCM_COLOR_MASK_G |
        GCM_COLOR_MASK_B | GCM_COLOR_MASK_A);
    rsxSetBlendFunc(context,
        GCM_SRC_ALPHA, GCM_ONE_MINUS_SRC_ALPHA,
        GCM_SRC_ALPHA, GCM_ONE_MINUS_SRC_ALPHA);
    rsxSetBlendEquation(context, GCM_FUNC_ADD, GCM_FUNC_ADD);
    rsxSetBlendEnable(context, GCM_TRUE);
    crash_log("dr2 state set");

    // POS = attrib 0, COLOR0 = attrib 3 (from hud_dim_vp: vertex.position/vertex.color).
    // Push color before position per vertex; the trailing position command latches
    // the vertex into the FIFO (matches Movian realityAttr4f/realityVertex4f ordering).
    const u8   col[4] = { 0, 0, 0, alpha };
    const float tl[4] = { x0, y0, 0.f, 1.f };
    const float tr[4] = { x1, y0, 0.f, 1.f };
    const float bl[4] = { x0, y1, 0.f, 1.f };
    const float br[4] = { x1, y1, 0.f, 1.f };

    rsxDrawVertexBegin(context, GCM_TYPE_TRIANGLE_STRIP);
    crash_log("dr3 begin");

    rsxDrawVertex4ub(context, GCM_VERTEX_ATTRIB_COLOR0, col);
    rsxDrawVertex4f (context, GCM_VERTEX_ATTRIB_POS,    tl);
    rsxDrawVertex4ub(context, GCM_VERTEX_ATTRIB_COLOR0, col);
    rsxDrawVertex4f (context, GCM_VERTEX_ATTRIB_POS,    tr);
    rsxDrawVertex4ub(context, GCM_VERTEX_ATTRIB_COLOR0, col);
    rsxDrawVertex4f (context, GCM_VERTEX_ATTRIB_POS,    bl);
    rsxDrawVertex4ub(context, GCM_VERTEX_ATTRIB_COLOR0, col);
    rsxDrawVertex4f (context, GCM_VERTEX_ATTRIB_POS,    br);
    crash_log("dr4 verts pushed");

    rsxDrawVertexEnd(context);
    rsxSetBlendEnable(context, GCM_FALSE);
    crash_log("dr5 end blend off");
    crash_log("dr6 return");
}
#endif /* dim path selector */

static void draw_circle(int cx, int cy, int r, u32 color) {
    int r2 = r * r;
    for (int dy = -r; dy <= r; dy++) {
        int sy = cy + dy;
        if (sy < 0 || (u32)sy >= display_height) continue;
        u32 *rowp = color_buffer[curr_fb] + (u32)sy * display_width;
        for (int dx = -r; dx <= r; dx++) {
            if (dx * dx + dy * dy > r2) continue;
            int sx = cx + dx;
            if (sx < 0 || (u32)sx >= display_width) continue;
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
        for (int dy = -half_h; dy <= half_h; dy++) {
            int sy = cy + dy;
            if (sy < 0 || (u32)sy >= display_height) continue;
            int abs_dy = dy < 0 ? -dy : dy;
            int span = (half_h - abs_dy) * (2 * half_w) / (half_h > 0 ? half_h : 1);
            int x0 = cx - half_w;
            int x1 = x0 + span;
            u32 *line = color_buffer[curr_fb] + (u32)sy * display_width;
            for (int sx = x0; sx <= x1; sx++) {
                if (sx >= 0 && (u32)sx < display_width)
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
    if (s_focus < 0)     return HUD_ACCENT;
    if (s_focus == slot) return HUD_FOCUSED;
    return HUD_DIMMED;
}

// -------------------------------------------------------
// Public API
// -------------------------------------------------------

void hud_gpu_init(void) {
    s_hud_vbuf = (u8*)rsxMemalign(128, 4 * 20);
    rsxAddressToOffset(s_hud_vbuf, &s_hud_vbuf_off);

    rsxFragmentProgram *fpo = (rsxFragmentProgram*)hud_dim_fp_data;
    void *fp_ucode; u32 fp_size;
    rsxFragmentProgramGetUCode(fpo, &fp_ucode, &fp_size);
    s_hud_fp_buf = (u32*)rsxMemalign(256, fp_size);
    memcpy(s_hud_fp_buf, fp_ucode, fp_size);
    rsxAddressToOffset(s_hud_fp_buf, &s_hud_fp_off);
}

void hud_gpu_shutdown(void) {
    if (s_hud_vbuf)   { rsxFree(s_hud_vbuf);   s_hud_vbuf   = NULL; }
    if (s_hud_fp_buf) { rsxFree(s_hud_fp_buf);  s_hud_fp_buf = NULL; }
}

void hud_init(u32 total_secs, const char *audio_label) {
    s_total_secs = total_secs;
    snprintf(s_audio_label, sizeof(s_audio_label), "%s",
             (audio_label && audio_label[0]) ? audio_label : "Audio");
    s_visible    = false;
    s_show_us    = 0;
    s_seek_delta = 0;
    s_focus      = -1;
    s_incr_idx   = 0;
    hud_gpu_init();
}

void hud_shutdown(void) {
    s_visible = false;
    hud_gpu_shutdown();
}
bool hud_is_visible(void) { return s_visible; }
int  hud_seek_delta(void) { return s_seek_delta; }

HudAction hud_handle_input(bool l2_pressed, bool r2_pressed, bool paused) {
    s_seek_delta = 0;

    // Any button activity wakes the HUD.
    if (btn_cur.cross || btn_cur.circle || btn_cur.square || btn_cur.triangle ||
        btn_cur.l1 || btn_cur.r1 || btn_cur.l2 || btn_cur.r2 ||
        l2_pressed || r2_pressed ||
        btn_cur.up || btn_cur.down || btn_cur.left || btn_cur.right)
        show_hud();

    // Auto-hide after timeout — only when playing; stay visible indefinitely while paused.
    if (s_visible && !paused && (timing_get_us() - s_show_us) >= HUD_SHOW_US) {
        s_visible = false;
        s_focus   = -1;
        return HUD_ACTION_NONE;
    }
    if (!s_visible) return HUD_ACTION_NONE;

    // Triangle: toggle focus mode (enter at play/pause slot, exit if already in focus).
    if (BTN_PRESSED(triangle)) {
        s_focus = (s_focus < 0) ? FOCUS_PP : -1;
        return HUD_ACTION_NONE;
    }

    // Circle: cancel focus mode.
    if (BTN_PRESSED(circle) && s_focus >= 0) {
        s_focus = -1;
        return HUD_ACTION_NONE;
    }

    if (s_focus >= 0) {
        // Left/right navigate all 5 slots (wraps).
        if (BTN_PRESSED(left))  {
            s_focus = (s_focus > 0) ? s_focus - 1 : FOCUS_COUNT - 1;
            return HUD_ACTION_NONE;
        }
        if (BTN_PRESSED(right)) {
            s_focus = (s_focus < FOCUS_COUNT - 1) ? s_focus + 1 : 0;
            return HUD_ACTION_NONE;
        }
        if (BTN_PRESSED(cross)) {
            if (s_focus == FOCUS_PP)    return HUD_ACTION_TOGGLE_PAUSE;
            if (s_focus == FOCUS_AUDIO) return HUD_ACTION_AUDIO_TRACK;
            if (s_focus == FOCUS_CC)    return HUD_ACTION_SUBTITLE;
            int incr = s_incr_vals[s_incr_idx];
            if (s_focus == FOCUS_REW)   { s_seek_delta = -incr; return HUD_ACTION_SEEK; }
            if (s_focus == FOCUS_FF)    { s_seek_delta = +incr; return HUD_ACTION_SEEK; }
        }
    } else {
        // Unfocused: up/down changes increment; left/right/L2/R2 seeks; cross pauses.
        if (BTN_PRESSED(up))   { if (s_incr_idx < 2) s_incr_idx++; return HUD_ACTION_NONE; }
        if (BTN_PRESSED(down)) { if (s_incr_idx > 0) s_incr_idx--; return HUD_ACTION_NONE; }
        int incr = s_incr_vals[s_incr_idx];

        // FF/REW with hold-to-repeat.  R2/L2 are ANALOG triggers whose digital bit
        // flickers off for a frame near the press threshold, so we cannot use the
        // raw edge (it re-fires erratically) nor reset on a single 0-frame (it
        // would cancel the hold every flicker).  Instead we keep a sticky
        // direction that survives brief dropouts (SEEK_RELEASE_GRACE_US), and
        // drive BOTH the initial fire and the repeats from it.
        int raw_dir = 0;
        if      (btn_cur.right || btn_cur.r2) raw_dir = +1;
        else if (btn_cur.left  || btn_cur.l2) raw_dir = -1;
        u64 now = timing_get_us();
        if (raw_dir != 0) s_seek_last_active_us = now;

        if (raw_dir != 0 && raw_dir != s_seek_repeat_dir) {
            // Genuine new hold (direction changed, or first press after a real
            // release) — fire once immediately.
            s_seek_repeat_dir    = raw_dir;
            s_seek_hold_start_us = now;
            s_seek_last_fire_us  = now;
            s_seek_delta         = raw_dir * incr;
            return HUD_ACTION_SEEK;
        }
        if (raw_dir == 0 && s_seek_repeat_dir != 0 &&
            now - s_seek_last_active_us >= SEEK_RELEASE_GRACE_US) {
            s_seek_repeat_dir = 0;                        // confirmed release
        }
        if (s_seek_repeat_dir != 0 &&
            now - s_seek_hold_start_us >= SEEK_REPEAT_DELAY_US &&
            now - s_seek_last_fire_us  >= SEEK_REPEAT_INTERVAL_US) {
            s_seek_last_fire_us = now;
            s_seek_delta = s_seek_repeat_dir * incr;
            return HUD_ACTION_SEEK;                       // auto-repeat
        }

        if (BTN_PRESSED(cross)) return HUD_ACTION_TOGGLE_PAUSE;
    }

    return HUD_ACTION_NONE;
}

void hud_draw(u64 elapsed_us, bool paused) {
    if (!s_visible) return;

    int dw = (int)display_width;
    int dh = (int)display_height;

    // ---- Background strip ----
    int strip_y = dh - HUD_STRIP_H;
    if (strip_y < 0) strip_y = 0;
    static int s_dl = 0; bool dl = (s_dl < 12); if (dl) s_dl++;
    if (dl) plog("hud_draw: A pre dim_rect");
#if defined(HUD_DIM_CPU)
    dim_rect_cpu(0, (u32)strip_y, (u32)dw, (u32)(dh - strip_y), 185);
#elif defined(HUD_DIM_GPU_ARRAY)
    draw_dim_rect(0, (u32)strip_y, (u32)dw, (u32)(dh - strip_y), 185);
    if (dl) plog("hud_draw: B pre rsxSync");
    rsxSync();
    if (dl) plog("hud_draw: C post rsxSync");
#else
    draw_dim_rect_inline(0, (u32)strip_y, (u32)dw, (u32)(dh - strip_y), 185);
    if (dl) plog("hud_draw: B pre rsxSync");
    rsxSync();
    if (dl) plog("hud_draw: C post rsxSync");
#endif

    // ---- Row centres ----
    int ctrl_cy  = dh - CTRL_Y_OFF;    // transport / seek bar row
    int audio_cy = dh - AUDIO_Y_OFF;   // audio + CC row (above seek bar)

    // ---- Time strings (needed for seek bar layout) ----
    u32 elapsed_secs = (u32)(elapsed_us / 1000000ULL);
    char elapsed_str[16];
    fmt_time(elapsed_str, sizeof(elapsed_str), elapsed_secs);
    int elapsed_w = ttf_text_width(elapsed_str, ROW_TEXT_PX);

    char rem_str[20] = "";
    int  rem_w = 0;
    if (s_total_secs > 0) {
        u32 rem = (s_total_secs > elapsed_secs) ? s_total_secs - elapsed_secs : 0;
        char t[16]; fmt_time(t, sizeof(t), rem);
        snprintf(rem_str, sizeof(rem_str), "-%s", t);
        rem_w = ttf_text_width(rem_str, ROW_TEXT_PX);
    }

    // ---- Right controls block: audio + CC ----
    int w_music  = (int)MUSIC_ICON_PX;
    int w_alabel = ttf_text_width(s_audio_label, ROW_TEXT_PX);
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
    if (s_total_secs > 0) {
        progress = (float)elapsed_secs / (float)s_total_secs;
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

    // ---- Seek-increment indicator (small, right-aligned, above audio row) ----
    {
        char ibuf[12];
        snprintf(ibuf, sizeof(ibuf), "+/- %s", s_incr_strs[s_incr_idx]);
        int iw = ttf_text_width(ibuf, INCR_PX);
        drawTTF((u32)(dw - RIGHT_PAD - iw), (u32)(dh - INCR_Y_OFF), ibuf, INCR_PX, 0x00888888);
    }

    // ---- Left transport controls ----
    // Iconic glyphs: y = top of glyph = ctrl_cy - half icon height.
    int icon_y = ctrl_cy - (int)(ROW_ICON_PX * 0.5f);
    int lx = LEFT_PAD;

    draw_iconic_glyph((u32)lx, (u32)icon_y, 'L', ROW_ICON_PX, ctrl_color(FOCUS_REW));
    lx += w_rew + CTRL_GAP;

    draw_pp_symbol(lx + PP_W / 2, ctrl_cy, paused, ctrl_color(FOCUS_PP));
    lx += PP_W + CTRL_GAP;

    draw_iconic_glyph((u32)lx, (u32)icon_y, 'R', ROW_ICON_PX, ctrl_color(FOCUS_FF));

    // ---- Audio / CC (above seek bar, right-aligned) ----
    int audio_icon_y = audio_cy - (int)(MUSIC_ICON_PX * 0.5f);
    int audio_text_y = audio_cy - (int)(ROW_TEXT_PX   * 0.5f);
    int cc_text_y    = audio_cy - (int)(CC_TEXT_PX    * 0.5f);
    int rx = rctrl_x0;

    drawIcon((u32)rx, (u32)audio_icon_y, MATERIAL_MUSIC_NOTE, MUSIC_ICON_PX,
             ctrl_color(FOCUS_AUDIO));
    rx += w_music + ICON_LABEL_GAP;
    drawTTF((u32)rx, (u32)audio_text_y, s_audio_label, ROW_TEXT_PX,
            ctrl_color(FOCUS_AUDIO));
    rx += w_alabel + AUDIO_SEP;

    drawTTF((u32)rx, (u32)cc_text_y, "CC", CC_TEXT_PX,
            ctrl_color(FOCUS_CC), /*bold=*/true);
}
