// Drawing primitives — RSX clears plus direct-to-framebuffer CPU fills,
// and small string utilities used by the draw paths.

#include <stdlib.h>
#include <ctype.h>

#include <rsx/rsx.h>

#include "ui_visuals.h"
#include "ui_wave.h"

// -------------------------------------------------------
// RSX drawing
// -------------------------------------------------------

void cpuClearFb(u32 color);   // defined below; used by clearScreen on emulator

void clearScreen(u32 color) {
    // Where CPU framebuffer writes are cheap (emulator), clear on the CPU so no
    // GPU op owns the display surface — otherwise RPCS3 presents the cached GPU
    // surface on flip and drops every CPU-drawn UI element.  See ui_wave.cpp.
    if (ui_cpu_bg()) { cpuClearFb(color); return; }
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

// JSON \uXXXX escapes -> UTF-8, rewritten in place.
//
// This used to write '?' for every codepoint outside printable ASCII, which is
// the second half of the mangled-apostrophe bug: even with the renderer now
// decoding UTF-8 (utf8_next() in render/ui_text.cpp), a title that arrived
// escaped would still have lost its apostrophes and accents here, before the
// renderer ever saw it.
//
// The rewrite always shrinks -- six bytes of escape become at most three of
// UTF-8, and a twelve-byte surrogate pair becomes four -- so dst can never
// overtake src and encoding in place is safe.
static inline int hex4(const char *h) {
    int v = 0;
    for (int i = 0; i < 4; i++) {
        char c = h[i];
        v = (v << 4) | (c <= '9' ? c - '0' : (c | 32) - 'a' + 10);
    }
    return v;
}

static inline bool is_u_escape(const char *s) {
    return s[0] == '\\' && s[1] == 'u' &&
           isxdigit((unsigned char)s[2]) && isxdigit((unsigned char)s[3]) &&
           isxdigit((unsigned char)s[4]) && isxdigit((unsigned char)s[5]);
}

void decode_unicode_escapes(char *str) {
    char *src = str, *dst = str;
    while (*src) {
        if (!is_u_escape(src)) { *dst++ = *src++; continue; }

        int code = hex4(src + 2);
        src += 6;

        // Surrogate pair -- 😀 is ONE codepoint above the BMP, and
        // only the pair is meaningful.  A high half with no low half after it
        // falls through to the '?' below rather than being encoded, since an
        // encoded surrogate is not a codepoint and utf8_next() would reject it.
        if (code >= 0xD800 && code <= 0xDBFF && is_u_escape(src)) {
            int lo = hex4(src + 2);
            if (lo >= 0xDC00 && lo <= 0xDFFF) {
                code = 0x10000 + ((code - 0xD800) << 10) + (lo - 0xDC00);
                src += 6;
            }
        }

        if (code < 0x80) {
            *dst++ = (char)code;
        } else if (code < 0x800) {
            *dst++ = (char)(0xC0 | (code >>  6));
            *dst++ = (char)(0x80 | (code        & 0x3F));
        } else if (code >= 0xD800 && code <= 0xDFFF) {
            *dst++ = '?';                       // unpaired surrogate half
        } else if (code < 0x10000) {
            *dst++ = (char)(0xE0 | (code >> 12));
            *dst++ = (char)(0x80 | ((code >> 6) & 0x3F));
            *dst++ = (char)(0x80 | (code        & 0x3F));
        } else {
            *dst++ = (char)(0xF0 | (code >> 18));
            *dst++ = (char)(0x80 | ((code >> 12) & 0x3F));
            *dst++ = (char)(0x80 | ((code >>  6) & 0x3F));
            *dst++ = (char)(0x80 | (code         & 0x3F));
        }
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

// Opaque bitmap blit -- see ui.h. Mirrors drawRect's own target/scissor
// handling exactly (dw/dh clamp then clip_rows), just copying source pixels
// row by row instead of filling one flat color.
void drawBitmapRect(const u32 *src, u32 src_stride,
                    u32 sx, u32 sy, u32 sw, u32 sh, u32 dx, u32 dy) {
    if (!src || sw == 0 || sh == 0) return;
    u32 dw = cpu_draw_w(), dh = cpu_draw_h();
    if (dx >= dw || dy >= dh) return;
    u32 x2 = (dx + sw > dw) ? dw : dx + sw;
    u32 y2 = (dy + sh > dh) ? dh : dy + sh;
    u32 y0 = dy;
    clip_rows(&y0, &y2);
    u32 opaque = s_rt_buf ? 0xFF000000u : 0u;   // "solid fill" rule from drawRect
    for (u32 r = y0; r < y2; r++) {
        const u32 *srow = src + (size_t)(sy + (r - dy)) * src_stride + sx;
        u32 *drow = cpu_draw_row(r) + dx;
        for (u32 c = 0; c < x2 - dx; c++) drow[c] = srow[c] | opaque;
    }
}

// Per-pixel alpha bitmap blit -- see ui.h. Mirrors drawRectBlend's two
// blend formulas exactly (argb_over for the RT/compose-target case, the
// manual src-over-onto-opaque-RGB formula for the live framebuffer), just
// driven by each source pixel's own alpha instead of one flat value.
void drawBitmapRectAlpha(const u32 *src, u32 src_stride,
                         u32 sx, u32 sy, u32 sw, u32 sh, u32 dx, u32 dy) {
    if (!src || sw == 0 || sh == 0) return;
    u32 dw = cpu_draw_w(), dh = cpu_draw_h();
    if (dx >= dw || dy >= dh) return;
    u32 x2 = (dx + sw > dw) ? dw : dx + sw;
    u32 y2 = (dy + sh > dh) ? dh : dy + sh;
    u32 y0 = dy;
    clip_rows(&y0, &y2);
    bool rt = (s_rt_buf != NULL);
    for (u32 r = y0; r < y2; r++) {
        const u32 *srow = src + (size_t)(sy + (r - dy)) * src_stride + sx;
        u32 *drow = cpu_draw_row(r);
        for (u32 c = dx; c < x2; c++) {
            u32 sp = srow[c - dx];
            u32 a  = (sp >> 24) & 0xFF;
            if (a == 0) continue;
            if (rt) {
                drow[c] = argb_over(drow[c], sp, a);
            } else if (a == 255) {
                drow[c] = sp & 0x00FFFFFFu;
            } else {
                u32 bg = drow[c], ia = 255 - a;
                u32 rr = (a * ((sp >> 16) & 0xFF) + ia * ((bg >> 16) & 0xFF)) / 255;
                u32 gg = (a * ((sp >>  8) & 0xFF) + ia * ((bg >>  8) & 0xFF)) / 255;
                u32 bb = (a * ( sp        & 0xFF) + ia * ( bg        & 0xFF)) / 255;
                drow[c] = (rr << 16) | (gg << 8) | bb;
            }
        }
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
