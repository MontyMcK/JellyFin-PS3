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

#define HUD_STRIP_H          64
#define CTRL_Y_OFF  (HUD_STRIP_H / 2)      // control row centred in the strip
#define TRACK_H               4
#define SCRUB_R               6
#define LEFT_PAD             50
#define RIGHT_PAD            50
#define CTRL_GAP             10            // gap between transport control glyphs
#define TIME_GAP             12            // gap between time label edge and seek bar
#define RCTRL_GAP            12            // gap between remaining time and right controls

#define ROW_ICON_PX          36.0f         // L2/R2 iconic glyph size
#define ROW_TEXT_PX          18.0f         // time + audio label text size
#define MUSIC_ICON_PX        24.0f
#define CC_TEXT_PX           20.0f
#define ICON_LABEL_GAP        6
#define AUDIO_SEP            16            // horizontal gap between audio btn and CC btn

#define PP_H                 22            // play/pause primitive bounding height (px)
#define PP_W                 22            // play/pause primitive bounding width  (px)

// Title overlay (top-left while paused)
#define TITLE_PX           24.0f           // a touch under the subtitle size
#define TITLE_TOP_PAD        34

// Popup menu (track selection)
#define MENU_MAX              9            // JF_MAX_STREAMS subs + "Off"
#define MENU_TITLE_PX      22.0f
#define MENU_TITLE_H         38            // title row height incl. padding
#define MENU_ROW_H           30
#define MENU_PAD             16            // popup inner padding
#define MENU_DOT_COL         22            // width of the current-entry dot column

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
static char s_title[128];
static bool s_visible    = false;
static u64  s_show_us    = 0;
static int  s_seek_delta = 0;
static int  s_focus      = -1;   // -1=none, 0..FOCUS_COUNT-1=focused slot
static int  s_incr_idx   = 0;    // 0=10s  1=30s  2=5min
static bool s_cc_active  = false; // subtitles on -> underline the CC button

// Popup menu state.  Items are caller-owned pointers (track labels live in
// the player's static JFTracks, so they outlive the menu).
static bool        s_menu_visible = false;
static char        s_menu_title[24];
static const char *s_menu_items[MENU_MAX];
static int         s_menu_n      = 0;
static int         s_menu_sel    = 0;   // cursor row
static int         s_menu_cur    = 0;   // active entry (accent dot)
static int         s_menu_choice = -1;  // last X-selected row


// RSX resources for the GPU-drawn dim quad
static u8  *s_hud_vbuf    = NULL;
static u32  s_hud_vbuf_off = 0;
static u32 *s_hud_fp_buf  = NULL;
static u32  s_hud_fp_off  = 0;

static const int  s_incr_vals[3]  = { 10, 30, 300 };

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
             (audio_label && audio_label[0]) ? audio_label : "AUDIO");
    s_visible      = false;
    s_show_us      = 0;
    s_seek_delta   = 0;
    s_focus        = -1;
    s_incr_idx     = 0;
    s_cc_active    = false;
    s_menu_visible = false;
    s_menu_n       = 0;
    s_menu_choice  = -1;
    s_title[0]     = '\0';
    hud_gpu_init();
}

void hud_shutdown(void) {
    s_visible = false;
    hud_gpu_shutdown();
}
bool hud_is_visible(void) { return s_visible; }
int  hud_seek_delta(void) { return s_seek_delta; }

void hud_set_audio_label(const char *label) {
    snprintf(s_audio_label, sizeof(s_audio_label), "%s",
             (label && label[0]) ? label : "AUDIO");
}

void hud_set_cc_active(bool active) { s_cc_active = active; }

void hud_set_title(const char *title) {
    snprintf(s_title, sizeof(s_title), "%s", title ? title : "");
}

void hud_open_menu(const char *title, const char *const *items,
                   int n_items, int current) {
    if (n_items > MENU_MAX) n_items = MENU_MAX;
    snprintf(s_menu_title, sizeof(s_menu_title), "%s", title ? title : "");
    for (int i = 0; i < n_items; i++) s_menu_items[i] = items[i];
    s_menu_n   = n_items;
    s_menu_cur = (current >= 0 && current < n_items) ? current : 0;
    s_menu_sel = s_menu_cur;
    s_menu_visible = (n_items > 0);
    show_hud();
}

int hud_menu_choice(void) { return s_menu_choice; }

HudAction hud_handle_input(bool l2_pressed, bool r2_pressed, bool paused) {
    s_seek_delta = 0;

    // Any button activity wakes the HUD.
    bool was_hidden = !s_visible;
    if (btn_cur.cross || btn_cur.circle || btn_cur.square || btn_cur.triangle ||
        btn_cur.l1 || btn_cur.r1 || btn_cur.l2 || btn_cur.r2 ||
        l2_pressed || r2_pressed ||
        btn_cur.up || btn_cur.down || btn_cur.left || btn_cur.right) {
        show_hud();
        // First button while the bar was hidden just reveals it, with the
        // play/pause control focused by default.
        if (was_hidden) s_focus = FOCUS_PP;
    }

    // Auto-hide after timeout — only when playing; stay visible indefinitely while paused.
    if (s_visible && !paused && (timing_get_us() - s_show_us) >= HUD_SHOW_US) {
        s_visible      = false;
        s_focus        = -1;
        s_menu_visible = false;
        return HUD_ACTION_NONE;
    }
    if (!s_visible) return HUD_ACTION_NONE;

    // While a popup menu is open it owns the input: up/down move the cursor,
    // X picks the entry, circle closes without picking.
    if (s_menu_visible) {
        if (BTN_PRESSED(up)) {
            if (s_menu_sel > 0) s_menu_sel--;
        } else if (BTN_PRESSED(down)) {
            if (s_menu_sel < s_menu_n - 1) s_menu_sel++;
        } else if (BTN_PRESSED(cross)) {
            s_menu_visible = false;
            s_menu_choice  = s_menu_sel;
            return HUD_ACTION_MENU_SELECT;
        } else if (BTN_PRESSED(circle)) {
            s_menu_visible = false;
        }
        return HUD_ACTION_NONE;
    }

    // D-pad left/right move the focus cursor across the control row, in screen
    // order: REW · PLAY/PAUSE · FF · AUDIO · CC.  This is how you reach the
    // AUDIO and CC controls.  R2/L2 (handled in the main loop) still scrub.
    // The reveal press above is swallowed so the bar only navigates once it's
    // already showing.
    if (!was_hidden) {
        if (BTN_PRESSED(left)) {
            if (s_focus < 0)      s_focus = FOCUS_PP;
            else if (s_focus > 0) s_focus--;
            return HUD_ACTION_NONE;
        }
        if (BTN_PRESSED(right)) {
            if (s_focus < 0)                   s_focus = FOCUS_PP;
            else if (s_focus < FOCUS_COUNT - 1) s_focus++;
            return HUD_ACTION_NONE;
        }
    }

    // X (cross) activates the focused control.
    if (BTN_PRESSED(cross)) {
        switch (s_focus) {
        case FOCUS_REW:   s_seek_delta = -s_incr_vals[s_incr_idx]; return HUD_ACTION_SEEK;
        case FOCUS_FF:    s_seek_delta = +s_incr_vals[s_incr_idx]; return HUD_ACTION_SEEK;
        case FOCUS_AUDIO: return HUD_ACTION_AUDIO_TRACK;
        case FOCUS_CC:    return HUD_ACTION_SUBTITLE;
        case FOCUS_PP:
        default:          return HUD_ACTION_TOGGLE_PAUSE;
        }
    }

    return HUD_ACTION_NONE;
}

void hud_draw(u64 elapsed_us, bool paused) {
    if (!s_visible) return;

    int dw = (int)display_width;
    int dh = (int)display_height;

    // ---- Title (top-left, only while paused) ----
    if (paused && s_title[0]) {
        int tw = ttf_text_width(s_title, TITLE_PX);
        if (tw > dw - 2 * LEFT_PAD) tw = dw - 2 * LEFT_PAD;
#if defined(HUD_DIM_CPU)
        dim_rect_cpu((u32)(LEFT_PAD - 12), (u32)(TITLE_TOP_PAD - 8),
                     (u32)(tw + 24), (u32)((int)TITLE_PX + 16), 185);
#elif defined(HUD_DIM_GPU_ARRAY)
        draw_dim_rect((u32)(LEFT_PAD - 12), (u32)(TITLE_TOP_PAD - 8),
                      (u32)(tw + 24), (u32)((int)TITLE_PX + 16), 185);
        rsxSync();
#else
        draw_dim_rect_inline((u32)(LEFT_PAD - 12), (u32)(TITLE_TOP_PAD - 8),
                             (u32)(tw + 24), (u32)((int)TITLE_PX + 16), 185);
        rsxSync();
#endif
        drawTTF((u32)LEFT_PAD, (u32)TITLE_TOP_PAD, s_title, TITLE_PX,
                HUD_FOCUSED);
    }

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
    drawTTF((u32)rx, (u32)audio_text_y, s_audio_label, ROW_TEXT_PX,
            ctrl_color(FOCUS_AUDIO));
    rx += w_alabel + AUDIO_SEP;

    drawTTF((u32)rx, (u32)cc_text_y, "CC", CC_TEXT_PX,
            ctrl_color(FOCUS_CC), /*bold=*/true);
    // Subtitles on: accent underline beneath the CC button.
    if (s_cc_active)
        drawRect((u32)rx, (u32)(cc_text_y + (int)CC_TEXT_PX + 3),
                 (u32)w_cc, 2, HUD_ACCENT);

    // ---- Popup menu (track selection), bottom-right above the strip ----
    if (s_menu_visible && s_menu_n > 0) {
        int max_w = ttf_text_width(s_menu_title, MENU_TITLE_PX);
        for (int i = 0; i < s_menu_n; i++) {
            int tw = MENU_DOT_COL + ttf_text_width(s_menu_items[i], ROW_TEXT_PX);
            if (tw > max_w) max_w = tw;
        }
        int mw = max_w + 2 * MENU_PAD;
        if (mw > dw - 2 * RIGHT_PAD) mw = dw - 2 * RIGHT_PAD;
        int mh  = MENU_TITLE_H + s_menu_n * MENU_ROW_H + MENU_PAD;
        int mx1 = dw - RIGHT_PAD;
        int mx0 = mx1 - mw;
        int my1 = dh - HUD_STRIP_H - 6;
        int my0 = my1 - mh;
        if (my0 < 6) my0 = 6;

#if defined(HUD_DIM_CPU)
        dim_rect_cpu((u32)mx0, (u32)my0, (u32)mw, (u32)(my1 - my0), 225);
#elif defined(HUD_DIM_GPU_ARRAY)
        draw_dim_rect((u32)mx0, (u32)my0, (u32)mw, (u32)(my1 - my0), 225);
        rsxSync();
#else
        draw_dim_rect_inline((u32)mx0, (u32)my0, (u32)mw, (u32)(my1 - my0), 225);
        rsxSync();
#endif

        drawTTF((u32)(mx0 + MENU_PAD),
                (u32)(my0 + (MENU_TITLE_H - (int)MENU_TITLE_PX) / 2),
                s_menu_title, MENU_TITLE_PX, HUD_ACCENT, /*bold=*/true);

        for (int i = 0; i < s_menu_n; i++) {
            int row_y0 = my0 + MENU_TITLE_H + i * MENU_ROW_H;
            int row_cy = row_y0 + MENU_ROW_H / 2;
            if (i == s_menu_sel)
                drawRect((u32)(mx0 + 4), (u32)row_y0,
                         (u32)(mw - 8), MENU_ROW_H, HUD_ACCENT_DIM);
            if (i == s_menu_cur)
                draw_circle(mx0 + MENU_PAD + 5, row_cy, 4, HUD_ACCENT);
            drawTTF((u32)(mx0 + MENU_PAD + MENU_DOT_COL),
                    (u32)(row_cy - (int)(ROW_TEXT_PX * 0.5f)),
                    s_menu_items[i], ROW_TEXT_PX,
                    (i == s_menu_sel) ? HUD_FOCUSED : 0x00AAAAAAUL);
        }
    }
}
