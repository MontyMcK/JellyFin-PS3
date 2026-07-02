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

bool cpu_row_clipped(int sy) {
    if (sy < g_cpu_clip_top || sy >= (int)display_height) return true;
    if (g_cpu_clip_bot && sy >= g_cpu_clip_bot)           return true;
    return false;
}

// Clamp a fill's [y, y2) row span to the active scissor.
static void clip_rows(u32 *y, u32 *y2) {
    if ((int)*y  < g_cpu_clip_top)                       *y  = (u32)g_cpu_clip_top;
    if (g_cpu_clip_bot && *y2 > (u32)g_cpu_clip_bot)     *y2 = (u32)g_cpu_clip_bot;
}

void drawRect(u32 x, u32 y, u32 w, u32 h, u32 color) {
    if (x >= display_width || y >= display_height || w == 0 || h == 0) return;
    u32 x2 = (x + w > display_width)  ? display_width  : x + w;
    u32 y2 = (y + h > display_height) ? display_height : y + h;
    clip_rows(&y, &y2);
    for (u32 r = y; r < y2; r++) {
        u32 *p = color_buffer[curr_fb] + r * display_width + x;
        u32  n = x2 - x;
        for (u32 c = 0; c < n; c++) p[c] = color;
    }
}

void drawRectBlend(u32 x, u32 y, u32 w, u32 h, u32 color, u8 alpha) {
    if (x >= display_width || y >= display_height || w == 0 || h == 0) return;
    if (alpha == 0) return;
    if (alpha == 255) { drawRect(x, y, w, h, color); return; }
    u32 x2 = (x + w > display_width)  ? display_width  : x + w;
    u32 y2 = (y + h > display_height) ? display_height : y + h;
    clip_rows(&y, &y2);
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
