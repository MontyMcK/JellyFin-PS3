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
