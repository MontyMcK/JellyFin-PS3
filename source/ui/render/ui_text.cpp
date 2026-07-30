// Text and glyph rendering — bitmap font, Open Sans TTF, Tabler Icons.
// Owns all font state and the stb_truetype implementation.

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include <rsx/rsx.h>

#include "ui_visuals.h"
#include "bitmap.h"
#include "plog.h"
#include "font8x8.xpm"
#include "opensans_regular.h"
#include "opensans_bold.h"
#include "tabler_icons.h"
#include "icons.h"

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
// Glyph cache
// -------------------------------------------------------
// Every drawTTF/drawIcon call used to re-parse the glyph outline and rasterize
// it from scratch — for every character, on every frame — and stb_truetype's
// default allocator ran several malloc/free pairs per glyph while doing it.  On
// a text-heavy screen (the A-Z jump bar, Settings, the item-info overlay) that
// was hundreds of heap round-trips per frame before a single pixel was blitted.
//
// Coverage bitmaps are cached here keyed by (font, pixel size, codepoint), so a
// warm screen rasterizes nothing and allocates nothing.  Glyphs are rendered
// straight into the arena with stbtt_MakeCodepointBitmap, which takes caller
// storage — so even a cache miss skips stb's output malloc.
//
// The arena is a bump allocator: when it fills, the whole cache is flushed
// rather than evicting entries individually.  That keeps it O(1) and
// fragmentation-free, and the UI's working set is small enough that a warm
// screen never refills it.  If the arena can't be allocated at boot the cache
// disables itself and every draw falls back to the original rasterize path, so
// low memory degrades speed but never correctness.

// Sizing: a flush costs one frame of rasterization (i.e. what every frame used
// to cost), so flushing when the user changes screen is harmless.  What must
// not happen is a single screen's glyphs overflowing the arena, which would
// flush mid-frame and thrash.  One dense screen is on the order of 300 distinct
// glyphs (~100 KB); 384 KB leaves roughly 3x headroom over that.  Watch for
// repeated "glyph cache flushed" lines in the log if this ever needs raising.
#define GC_ARENA_BYTES  (384 * 1024)   // ~1% of free heap; ~1100 glyphs
#define GC_SLOTS        2048           // power of two, open-addressed
#define GC_FONT_REG     0
#define GC_FONT_BOLD    1
#define GC_FONT_ICONS   2

typedef struct {
    float px;
    int   cp;
    u32   pix;            // byte offset into s_gc_arena (valid when w && h)
    s16   w, h, xoff, yoff;
    u8    font;
    bool  used;
} GlyphSlot;

static GlyphSlot  s_gc[GC_SLOTS];
static u8        *s_gc_arena   = NULL;
static u32        s_gc_used    = 0;
static u32        s_gc_count   = 0;
static u32        s_gc_flushes = 0;
static bool       s_gc_on      = false;

// Per-font ascent (size-independent) and per-(font,ASCII) unscaled advance.
// Both were re-read from the font tables on every call; neither ever changes.
static int  s_ascent[3];
static int  s_adv[3][128];
static bool s_adv_ok[3][128];

static inline int font_id_of(const stbtt_fontinfo *fi) {
    if (fi == &s_font_bold) return GC_FONT_BOLD;
    if (fi == &s_icons)     return GC_FONT_ICONS;
    return GC_FONT_REG;
}

// Unscaled horizontal advance, cached for ASCII (multiply by the pixel scale).
static inline int glyph_advance(const stbtt_fontinfo *fi, int id, int cp) {
    if (cp < 0 || cp >= 128) {
        int a;
        stbtt_GetCodepointHMetrics(fi, cp, &a, NULL);
        return a;
    }
    if (!s_adv_ok[id][cp]) {
        stbtt_GetCodepointHMetrics(fi, cp, &s_adv[id][cp], NULL);
        s_adv_ok[id][cp] = true;
    }
    return s_adv[id][cp];
}

static inline u32 gc_hash(u8 font, float px, int cp) {
    u32 h = (u32)cp * 2654435761u;
    h ^= (u32)(px * 4.0f + 0.5f) * 40503u;   // quarter-pixel key resolution
    h ^= (u32)font * 2246822519u;
    h ^= h >> 15;
    return h;
}

static void gc_flush(void) {
    memset(s_gc, 0, sizeof(s_gc));
    s_gc_used  = 0;
    s_gc_count = 0;
    s_gc_flushes++;
    plog("ttf: glyph cache flushed (arena full)");
}

// Look up a glyph, rasterizing and caching it on miss.  Returns NULL when the
// cache is unavailable or the glyph is too large to cache — callers fall back
// to rasterizing directly.  px is compared exactly (call sites pass the same
// constants every frame); the hash only quantizes it to pick a bucket.
static const GlyphSlot *gc_glyph(const stbtt_fontinfo *fi, u8 font, float px,
                                 float scale, int cp) {
    if (!s_gc_on) return NULL;

    const u32 mask = GC_SLOTS - 1;
    u32 i = gc_hash(font, px, cp) & mask;

    for (u32 p = 0; p < GC_SLOTS; p++, i = (i + 1) & mask) {
        GlyphSlot *s = &s_gc[i];
        if (s->used) {
            if (s->cp == cp && s->font == font && s->px == px) return s;
            continue;   // collision: keep probing
        }

        // Miss.  Measure first so the arena cost is known before committing.
        int x0, y0, x1, y1;
        stbtt_GetCodepointBitmapBox(fi, cp, scale, scale, &x0, &y0, &x1, &y1);
        int gw = x1 - x0, gh = y1 - y0;
        if (gw < 0) gw = 0;
        if (gh < 0) gh = 0;
        u32 need = (u32)gw * (u32)gh;

        if (need > GC_ARENA_BYTES) return NULL;   // absurdly large: don't cache

        // Out of arena, or the table is getting dense enough to hurt probing.
        if (s_gc_used + need > GC_ARENA_BYTES ||
            (s_gc_count + 1) * 10 > GC_SLOTS * 7) {
            gc_flush();
            i = gc_hash(font, px, cp) & mask;   // table is empty: this slot is free
            s = &s_gc[i];
        }

        if (need) {
            stbtt_MakeCodepointBitmap(fi, s_gc_arena + s_gc_used,
                                      gw, gh, gw, scale, scale, cp);
            s->pix = s_gc_used;
            s_gc_used += need;
        } else {
            s->pix = 0;                          // space and friends: no pixels
        }
        s->px   = px;   s->cp   = cp;    s->font = font;
        s->w    = (s16)gw;  s->h    = (s16)gh;
        s->xoff = (s16)x0;  s->yoff = (s16)y0;
        s->used = true;
        s_gc_count++;
        return s;
    }
    return NULL;   // table full even after a flush — cannot happen in practice
}

// Composite one coverage bitmap at (x0, y0).
//
// gamma_aa picks the blend: text has always used the gamma-correct path (see
// the LUTs above), icons the plain 8-bit one.  Both are kept exactly as they
// were so nothing changes on screen.
static void blit_coverage(const unsigned char *bm, int w, int h,
                          int x0, int y0, u32 color, bool gamma_aa) {
    u32  r_fg = (color >> 16) & 0xFF;
    u32  g_fg = (color >>  8) & 0xFF;
    u32  b_fg =  color        & 0xFF;
    bool rt   = cpu_rt_on();
    u32  tw_  = cpu_draw_w();

    for (int gy = 0; gy < h; gy++) {
        int sy = y0 + gy;
        if (cpu_row_clipped(sy)) continue;
        u32                 *row = cpu_draw_row((u32)sy);
        const unsigned char *src = bm + (size_t)gy * (size_t)w;
        for (int gx = 0; gx < w; gx++) {
            int sx = x0 + gx;
            if (sx < 0 || (u32)sx >= tw_) continue;
            u32 a = src[gx];
            if (a == 0) continue;
            if (rt) { row[sx] = argb_over(row[sx], color, a); continue; }
            if (a == 255) { row[sx] = color; continue; }
            u32 bg = row[sx];
            if (gamma_aa) {
                row[sx] = ((u32)aa_blend(a, r_fg, (bg >> 16) & 0xFF) << 16) |
                          ((u32)aa_blend(a, g_fg, (bg >>  8) & 0xFF) <<  8) |
                           (u32)aa_blend(a, b_fg,  bg        & 0xFF);
            } else {
                u32 r_bg = (bg >> 16) & 0xFF;
                u32 g_bg = (bg >>  8) & 0xFF;
                u32 b_bg =  bg        & 0xFF;
                row[sx] = (((a * r_fg + (255 - a) * r_bg) / 255) << 16) |
                          (((a * g_fg + (255 - a) * g_bg) / 255) <<  8) |
                           ((a * b_fg + (255 - a) * b_bg) / 255);
            }
        }
    }
}

// Draw one glyph through the cache, falling back to a direct rasterize when
// the cache is unavailable.  Returns the glyph's cached box via *out when the
// caller needs its metrics (drawTTF_vcentered), else pass NULL.
static void draw_glyph(const stbtt_fontinfo *fi, u8 font, float px, float scale,
                       int cp, int pen_x, int pen_y, u32 color, bool gamma_aa) {
    const GlyphSlot *g = gc_glyph(fi, font, px, scale, cp);
    if (g) {
        if (g->w > 0 && g->h > 0)
            blit_coverage(s_gc_arena + g->pix, g->w, g->h,
                          pen_x + g->xoff, pen_y + g->yoff, color, gamma_aa);
        return;
    }
    int w, h, xoff, yoff;
    unsigned char *bm = stbtt_GetCodepointBitmap(fi, scale, scale, cp,
                                                 &w, &h, &xoff, &yoff);
    if (!bm) return;
    blit_coverage(bm, w, h, pen_x + xoff, pen_y + yoff, color, gamma_aa);
    stbtt_FreeBitmap(bm, NULL);
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
    int   id    = font_id_of(fi);
    float scale = stbtt_ScaleForPixelHeight(fi, px);
    float xf = 0.0f;
    int prev_cp = 0;
    while (*text) {
        int cp = (unsigned char)*text;
        if (prev_cp) xf += stbtt_GetCodepointKernAdvance(fi, prev_cp, cp) * scale;
        xf += (float)glyph_advance(fi, id, cp) * scale;
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

    int   id       = font_id_of(fi);
    float scale    = stbtt_ScaleForPixelHeight(fi, px);
    int   baseline = (int)((float)s_ascent[id] * scale);

    float xf      = (float)x;
    int   prev_cp = 0;

    while (*text) {
        int cp = (unsigned char)*text;

        if (prev_cp)
            xf += stbtt_GetCodepointKernAdvance(fi, prev_cp, cp) * scale;

        draw_glyph(fi, (u8)id, px, scale, cp,
                   (int)xf, (int)y + baseline, color, true);

        xf += (float)glyph_advance(fi, id, cp) * scale;
        prev_cp = cp;
        text++;
    }
}

void drawTTF_vcentered(u32 x, int cy, const char *text, float px, u32 color,
                       bool bold) {
    if (!s_ttf_ok) { drawTTF(x, (u32)(cy - (int)(px * 0.5f)), text, px, color, bold); return; }
    stbtt_fontinfo *fi = (bold && s_ttf_bold_ok) ? &s_font_bold : &s_font;
    int   id       = font_id_of(fi);
    float scale    = stbtt_ScaleForPixelHeight(fi, px);
    int   baseline = (int)((float)s_ascent[id] * scale);

    // Union of every glyph's bitmap box (baseline-relative) = the ink extent.
    // Measuring through the cache also warms it for the drawTTF below, so a
    // centred string rasterizes at most once instead of once per frame.
    int y0min = 0, y1max = 0;
    bool any = false;
    for (const char *p = text; *p; p++) {
        int cp = (unsigned char)*p;
        int gy0, gy1;
        const GlyphSlot *g = gc_glyph(fi, (u8)id, px, scale, cp);
        if (g) {
            if (g->w <= 0 && g->h <= 0) continue;   // space etc.
            gy0 = g->yoff;
            gy1 = g->yoff + g->h;
        } else {
            int gx0, gx1;
            stbtt_GetCodepointBitmapBox(fi, cp, scale, scale,
                                        &gx0, &gy0, &gx1, &gy1);
            if (gx1 <= gx0 && gy1 <= gy0) continue;
        }
        if (!any || gy0 < y0min) y0min = gy0;
        if (!any || gy1 > y1max) y1max = gy1;
        any = true;
    }
    if (!any) return;

    // drawTTF puts glyph ink at y + baseline + gy; centre that span on cy.
    int y = cy - baseline - (y0min + y1max) / 2;
    if (y < 0) y = 0;
    drawTTF(x, (u32)y, text, px, color, bold);
}

void drawIcon(u32 x, u32 y, int codepoint, float px, u32 color) {
    if (!s_icons_ok) return;
    float scale    = stbtt_ScaleForPixelHeight(&s_icons, px);
    int   baseline = (int)((float)s_ascent[GC_FONT_ICONS] * scale);
    // Icons keep the plain 8-bit blend (gamma_aa = false) they have always used.
    draw_glyph(&s_icons, GC_FONT_ICONS, px, scale, codepoint,
               (int)x, (int)y + baseline, color, false);
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
    if (stbtt_InitFont(&s_icons, (unsigned char*)TablerIcons_ttf, 0))
        s_icons_ok = true;

    // Ascent is size-independent — read it once here instead of on every call.
    if (s_ttf_ok)
        stbtt_GetFontVMetrics(&s_font,      &s_ascent[GC_FONT_REG],   NULL, NULL);
    if (s_ttf_bold_ok)
        stbtt_GetFontVMetrics(&s_font_bold, &s_ascent[GC_FONT_BOLD],  NULL, NULL);
    if (s_icons_ok)
        stbtt_GetFontVMetrics(&s_icons,     &s_ascent[GC_FONT_ICONS], NULL, NULL);

    // Glyph cache.  A failed allocation is non-fatal: every draw then falls
    // back to rasterizing directly, exactly as it did before the cache existed.
    s_gc_arena = (u8*)malloc(GC_ARENA_BYTES);
    s_gc_on    = (s_gc_arena != NULL);
    plog(s_gc_on ? "ttf: glyph cache ready (256K)"
                 : "ttf: glyph cache alloc FAILED - using direct rasterize");
}

// Rasterize one glyph purely to populate the cache.  If the cache is
// unavailable this still does the old alloc/rasterize/free, which warms the
// malloc pool and stbtt's i-cache the way this routine always used to.
static void prewarm_glyph(const stbtt_fontinfo *fi, u8 font, float px,
                          float scale, int cp) {
    if (gc_glyph(fi, font, px, scale, cp)) return;
    int w, h, xo, yo;
    unsigned char *bm = stbtt_GetCodepointBitmap(fi, scale, scale, cp,
                                                 &w, &h, &xo, &yo);
    if (bm) stbtt_FreeBitmap(bm, NULL);
}

// Pre-rasterize every glyph the HUD will ever draw, at each size
// player_hud.cpp uses.  Must be called before the first hud_draw().  These now
// land in the glyph cache and stay there, so the HUD's text costs nothing to
// re-draw; before the cache existed this could only warm the allocator.
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
            for (const char *cp = reg[s].chars; *cp; cp++)
                prewarm_glyph(&s_font, GC_FONT_REG, reg[s].px, sc,
                              (unsigned char)*cp);
        }
    }
    // OpenSans Bold: "CC" label (20px).
    if (s_ttf_bold_ok) {
        float sc = stbtt_ScaleForPixelHeight(&s_font_bold, 20.0f);
        for (const char *cp = "C"; *cp; cp++)
            prewarm_glyph(&s_font_bold, GC_FONT_BOLD, 20.0f, sc,
                          (unsigned char)*cp);
    }
    // Material Icons: music note codepoint (24px).
    if (s_icons_ok) {
        float sc = stbtt_ScaleForPixelHeight(&s_icons, 24.0f);
        prewarm_glyph(&s_icons, GC_FONT_ICONS, 24.0f, sc, ICON_MUSIC);
    }
}

void visuals_cleanup(void) {
    bitmapDestroy(&fontBitmap);
}
