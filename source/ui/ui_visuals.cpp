#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>

#include <rsx/rsx.h>

#include "ui_visuals.h"
#include "jellyfin_api.h"
#include "bitmap.h"
#include "thumbnail_cache.h"
#include "font8x8.xpm"
#include "timing.h"
#include "opensans_regular.h"
#include "opensans_bold.h"
#include "material_icons.h"
#include "iconic_psx.h"
#include "plog.h"

#define STB_TRUETYPE_IMPLEMENTATION
#ifdef __GNUC__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wsign-compare"
#endif
#include "stb_truetype.h"
#ifdef __GNUC__
#pragma GCC diagnostic pop
#endif

// -------------------------------------------------------
// Font state
// -------------------------------------------------------

static Bitmap fontBitmap;

static stbtt_fontinfo  s_font;
static unsigned char  *s_font_buf = NULL;
static bool            s_ttf_ok   = false;
static stbtt_fontinfo  s_font_bold;
static bool            s_ttf_bold_ok = false;
static stbtt_fontinfo  s_icons;
static bool            s_icons_ok    = false;
static stbtt_fontinfo  s_iconic;
static bool            s_iconic_ok   = false;

// Gamma LUTs for correct anti-aliasing.  Blending coverage in linear light
// (instead of straight 8-bit sRGB) stops the soft glyph edges from going muddy
// grey/dark — which otherwise reads as a faint black outline, especially for
// white text on the dark on-screen-keyboard keys.
static u8   s_g2l[256];   // sRGB byte -> linear (gamma 2.0)
static u8   s_l2g[256];   // linear    -> sRGB byte
static bool s_gamma_ok = false;

static void gamma_init(void) {
    for (int i = 0; i < 256; i++) {
        s_g2l[i] = (u8)((i * i) / 255);
        s_l2g[i] = (u8)(sqrtf((float)i / 255.0f) * 255.0f + 0.5f);
    }
    s_gamma_ok = true;
}

// Composite one foreground channel over a background channel at coverage a.
static inline u8 aa_blend(u8 a, u8 fg, u8 bg) {
    return s_l2g[(a * s_g2l[fg] + (255 - a) * s_g2l[bg]) / 255];
}

// -------------------------------------------------------
// Tab icon codepoints (Material Icons)
// -------------------------------------------------------

static const int TAB_CODEPOINTS[XMB_TAB_COUNT] = {
    0xE8B6,  // XMB_TAB_SEARCH      (search/magnifier)
    0xE54D,  // XMB_TAB_MOVIES      (movie/film)
    0xE333,  // XMB_TAB_TV          (TV)
    0xE3A1,  // XMB_TAB_MUSIC       (music note)
    0xE8EF,  // XMB_TAB_COLLECTIONS (collections/four squares)
    0xE8B8,  // XMB_TAB_SETTINGS    (settings/gear)
};

// -------------------------------------------------------
// RSX drawing
// -------------------------------------------------------

void clearScreen(u32 color) {
    rsxSetClearColor(context, color);
    rsxSetClearDepthStencil(context, 0xffff);
    rsxClearSurface(context,
        GCM_CLEAR_R|GCM_CLEAR_G|GCM_CLEAR_B|GCM_CLEAR_A|GCM_CLEAR_S|GCM_CLEAR_Z);
}

void drawChar(u32 x, u32 y, char c) {
    if (c < 32 || c > 126) c = '?';
    int idx = c - 32;
    int srcX = (idx % 16) * 8;
    int srcY = (idx / 16) * 8;

    gcmTransferScale   scale;
    gcmTransferSurface surface;

    scale.conversion = GCM_TRANSFER_CONVERSION_TRUNCATE;
    scale.format     = GCM_TRANSFER_SCALE_FORMAT_A8R8G8B8;
    scale.origin     = GCM_TRANSFER_ORIGIN_CORNER;
    scale.operation  = GCM_TRANSFER_OPERATION_SRCCOPY_AND;
    scale.interp     = GCM_TRANSFER_INTERPOLATOR_NEAREST;
    scale.clipX=0; scale.clipY=0;
    scale.clipW=display_width; scale.clipH=display_height;
    scale.outX=x; scale.outY=y;
    scale.outW=CHAR_SIZE; scale.outH=CHAR_SIZE;
    scale.ratioX=rsxGetFixedSint32(1.f/FONT_SCALE);
    scale.ratioY=rsxGetFixedSint32(1.f/FONT_SCALE);
    scale.inX=rsxGetFixedUint16(srcX);
    scale.inY=rsxGetFixedUint16(srcY);
    scale.inW=fontBitmap.width; scale.inH=fontBitmap.height;
    scale.offset=fontBitmap.offset;
    scale.pitch=sizeof(u32)*fontBitmap.width;

    surface.format=GCM_TRANSFER_SURFACE_FORMAT_A8R8G8B8;
    surface.pitch=color_pitch;
    surface.offset=color_offset[curr_fb];

    rsxSetTransferScaleMode(context, GCM_TRANSFER_LOCAL_TO_LOCAL, GCM_TRANSFER_SURFACE);
    rsxSetTransferScaleSurface(context, &scale, &surface);
}

void drawText(u32 x, u32 y, const char *text) {
    u32 cx = x;
    while (*text) {
        if (*text == '\n') { cx = x; y += LINE_HEIGHT; }
        else { drawChar(cx, y, *text); cx += CHAR_SIZE; }
        text++;
    }
}

void drawTextf(u32 x, u32 y, const char *fmt, ...) {
    char buf[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    drawText(x, y, buf);
}

void drawTextScaled(u32 x, u32 y, const char *text, int px) {
    if (px <= 0) return;
    u32 cx = x;
    while (*text) {
        if (*text == '\n') { cx = x; y += (u32)px; }
        else {
            char c = *text;
            if (c < 32 || c > 126) c = '?';
            int idx = c - 32;
            int srcX = (idx % 16) * 8;
            int srcY = (idx / 16) * 8;

            gcmTransferScale   scale;
            gcmTransferSurface surface;

            scale.conversion = GCM_TRANSFER_CONVERSION_TRUNCATE;
            scale.format     = GCM_TRANSFER_SCALE_FORMAT_A8R8G8B8;
            scale.origin     = GCM_TRANSFER_ORIGIN_CORNER;
            scale.operation  = GCM_TRANSFER_OPERATION_SRCCOPY_AND;
            scale.interp     = GCM_TRANSFER_INTERPOLATOR_NEAREST;
            scale.clipX=0; scale.clipY=0;
            scale.clipW=display_width; scale.clipH=display_height;
            scale.outX=cx; scale.outY=y;
            scale.outW=(u32)px; scale.outH=(u32)px;
            scale.ratioX=rsxGetFixedSint32(8.0f / px);
            scale.ratioY=rsxGetFixedSint32(8.0f / px);
            scale.inX=rsxGetFixedUint16(srcX);
            scale.inY=rsxGetFixedUint16(srcY);
            scale.inW=fontBitmap.width; scale.inH=fontBitmap.height;
            scale.offset=fontBitmap.offset;
            scale.pitch=sizeof(u32)*fontBitmap.width;

            surface.format=GCM_TRANSFER_SURFACE_FORMAT_A8R8G8B8;
            surface.pitch=color_pitch;
            surface.offset=color_offset[curr_fb];

            rsxSetTransferScaleMode(context, GCM_TRANSFER_LOCAL_TO_LOCAL, GCM_TRANSFER_SURFACE);
            rsxSetTransferScaleSurface(context, &scale, &surface);

            cx += (u32)px;
        }
        text++;
    }
}

void drawHeader(void) {
    clearScreen(0x0d0d1a);
    rsxSync();
    drawTTF(40, 30, "JELLYFIN PS3", 16, 0x00FFFFFF);
}

void decode_unicode_escapes(char *str) {
    char *src = str, *dst = str;
    while (*src) {
        if (src[0]=='\\' && src[1]=='u' &&
            isxdigit(src[2]) && isxdigit(src[3]) &&
            isxdigit(src[4]) && isxdigit(src[5])) {
            char hex[5] = {src[2],src[3],src[4],src[5],0};
            int code = (int)strtol(hex, NULL, 16);
            *dst++ = (code >= 32 && code < 127) ? (char)code : '?';
            src += 6;
        } else { *dst++ = *src++; }
    }
    *dst = '\0';
}

// -------------------------------------------------------
// CPU drawing (call only after rsxSync, before RSX commands)
// color: 0x00RRGGBB  (X8R8G8B8, X byte unused by display)
// -------------------------------------------------------

void drawRect(u32 x, u32 y, u32 w, u32 h, u32 color) {
    if (x >= display_width || y >= display_height || w == 0 || h == 0) return;
    u32 x2 = (x + w > display_width)  ? display_width  : x + w;
    u32 y2 = (y + h > display_height) ? display_height : y + h;
    for (u32 r = y; r < y2; r++) {
        u32 *p = color_buffer[curr_fb] + r * display_width + x;
        u32  n = x2 - x;
        for (u32 c = 0; c < n; c++) p[c] = color;
    }
}

void cpuClearFb(u32 color) {
    u32 *fb = color_buffer[curr_fb];
    u32  n  = display_width * display_height;
    for (u32 i = 0; i < n; i++) fb[i] = color;
}

// -------------------------------------------------------
// TTF text rendering (CPU write — call after rsxSync, before flip)
// color: 0x00RRGGBB.  Falls back to drawTextScaled if font not loaded.
// -------------------------------------------------------

int ttf_text_width(const char *text, float px) {
    if (!s_ttf_ok) return (int)(strlen(text) * px);
    float scale = stbtt_ScaleForPixelHeight(&s_font, px);
    float xf = 0.0f;
    int prev_cp = 0;
    while (*text) {
        int cp = (unsigned char)*text;
        if (prev_cp) xf += stbtt_GetCodepointKernAdvance(&s_font, prev_cp, cp) * scale;
        int advance;
        stbtt_GetCodepointHMetrics(&s_font, cp, &advance, NULL);
        xf += (float)advance * scale;
        prev_cp = cp;
        text++;
    }
    return (int)xf;
}

void drawTTF(u32 x, u32 y, const char *text, float px, u32 color, bool bold) {
    if (!s_ttf_ok) {
        drawTextScaled(x, y, text, (int)px);
        return;
    }

    stbtt_fontinfo *fi = (bold && s_ttf_bold_ok) ? &s_font_bold : &s_font;

    float scale = stbtt_ScaleForPixelHeight(fi, px);
    int ascent;
    stbtt_GetFontVMetrics(fi, &ascent, NULL, NULL);
    int baseline = (int)((float)ascent * scale);

    u32 r_fg = (color >> 16) & 0xFF;
    u32 g_fg = (color >>  8) & 0xFF;
    u32 b_fg =  color        & 0xFF;

    float xf     = (float)x;
    int   prev_cp = 0;

    while (*text) {
        int cp = (unsigned char)*text;

        if (prev_cp)
            xf += stbtt_GetCodepointKernAdvance(fi, prev_cp, cp) * scale;

        int w, h, xoff, yoff;
        unsigned char *bm = stbtt_GetCodepointBitmap(
            fi, scale, scale, cp, &w, &h, &xoff, &yoff);

        if (bm) {
            int draw_x0 = (int)xf + xoff;
            int draw_y0 = (int)y + baseline + yoff;

            for (int gy = 0; gy < h; gy++) {
                int sy = draw_y0 + gy;
                if (sy < 0 || (u32)sy >= display_height) continue;
                u32 *row = color_buffer[curr_fb] + (u32)sy * display_width;
                for (int gx = 0; gx < w; gx++) {
                    int sx = draw_x0 + gx;
                    if (sx < 0 || (u32)sx >= display_width) continue;
                    u32 a = bm[gy * w + gx];
                    if (a == 0) continue;
                    if (a == 255) { row[sx] = color; continue; }
                    u32 bg   = row[sx];
                    u32 r_out = aa_blend(a, r_fg, (bg >> 16) & 0xFF);
                    u32 g_out = aa_blend(a, g_fg, (bg >>  8) & 0xFF);
                    u32 b_out = aa_blend(a, b_fg,  bg        & 0xFF);
                    row[sx] = (r_out << 16) | (g_out << 8) | b_out;
                }
            }
            stbtt_FreeBitmap(bm, NULL);
        }

        int advance;
        stbtt_GetCodepointHMetrics(fi, cp, &advance, NULL);
        xf += (float)advance * scale;

        prev_cp = cp;
        text++;
    }
}

void drawIcon(u32 x, u32 y, int codepoint, float px, u32 color) {
    if (!s_icons_ok) return;
    float scale = stbtt_ScaleForPixelHeight(&s_icons, px);
    int ascent;
    stbtt_GetFontVMetrics(&s_icons, &ascent, NULL, NULL);
    int baseline = (int)((float)ascent * scale);
    int w, h, xoff, yoff;
    unsigned char *bm = stbtt_GetCodepointBitmap(
        &s_icons, scale, scale, codepoint, &w, &h, &xoff, &yoff);
    if (!bm) return;
    u32 r_fg = (color >> 16) & 0xFF;
    u32 g_fg = (color >>  8) & 0xFF;
    u32 b_fg =  color        & 0xFF;
    int draw_x0 = (int)x + xoff;
    int draw_y0 = (int)y + baseline + yoff;
    for (int gy = 0; gy < h; gy++) {
        int sy = draw_y0 + gy;
        if (sy < 0 || (u32)sy >= display_height) continue;
        u32 *row = color_buffer[curr_fb] + (u32)sy * display_width;
        for (int gx = 0; gx < w; gx++) {
            int sx = draw_x0 + gx;
            if (sx < 0 || (u32)sx >= display_width) continue;
            u32 a = bm[gy * w + gx];
            if (a == 0) continue;
            if (a == 255) { row[sx] = color; continue; }
            u32 bg   = row[sx];
            u32 r_bg = (bg >> 16) & 0xFF;
            u32 g_bg = (bg >>  8) & 0xFF;
            u32 b_bg =  bg        & 0xFF;
            u32 r_out = (a * r_fg + (255 - a) * r_bg) / 255;
            u32 g_out = (a * g_fg + (255 - a) * g_bg) / 255;
            u32 b_out = (a * b_fg + (255 - a) * b_bg) / 255;
            row[sx] = (r_out << 16) | (g_out << 8) | b_out;
        }
    }
    stbtt_FreeBitmap(bm, NULL);
}

// -------------------------------------------------------
// Lifecycle
// -------------------------------------------------------

void ttf_init(void) {
    gamma_init();
    bitmapSetXpm(&fontBitmap, font8x8_xpm);
    s_font_buf = (unsigned char*)OpenSans_Regular_ttf;
    if (stbtt_InitFont(&s_font, s_font_buf, 0))
        s_ttf_ok = true;
    if (stbtt_InitFont(&s_font_bold, (unsigned char*)OpenSans_Bold_ttf, 0))
        s_ttf_bold_ok = true;
    if (stbtt_InitFont(&s_icons, (unsigned char*)MaterialIcons_Regular_ttf, 0))
        s_icons_ok = true;
    if (stbtt_InitFont(&s_iconic, (unsigned char*)Iconic_PSx_ttf, 0))
        s_iconic_ok = true;
}

// Warm the malloc pool and stbtt i-cache for every glyph the HUD will ever draw.
// Must be called before the first hud_draw() call — no pixel writes, just
// alloc+rasterize+free for each codepoint at each size used by player_hud.cpp.
void ttf_prewarm_hud(void) {
    // OpenSans Regular: seek-increment (13px), time labels + audio track label (18px).
    // Full printable ASCII at 18px covers all possible track name characters.
    if (s_ttf_ok) {
        static const struct { const char *chars; float px; } reg[] = {
            { "+/- 0123456789smni",  13.0f },
            { " !\"#$%&'()*+,-./"
              "0123456789:;<=>?@"
              "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
              "[\\]^_`"
              "abcdefghijklmnopqrstuvwxyz{|}~", 18.0f },
        };
        for (int s = 0; s < 2; s++) {
            float sc = stbtt_ScaleForPixelHeight(&s_font, reg[s].px);
            for (const char *cp = reg[s].chars; *cp; cp++) {
                int w, h, xo, yo;
                unsigned char *bm = stbtt_GetCodepointBitmap(
                    &s_font, sc, sc, (unsigned char)*cp, &w, &h, &xo, &yo);
                if (bm) stbtt_FreeBitmap(bm, NULL);
            }
        }
    }
    // OpenSans Bold: "CC" label (20px).
    if (s_ttf_bold_ok) {
        float sc = stbtt_ScaleForPixelHeight(&s_font_bold, 20.0f);
        for (const char *cp = "C"; *cp; cp++) {
            int w, h, xo, yo;
            unsigned char *bm = stbtt_GetCodepointBitmap(
                &s_font_bold, sc, sc, (unsigned char)*cp, &w, &h, &xo, &yo);
            if (bm) stbtt_FreeBitmap(bm, NULL);
        }
    }
    // Material Icons: music note codepoint (24px).
    if (s_icons_ok) {
        float sc = stbtt_ScaleForPixelHeight(&s_icons, 24.0f);
        int w, h, xo, yo;
        unsigned char *bm = stbtt_GetCodepointBitmap(
            &s_icons, sc, sc, 0xE405, &w, &h, &xo, &yo);
        if (bm) stbtt_FreeBitmap(bm, NULL);
    }
    // Iconic PSx: rewind 'L' and fast-forward 'R' at ROW_ICON_PX (24px).
    // Play/pause now uses CPU-drawn primitives — no Iconic glyph needed.
    if (s_iconic_ok) {
        static const struct { int cp; float px; } iconic[] = {
            { 'L', 24.0f }, { 'R', 24.0f },
        };
        for (int i = 0; i < 2; i++) {
            float sc = stbtt_ScaleForPixelHeight(&s_iconic, iconic[i].px);
            int w, h, xo, yo;
            unsigned char *bm = stbtt_GetCodepointBitmap(
                &s_iconic, sc, sc, iconic[i].cp, &w, &h, &xo, &yo);
            if (bm) stbtt_FreeBitmap(bm, NULL);
        }
    }
}

void visuals_cleanup(void) {
    bitmapDestroy(&fontBitmap);
}

// -------------------------------------------------------
// XMB rendering helpers
// -------------------------------------------------------

// Returns the display-width x-centre of tab slot t.
static int xmb_tab_cx(int t) {
    return (int)(((t + 0.5f) * display_width) / XMB_TAB_COUNT);
}

void xmb_draw_tabs(void) {
    const int TAB_SPACING  = 96;
    const int group_half_w = ((XMB_TAB_COUNT - 1) * TAB_SPACING) / 2;
    const int tab_group_x0 = (int)display_width / 2 - group_half_w;

    for (int t = 0; t < XMB_TAB_COUNT; t++) {
        if (!g_tabs[t].enabled) continue;

        int cx      = tab_group_x0 + t * TAB_SPACING;
        bool active = (t == g_active_tab);
        int icon_px = active ? 96 : 32;
        int icon_x  = cx - icon_px / 2;
        int icon_y  = XMB_TOPBAR_H + (XMB_TABBAR_H - icon_px) / 2 - 8;

        drawIcon((u32)icon_x, (u32)icon_y, TAB_CODEPOINTS[t], (float)icon_px, 0x00ae99d6);
    }
}

// Draw the meta string "year . duration . genre" at (x, y).
void xmb_draw_meta(u32 x, u32 y, const XMBItem *it, float px) {
    char meta[64] = "";
    if (it->year_str[0])     { snprintf(meta, sizeof(meta), "%s", it->year_str); }
    if (it->duration_str[0]) {
        if (meta[0]) strncat(meta, " . ", sizeof(meta)-strlen(meta)-1);
        strncat(meta, it->duration_str, sizeof(meta)-strlen(meta)-1);
    }
    if (it->genre[0]) {
        if (meta[0]) strncat(meta, " . ", sizeof(meta)-strlen(meta)-1);
        strncat(meta, it->genre, sizeof(meta)-strlen(meta)-1);
    }
    if (meta[0]) drawTTF(x, y, meta, px, 0x00FFFFFF);
}

void xmb_draw_item_list(int tab) {
    int count  = g_item_count[tab];
    int vis    = XMB_ITEMS_VIS;
    int W      = (int)display_width;
    int list_x = (W - XMB_LIST_W) / 2;

    for (int i = 0; i < vis; i++) {
        int idx = g_scroll_top + i;
        if (idx >= count) break;
        const XMBItem *it = &g_items[tab][idx];

        int iy = XMB_CONTENT_Y + i * XMB_ROW_STRIDE;
        int tx = list_x + 16 + XMB_THUMB_W + 16;

        drawTTF((u32)tx, (u32)(iy + 20), it->name, 22, 0x00FFFFFF);
        xmb_draw_meta((u32)tx, (u32)(iy + 50), it);

        if (it->codec[0]) {
            int bx = list_x + XMB_LIST_W - 75;
            int by = iy + (XMB_ROW_H - 22) / 2 + 4;
            drawTTF((u32)bx, (u32)by, it->codec, 13, 0x00FFFFFF);
        }

        if (i == 0 && g_scroll_top > 0)
            drawTTF(display_width - 20, (u32)iy, "^", 8, 0x00FFFFFF);
        if (i == vis-1 && (g_scroll_top + vis < count ||
                           g_tab_start[tab] + count < g_tab_total[tab]))
            drawTTF(display_width - 20, (u32)iy, "v", 8, 0x00FFFFFF);
    }
}

// CPU draws for item list (highlight, thumb, badge bg)
void xmb_cpu_draw_items(int tab) {
    int count  = g_item_count[tab];
    int vis    = XMB_ITEMS_VIS;
    int W      = (int)display_width;
    int list_x = (W - XMB_LIST_W) / 2;

    for (int i = 0; i < vis; i++) {
        int idx = g_scroll_top + i;
        if (idx >= count) break;

        int iy = XMB_CONTENT_Y + i * XMB_ROW_STRIDE;

        if (idx == g_sel) {
            drawRect((u32)list_x, (u32)iy,
                     (u32)XMB_LIST_W, (u32)XMB_ROW_H, XMB_HIGHLIGHT);
            drawRect((u32)(list_x - 5), (u32)iy,
                     4, (u32)XMB_ROW_H, XMB_ACCENT);
        }

        const XMBItem *it = &g_items[tab][idx];
        int thumb_x = list_x + 16;
        int thumb_y = iy + (XMB_ROW_H - XMB_THUMB_H) / 2;
        thumb_request(it->id);
        const Bitmap *th = thumb_get(it->id);
        if (th) {
            u32 *fb = color_buffer[curr_fb];
            int stride = (int)display_width;
            for (int row = 0; row < (int)XMB_THUMB_H; row++) {
                int sy = thumb_y + row;
                if (sy < 0 || sy >= (int)display_height) continue;
                for (int col = 0; col < (int)XMB_THUMB_W; col++) {
                    int sx = thumb_x + col;
                    if (sx < 0 || sx >= (int)display_width) continue;
                    fb[sy * stride + sx] = th->pixels[row * XMB_THUMB_W + col];
                }
            }
        } else {
            drawRect((u32)thumb_x, (u32)thumb_y, XMB_THUMB_W, XMB_THUMB_H, XMB_THUMB_DIM);
        }

        int badge_x = list_x + XMB_LIST_W - 80;
        int badge_y = iy + (XMB_ROW_H - 22) / 2;
        drawRect((u32)badge_x, (u32)badge_y, 66, 22, XMB_BADGE_BG);
    }
}

// CPU draws for the TV sub-item list (seasons or episodes)
void xmb_cpu_draw_sub(void) {
    int vis    = XMB_ITEMS_VIS;
    int W      = (int)display_width;
    int list_x = (W - XMB_LIST_W) / 2;
    int y0     = XMB_CONTENT_Y + 26;
    for (int i = 0; i < vis; i++) {
        int idx = g_tv_sub_scroll + i;
        if (idx >= g_tv_sub_count) break;
        int iy = y0 + i * XMB_ROW_STRIDE;
        if (idx == g_tv_sub_sel) {
            drawRect((u32)list_x, (u32)iy,
                     (u32)XMB_LIST_W, (u32)XMB_ROW_H, XMB_HIGHLIGHT);
            drawRect((u32)(list_x - 5), (u32)iy, 4, (u32)XMB_ROW_H, XMB_ACCENT);
        }
        const XMBItem *it = &g_tv_sub_items[idx];
        int thumb_x = list_x + 16;
        int thumb_y = iy + (XMB_ROW_H - XMB_THUMB_H) / 2;
        thumb_request(it->id);
        const Bitmap *th = thumb_get(it->id);
        if (th) {
            u32 *fb = color_buffer[curr_fb];
            int stride = (int)display_width;
            for (int row = 0; row < (int)XMB_THUMB_H; row++) {
                int sy = thumb_y + row;
                if (sy < 0 || sy >= (int)display_height) continue;
                for (int col = 0; col < (int)XMB_THUMB_W; col++) {
                    int sx = thumb_x + col;
                    if (sx < 0 || sx >= (int)display_width) continue;
                    fb[sy * stride + sx] = th->pixels[row * XMB_THUMB_W + col];
                }
            }
        } else {
            drawRect((u32)thumb_x, (u32)thumb_y, XMB_THUMB_W, XMB_THUMB_H, XMB_THUMB_DIM);
        }
        int badge_x = list_x + XMB_LIST_W - 80;
        int badge_y = iy + (XMB_ROW_H - 22) / 2;
        drawRect((u32)badge_x, (u32)badge_y, 66, 22, XMB_BADGE_BG);
    }
}

// TTF draws for the TV sub-item list
void xmb_draw_sub_list(void) {
    int vis    = XMB_ITEMS_VIS;
    int W      = (int)display_width;
    int list_x = (W - XMB_LIST_W) / 2;
    int y0     = XMB_CONTENT_Y + 26;
    for (int i = 0; i < vis; i++) {
        int idx = g_tv_sub_scroll + i;
        if (idx >= g_tv_sub_count) break;
        const XMBItem *it = &g_tv_sub_items[idx];
        int iy = y0 + i * XMB_ROW_STRIDE;
        int tx = list_x + 16 + XMB_THUMB_W + 16;
        drawTTF((u32)tx, (u32)(iy + 20), it->name, 22, 0x00FFFFFF);
        xmb_draw_meta((u32)tx, (u32)(iy + 50), it);
        if (it->codec[0]) {
            int bx = list_x + XMB_LIST_W - 75;
            int by = iy + (XMB_ROW_H - 22) / 2 + 4;
            drawTTF((u32)bx, (u32)by, it->codec, 13, 0x00FFFFFF);
        }
        if (i == 0 && g_tv_sub_scroll > 0)
            drawTTF(display_width - 20, (u32)iy, "^", 8, 0x00FFFFFF);
        if (i == vis-1 && (g_tv_sub_scroll + vis < g_tv_sub_count ||
                           g_tv_sub_start + g_tv_sub_count < g_tv_sub_total))
            drawTTF(display_width - 20, (u32)iy, "v", 8, 0x00FFFFFF);
    }
}

// CPU draws for the Collections sub-item list (movies inside a collection)
void xmb_cpu_draw_col_sub(void) {
    int vis    = XMB_ITEMS_VIS;
    int W      = (int)display_width;
    int list_x = (W - XMB_LIST_W) / 2;
    int y0     = XMB_CONTENT_Y + 26;
    for (int i = 0; i < vis; i++) {
        int idx = g_col_sub_scroll + i;
        if (idx >= g_col_sub_count) break;
        int iy = y0 + i * XMB_ROW_STRIDE;
        if (idx == g_col_sub_sel) {
            drawRect((u32)list_x, (u32)iy,
                     (u32)XMB_LIST_W, (u32)XMB_ROW_H, XMB_HIGHLIGHT);
            drawRect((u32)(list_x - 5), (u32)iy, 4, (u32)XMB_ROW_H, XMB_ACCENT);
        }
        const XMBItem *it = &g_col_sub_items[idx];
        int thumb_x = list_x + 16;
        int thumb_y = iy + (XMB_ROW_H - XMB_THUMB_H) / 2;
        thumb_request(it->id);
        const Bitmap *th = thumb_get(it->id);
        if (th) {
            u32 *fb = color_buffer[curr_fb];
            int stride = (int)display_width;
            for (int row = 0; row < (int)XMB_THUMB_H; row++) {
                int sy = thumb_y + row;
                if (sy < 0 || sy >= (int)display_height) continue;
                for (int col = 0; col < (int)XMB_THUMB_W; col++) {
                    int sx = thumb_x + col;
                    if (sx < 0 || sx >= (int)display_width) continue;
                    fb[sy * stride + sx] = th->pixels[row * XMB_THUMB_W + col];
                }
            }
        } else {
            drawRect((u32)thumb_x, (u32)thumb_y, XMB_THUMB_W, XMB_THUMB_H, XMB_THUMB_DIM);
        }
        int badge_x = list_x + XMB_LIST_W - 80;
        int badge_y = iy + (XMB_ROW_H - 22) / 2;
        drawRect((u32)badge_x, (u32)badge_y, 66, 22, XMB_BADGE_BG);
    }
}

// TTF draws for the Collections sub-item list
void xmb_draw_col_sub_list(void) {
    int vis    = XMB_ITEMS_VIS;
    int W      = (int)display_width;
    int list_x = (W - XMB_LIST_W) / 2;
    int y0     = XMB_CONTENT_Y + 26;
    for (int i = 0; i < vis; i++) {
        int idx = g_col_sub_scroll + i;
        if (idx >= g_col_sub_count) break;
        const XMBItem *it = &g_col_sub_items[idx];
        int iy = y0 + i * XMB_ROW_STRIDE;
        int tx = list_x + 16 + XMB_THUMB_W + 16;
        drawTTF((u32)tx, (u32)(iy + 20), it->name, 22, 0x00FFFFFF);
        xmb_draw_meta((u32)tx, (u32)(iy + 50), it);
        if (it->codec[0]) {
            int bx = list_x + XMB_LIST_W - 75;
            int by = iy + (XMB_ROW_H - 22) / 2 + 4;
            drawTTF((u32)bx, (u32)by, it->codec, 13, 0x00FFFFFF);
        }
        if (i == 0 && g_col_sub_scroll > 0)
            drawTTF(display_width - 20, (u32)iy, "^", 8, 0x00FFFFFF);
        if (i == vis-1 && (g_col_sub_scroll + vis < g_col_sub_count ||
                           g_col_sub_start + g_col_sub_count < g_col_sub_total))
            drawTTF(display_width - 20, (u32)iy, "v", 8, 0x00FFFFFF);
    }
}

// -------------------------------------------------------
// Search / OSK rendering
// -------------------------------------------------------

void xmb_cpu_draw_osk(void) {
    int W = (int)display_width;
    int total_w = 10 * OSK_STEP_X - OSK_GAP;
    int osk_x0  = (W - total_w) / 2;

    drawRect((u32)osk_x0, (u32)(XMB_CONTENT_Y + 8),
             (u32)total_w, 40, 0x001A1040);

    for (int r = 0; r <= OSK_ROWS_N; r++) {
        if (r == OSK_ROWS_N) {
            int space_w = 5 * OSK_STEP_X - OSK_GAP;
            int sy = OSK_Y0 + r * OSK_STEP_Y;
            bool sp_sel = (r == g_osk_row && g_osk_col == 0);
            drawRect((u32)osk_x0, (u32)sy, (u32)space_w, OSK_KEY_H,
                     sp_sel ? XMB_KEY_SEL : XMB_KEY_NORMAL);
            int bsx = osk_x0 + space_w + OSK_GAP;
            bool bs_sel = (r == g_osk_row && g_osk_col == 1);
            drawRect((u32)bsx, (u32)sy, OSK_KEY_W, OSK_KEY_H,
                     bs_sel ? XMB_KEY_SEL : XMB_KEY_NORMAL);
            int clx = bsx + OSK_KEY_W + OSK_GAP;
            bool cl_sel = (r == g_osk_row && g_osk_col == 2);
            drawRect((u32)clx, (u32)sy, OSK_KEY_W, OSK_KEY_H,
                     cl_sel ? XMB_KEY_SEL : XMB_KEY_NORMAL);
        } else {
            const char **rows = g_osk_sym ? OSK_SYMBOLS : OSK_LETTERS;
            int rlen = (int)strlen(rows[r]);
            if (r == OSK_ROWS_N - 1) rlen++;
            int ry = OSK_Y0 + r * OSK_STEP_Y;
            for (int c = 0; c < rlen; c++) {
                bool sel = (r == g_osk_row && c == g_osk_col);
                drawRect((u32)(osk_x0 + c * OSK_STEP_X), (u32)ry,
                         OSK_KEY_W, OSK_KEY_H,
                         sel ? XMB_KEY_SEL : XMB_KEY_NORMAL);
            }
        }
    }
}

void xmb_rsx_draw_osk(void) {
    int W = (int)display_width;
    int total_w = 10 * OSK_STEP_X - OSK_GAP;
    int osk_x0  = (W - total_w) / 2;

    u64 us = timing_get_us();
    bool cursor = ((us / 500000) & 1) == 0;

    char disp[68];
    snprintf(disp, sizeof(disp), "%s%s", g_search_buf, cursor ? "_" : " ");
    {
        int bar_cx = (int)display_width / 2;
        int text_w = (int)strlen(disp) * 18;
        int tx = bar_cx - text_w / 2;
        if (tx < (int)XMB_ITEM_PAD + 4) tx = (int)XMB_ITEM_PAD + 4;
        drawTTF((u32)tx, (u32)(XMB_CONTENT_Y + 18), disp, 18, 0x00FFFFFF);
    }

    for (int r = 0; r <= OSK_ROWS_N; r++) {
        if (r == OSK_ROWS_N) {
            int space_w = 5 * OSK_STEP_X - OSK_GAP;
            int sy = OSK_Y0 + r * OSK_STEP_Y + (OSK_KEY_H - 8) / 2;
            int cx_space = osk_x0 + space_w / 2 - 20;
            drawTTF((u32)(cx_space > 0 ? cx_space : 0), (u32)sy, "SPACE", 21, 0x00FFFFFF);

            int bsx = osk_x0 + space_w + OSK_GAP;
            drawTTF((u32)(bsx + (OSK_KEY_W - 8) / 2), (u32)sy, "<", 21, 0x00FFFFFF);

            int clx = bsx + OSK_KEY_W + OSK_GAP;
            drawTTF((u32)(clx + (OSK_KEY_W - 40) / 2), (u32)sy, "CLEAR", 21, 0x00FFFFFF);
        } else {
            const char **rows = g_osk_sym ? OSK_SYMBOLS : OSK_LETTERS;
            const char  *row  = rows[r];
            int base_len = strlen(row);
            int ry = OSK_Y0 + r * OSK_STEP_Y + (OSK_KEY_H - 8) / 2;

            for (int c = 0; c < base_len; c++) {
                char label[3] = { row[c], '\0', '\0' };
                int kx = osk_x0 + c * OSK_STEP_X + (OSK_KEY_W - 8) / 2;
                drawTTF((u32)kx, (u32)ry, label, 21, 0x00FFFFFF);
            }
            if (r == OSK_ROWS_N - 1) {
                const char *toggle_lbl = g_osk_sym ? "ABC" : "#+=";
                int kx = osk_x0 + base_len * OSK_STEP_X + (OSK_KEY_W - 24) / 2;
                drawTTF((u32)(kx > 0 ? kx : 0), (u32)ry, toggle_lbl, 21, 0x00FFFFFF);
            }
        }
    }

    int kb_bottom = OSK_Y0 + (OSK_ROWS_N + 1) * OSK_STEP_Y + 20;
    int results_y = kb_bottom;
    int count = g_search_results_count;
    int vis_r = ((int)display_height - XMB_BOTTOM_PAD - results_y) / XMB_ROW_STRIDE;
    if (vis_r < 0) vis_r = 0;
    {
        int sr_list_x = ((int)display_width - XMB_LIST_W) / 2;
        int sr_tx     = sr_list_x + 16 + XMB_THUMB_W + 16;
        for (int i = 0; i < vis_r; i++) {
            int idx = g_search_scroll + i;
            if (idx >= count) break;
            const XMBItem *it = &g_search_results[idx];
            int iy = results_y + i * XMB_ROW_STRIDE;
            drawTTF((u32)sr_tx, (u32)(iy + 20), it->name, 18, 0x00FFFFFF);
            xmb_draw_meta((u32)sr_tx, (u32)(iy + 48), it, 18);
        }
    }
    if (count == 0 && g_search_buf[0]) {
        drawTTF((u32)XMB_ITEM_PAD, (u32)results_y, "No results.", 8, 0x00FFFFFF);
    }
}

// Alphabetical jump bar rendered to the left of the item list.
// Always visible on library tabs at depth 0; letters are dimmed when unfocused,
// accent-coloured on the selected entry when g_jumpbar_active is true.
void xmb_draw_jumpbar(int /*tab*/) {
    int W      = (int)display_width;
    int list_x = (W - XMB_LIST_W) / 2;
    int vis    = XMB_ITEMS_VIS;

    int bar_top = XMB_CONTENT_Y;
    int bar_bot = XMB_CONTENT_Y + vis * XMB_ROW_STRIDE - XMB_ROW_GAP;
    int bar_h   = bar_bot - bar_top;
    int jbar_x  = list_x - JBAR_GAP - JBAR_W;
    if (jbar_x < 0) jbar_x = 0;

    // Step height evenly divides the bar; font fills each slot (1.2× gives glyph ascender
    // room without adjacent letters visually overlapping on TV at viewing distance).
    float entry_h = (float)bar_h / (float)JBAR_ENTRIES;
    float font_px = entry_h * 1.2f;
    if (font_px < 12.0f) font_px = 12.0f;
    if (font_px > 28.0f) font_px = 28.0f;

    static const char * const jbar_labels[JBAR_ENTRIES] = {
        "#","A","B","C","D","E","F","G","H","I","J","K","L","M",
        "N","O","P","Q","R","S","T","U","V","W","X","Y","Z"
    };

    for (int i = 0; i < JBAR_ENTRIES; i++) {
        int ey = bar_top + (int)(i * entry_h);
        int ty = ey + (int)((entry_h - font_px) * 0.5f);
        if (ty < 0) ty = 0;
        if ((u32)ty >= display_height) continue;
        bool sel = g_jumpbar_active && (i == g_jumpbar_sel);
        u32 color = sel ? 0x00FFFFFFUL : 0x00554477UL;
        drawTTF((u32)jbar_x, (u32)ty, jbar_labels[i], font_px, color);
    }
}

// -------------------------------------------------------
// Controller hints bar (font-based)
// -------------------------------------------------------

// Draw one Iconic PSx glyph (single ASCII char) at (x,y).
void draw_iconic_glyph(u32 x, u32 y, char glyph, float px, u32 color) {
    if (!s_iconic_ok) return;
    int cp = (unsigned char)glyph;
    float scale = stbtt_ScaleForPixelHeight(&s_iconic, px);
    int ascent;
    stbtt_GetFontVMetrics(&s_iconic, &ascent, NULL, NULL);
    int baseline = (int)((float)ascent * scale);
    int w, h, xoff, yoff;
    unsigned char *bm = stbtt_GetCodepointBitmap(
        &s_iconic, scale, scale, cp, &w, &h, &xoff, &yoff);
    if (!bm) return;
    u32 r_fg = (color >> 16) & 0xFF;
    u32 g_fg = (color >>  8) & 0xFF;
    u32 b_fg =  color        & 0xFF;
    int dx0 = (int)x + xoff;
    int dy0 = (int)y + baseline + yoff;
    for (int gy = 0; gy < h; gy++) {
        int sy = dy0 + gy;
        if (sy < 0 || (u32)sy >= display_height) continue;
        u32 *row = color_buffer[curr_fb] + (u32)sy * display_width;
        for (int gx = 0; gx < w; gx++) {
            int sx = dx0 + gx;
            if (sx < 0 || (u32)sx >= display_width) continue;
            u32 a = bm[gy * w + gx];
            if (a == 0) continue;
            if (a == 255) { row[sx] = color; continue; }
            u32 bg   = row[sx];
            u32 r_bg = (bg >> 16) & 0xFF;
            u32 g_bg = (bg >>  8) & 0xFF;
            u32 b_bg =  bg        & 0xFF;
            row[sx] = (((a*r_fg + (255-a)*r_bg)/255) << 16) |
                      (((a*g_fg + (255-a)*g_bg)/255) <<  8) |
                       ((a*b_fg + (255-a)*b_bg)/255);
        }
    }
    stbtt_FreeBitmap(bm, NULL);
}

// Return the advance width in pixels for one Iconic PSx glyph.
int iconic_adv_px(char glyph, float px) {
    if (!s_iconic_ok) return (int)px;
    float sc = stbtt_ScaleForPixelHeight(&s_iconic, px);
    int adv;
    stbtt_GetCodepointHMetrics(&s_iconic, (unsigned char)glyph, &adv, NULL);
    return (int)((float)adv * sc);
}

// Render an array of (glyph, label) hint pairs as a centered horizontal row
// at the bottom of the screen.  Glyphs drawn in tab-icon colour; labels in white.
void draw_hints_bar(const Hint *hints, int n) {
    if (n <= 0) return;

    const float icon_px = 33.0f;
    const float text_px = 24.0f;
    const int   gap_it  =  8;   // pixels between icon and its label
    const int   gap_sep = 28;   // pixels between hint pairs

    // Measure total width
    int total_w = 0;
    for (int i = 0; i < n; i++) {
        total_w += iconic_adv_px(hints[i].glyph, icon_px);
        total_w += gap_it;
        total_w += ttf_text_width(hints[i].label, text_px);
        if (i < n - 1) total_w += gap_sep;
    }

    int x = ((int)display_width - total_w) / 2;
    if (x < (int)XMB_ITEM_PAD) x = (int)XMB_ITEM_PAD;
    int y = (int)display_height - (int)XMB_BOTTOM_PAD + 18;
    if (y < 0 || (u32)y >= display_height) return;

    for (int i = 0; i < n; i++) {
        draw_iconic_glyph((u32)x, (u32)y, hints[i].glyph, icon_px, 0x00ae99d6);
        x += iconic_adv_px(hints[i].glyph, icon_px) + gap_it;
        drawTTF((u32)x, (u32)(y + 5), hints[i].label, text_px, 0x00FFFFFF);
        x += ttf_text_width(hints[i].label, text_px);
        if (i < n - 1) x += gap_sep;
    }
}

// Render L1 + R1 Iconic PSx glyphs right-aligned in the top bar.
void draw_topbar_lr(void) {
    if (!s_iconic_ok) return;
    const float icon_px = 40.0f;
    int w_l     = iconic_adv_px('l', icon_px);
    int w_r     = iconic_adv_px('r', icon_px);
    int total_w = w_l + 4 + w_r;
    int x = ((int)display_width - total_w) / 2;
    if (x < 0) x = 0;
    int y = (XMB_TOPBAR_H - (int)icon_px) / 2;
    if (y < 0) y = 0;
    draw_iconic_glyph((u32)x, (u32)y, 'l', icon_px, 0x00ae99d6);
    x += w_l + 4;
    draw_iconic_glyph((u32)x, (u32)y, 'r', icon_px, 0x00ae99d6);
}

// CPU draws for search results list (thumbs + selection highlight)
void xmb_cpu_draw_search_results(void) {
    int results_y = OSK_Y0 + (OSK_ROWS_N + 1) * OSK_STEP_Y + 20;
    int count     = g_search_results_count;
    int vis_r     = ((int)display_height - XMB_BOTTOM_PAD - results_y) / XMB_ROW_STRIDE;
    if (vis_r < 0) vis_r = 0;
    int W      = (int)display_width;
    int list_x = (W - XMB_LIST_W) / 2;
    for (int i = 0; i < vis_r; i++) {
        int idx = g_search_scroll + i;
        if (idx >= count) break;
        int iy = results_y + i * XMB_ROW_STRIDE;
        if (g_search_focus_results && idx == g_search_sel) {
            drawRect((u32)list_x, (u32)iy,
                     (u32)XMB_LIST_W, (u32)XMB_ROW_H, XMB_HIGHLIGHT);
            drawRect((u32)(list_x - 5), (u32)iy,
                     4, (u32)XMB_ROW_H, XMB_ACCENT);
        }
        const XMBItem *it = &g_search_results[idx];
        int thumb_x = list_x + 16;
        int thumb_y = iy + (XMB_ROW_H - XMB_THUMB_H) / 2;
        thumb_request(it->id);
        const Bitmap *th = thumb_get(it->id);
        if (th) {
            u32 *fb = color_buffer[curr_fb];
            int stride = (int)display_width;
            for (int row = 0; row < (int)XMB_THUMB_H; row++) {
                int sy = thumb_y + row;
                if (sy < 0 || sy >= (int)display_height) continue;
                for (int col = 0; col < (int)XMB_THUMB_W; col++) {
                    int sx = thumb_x + col;
                    if (sx < 0 || sx >= (int)display_width) continue;
                    fb[sy * stride + sx] = th->pixels[row * XMB_THUMB_W + col];
                }
            }
        } else {
            drawRect((u32)thumb_x, (u32)thumb_y, XMB_THUMB_W, XMB_THUMB_H, XMB_THUMB_DIM);
        }
    }
}

// -------------------------------------------------------
// Settings tab — account info card + selectable action rows.
// Only "Log Out" exists for now (XMB_SETTINGS_COUNT == 1).
// -------------------------------------------------------
static const char *SETTINGS_LABELS[XMB_SETTINGS_COUNT] = { "Log Out" };

// Y of the i-th action row, below the account info card.
static int settings_row_y(int i) {
    return XMB_CONTENT_Y + 120 + i * XMB_ROW_STRIDE;
}

// CPU phase: account card background + selection highlight (or confirm dim).
void xmb_cpu_draw_settings(void) {
    int W      = (int)display_width;
    int list_x = (W - XMB_LIST_W) / 2;

    if (g_settings_confirm) {
        // Solid backdrop behind the confirmation prompt.
        drawRect(0, (u32)XMB_CONTENT_Y, display_width,
                 display_height - XMB_CONTENT_Y - XMB_BOTTOM_PAD, XMB_HIGHLIGHT);
        return;
    }

    // Account info card.
    drawRect((u32)list_x, (u32)XMB_CONTENT_Y, (u32)XMB_LIST_W, 96, XMB_THUMB_DIM);

    // Highlight the selected action row.
    for (int i = 0; i < XMB_SETTINGS_COUNT; i++) {
        if (i != g_settings_sel) continue;
        int iy = settings_row_y(i);
        drawRect((u32)list_x, (u32)iy, (u32)XMB_LIST_W, (u32)XMB_ROW_H, XMB_HIGHLIGHT);
        drawRect((u32)(list_x - 5), (u32)iy, 4, (u32)XMB_ROW_H, XMB_ACCENT);
    }
}

// RSX phase: account text, action labels, confirm prompt.
void xmb_draw_settings(void) {
    int W      = (int)display_width;
    int list_x = (W - XMB_LIST_W) / 2;
    int tx     = list_x + 20;

    // Account info.
    drawTTF((u32)tx, (u32)(XMB_CONTENT_Y + 22), "ACCOUNT", 14, 0x00A090C0);
    char line[320];
    snprintf(line, sizeof(line), "Signed in as %s",
             g_username[0] ? g_username : "(unknown)");
    drawTTF((u32)tx, (u32)(XMB_CONTENT_Y + 44), line, 20, 0x00FFFFFF);
    snprintf(line, sizeof(line), "Server  %s",
             g_server[0] ? g_server : "(none)");
    drawTTF((u32)tx, (u32)(XMB_CONTENT_Y + 72), line, 14, 0x00A0A0A0);

    // Action rows.
    for (int i = 0; i < XMB_SETTINGS_COUNT; i++) {
        int iy = settings_row_y(i);
        drawTTF((u32)tx, (u32)(iy + 26), SETTINGS_LABELS[i], 22, 0x00FFFFFF);
    }

    // Confirmation prompt.
    if (g_settings_confirm) {
        int cy = XMB_CONTENT_Y + 140;
        const char *q = "Log out of this account?";
        int qw = ttf_text_width(q, 24);
        drawTTF((u32)((W - qw) / 2), (u32)cy, q, 24, 0x00FFFFFF);
        const char *h = "X  Log Out        O  Cancel";
        int hw = ttf_text_width(h, 18);
        drawTTF((u32)((W - hw) / 2), (u32)(cy + 44), h, 18, 0x00C0B0E0);
    }
}
