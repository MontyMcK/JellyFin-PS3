#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>

#include <rsx/rsx.h>

#include "ui_visuals.h"
#include "bitmap.h"
#include "font8x8.xpm"
#include "timing.h"
#include "opensans_regular.h"
#include "opensans_bold.h"
#include "material_icons.h"
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

// -------------------------------------------------------
// Keyboard data (extern in ui_visuals.h, used in ui.cpp too)
// -------------------------------------------------------

const char *KB_ROWS[KB_LETTER_ROWS] = {
    "1234567890",
    "qwertyuiop",
    "asdfghjkl",
    "zxcvbnm",
};

const SpecialKey SPECIAL[SPECIAL_N] = {
    {"SPC",' '}, {".",'.'},  {":",':'}, {"/",'/'},
    {"@",'@'},   {"-",'-'},  {"_",'_'}, {"DEL",'\b'}, {"OK",'\r'},
};

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
// Legacy on-screen keyboard (used by login / server-url screens)
// -------------------------------------------------------

static int row_len_kb(int r) {
    return (r < KB_LETTER_ROWS) ? (int)strlen(KB_ROWS[r]) : SPECIAL_N;
}

void draw_keyboard(const char *prompt, const char *input, bool is_password) {
    drawHeader();
    drawTTF(40, 85, prompt, 14, 0x00FFFFFF);

    char display[256] = "";
    int ilen = strlen(input);
    if (is_password) {
        memset(display, '*', ilen);
        display[ilen] = '\0';
    } else {
        if (ilen > 34) strncpy(display, input + ilen - 34, 35);
        else           strncpy(display, input, 255);
    }
    char input_line[260];
    snprintf(input_line, sizeof(input_line), "> %s_", display);
    drawTTF(40, 115, input_line, 14, 0x00FFFFFF);
    if (kb_caps) drawTTF(40, 115+LINE_HEIGHT*2, "CAPS ON", 14, 0x00FFFFFF);

    int kb_x = 50, kb_y = 175;
    int key_h = LINE_HEIGHT + 8;

    for (int r = 0; r < TOTAL_ROWS; r++) {
        int key_w = (r < KB_LETTER_ROWS) ? (CHAR_SIZE + 16) : (CHAR_SIZE * 3 + 8);
        int rlen  = row_len_kb(r);
        for (int c = 0; c < rlen; c++) {
            int  xk  = kb_x + c * key_w;
            int  yk  = kb_y + r * key_h;
            bool sel = (r == kb_row && c == kb_col);

            char buf[8];
            const char *label;
            if (r < KB_LETTER_ROWS) {
                char ch = KB_ROWS[r][c];
                buf[0] = kb_caps ? (char)toupper(ch) : ch;
                buf[1] = '\0';
                label  = buf;
            } else {
                label = SPECIAL[c].label;
            }

            if (sel) {
                char sel_buf[12];
                snprintf(sel_buf, sizeof(sel_buf), "[%s]", label);
                drawTTF((u32)(xk - CHAR_SIZE), (u32)yk, sel_buf, 14, 0x00FFFFFF);
            } else {
                drawTTF((u32)xk, (u32)yk, label, 14, 0x00FFFFFF);
            }
        }
    }

    drawTTF(40, 660, "Dpad:move  X:type  Tri:caps  Sq:del  START:done  SEL:cancel", 12, 0x00FFFFFF);
    flip();
}

// -------------------------------------------------------
// Lifecycle
// -------------------------------------------------------

void ttf_init(void) {
    bitmapSetXpm(&fontBitmap, font8x8_xpm);
    s_font_buf = (unsigned char*)OpenSans_Regular_ttf;
    if (stbtt_InitFont(&s_font, s_font_buf, 0))
        s_ttf_ok = true;
    if (stbtt_InitFont(&s_font_bold, (unsigned char*)OpenSans_Bold_ttf, 0))
        s_ttf_bold_ok = true;
    if (stbtt_InitFont(&s_icons, (unsigned char*)MaterialIcons_Regular_ttf, 0))
        s_icons_ok = true;
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
    int count = g_item_count[tab];
    int vis   = XMB_ITEMS_VIS;

    for (int i = 0; i < vis; i++) {
        int idx = g_scroll_top + i;
        if (idx >= count) break;
        const XMBItem *it = &g_items[tab][idx];

        int iy = XMB_CONTENT_Y + i * XMB_ITEM_H;

        int tx = XMB_ITEM_PAD + XMB_THUMB_W + 16;
        drawTTF((u32)tx, (u32)(iy + 8), it->name, 22, 0x00FFFFFF);

        xmb_draw_meta((u32)tx, (u32)(iy + 32), it);

        if (it->codec[0]) {
            int bx = (int)display_width - XMB_ITEM_PAD - 60;
            drawTTF((u32)bx, (u32)(iy + 10), it->codec, 13, 0x00FFFFFF);
        }

        if (i == 0 && g_scroll_top > 0)
            drawTTF(display_width - 20, (u32)iy, "^", 8, 0x00FFFFFF);
        if (i == vis-1 && g_scroll_top + vis < count)
            drawTTF(display_width - 20, (u32)iy, "v", 8, 0x00FFFFFF);
    }
}

// CPU draws for item list (highlight, thumb, badge bg)
void xmb_cpu_draw_items(int tab) {
    int count = g_item_count[tab];
    int vis   = XMB_ITEMS_VIS;
    int W     = (int)display_width;

    for (int i = 0; i < vis; i++) {
        int idx = g_scroll_top + i;
        if (idx >= count) break;

        int iy = XMB_CONTENT_Y + i * XMB_ITEM_H;

        if (idx == g_sel) {
            drawRect((u32)XMB_ITEM_PAD, (u32)iy,
                     (u32)(W - XMB_ITEM_PAD * 2), (u32)(XMB_ITEM_H - 2),
                     XMB_HIGHLIGHT);
            drawRect((u32)(XMB_ITEM_PAD - 5), (u32)iy,
                     4, (u32)(XMB_ITEM_H - 2), XMB_ACCENT);
        }

        drawRect((u32)(XMB_ITEM_PAD + 4), (u32)(iy + 8),
                 XMB_THUMB_W, XMB_THUMB_H, XMB_THUMB_DIM);

        drawRect((u32)(W - XMB_ITEM_PAD - 70), (u32)(iy + 6),
                 66, 22, XMB_BADGE_BG);
    }
}

// CPU draws for the TV sub-item list (seasons or episodes)
void xmb_cpu_draw_sub(void) {
    int vis = XMB_ITEMS_VIS;
    int W   = (int)display_width;
    int y0  = XMB_CONTENT_Y + 26;
    for (int i = 0; i < vis; i++) {
        int idx = g_tv_sub_scroll + i;
        if (idx >= g_tv_sub_count) break;
        int iy = y0 + i * XMB_ITEM_H;
        if (idx == g_tv_sub_sel) {
            drawRect((u32)XMB_ITEM_PAD, (u32)iy,
                     (u32)(W - XMB_ITEM_PAD * 2), (u32)(XMB_ITEM_H - 2), XMB_HIGHLIGHT);
            drawRect((u32)(XMB_ITEM_PAD - 5), (u32)iy, 4, (u32)(XMB_ITEM_H - 2), XMB_ACCENT);
        }
        drawRect((u32)(XMB_ITEM_PAD + 4), (u32)(iy + 8), XMB_THUMB_W, XMB_THUMB_H, XMB_THUMB_DIM);
        drawRect((u32)(W - XMB_ITEM_PAD - 70), (u32)(iy + 6), 66, 22, XMB_BADGE_BG);
    }
}

// TTF draws for the TV sub-item list
void xmb_draw_sub_list(void) {
    int vis = XMB_ITEMS_VIS;
    int y0  = XMB_CONTENT_Y + 26;
    for (int i = 0; i < vis; i++) {
        int idx = g_tv_sub_scroll + i;
        if (idx >= g_tv_sub_count) break;
        const XMBItem *it = &g_tv_sub_items[idx];
        int iy = y0 + i * XMB_ITEM_H;
        int tx = XMB_ITEM_PAD + XMB_THUMB_W + 16;
        drawTTF((u32)tx, (u32)(iy + 8), it->name, 22, 0x00FFFFFF);
        xmb_draw_meta((u32)tx, (u32)(iy + 32), it);
        if (it->codec[0]) {
            int bx = (int)display_width - XMB_ITEM_PAD - 60;
            drawTTF((u32)bx, (u32)(iy + 10), it->codec, 13, 0x00FFFFFF);
        }
        if (i == 0 && g_tv_sub_scroll > 0)
            drawTTF(display_width - 20, (u32)iy, "^", 8, 0x00FFFFFF);
        if (i == vis-1 && g_tv_sub_scroll + vis < g_tv_sub_count)
            drawTTF(display_width - 20, (u32)iy, "v", 8, 0x00FFFFFF);
    }
}

// CPU draws for the Collections sub-item list (movies inside a collection)
void xmb_cpu_draw_col_sub(void) {
    int vis = XMB_ITEMS_VIS;
    int W   = (int)display_width;
    int y0  = XMB_CONTENT_Y + 26;
    for (int i = 0; i < vis; i++) {
        int idx = g_col_sub_scroll + i;
        if (idx >= g_col_sub_count) break;
        int iy = y0 + i * XMB_ITEM_H;
        if (idx == g_col_sub_sel) {
            drawRect((u32)XMB_ITEM_PAD, (u32)iy,
                     (u32)(W - XMB_ITEM_PAD * 2), (u32)(XMB_ITEM_H - 2), XMB_HIGHLIGHT);
            drawRect((u32)(XMB_ITEM_PAD - 5), (u32)iy, 4, (u32)(XMB_ITEM_H - 2), XMB_ACCENT);
        }
        drawRect((u32)(XMB_ITEM_PAD + 4), (u32)(iy + 8), XMB_THUMB_W, XMB_THUMB_H, XMB_THUMB_DIM);
        drawRect((u32)(W - XMB_ITEM_PAD - 70), (u32)(iy + 6), 66, 22, XMB_BADGE_BG);
    }
}

// TTF draws for the Collections sub-item list
void xmb_draw_col_sub_list(void) {
    int vis = XMB_ITEMS_VIS;
    int y0  = XMB_CONTENT_Y + 26;
    for (int i = 0; i < vis; i++) {
        int idx = g_col_sub_scroll + i;
        if (idx >= g_col_sub_count) break;
        const XMBItem *it = &g_col_sub_items[idx];
        int iy = y0 + i * XMB_ITEM_H;
        int tx = XMB_ITEM_PAD + XMB_THUMB_W + 16;
        drawTTF((u32)tx, (u32)(iy + 8), it->name, 22, 0x00FFFFFF);
        xmb_draw_meta((u32)tx, (u32)(iy + 32), it);
        if (it->codec[0]) {
            int bx = (int)display_width - XMB_ITEM_PAD - 60;
            drawTTF((u32)bx, (u32)(iy + 10), it->codec, 13, 0x00FFFFFF);
        }
        if (i == 0 && g_col_sub_scroll > 0)
            drawTTF(display_width - 20, (u32)iy, "^", 8, 0x00FFFFFF);
        if (i == vis-1 && g_col_sub_scroll + vis < g_col_sub_count)
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
    int vis_r = ((int)display_height - XMB_BOTTOM_PAD - results_y) / XMB_ITEM_H;
    if (vis_r < 0) vis_r = 0;
    for (int i = 0; i < vis_r && i < count; i++) {
        const XMBItem *it = &g_search_results[i];
        int iy = results_y + i * XMB_ITEM_H;
        drawTTF((u32)(XMB_ITEM_PAD + XMB_THUMB_W + 16), (u32)(iy + 8), it->name, 18, 0x00FFFFFF);
        xmb_draw_meta((u32)(XMB_ITEM_PAD + XMB_THUMB_W + 16), (u32)(iy + 30), it, 18);
    }
    if (count == 0 && g_search_buf[0]) {
        drawTTF((u32)XMB_ITEM_PAD, (u32)results_y, "No results.", 8, 0x00FFFFFF);
    }
}

// CPU draws for search results list (thumbs)
void xmb_cpu_draw_search_results(void) {
    int results_y = OSK_Y0 + (OSK_ROWS_N + 1) * OSK_STEP_Y + 20;
    int count = g_search_results_count;
    int vis_r = ((int)display_height - XMB_BOTTOM_PAD - results_y) / XMB_ITEM_H;
    if (vis_r < 0) vis_r = 0;
    for (int i = 0; i < vis_r && i < count; i++) {
        int iy = results_y + i * XMB_ITEM_H;
        drawRect((u32)(XMB_ITEM_PAD + 4), (u32)(iy + 8), XMB_THUMB_W, XMB_THUMB_H, XMB_THUMB_DIM);
    }
}
