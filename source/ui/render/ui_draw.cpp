// Drawing primitives — RSX clears plus direct-to-framebuffer CPU fills,
// and small string utilities used by the draw paths.

#include <stdlib.h>
#include <ctype.h>

#include <rsx/rsx.h>

#include "ui_visuals.h"

// -------------------------------------------------------
// RSX drawing
// -------------------------------------------------------

void clearScreen(u32 color) {
    rsxSetClearColor(context, color);
    rsxSetClearDepthStencil(context, 0xffff);
    rsxClearSurface(context,
        GCM_CLEAR_R|GCM_CLEAR_G|GCM_CLEAR_B|GCM_CLEAR_A|GCM_CLEAR_S|GCM_CLEAR_Z);
}

void drawHeader(void) {
    clearScreen(XMB_BG);
    rsxSync();
    drawTTF(XMB_ITEM_PAD, 20, "Jellyfin", 22, XMB_TEXT, true);
    drawTTF(XMB_ITEM_PAD + ttf_text_width("Jellyfin", 22, true) + 8, 27, "PS3",
            13, XMB_ACCENT, true);
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

// CPU-draw vertical scissor (see ui.h).  Defaults cover the whole frame.
int g_cpu_clip_top = 0;
int g_cpu_clip_bot = 0;   // 0 == bottom edge

// CPU compose target (see ui.h).  NULL = draw to the framebuffer.
static u32 *s_rt_buf = NULL;
static u32  s_rt_w   = 0;
static u32  s_rt_h   = 0;

void cpu_rt_begin(u32 *buf, u32 w, u32 h) { s_rt_buf = buf; s_rt_w = w; s_rt_h = h; }
void cpu_rt_end(void)                     { s_rt_buf = NULL; }
bool cpu_rt_on(void)                      { return s_rt_buf != NULL; }
u32  cpu_draw_w(void)                     { return s_rt_buf ? s_rt_w : display_width; }
u32  cpu_draw_h(void)                     { return s_rt_buf ? s_rt_h : display_height; }
u32 *cpu_draw_row(u32 y) {
    return s_rt_buf ? s_rt_buf + y * s_rt_w
                    : color_buffer[curr_fb] + y * display_width;
}

bool cpu_row_clipped(int sy) {
    if (s_rt_buf)   // scissor is a framebuffer concept; RT clips to its bounds
        return (sy < 0 || sy >= (int)s_rt_h);
    if (sy < g_cpu_clip_top || sy >= (int)display_height) return true;
    if (g_cpu_clip_bot && sy >= g_cpu_clip_bot)           return true;
    return false;
}

// Clamp a fill's [y, y2) row span to the active scissor.
static void clip_rows(u32 *y, u32 *y2) {
    if (s_rt_buf) return;
    if ((int)*y  < g_cpu_clip_top)                       *y  = (u32)g_cpu_clip_top;
    if (g_cpu_clip_bot && *y2 > (u32)g_cpu_clip_bot)     *y2 = (u32)g_cpu_clip_bot;
}

void drawRect(u32 x, u32 y, u32 w, u32 h, u32 color) {
    u32 dw = cpu_draw_w(), dh = cpu_draw_h();
    if (x >= dw || y >= dh || w == 0 || h == 0) return;
    u32 x2 = (x + w > dw) ? dw : x + w;
    u32 y2 = (y + h > dh) ? dh : y + h;
    clip_rows(&y, &y2);
    if (s_rt_buf) color |= 0xFF000000u;   // solid fill: opaque in the RT
    for (u32 r = y; r < y2; r++) {
        u32 *p = cpu_draw_row(r) + x;
        u32  n = x2 - x;
        for (u32 c = 0; c < n; c++) p[c] = color;
    }
}

void drawRectBlend(u32 x, u32 y, u32 w, u32 h, u32 color, u8 alpha) {
    u32 dw = cpu_draw_w(), dh = cpu_draw_h();
    if (x >= dw || y >= dh || w == 0 || h == 0) return;
    if (alpha == 0) return;
    if (alpha == 255) { drawRect(x, y, w, h, color); return; }
    u32 x2 = (x + w > dw) ? dw : x + w;
    u32 y2 = (y + h > dh) ? dh : y + h;
    clip_rows(&y, &y2);
    if (s_rt_buf) {
        for (u32 r = y; r < y2; r++) {
            u32 *p = cpu_draw_row(r);
            for (u32 c = x; c < x2; c++)
                p[c] = argb_over(p[c], color, alpha);
        }
        return;
    }
    u32 r_fg = (color >> 16) & 0xFF;
    u32 g_fg = (color >>  8) & 0xFF;
    u32 b_fg =  color        & 0xFF;
    u32 a = alpha, ia = 255 - a;
    for (u32 r = y; r < y2; r++) {
        u32 *p = color_buffer[curr_fb] + r * display_width;
        for (u32 c = x; c < x2; c++) {
            u32 bg = p[c];
            p[c] = (((a*r_fg + ia*((bg>>16)&0xFF))/255) << 16) |
                   (((a*g_fg + ia*((bg>> 8)&0xFF))/255) <<  8) |
                    ((a*b_fg + ia*( bg     &0xFF))/255);
        }
    }
}

void cpuClearFb(u32 color) {
    u32 *fb = color_buffer[curr_fb];
    u32  n  = display_width * display_height;
    for (u32 i = 0; i < n; i++) fb[i] = color;
}
