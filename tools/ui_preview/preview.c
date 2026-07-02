// Host-side preview renderer for the Jellyfin-PS3 XMB redesign.
// Transcribes the draw primitives and layout math from source/ui so the
// new look can be checked pixel-accurately without a PS3.
// Build: gcc -O2 preview.c -lm -o preview && ./preview
// Output: PPM frames in the current directory.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <stdint.h>
#include <ctype.h>

#define STB_TRUETYPE_IMPLEMENTATION
#include "../../source/gfx/stb_truetype.h"

typedef uint32_t u32;
typedef uint8_t  u8;

#define W 1280
#define H 720
static u32 fb[W * H];
static const u32 display_width = W, display_height = H;

// ---- palette / layout straight from ui_visuals.h ----
#define XMB_BG          0x000B0E1EUL
#define XMB_BG_TOP      0x00151A38UL
#define XMB_BG_BOT      0x0005060CUL
#define XMB_ACCENT      0x008F6FE8UL
#define XMB_ACCENT_DEEP 0x005B43A8UL
#define XMB_TEXT        0x00E9EBF5UL
#define XMB_TEXT_DIM    0x0099A0BCUL
#define XMB_TEXT_FAINT  0x005C6386UL
#define XMB_WHITE       0x00FFFFFFUL
#define XMB_PANEL       0x00191E3CUL
#define XMB_PANEL_HI    0x00232950UL
#define XMB_HAIRLINE    0x002C3258UL
#define XMB_THUMB_DIM   0x0012162CUL
#define XMB_TRACK       0x0010142AUL
#define XMB_KEY_NORMAL  0x001A1F3EUL
#define XMB_KEY_SEL     0x00F0F2FAUL
#define XMB_KEY_LABEL_SEL 0x0010142AUL
#define XMB_ICON_IDLE   0x00646C96UL

#define XMB_TOPBAR_H    64
#define XMB_TABBAR_H    80
#define XMB_DIVIDER_Y   (XMB_TOPBAR_H + XMB_TABBAR_H)
#define XMB_CONTENT_Y   (XMB_DIVIDER_Y + 30)
#define XMB_BOTTOM_PAD  70
#define XMB_ITEM_PAD    40
#define XMB_LIST_W      780
#define XMB_ROW_H        88
#define XMB_ROW_GAP      16
#define XMB_ROW_STRIDE  (XMB_ROW_H + XMB_ROW_GAP)
#define XMB_THUMB_W     52
#define XMB_THUMB_H     74

#define XMB_GRID_ROWS       2
#define XMB_PORTRAIT_COLS   5
#define XMB_CARD_GAP_X   24
#define XMB_CARD_TEXT_H  50
#define XMB_GRID_Y0      (XMB_CONTENT_Y + 8)
#define XMB_GRID_AVAIL_H ((int)display_height - XMB_BOTTOM_PAD - XMB_GRID_Y0 - 26)
#define XMB_CARD_H_FIT   (XMB_GRID_AVAIL_H / XMB_GRID_ROWS - XMB_CARD_TEXT_H - 6)

#define JBAR_ENTRIES 27
#define JBAR_W       20
#define JBAR_GAP      8

#define OSK_ROWS_N 4
#define OSK_KEY_W    80
#define OSK_KEY_H    44
#define OSK_GAP       8
#define OSK_STEP_X   (OSK_KEY_W + OSK_GAP)
#define OSK_STEP_Y   (OSK_KEY_H + OSK_GAP)

#define XMB_TAB_COUNT 7

// ---- fonts ----
static stbtt_fontinfo s_font, s_font_bold, s_icons, s_iconic;
static u8 s_g2l[256], s_l2g[256];

static void gamma_init(void) {
    for (int i = 0; i < 256; i++) {
        s_g2l[i] = (u8)((i * i) / 255);
        s_l2g[i] = (u8)(sqrtf((float)i / 255.0f) * 255.0f + 0.5f);
    }
}
static inline u8 aa_blend(u8 a, u8 fg, u8 bg) {
    return s_l2g[(a * s_g2l[fg] + (255 - a) * s_g2l[bg]) / 255];
}

static unsigned char *load_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "missing %s\n", path); exit(1); }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    unsigned char *buf = malloc(n);
    fread(buf, 1, n, f); fclose(f);
    return buf;
}

// ---- primitives (transcribed) ----
static void drawRect(u32 x, u32 y, u32 w, u32 h, u32 color) {
    if (x >= W || y >= H || w == 0 || h == 0) return;
    u32 x2 = (x + w > W) ? W : x + w;
    u32 y2 = (y + h > H) ? H : y + h;
    for (u32 r = y; r < y2; r++)
        for (u32 c = x; c < x2; c++) fb[r * W + c] = color;
}

static void drawRectBlend(u32 x, u32 y, u32 w, u32 h, u32 color, u8 alpha) {
    if (x >= W || y >= H || w == 0 || h == 0 || alpha == 0) return;
    if (alpha == 255) { drawRect(x, y, w, h, color); return; }
    u32 x2 = (x + w > W) ? W : x + w;
    u32 y2 = (y + h > H) ? H : y + h;
    u32 rf = (color >> 16) & 0xFF, gf = (color >> 8) & 0xFF, bf = color & 0xFF;
    u32 a = alpha, ia = 255 - a;
    for (u32 r = y; r < y2; r++)
        for (u32 c = x; c < x2; c++) {
            u32 bg = fb[r * W + c];
            fb[r * W + c] = (((a*rf + ia*((bg>>16)&0xFF))/255) << 16) |
                            (((a*gf + ia*((bg>> 8)&0xFF))/255) <<  8) |
                             ((a*bf + ia*( bg     &0xFF))/255);
        }
}

static int ttf_text_width(const char *text, float px) {
    float scale = stbtt_ScaleForPixelHeight(&s_font, px);
    float xf = 0; int prev = 0;
    while (*text) {
        int cp = (unsigned char)*text;
        if (prev) xf += stbtt_GetCodepointKernAdvance(&s_font, prev, cp) * scale;
        int adv; stbtt_GetCodepointHMetrics(&s_font, cp, &adv, NULL);
        xf += adv * scale; prev = cp; text++;
    }
    return (int)xf;
}

static void drawTTF(u32 x, u32 y, const char *text, float px, u32 color, int bold) {
    stbtt_fontinfo *fi = bold ? &s_font_bold : &s_font;
    float scale = stbtt_ScaleForPixelHeight(fi, px);
    int ascent; stbtt_GetFontVMetrics(fi, &ascent, NULL, NULL);
    int baseline = (int)(ascent * scale);
    u32 rf = (color >> 16) & 0xFF, gf = (color >> 8) & 0xFF, bf = color & 0xFF;
    float xf = x; int prev = 0;
    while (*text) {
        int cp = (unsigned char)*text;
        if (prev) xf += stbtt_GetCodepointKernAdvance(fi, prev, cp) * scale;
        int w, h, xo, yo;
        unsigned char *bm = stbtt_GetCodepointBitmap(fi, scale, scale, cp, &w, &h, &xo, &yo);
        if (bm) {
            int x0 = (int)xf + xo, y0 = (int)y + baseline + yo;
            for (int gy = 0; gy < h; gy++) {
                int sy = y0 + gy; if (sy < 0 || sy >= H) continue;
                for (int gx = 0; gx < w; gx++) {
                    int sx = x0 + gx; if (sx < 0 || sx >= W) continue;
                    u32 a = bm[gy * w + gx]; if (!a) continue;
                    if (a == 255) { fb[sy * W + sx] = color; continue; }
                    u32 bg = fb[sy * W + sx];
                    fb[sy * W + sx] = (aa_blend(a, rf, (bg>>16)&0xFF) << 16) |
                                      (aa_blend(a, gf, (bg>> 8)&0xFF) <<  8) |
                                       aa_blend(a, bf,  bg     &0xFF);
                }
            }
            stbtt_FreeBitmap(bm, NULL);
        }
        int adv; stbtt_GetCodepointHMetrics(fi, cp, &adv, NULL);
        xf += adv * scale; prev = cp; text++;
    }
}

static void draw_glyph_font(stbtt_fontinfo *fi, u32 x, u32 y, int cp, float px, u32 color) {
    float scale = stbtt_ScaleForPixelHeight(fi, px);
    int ascent; stbtt_GetFontVMetrics(fi, &ascent, NULL, NULL);
    int baseline = (int)(ascent * scale);
    int w, h, xo, yo;
    unsigned char *bm = stbtt_GetCodepointBitmap(fi, scale, scale, cp, &w, &h, &xo, &yo);
    if (!bm) return;
    u32 rf = (color >> 16) & 0xFF, gf = (color >> 8) & 0xFF, bf = color & 0xFF;
    int x0 = (int)x + xo, y0 = (int)y + baseline + yo;
    for (int gy = 0; gy < h; gy++) {
        int sy = y0 + gy; if (sy < 0 || sy >= H) continue;
        for (int gx = 0; gx < w; gx++) {
            int sx = x0 + gx; if (sx < 0 || sx >= W) continue;
            u32 a = bm[gy * w + gx]; if (!a) continue;
            if (a == 255) { fb[sy * W + sx] = color; continue; }
            u32 bg = fb[sy * W + sx];
            u32 rb=(bg>>16)&0xFF, gb=(bg>>8)&0xFF, bb=bg&0xFF;
            fb[sy*W+sx] = (((a*rf+(255-a)*rb)/255)<<16) |
                          (((a*gf+(255-a)*gb)/255)<<8) |
                           ((a*bf+(255-a)*bb)/255);
        }
    }
    stbtt_FreeBitmap(bm, NULL);
}

static void drawIcon(u32 x, u32 y, int cp, float px, u32 color) {
    draw_glyph_font(&s_icons, x, y, cp, px, color);
}
static void draw_iconic_glyph(u32 x, u32 y, char g, float px, u32 color) {
    draw_glyph_font(&s_iconic, x, y, (unsigned char)g, px, color);
}
static int iconic_adv_px(char g, float px) {
    float sc = stbtt_ScaleForPixelHeight(&s_iconic, px);
    int adv; stbtt_GetCodepointHMetrics(&s_iconic, (unsigned char)g, &adv, NULL);
    return (int)(adv * sc);
}
static void draw_iconic_glyph_vcentered(u32 x, int cy, char g, float px, u32 color) {
    float scale = stbtt_ScaleForPixelHeight(&s_iconic, px);
    int ascent; stbtt_GetFontVMetrics(&s_iconic, &ascent, NULL, NULL);
    int baseline = (int)(ascent * scale);
    int x0, y0, x1, y1;
    stbtt_GetCodepointBitmapBox(&s_iconic, (unsigned char)g, scale, scale, &x0, &y0, &x1, &y1);
    int y = cy - (y1 - y0) / 2 - baseline - y0;
    if (y < 0) y = 0;
    draw_iconic_glyph(x, (u32)y, g, px, color);
}

// ---- background: gradient + translucent waves (mirrors ui_wave.cpp) ----
static void draw_background(void) {
    u8 tr=(XMB_BG_TOP>>16)&0xFF, tg=(XMB_BG_TOP>>8)&0xFF, tb=XMB_BG_TOP&0xFF;
    u8 br=(XMB_BG_BOT>>16)&0xFF, bg=(XMB_BG_BOT>>8)&0xFF, bb=XMB_BG_BOT&0xFF;
    for (int y = 0; y < H; y++) {
        float t = (float)y / (H - 1);
        u32 r = (u32)(tr + (br - tr) * t);
        u32 g = (u32)(tg + (bg - tg) * t);
        u32 b = (u32)(tb + (bb - tb) * t);
        u32 c = (r << 16) | (g << 8) | b;
        for (int x = 0; x < W; x++) fb[y * W + x] = c;
    }
    static const u32   C[3]  = { 0x004A52A8, 0x006C5BD4, 0x003A4290 };
    static const u8    A[3]  = { 56, 42, 72 };
    static const float AMP[3]  = { 30, 22, 15 };
    static const float FREQ[3] = { 1.5f, 1.8f, 2.1f };
    static const float BASE[3] = { 0.78f, 0.85f, 0.91f };  // low: bottom-quarter ribbons
    static const float PH[3]   = { 1.3f, 3.1f, 5.2f };  // arbitrary frozen phase
    for (int li = 0; li < 3; li++) {
        u32 rf=(C[li]>>16)&0xFF, gf=(C[li]>>8)&0xFF, bf=C[li]&0xFF;
        for (int x = 0; x < W; x++) {
            float wy = H*BASE[li] + sinf((float)x / W * (FREQ[li]*3.14159265f) + PH[li]) * AMP[li];
            int y0 = (int)wy; if (y0 < 0) y0 = 0;
            for (int y = y0; y < H; y++) {
                // vertex alpha interpolates from A at crest to 0 at bottom
                float t = (float)(y - y0) / (float)(H - y0);
                u32 a = (u32)(A[li] * (1.0f - t));
                if (!a) continue;
                u32 bgp = fb[y * W + x];
                fb[y*W+x] = (((a*rf + (255-a)*((bgp>>16)&0xFF))/255)<<16) |
                            (((a*gf + (255-a)*((bgp>> 8)&0xFF))/255)<<8) |
                             ((a*bf + (255-a)*( bgp     &0xFF))/255);
            }
        }
    }
}

// ---- chrome (mirrors ui_widgets.cpp) ----
static const int TAB_CP[XMB_TAB_COUNT] = {0xE8B6,0xE889,0xE54D,0xE333,0xE3A1,0xE8EF,0xE8B8};
static const char *TAB_LABELS[XMB_TAB_COUNT] =
    {"Search","Continue Watching","Movies","TV Shows","Music","Collections","Settings"};

static void xmb_draw_topbar(void) {
    drawTTF(XMB_ITEM_PAD, 20, "Jellyfin", 22, XMB_TEXT, 1);
    drawTTF(XMB_ITEM_PAD + ttf_text_width("Jellyfin", 22) + 8, 27, "PS3", 13, XMB_ACCENT, 1);
    const char *t_str = "21:34", *d_str = "13/6";
    int tw = ttf_text_width(t_str, 19), dw = ttf_text_width(d_str, 13);
    int tx = W - XMB_ITEM_PAD - tw;
    drawTTF(tx, 22, t_str, 19, XMB_TEXT_DIM, 0);
    drawTTF(tx - dw - 10, 27, d_str, 13, XMB_TEXT_FAINT, 0);
}

static void xmb_draw_divider(void) {
    u32 *row = fb + XMB_DIVIDER_Y * W;
    const u32 cr = 0x8A, cg = 0x93, cb = 0xC8;
    for (int x = 0; x < W; x++) {
        int d = x < W / 2 ? x : W - x;
        u32 a = (u32)(72 * d * 2 / W);
        if (!a) continue;
        u32 bg = row[x];
        row[x] = (((a*cr + (255-a)*((bg>>16)&0xFF))/255) << 16) |
                 (((a*cg + (255-a)*((bg>> 8)&0xFF))/255) <<  8) |
                  ((a*cb + (255-a)*( bg     &0xFF))/255);
    }
}

// `enabled` may be NULL to mean "all tabs present" (the usual preview case);
// pass a per-tab flag array to preview a server with missing libraries.
static void xmb_draw_tabs_ex(int active, const int *enabled) {
    const int SP = 96;
    const int icon_cy = XMB_TOPBAR_H + 30;

    // Pack only the enabled tabs and center that group (mirrors ui_widgets.cpp).
    int idx[XMB_TAB_COUNT], n = 0;
    for (int t = 0; t < XMB_TAB_COUNT; t++)
        if (!enabled || enabled[t]) idx[n++] = t;
    if (n == 0) return;

    const int gw = (n - 1) * SP;
    const int x0 = W / 2 - gw / 2;
    for (int i = 0; i < n; i++) {
        int t = idx[i];
        int cx = x0 + i * SP;
        int act = (t == active);
        int px = act ? 60 : 28;
        drawIcon(cx - px / 2, icon_cy - px / 2, TAB_CP[t], px, act ? XMB_TEXT : XMB_ICON_IDLE);
        if (act) {
            int lw = ttf_text_width(TAB_LABELS[t], 14);
            drawTTF(cx - lw / 2, XMB_TOPBAR_H + 60, TAB_LABELS[t], 14, XMB_TEXT, 0);
        }
    }
    const float lr = 26.0f;
    int lx = x0 - SP/2 - iconic_adv_px('l', lr)/2;
    int rx = x0 + gw + SP/2 - iconic_adv_px('r', lr)/2;
    draw_iconic_glyph_vcentered(lx, icon_cy, 'l', lr, XMB_TEXT_FAINT);
    draw_iconic_glyph_vcentered(rx, icon_cy, 'r', lr, XMB_TEXT_FAINT);
}

static void xmb_draw_tabs(int active) { xmb_draw_tabs_ex(active, NULL); }

typedef struct { char glyph; const char *label; } Hint;
static void draw_hints_bar(const Hint *hints, int n) {
    const float icon_px = 24.0f, text_px = 15.0f;
    const int gap_it = 7, gap_sep = 26;
    int total = 0;
    for (int i = 0; i < n; i++) {
        total += iconic_adv_px(hints[i].glyph, icon_px) + gap_it
               + ttf_text_width(hints[i].label, text_px);
        if (i < n-1) total += gap_sep;
    }
    int x = (W - total) / 2;
    int cy = H - XMB_BOTTOM_PAD + 34;
    for (int i = 0; i < n; i++) {
        draw_iconic_glyph_vcentered(x, cy, hints[i].glyph, icon_px, 0x00AEB5D2);
        x += iconic_adv_px(hints[i].glyph, icon_px) + gap_it;
        drawTTF(x, cy - (int)(text_px*0.55f), hints[i].label, text_px, XMB_TEXT_DIM, 0);
        x += ttf_text_width(hints[i].label, text_px);
        if (i < n-1) x += gap_sep;
    }
}

static void xmb_draw_jumpbar(int gx0, int stride) {
    int bar_top = XMB_GRID_Y0;
    int bar_bot = XMB_GRID_Y0 + XMB_GRID_ROWS * stride - XMB_CARD_TEXT_H;
    int bar_h = bar_bot - bar_top;
    int jx = gx0 - JBAR_GAP*3 - JBAR_W;
    float eh = (float)bar_h / JBAR_ENTRIES;
    float fpx = eh * 1.2f; if (fpx < 12) fpx = 12; if (fpx > 28) fpx = 28;
    static const char *L[27] = {"#","A","B","C","D","E","F","G","H","I","J","K","L","M",
        "N","O","P","Q","R","S","T","U","V","W","X","Y","Z"};
    for (int i = 0; i < 27; i++) {
        int ty = bar_top + (int)(i*eh) + (int)((eh-fpx)*0.5f);
        drawTTF(jx, ty, L[i], fpx, 0x00363D63, 0);
    }
}

// fake poster: vertical two-tone + big initial
static void fake_poster(int x, int y, int w, int h, u32 c1, u32 c2, const char *ini) {
    for (int r = 0; r < h; r++) {
        float t = (float)r / h;
        u32 cr = (u32)(((c1>>16)&0xFF)*(1-t) + ((c2>>16)&0xFF)*t);
        u32 cg = (u32)(((c1>> 8)&0xFF)*(1-t) + ((c2>> 8)&0xFF)*t);
        u32 cb = (u32)(( c1     &0xFF)*(1-t) + ( c2     &0xFF)*t);
        drawRect(x, y+r, w, 1, (cr<<16)|(cg<<8)|cb);
    }
    int iw = ttf_text_width(ini, 60);
    drawTTF(x + (w-iw)/2, y + h/2 - 36, ini, 60, 0x00FFFFFF & 0x00DDDDEE, 1);
}

static void save_ppm(const char *path) {
    FILE *f = fopen(path, "wb");
    fprintf(f, "P6\n%d %d\n255\n", W, H);
    for (int i = 0; i < W*H; i++) {
        u8 px[3] = { (fb[i]>>16)&0xFF, (fb[i]>>8)&0xFF, fb[i]&0xFF };
        fwrite(px, 1, 3, f);
    }
    fclose(f);
}

// ---- screens ----
static void screen_movies(void) {
    draw_background();
    xmb_draw_divider();
    xmb_draw_topbar();
    xmb_draw_tabs(2);

    // grid geometry (portrait)
    int card_h = XMB_CARD_H_FIT;
    int card_w = card_h * 2 / 3;
    int cols = XMB_PORTRAIT_COLS;
    int stride = card_h + XMB_CARD_TEXT_H + 6;
    int grid_w = cols*card_w + (cols-1)*XMB_CARD_GAP_X;
    int gx0 = (W - grid_w) / 2;

    static const char *titles[10] = {"Interstellar","Dune","Arrival","Blade Runner 2049",
        "The Matrix","Inception","Alien","Heat","Drive","Tenet"};
    static const char *metas[10]  = {"2014 \xB7 2h 49m \xB7 Sci-Fi","","","","","","","","",""};
    static const u32 pc[10][2] = {
        {0x23406b,0x0d1526},{0x7a4a21,0x2a1505},{0x2e4f4a,0x101e1c},{0x6b3a14,0x1c0f06},
        {0x1d3b22,0x0a140c},{0x37415e,0x12141f},{0x3a3a45,0x121218},{0x2b3a4d,0x0e131a},
        {0x5e2438,0x1c0a10},{0x2f4358,0x0f161e}};
    int sel = 0;
    for (int i = 0; i < cols*XMB_GRID_ROWS; i++) {
        int cx = gx0 + (i % cols) * (card_w + XMB_CARD_GAP_X);
        int cy = XMB_GRID_Y0 + (i / cols) * stride;
        if (i == 7) {  // one loading placeholder
            drawRect(cx, cy, card_w, card_h, XMB_THUMB_DIM);
            drawIcon(cx + (card_w-32)/2, cy + (card_h-32)/2, 0xE3F4, 32, 0x00262C4E);
        } else {
            char ini[2] = { titles[i][0], 0 };
            fake_poster(cx, cy, card_w, card_h, pc[i][0], pc[i][1], ini);
        }
        if (i == 2) { // progress bar demo
            drawRect(cx, cy+card_h-4, card_w, 4, XMB_TRACK);
            drawRect(cx, cy+card_h-4, card_w*62/100, 4, XMB_ACCENT);
        }
        if (i == sel) {
            const int T=2, G=2, O=G+T;
            drawRect(cx-O, cy-O, card_w+2*O, T, XMB_KEY_SEL);
            drawRect(cx-O, cy+card_h+G, card_w+2*O, T, XMB_KEY_SEL);
            drawRect(cx-O, cy-G, T, card_h+2*G, XMB_KEY_SEL);
            drawRect(cx+card_w+G, cy-G, T, card_h+2*G, XMB_KEY_SEL);
            drawRectBlend(cx-O-1, cy-O-1, card_w+2*O+2, 1, XMB_KEY_SEL, 70);
            drawRectBlend(cx-O-1, cy+card_h+O, card_w+2*O+2, 1, XMB_KEY_SEL, 70);
            drawRectBlend(cx-O-1, cy-O, 1, card_h+2*O, XMB_KEY_SEL, 70);
            drawRectBlend(cx+card_w+O, cy-O, 1, card_h+2*O, XMB_KEY_SEL, 70);
        }
        // titles
        if (i == sel) {
            drawTTF(cx, cy+card_h+7, titles[i], 20, XMB_WHITE, 1);
            if (metas[i][0]) drawTTF(cx, cy+card_h+7+27, metas[i], 14, XMB_TEXT_DIM, 0);
        } else {
            drawTTF(cx, cy+card_h+8, titles[i], 16, XMB_TEXT_DIM, 0);
        }
    }
    xmb_draw_jumpbar(gx0, stride);
    drawIcon(gx0+grid_w+10, XMB_GRID_Y0 + XMB_GRID_ROWS*stride - 60, 0xE313, 22, XMB_ICON_IDLE);

    Hint h[] = {{'E',"Jump"},{'X',"Select"},{'T',"Info"}};
    draw_hints_bar(h, 3);
    save_ppm("1_movies.ppm");
}

static void screen_search(void) {
    draw_background();
    xmb_draw_divider();
    xmb_draw_topbar();
    xmb_draw_tabs(0);

    int total_w = 10*OSK_STEP_X - OSK_GAP;
    int x0 = (W - total_w)/2;
    int OSK_Y0 = XMB_CONTENT_Y + 58;
    // field
    drawRect(x0, XMB_CONTENT_Y+8, total_w, 40, 0x00131830);
    drawRect(x0, XMB_CONTENT_Y+8+38, total_w, 2, XMB_ACCENT);
    drawIcon(x0+14, XMB_CONTENT_Y+8+10, 0xE8B6, 20, XMB_TEXT_FAINT);
    drawTTF(x0+44, XMB_CONTENT_Y+8+9, "_", 18, XMB_TEXT, 0);
    drawTTF(x0+58, XMB_CONTENT_Y+8+9, "Search your library", 17, XMB_TEXT_FAINT, 0);

    static const char *R[4] = {"abcdefghij","klmnopqrst","uvwxyz0123","456789"};
    int sel_r = 1, sel_c = 2;
    for (int r = 0; r <= OSK_ROWS_N; r++) {
        int ry = OSK_Y0 + r*OSK_STEP_Y;
        if (r == OSK_ROWS_N) {
            int sw = 5*OSK_STEP_X - OSK_GAP;
            drawRect(x0, ry, sw, OSK_KEY_H, XMB_KEY_NORMAL);
            int lw = ttf_text_width("Space", 20);
            drawTTF(x0+(sw-lw)/2, ry+(OSK_KEY_H-20)/2-2, "Space", 20, XMB_TEXT, 0);
            int bsx = x0+sw+OSK_GAP;
            drawRect(bsx, ry, OSK_KEY_W, OSK_KEY_H, XMB_KEY_NORMAL);
            drawIcon(bsx+(OSK_KEY_W-22)/2, ry+(OSK_KEY_H-22)/2, 0xE14A, 22, XMB_TEXT);
            int clx = bsx+OSK_KEY_W+OSK_GAP;
            drawRect(clx, ry, OSK_KEY_W, OSK_KEY_H, XMB_KEY_NORMAL);
            lw = ttf_text_width("Clear", 20);
            drawTTF(clx+(OSK_KEY_W-lw)/2, ry+(OSK_KEY_H-20)/2-2, "Clear", 20, XMB_TEXT, 0);
        } else {
            int rlen = (int)strlen(R[r]);
            int extra = (r == OSK_ROWS_N-1) ? 1 : 0;
            for (int c = 0; c < rlen+extra; c++) {
                int sel = (r==sel_r && c==sel_c);
                int kx = x0 + c*OSK_STEP_X;
                drawRect(kx, ry, OSK_KEY_W, OSK_KEY_H, sel ? XMB_KEY_SEL : XMB_KEY_NORMAL);
                char lbl[4] = {0};
                if (c < rlen) { lbl[0] = R[r][c]; }
                else strcpy(lbl, "#+=");
                int lw = ttf_text_width(lbl, 20);
                drawTTF(kx+(OSK_KEY_W-lw)/2, ry+(OSK_KEY_H-20)/2-2, lbl, 20,
                        sel ? XMB_KEY_LABEL_SEL : XMB_TEXT, 0);
            }
        }
    }
    Hint h[] = {{'X',"Type"},{'C',"Clear"}};
    draw_hints_bar(h, 2);
    save_ppm("2_search.ppm");
}

static void screen_settings(void) {
    draw_background();
    xmb_draw_divider();
    xmb_draw_topbar();
    xmb_draw_tabs(6);

    int list_x = (W - XMB_LIST_W)/2;
    int py = XMB_CONTENT_Y + 16;
    drawRect(list_x, py, XMB_LIST_W, 96, XMB_PANEL);
    drawRect(list_x, py, XMB_LIST_W, 1, XMB_HAIRLINE);
    drawRect(list_x, py+95, XMB_LIST_W, 1, XMB_HAIRLINE);
    drawRect(list_x, py, 1, 96, XMB_HAIRLINE);
    drawRect(list_x+XMB_LIST_W-1, py, 1, 96, XMB_HAIRLINE);
    // avatar
    for (int dy = -22; dy <= 22; dy++) {
        int hw = (int)(sqrtf(22*22 - dy*dy) + 0.5f);
        drawRect(list_x+46-hw, py+48+dy, 2*hw+1, 1, XMB_ACCENT_DEEP);
    }
    int iw = ttf_text_width("M", 22);
    drawTTF(list_x+46-iw/2, py+48-12, "M", 22, XMB_WHITE, 1);
    int tx = list_x + 84;
    drawTTF(tx, py+14, "Account", 13, XMB_TEXT_FAINT, 0);
    drawTTF(tx, py+34, "Monty", 21, XMB_TEXT, 1);
    drawTTF(tx, py+64, "http://192.168.1.20:8096", 14, XMB_TEXT_DIM, 0);

    int iy = py + 96 + 24;
    drawRect(list_x, iy, XMB_LIST_W, 56, XMB_PANEL_HI);
    drawRect(list_x-4, iy, 3, 56, XMB_ACCENT);
    drawIcon(list_x+20, iy+(56-20)/2, 0xE879, 20, XMB_WHITE);
    drawTTF(list_x+52, iy+(56-18)/2-2, "Log Out", 18, XMB_WHITE, 1);

    const char *ver = "Jellyfin for PS3 \xB7 built Jun 13 2026";
    int vw = ttf_text_width(ver, 13);
    drawTTF((W-vw)/2, H-XMB_BOTTOM_PAD-26, ver, 13, XMB_TEXT_FAINT, 0);

    Hint h[] = {{'X',"Select"}};
    draw_hints_bar(h, 1);
    save_ppm("3_settings.ppm");
}

static void screen_info(void) {
    draw_background();
    int X = XMB_ITEM_PAD, Y = XMB_TOPBAR_H + 12;
    drawTTF(X, Y, "Interstellar", 40, XMB_WHITE, 1);
    Y += 62;
    int mx = X;
    drawTTF(mx, Y, "2014 \xB7 2h 49m", 18, XMB_TEXT_DIM, 0);
    mx += ttf_text_width("2014 \xB7 2h 49m", 18) + 18;
    int cw = ttf_text_width("PG-13", 13) + 16;
    drawRect(mx, Y, cw, 1, XMB_HAIRLINE); drawRect(mx, Y+21, cw, 1, XMB_HAIRLINE);
    drawRect(mx, Y, 1, 22, XMB_HAIRLINE); drawRect(mx+cw-1, Y, 1, 22, XMB_HAIRLINE);
    drawTTF(mx+8, Y+3, "PG-13", 13, XMB_TEXT_DIM, 0);
    mx += cw + 18;
    drawIcon(mx, Y+1, 0xE838, 18, 0x00E8B64C);
    drawTTF(mx+24, Y, "8.7", 18, XMB_TEXT_DIM, 0);
    Y += 44;
    drawTTF(X, Y, "Mankind's next step will be our greatest.", 20, 0x00AFA3E8, 0);
    Y += 38;
    const char *ov[4] = {
        "The adventures of a group of explorers who make use of a newly discovered",
        "wormhole to surpass the limitations on human space travel and conquer the",
        "vast distances involved in an interstellar voyage.", NULL };
    for (int i = 0; ov[i]; i++) { drawTTF(X, Y, ov[i], 19, 0x00C9CEE4, 0); Y += 30; }
    Y += 14;
    struct { const char *l, *v; } facts[] = {
        {"Video","1080p H.264 \xB7 23.976 fps"},
        {"Audio","English \xB7 AC3 5.1"},
        {"Genres","Adventure, Drama, Science Fiction"},
        {"Studios","Paramount, Legendary Pictures"}};
    for (int i = 0; i < 4; i++) {
        drawTTF(X, Y+2, facts[i].l, 15, XMB_TEXT_FAINT, 0);
        drawTTF(X+120, Y, facts[i].v, 17, XMB_TEXT, 0);
        Y += 32;
    }
    Hint h[] = {{'C',"Back"}};
    draw_hints_bar(h, 1);
    save_ppm("4_info.ppm");
}

static void screen_confirm(void) {
    draw_background();
    xmb_draw_divider();
    xmb_draw_topbar();
    xmb_draw_tabs(6);
    int mw = 520, mh = 118, mx = (W-mw)/2, my = XMB_CONTENT_Y + 100;
    drawRect(mx, my, mw, mh, XMB_PANEL);
    drawRect(mx, my, mw, 1, XMB_HAIRLINE); drawRect(mx, my+mh-1, mw, 1, XMB_HAIRLINE);
    drawRect(mx, my, 1, mh, XMB_HAIRLINE); drawRect(mx+mw-1, my, 1, mh, XMB_HAIRLINE);
    const char *q = "Log out of this account?";
    int qw = ttf_text_width(q, 21);
    drawTTF(mx+(mw-qw)/2, my+28, q, 21, XMB_TEXT, 1);
    const char *s = "You'll need to sign in again to browse your library.";
    int sw = ttf_text_width(s, 14);
    drawTTF(mx+(mw-sw)/2, my+66, s, 14, XMB_TEXT_DIM, 0);
    Hint h[] = {{'X',"Confirm"},{'C',"Cancel"}};
    draw_hints_bar(h, 2);
    save_ppm("5_confirm.ppm");
}

// ---- login OSK (mirrors source/ui/osk/ui_osk_login.cpp) ----
#define OSK_MAX_ROWS 6
#define OSK_LBL_PX   31.5f
enum OKind { OK_CHAR, OK_SHIFT, OK_SYM, OK_BACK, OK_SPACE, OK_ENTER };
typedef struct { int kind; char ch; const char *label; int cols; } OKey;
typedef struct { OKey keys[12]; int n; } ORow;

static OKey okey(int kind, char ch, const char *label, int cols) {
    OKey k; k.kind = kind; k.ch = ch; k.label = label; k.cols = cols; return k;
}

static int osk_build(ORow rows[OSK_MAX_ROWS], int sym, int caps) {
    for (int i = 0; i < OSK_MAX_ROWS; i++) rows[i].n = 0;
    int nr;
    if (!sym) {
        const char *R0="1234567890",*R1="qwertyuiop",*R2="asdfghjkl",*R3="zxcvbnm";
        for (const char *p=R0;*p;p++) rows[0].keys[rows[0].n++]=okey(OK_CHAR,*p,0,1);
        for (const char *p=R1;*p;p++) rows[1].keys[rows[1].n++]=okey(OK_CHAR,caps?toupper(*p):*p,0,1);
        for (const char *p=R2;*p;p++) rows[2].keys[rows[2].n++]=okey(OK_CHAR,caps?toupper(*p):*p,0,1);
        rows[3].keys[rows[3].n++]=okey(OK_SHIFT,0,"Caps",2);
        for (const char *p=R3;*p;p++) rows[3].keys[rows[3].n++]=okey(OK_CHAR,caps?toupper(*p):*p,0,1);
        rows[3].keys[rows[3].n++]=okey(OK_BACK,0,"<",2);
        nr=4;
    } else {
        const char *R0="1234567890",*R1="!@#$%^&*()",*R2="-_=+[]{}|\\",*R3=":;\"'`~<>?",*R4=".,/?";
        for (const char *p=R0;*p;p++) rows[0].keys[rows[0].n++]=okey(OK_CHAR,*p,0,1);
        for (const char *p=R1;*p;p++) rows[1].keys[rows[1].n++]=okey(OK_CHAR,*p,0,1);
        for (const char *p=R2;*p;p++) rows[2].keys[rows[2].n++]=okey(OK_CHAR,*p,0,1);
        for (const char *p=R3;*p;p++) rows[3].keys[rows[3].n++]=okey(OK_CHAR,*p,0,1);
        for (const char *p=R4;*p;p++) rows[4].keys[rows[4].n++]=okey(OK_CHAR,*p,0,1);
        rows[4].keys[rows[4].n++]=okey(OK_BACK,0,"<",2);
        nr=5;
    }
    rows[nr].keys[rows[nr].n++]=okey(OK_SYM,0,sym?"ABC":"#+=",2);
    rows[nr].keys[rows[nr].n++]=okey(OK_SPACE,0,"Space",5);
    rows[nr].keys[rows[nr].n++]=okey(OK_ENTER,0,"Enter",3);
    return nr+1;
}

static int orow_units(const ORow *r) {
    int u=0; for (int i=0;i<r->n;i++) u+=r->keys[i].cols; return u;
}

static void screen_login_osk(int sym, int sr, int sc, const char *out, const char *path) {
    draw_background();
    xmb_draw_divider();
    int total_w = 10*OSK_STEP_X - OSK_GAP;
    int barx = (W - total_w)/2;
    int y0 = XMB_CONTENT_Y + 58;
    const int field_h = 40;

    ORow rows[OSK_MAX_ROWS];
    int nrows = osk_build(rows, sym, 0);

    drawRect(barx, XMB_CONTENT_Y+8, total_w, field_h, 0x00131830);
    drawRect(barx, XMB_CONTENT_Y+8+field_h-2, total_w, 2, XMB_ACCENT);

    for (int r=0;r<nrows;r++) {
        int rw = orow_units(&rows[r])*OSK_STEP_X - OSK_GAP;
        int cx = (W - rw)/2;
        int ry = y0 + r*OSK_STEP_Y;
        for (int c=0;c<rows[r].n;c++) {
            const OKey *k=&rows[r].keys[c];
            int kw = k->cols*OSK_STEP_X - OSK_GAP;
            u32 col = (r==sr && c==sc) ? XMB_KEY_SEL : XMB_KEY_NORMAL;
            drawRect(cx, ry, kw, OSK_KEY_H, col);
            cx += k->cols*OSK_STEP_X;
        }
    }

    drawTTF(XMB_ITEM_PAD, 20, "Jellyfin", 22, XMB_TEXT, 1);
    drawTTF(XMB_ITEM_PAD + ttf_text_width("Jellyfin", 22) + 8, 27, "PS3", 13, XMB_ACCENT, 1);
    const char *prompt = "Server URL (e.g. http://192.168.1.2:8096)";
    int pw = ttf_text_width(prompt, 18);
    int px = W/2 - pw/2; if (px < XMB_ITEM_PAD) px = XMB_ITEM_PAD;
    drawTTF(px, XMB_DIVIDER_Y-34, prompt, 18, XMB_TEXT_DIM, 0);

    char disp[96]; snprintf(disp, sizeof(disp), "%s_", out);
    float typed_px = 22.0f;
    int tw = ttf_text_width(disp, typed_px);
    int tx = W/2 - tw/2; if (tx < barx+14) tx = barx+14;
    drawTTF(tx, XMB_CONTENT_Y+8+(field_h-(int)typed_px)/2, disp, typed_px, XMB_TEXT, 0);

    for (int r=0;r<nrows;r++) {
        int rw = orow_units(&rows[r])*OSK_STEP_X - OSK_GAP;
        int cx = (W - rw)/2;
        for (int c=0;c<rows[r].n;c++) {
            const OKey *k=&rows[r].keys[c];
            int kw = k->cols*OSK_STEP_X - OSK_GAP;
            int sel = (r==sr && c==sc);
            int ry = y0 + r*OSK_STEP_Y;
            u32 clr = sel ? XMB_KEY_LABEL_SEL : XMB_TEXT;
            if (k->kind==OK_BACK) {
                drawIcon(cx+(kw-22)/2, ry+(OSK_KEY_H-22)/2, 0xE14A, 22.0f, clr);
            } else {
                char chbuf[2]={k->ch,0};
                const char *lbl=(k->kind==OK_CHAR)?chbuf:k->label;
                float lbl_px=(k->kind==OK_CHAR)?OSK_LBL_PX:18.0f;
                int lw=ttf_text_width(lbl, lbl_px);
                int lx=cx+(kw-lw)/2;
                int ly=ry+(OSK_KEY_H-(int)lbl_px)/2;
                drawTTF(lx, ly, lbl, lbl_px, clr, 0);
            }
            cx += k->cols*OSK_STEP_X;
        }
    }

    Hint h[] = {{'X',"Select"},{'S',"Delete"},{'A',"Done"},{'C',"Cancel"}};
    draw_hints_bar(h, 4);
    save_ppm(path);
}

int main(void) {
    gamma_init();
    stbtt_InitFont(&s_font, load_file("../../source/gfx/fonts/OpenSans-Regular.ttf"), 0);
    stbtt_InitFont(&s_font_bold, load_file("../../source/gfx/fonts/OpenSans-Bold.ttf"), 0);
    stbtt_InitFont(&s_icons, load_file("../../source/gfx/fonts/MaterialIcons-Regular.ttf"), 0);
    stbtt_InitFont(&s_iconic, load_file("iconic_psx.ttf"), 0);
    screen_movies();
    screen_search();
    screen_settings();
    screen_info();
    screen_confirm();
    screen_login_osk(0, 1, 0, "http://192.168.1.20", "6_osk_letters.ppm");
    screen_login_osk(1, 4, 2, "http://192.168.1.20:", "7_osk_symbols.ppm");
    printf("done\n");
    return 0;
}
