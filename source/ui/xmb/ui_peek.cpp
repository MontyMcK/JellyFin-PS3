// The Triangle quick-peek on the Movies and TV grids.  Motion: render/peek.h
// (host-tested).  This file draws it and owns its input.
//
// Triangle on a poster turns it over: the card spins about its vertical axis
// as it lifts out of the grid and grows into a 4:3 panel, and its back holds
// the synopsis, the meta line and the cast.  It is the extra layer between
// the grid and the full detail page: X from the peek opens detail, Triangle,
// Circle or the d-pad turn the card back and lay it down where it came from.
//
// It piggybacks on what detail already uses without detail's cost: the text
// comes from the facts worker (api/api_facts.cpp -- its own thread and
// buffer, now also asked for Overview and People), so opening a peek never
// blocks a frame; the synopsis fades in when it lands.  No backdrop is
// loaded.  The poster is the grid's own cached thumbnail, drawn from VRAM.
//
// DRAW ORDER.  The grid's titles are text-phase runs, and the peek must cover
// them.  So it draws after the frame's text flush: its GPU work (a dim, the
// card, the panel) is FIFO-ordered behind the flushed runs, then one rsxSync,
// then its own text window.  That extra fence exists only while a peek is on
// screen; the grid pays nothing otherwise.
//
// Only with the spine gate on.

#include <stdio.h>
#include <string.h>

#include "ui_internal.h"
#include "ui_render_internal.h"
#include "ui_card_gpu.h"
#include "ui_wave.h"
#include "ui_text_gpu.h"
#include "ui_art.h"
#include "ui_depth.h"        // depth_last_focus_rect, depth_fit_text
#include "depth.h"           // depth_mix_q
#include "peek.h"
#include "api_facts.h"
#include "thumbnail_cache.h"
#include "timing.h"
#include "plog.h"
#include "slog.h"

static peek_anim s_pk;
static XMBItem   s_item;
static int       s_src_w = 0, s_src_h = 0;   // the grid's thumbnail size
static peek_rect s_from;                     // the card's rect in the grid
static char      s_img_id[64];               // the poster: cache id + kind
static ThumbImg  s_img = THUMB_IMG_PRIMARY;
static void    (*s_open)(void) = NULL;       // what X does from the peek

// Text mode (2026-09-27): the same card turn for a Settings row -- Triangle
// flips the highlighted row over and its back is the setting's description.
static bool      s_text_mode = false;
static char      s_text_title[64];
static char      s_text_body[320];

// Wrapped synopsis, computed once per item/size (never per frame).
#define PK_LINES 14
static char  s_lines[PK_LINES][128];
static int   s_nlines = -1;
static char  s_wrap_key[64];
static float s_wrap_px = 0.0f;
static int   s_wrap_w  = 0;

bool peek_active(void) { return s_pk.phase == PEEK_OPENING || s_pk.phase == PEEK_OPEN; }
bool peek_visible(void) { return s_pk.phase != PEEK_OFF; }

void peek_open_item(const XMBItem *it, int src_w, int src_h, void (*open)(void)) {
    if (!it || !it->id[0]) return;
    s_text_mode = false;
    s_item  = *it;
    s_open  = open;
    s_src_w = src_w;
    s_src_h = src_h;
    s_img   = THUMB_IMG_PRIMARY;
    snprintf(s_img_id, sizeof(s_img_id), "%s", it->id);
    // The stage's own image for the focused card, when it drew one: Home's
    // rows and the column cache at their own sizes (an episode may even show
    // its series' poster), and asking for the item's primary at another size
    // would be a miss and a refetch mid-spin.
    {
        char id[64]; int w = 0, h = 0; ThumbImg img = THUMB_IMG_PRIMARY;
        // Not on Search: its list is no card, so the "last focused card" is
        // another tab's -- a stranger's poster flying in from nowhere.
        if (g_active_tab != XMB_TAB_SEARCH &&
            depth_last_focus_card(id, sizeof(id), &w, &h, &img)) {
            snprintf(s_img_id, sizeof(s_img_id), "%s", id);
            s_src_w = w; s_src_h = h; s_img = img;
        }
    }
    int x = 0, y = 0, w = 0, h = 0;
    if (g_active_tab != XMB_TAB_SEARCH &&
        depth_last_focus_rect(&x, &y, &w, &h) && w > 0 && h > 0) {
        s_from.x = (float)x; s_from.y = (float)y; s_from.w = (float)w; s_from.h = (float)h;
    } else {
        s_from.w = (float)UIS_W(150); s_from.h = (float)UIS_H(225);
        s_from.x = (float)display_width * 0.5f - s_from.w * 0.5f;
        s_from.y = (float)display_height * 0.5f - s_from.h * 0.5f;
    }
    s_nlines = -1;
    facts_request(it->id);                    // lands while the card turns
    peek_open(&s_pk, timing_get_us());
    slog_state("PEEK_OPEN item_id=%s", it->id);
    char msg[160];
    snprintf(msg, sizeof(msg), "peek: open %s type=%s img=%s %dx%d/%d",
             it->id, it->type, s_img_id, s_src_w, s_src_h, (int)s_img);
    plog(msg);
}

static void peek_close_now(void) { peek_close(&s_pk, timing_get_us()); }

void peek_open_text(const char *title, const char *body, int x, int y, int w, int h) {
    s_text_mode = true;
    memset(&s_item, 0, sizeof s_item);
    snprintf(s_text_title, sizeof s_text_title, "%s", title ? title : "");
    snprintf(s_text_body, sizeof s_text_body, "%s", body ? body : "");
    // The wrap cache is keyed by s_item.id: make it the title.
    snprintf(s_item.id, sizeof s_item.id, "text:%.40s", s_text_title);
    s_open = NULL;
    s_src_w = s_src_h = 0;
    s_img_id[0] = 0;
    s_from.x = (float)x; s_from.y = (float)y; s_from.w = (float)w; s_from.h = (float)h;
    s_nlines = -1;
    peek_open(&s_pk, timing_get_us());
}

// Input while a peek is up (the grid gets none).  Returns true when the
// peek consumed this frame.
bool peek_input(void) {
    if (!peek_active()) return false;
    if (BTN_PRESSED(triangle) || BTN_PRESSED(circle) ||
        BTN_PRESSED(up) || BTN_PRESSED(down) || BTN_PRESSED(left) || BTN_PRESSED(right) ||
        BTN_PRESSED(l1) || BTN_PRESSED(r1)) {
        peek_close_now();
        return true;
    }
    if (s_text_mode) {
        if (BTN_PRESSED(cross)) peek_close_now();   // nothing deeper to open
        return true;
    }
    if (BTN_PRESSED(cross)) {
        // The next layer: full detail (or a series' seasons).  The peek is
        // dropped at once -- detail flies its own poster out of the grid.
        s_pk.phase = PEEK_OFF;
        XMBItem copy = s_item;
        if (s_open)                                s_open();
        else if (strcmp(copy.type, "Series") == 0) xmb_open_series(&copy);
        else                                       xmb_show_item_info(&copy);
        init_btns();
        return true;
    }
    return true;
}

static inline u8 a8(float a) {
    a = a < 0.0f ? 0.0f : (a > 1.0f ? 1.0f : a);
    return (u8)(a * 255.0f + 0.5f);
}

// The panel in screen px: 4:3, 450 authored tall, centred on the canvas.
static peek_rect peek_panel_px(void) {
    const float h  = (float)UIS_H(450);
    const float cx = (float)(XMB_OX + UIS_W(640));
    const float cy = (float)(XMB_OY + UIS_H(372));
    return peek_panel(cx, cy, h);
}

static void wrap_synopsis(const char *text, float px, int max_w) {
    if (s_nlines >= 0 && s_wrap_px == px && s_wrap_w == max_w &&
        strcmp(s_wrap_key, s_item.id) == 0)
        return;
    snprintf(s_wrap_key, sizeof s_wrap_key, "%s", s_item.id);
    s_wrap_px = px; s_wrap_w = max_w;
    s_nlines = 0;
    const char *p = text;
    while (*p && s_nlines < PK_LINES) {
        while (*p == ' ' || *p == '\n' || *p == '\r') p++;
        if (!*p) break;
        char line[128]; int len = 0, last_sp = -1;
        const char *q = p;
        while (*q && *q != '\n' && len < (int)sizeof line - 1) {
            line[len] = *q; line[len + 1] = 0;
            if (ttf_text_width(line, px) > max_w) break;
            if (*q == ' ') last_sp = len;
            len++; q++;
        }
        if (*q && *q != '\n' && last_sp > 0 && len < (int)sizeof line - 1) {
            len = last_sp;                       // break at the last space
            q = p + last_sp;
        }
        // Never end a line mid-character.
        while (len > 0 && (((unsigned char)p[len] & 0xC0) == 0x80)) { len--; q = p + len; }
        if (len <= 0) break;
        memcpy(s_lines[s_nlines], p, (size_t)len);
        s_lines[s_nlines][len] = 0;
        s_nlines++;
        p = q;
    }
    if (*p && s_nlines == PK_LINES) {
        // More than fits: end the last line with an ellipsis.
        char *l = s_lines[PK_LINES - 1];
        int len = (int)strlen(l);
        while (len > 0 && ttf_text_width(l, px) + ttf_text_width("...", px) > max_w) l[--len] = 0;
        while (len > 0 && l[len - 1] == ' ') l[--len] = 0;
        snprintf(l + len, 128 - len, "...");
    }
}

// After the frame's text flush (ui_xmb.cpp).  GPU, fence, text.
void peek_draw_over(void) {
    if (!g_spine_on || s_pk.phase == PEEK_OFF) return;
    const unsigned long long now = timing_get_us();
    peek_tick(&s_pk, now);
    if (s_pk.phase == PEEK_OFF) return;
    if (peek_active()) facts_request(s_item.id);

    if (s_text_mode) {
        // A wide, low panel: a title and a couple of lines, not a synopsis.
        peek_rect to;
        to.w = (float)UIS_W(760); to.h = (float)UIS_H(210);
        to.x = (float)(XMB_OX + UIS_W(640)) - to.w * 0.5f;
        to.y = (float)(XMB_OY + UIS_H(372)) - to.h * 0.5f;
        const peek_frame f = peek_eval(peek_progress(&s_pk, now), s_from, to);
        const int W = (int)display_width, H = (int)display_height;
        const int rx = (int)(f.r.x + 0.5f), ry = (int)(f.r.y + 0.5f);
        const int rw = (int)(f.r.w + 0.5f), rh = (int)(f.r.h + 0.5f);
        ui_rect_gpu_draw(0, 0, W, H, 0x00000000, a8(0.66f * f.lift));
        wave_draw_glow_gpu(rx + rw / 2, ry + rh / 2, (int)(f.r.w * 0.5f + UIS_W(90) * f.lift),
                           rh / 2 + UIS_H(70), (u8)(XMB_ACCENT >> 16), (u8)(XMB_ACCENT >> 8),
                           (u8)XMB_ACCENT, a8(0.22f * f.lift));
        if (rw >= 1 && rh >= 1) {
            if (!f.back) {
                // The front: the Settings row itself.
                ui_rect_gpu_draw(rx, ry, rw, rh, XMB_PANEL_HI, 255);
            } else {
                const int r = UIS_H(10) < rw / 2 ? UIS_H(10) : rw / 2;
                wave_draw_rrect_outline_gpu(rx, ry, rw, rh, r, 1,
                                            art_mix(XMB_HAIRLINE, XMB_ACCENT, 0.55f), 255,
                                            0x000B0D1A, 0x000B0D1A, 244);
            }
        }
        rsxSync();
        ui_text_gpu_begin();
        if (f.back && f.content_a > 0.02f) {
            const float t = f.content_a;
            const u32 bg = 0x000B0D1A;
            const int pad = UIS_W(32);
            const int tx = (int)to.x + pad, tw = (int)to.w - 2 * pad;
            int ty = (int)to.y + UIS_H(26);
            xmb_draw_eyebrow(tx, ty, "Settings", depth_mix_q(bg, XMB_ACCENT_ALT, t));
            ty += UIS_H(22);
            {
                char tl[96];
                const float tpx = UIS_TF(24.0f);
                depth_fit_text(tl, sizeof tl, s_text_title, tpx, UI_FACE_DISPLAY, tw);
                drawTTF_face((u32)tx, (u32)ty, tl, tpx, depth_mix_q(bg, XMB_TEXT, t), UI_FACE_DISPLAY);
                ty += UIS_H(42);
            }
            const float spx = UIS_TF(15.0f);
            const int   lh  = UIS_H(24);
            wrap_synopsis(s_text_body, spx, tw);
            const int max_lines = ((int)(to.y + to.h) - UIS_H(18) - ty) / lh;
            for (int i = 0; i < s_nlines && i < max_lines; i++)
                drawTTF((u32)tx, (u32)(ty + i * lh), s_lines[i], spx,
                        depth_mix_q(bg, XMB_TEXT, t * 0.92f));
        }
        if (peek_active() && f.lift > 0.5f) {
            Hint h[1];
            h[0].glyph = 'T'; h[0].label = "Close";
            draw_hints_bar(h, 1);
        }
        ui_text_gpu_flush();
        return;
    }

    const peek_rect to = peek_panel_px();
    const peek_frame f = peek_eval(peek_progress(&s_pk, now), s_from, to);
    const int W = (int)display_width, H = (int)display_height;
    const int rx = (int)(f.r.x + 0.5f), ry = (int)(f.r.y + 0.5f);
    const int rw = (int)(f.r.w + 0.5f), rh = (int)(f.r.h + 0.5f);

    if (s_src_w > 0 && s_src_h > 0) thumb_request(s_img_id, s_src_w, s_src_h, s_img);
    const art_palette pal = ui_art_palette(s_item.id,
        s_src_w > 0 ? thumb_get(s_img_id, s_src_w, s_src_h, s_img) : NULL);

    // ---- GPU: the room dims, the card turns ------------------------------
    ui_rect_gpu_draw(0, 0, W, H, 0x00000000, a8(0.66f * f.lift));
    {
        const u32 g = pal.glow;
        wave_draw_glow_gpu(rx + rw / 2, ry + rh / 2, (int)(f.r.w * 0.5f + UIS_W(90) * f.lift),
                           rh / 2 + UIS_H(70), (u8)(g >> 16), (u8)(g >> 8), (u8)g,
                           a8(0.30f * f.lift));
    }
    u32 off = 0, pitch = 0;
    const bool tex = s_src_w > 0 && thumb_gpu_texture(s_img_id, s_src_w, s_src_h, &off, &pitch, s_img);
    if (rw >= 1 && rh >= 1) {
        if (!f.back) {
            if (tex) ui_card_gpu_draw_ex(off, (u32)s_src_w, (u32)s_src_h, pitch,
                                         rx, ry, rw, rh, 0.0f, 1.0f, 255);
            else     ui_rect_gpu_draw(rx, ry, rw, rh, XMB_THUMB_DIM, 255);
            ui_card_gpu_end();
        } else {
            // The back: dark glass, a hairline in the artwork's accent.
            const int r = UIS_H(10) < rw / 2 ? UIS_H(10) : rw / 2;
            wave_draw_rrect_outline_gpu(rx, ry, rw, rh, r, 1,
                                        art_mix(XMB_HAIRLINE, pal.accent, 0.55f), 255,
                                        0x000B0D1A, art_mix(0x000B0D1A, pal.deep, 0.6f), 244);
        }
    }
    // The poster again, small, on the back once it has turned.
    const int pad = UIS_W(28);
    const int pw  = UIS_W(150), ph = UIS_H(225);
    const int px_ = (int)to.x + pad, py_ = (int)to.y + pad;
    if (f.back && f.content_a > 0.0f && tex) {
        ui_card_gpu_draw_ex(off, (u32)s_src_w, (u32)s_src_h, pitch, px_, py_, pw, ph,
                            0.0f, 1.0f, a8(f.content_a));
        ui_card_gpu_end();
    }

    // ---- the fence, then text ---------------------------------------------
    rsxSync();
    ui_text_gpu_begin();
    if (f.back && f.content_a > 0.02f) {
        const float t = f.content_a;
        const u32 bg = 0x000B0D1A;
        ItemFacts fx;
        const bool have = facts_get(s_item.id, &fx, NULL);

        // Left column under the poster: the cast.
        int cy = py_ + ph + UIS_H(18);
        if (have && fx.n_cast > 0) {
            xmb_draw_eyebrow(px_, cy, "Cast", depth_mix_q(bg, XMB_ACCENT_ALT, t));
            cy += UIS_H(20);
            for (int i = 0; i < fx.n_cast; i++) {
                char nm[64];
                depth_fit_text(nm, sizeof nm, fx.cast[i], UIS_TF(12.5f), UI_FACE_TAB_REG, pw);
                drawTTF_face((u32)px_, (u32)cy, nm, UIS_TF(12.5f),
                             depth_mix_q(bg, XMB_TEXT_DIM, t), UI_FACE_TAB_REG);
                cy += UIS_H(19);
            }
        }

        // Right column: title, meta, synopsis.
        const int tx = px_ + pw + UIS_W(26);
        const int tw = (int)(to.x + to.w) - pad - tx;
        int ty = py_;
        {
            char tl[160];
            const float tpx = UIS_TF(24.0f);
            depth_fit_text(tl, sizeof tl, s_item.name, tpx, UI_FACE_DISPLAY, tw);
            drawTTF_face((u32)tx, (u32)ty, tl, tpx, depth_mix_q(bg, XMB_TEXT, t), UI_FACE_DISPLAY);
            ty += UIS_H(36);
        }
        {
            char meta[160] = "";
            const char *parts[4] = { s_item.year_str, have ? fx.rating : "",
                                     s_item.duration_str, s_item.genre };
            int n = 0;
            for (int i = 0; i < 4; i++) {
                if (!parts[i] || !parts[i][0]) continue;
                n += snprintf(meta + n, sizeof meta - (size_t)n, "%s%s",
                              n ? "  \xC2\xB7  " : "", parts[i]);
                if (n >= (int)sizeof meta) break;
            }
            if (meta[0]) {
                char fit[160];
                depth_fit_text(fit, sizeof fit, meta, UIS_TF(13.0f), UI_FACE_REGULAR, tw);
                drawTTF((u32)tx, (u32)ty, fit, UIS_TF(13.0f), depth_mix_q(bg, XMB_TEXT_DIM, t));
            }
            ty += UIS_H(30);
        }
        if (have && fx.audio[0]) {
            char tech[96];
            snprintf(tech, sizeof tech, "%s%s%s", fx.video, fx.video[0] ? "  \xC2\xB7  " : "", fx.audio);
            char fit[96];
            depth_fit_text(fit, sizeof fit, tech, UIS_TF(10.5f), UI_FACE_SPEC, tw);
            drawTTF_face((u32)tx, (u32)ty, fit, UIS_TF(10.5f),
                         depth_mix_q(bg, fx.lossless ? XMB_ACCENT_ALT : XMB_TEXT_FAINT, t),
                         UI_FACE_SPEC);
            ty += UIS_H(26);
        }
        const float spx = UIS_TF(13.5f);
        const int   lh  = UIS_H(20);
        if (have && fx.overview[0]) {
            wrap_synopsis(fx.overview, spx, tw);
            const int max_lines = ((int)(to.y + to.h) - pad - ty) / lh;
            for (int i = 0; i < s_nlines && i < max_lines; i++)
                drawTTF((u32)tx, (u32)(ty + i * lh), s_lines[i], spx,
                        depth_mix_q(bg, XMB_TEXT, t * 0.92f));
        } else {
            drawTTF((u32)tx, (u32)ty, have ? "No synopsis." : "Loading...", spx,
                    depth_mix_q(bg, XMB_TEXT_FAINT, t));
        }
    }
    if (peek_active() && f.lift > 0.5f) {
        Hint h[2];
        h[0].glyph = 'X'; h[0].label = "Details";
        h[1].glyph = 'T'; h[1].label = "Close";
        draw_hints_bar(h, 2);
    }
    ui_text_gpu_flush();
}
