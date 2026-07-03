// Text and glyph rendering — bitmap font, Open Sans TTF, Material Icons,
// Iconic PSx.  Owns all font state and the stb_truetype implementation.

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <math.h>

#include <rsx/rsx.h>

#include "ui_visuals.h"
#include "bitmap.h"
#include "font8x8.xpm"
#include "opensans_regular.h"
#include "opensans_bold.h"
#include "material_icons.h"
#include "iconic_psx.h"

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

static void gamma_init(void) {
    for (int i = 0; i < 256; i++) {
        s_g2l[i] = (u8)((i * i) / 255);
        s_l2g[i] = (u8)(sqrtf((float)i / 255.0f) * 255.0f + 0.5f);
    }
}

// Composite one foreground channel over a background channel at coverage a.
static inline u8 aa_blend(u8 a, u8 fg, u8 bg) {
    return s_l2g[(a * s_g2l[fg] + (255 - a) * s_g2l[bg]) / 255];
}

// -------------------------------------------------------
// Bitmap (8x8) font — RSX transfer-scale blits
// -------------------------------------------------------

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

// -------------------------------------------------------
// TTF text rendering (CPU write — call after rsxSync, before flip)
// color: 0x00RRGGBB.  Falls back to drawTextScaled if font not loaded.
// -------------------------------------------------------

int ttf_text_width(const char *text, float px, bool bold) {
    if (!s_ttf_ok) return (int)(strlen(text) * px);
    stbtt_fontinfo *fi = (bold && s_ttf_bold_ok) ? &s_font_bold : &s_font;
    float scale = stbtt_ScaleForPixelHeight(fi, px);
    float xf = 0.0f;
    int prev_cp = 0;
    while (*text) {
        int cp = (unsigned char)*text;
        if (prev_cp) xf += stbtt_GetCodepointKernAdvance(fi, prev_cp, cp) * scale;
        int advance;
        stbtt_GetCodepointHMetrics(fi, cp, &advance, NULL);
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

            bool rt = cpu_rt_on();
            u32  tw_ = cpu_draw_w();
            for (int gy = 0; gy < h; gy++) {
                int sy = draw_y0 + gy;
                if (cpu_row_clipped(sy)) continue;
                u32 *row = cpu_draw_row((u32)sy);
                for (int gx = 0; gx < w; gx++) {
                    int sx = draw_x0 + gx;
                    if (sx < 0 || (u32)sx >= tw_) continue;
                    u32 a = bm[gy * w + gx];
                    if (a == 0) continue;
                    if (rt) { row[sx] = argb_over(row[sx], color, a); continue; }
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
    bool rt = cpu_rt_on();
    u32  tw_ = cpu_draw_w();
    for (int gy = 0; gy < h; gy++) {
        int sy = draw_y0 + gy;
        if (cpu_row_clipped(sy)) continue;
        u32 *row = cpu_draw_row((u32)sy);
        for (int gx = 0; gx < w; gx++) {
            int sx = draw_x0 + gx;
            if (sx < 0 || (u32)sx >= tw_) continue;
            u32 a = bm[gy * w + gx];
            if (a == 0) continue;
            if (rt) { row[sx] = argb_over(row[sx], color, a); continue; }
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
// Iconic PSx glyphs (controller buttons)
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
    bool rt = cpu_rt_on();
    u32  tw_ = cpu_draw_w();
    u32  th_ = cpu_draw_h();
    for (int gy = 0; gy < h; gy++) {
        int sy = dy0 + gy;
        if (sy < 0 || (u32)sy >= th_) continue;
        u32 *row = cpu_draw_row((u32)sy);
        for (int gx = 0; gx < w; gx++) {
            int sx = dx0 + gx;
            if (sx < 0 || (u32)sx >= tw_) continue;
            u32 a = bm[gy * w + gx];
            if (a == 0) continue;
            if (rt) { row[sx] = argb_over(row[sx], color, a); continue; }
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

// Draw one Iconic PSx glyph with its ink vertically centred on cy.
// draw_iconic_glyph()'s y is the top of the em box, but a glyph's ink only
// covers part of that, so em-box centring sits visibly off — centre on the
// actual bitmap box instead.
void draw_iconic_glyph_vcentered(u32 x, int cy, char glyph, float px, u32 color) {
    if (!s_iconic_ok) return;
    float scale = stbtt_ScaleForPixelHeight(&s_iconic, px);
    int ascent;
    stbtt_GetFontVMetrics(&s_iconic, &ascent, NULL, NULL);
    int baseline = (int)((float)ascent * scale);
    int x0, y0, x1, y1;
    stbtt_GetCodepointBitmapBox(&s_iconic, (unsigned char)glyph,
                                scale, scale, &x0, &y0, &x1, &y1);
    // draw_iconic_glyph puts the ink top at y + baseline + y0; pick y so the
    // ink spans [cy - h/2, cy + h/2).
    int y = cy - (y1 - y0) / 2 - baseline - y0;
    if (y < 0) y = 0;
    draw_iconic_glyph(x, (u32)y, glyph, px, color);
}

// Return the advance width in pixels for one Iconic PSx glyph.
int iconic_adv_px(char glyph, float px) {
    if (!s_iconic_ok) return (int)px;
    float sc = stbtt_ScaleForPixelHeight(&s_iconic, px);
    int adv;
    stbtt_GetCodepointHMetrics(&s_iconic, (unsigned char)glyph, &adv, NULL);
    return (int)((float)adv * sc);
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
