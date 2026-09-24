// Text and glyph rendering — bitmap font, Open Sans TTF, Tabler Icons.
// Owns all font state and the stb_truetype implementation.

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include <rsx/rsx.h>

#include "ui_visuals.h"
#include "ui_text_gpu.h"
#include "ui_strobe_test.h"
#include "bitmap.h"
#include "plog.h"
#include "font8x8.xpm"
#include "opensans_regular.h"
#include "opensans_bold.h"
#include "tabler_icons.h"
#include "notosans_bold.h"
#include "robotocondensed_bold.h"
// The v1.0 type system.  Six roles, and the design's CSS spells each one as a
// STACK, not a single face -- `--font-display: "GT America Expanded", "Rodin"`.
// That is not decoration: GT America Expanded Bold is a 99-glyph subset with no
// hyphen, ampersand, slash or accent, so "Spider-Man" needs Rodin for one
// character in the middle of the word.  See face_chain() below.
#include "rodin_latin.h"          // --font-system / --font-tech, and every fallback
#include "gtamerica_expanded.h"   // --font-display  (subset; falls back to Rodin)
#include "microgramma.h"          // --font-eyebrow  (was --font-display before v1.0)
#include "satoshi_bold.h"         // --font-tab      700: tab labels, clock
#include "satoshi_regular.h"      // --font-tab      400/500: date, cast names
#include "michroma.h"             // --font-spec
#include "mata_bold.h"            // the lockup wordmark, and nothing else
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
// Subtitle faces.  Bold only -- subtitles are never set in a regular weight --
// and subset to Latin plus SubRip's punctuation, which is why two extra faces
// cost 41 KB instead of 1.1 MB.  See source/gfx/fonts/LICENSES.md.
static stbtt_fontinfo  s_font_noto;
static stbtt_fontinfo  s_font_robocond;
static bool            s_noto_ok = false, s_robocond_ok = false;
// v1.0 type-system faces.  s_font/s_font_bold are backed by Rodin (below), so
// only the four narrow-purpose roles need their own slots.
static stbtt_fontinfo  s_font_display;   // GT America Expanded Bold
static stbtt_fontinfo  s_font_eyebrow;   // Microgramma
static stbtt_fontinfo  s_font_tab;       // Satoshi Bold
static stbtt_fontinfo  s_font_tab_reg;   // Satoshi Regular
static stbtt_fontinfo  s_font_spec;      // Michroma
static stbtt_fontinfo  s_font_lockup;    // Mata Bold
static bool            s_display_ok = false, s_spec_ok = false;
static bool            s_eyebrow_ok = false, s_tab_ok = false, s_tab_reg_ok = false;
static bool            s_lockup_ok  = false;
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
#define GC_FONT_NOTO    3
#define GC_FONT_ROBOCND 4
#define GC_FONT_DISPLAY 5
#define GC_FONT_SPEC    6
#define GC_FONT_EYEBROW 7
#define GC_FONT_TAB     8
#define GC_FONT_TABREG  9
#define GC_FONT_LOCKUP 10
#define GC_FONT_COUNT  11

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
static int  s_ascent[GC_FONT_COUNT];
static int  s_adv[GC_FONT_COUNT][128];
static bool s_adv_ok[GC_FONT_COUNT][128];

static inline int font_id_of(const stbtt_fontinfo *fi) {
    if (fi == &s_font_bold)    return GC_FONT_BOLD;
    if (fi == &s_icons)        return GC_FONT_ICONS;
    if (fi == &s_font_noto)    return GC_FONT_NOTO;
    if (fi == &s_font_robocond) return GC_FONT_ROBOCND;
    if (fi == &s_font_display) return GC_FONT_DISPLAY;
    if (fi == &s_font_spec)    return GC_FONT_SPEC;
    if (fi == &s_font_eyebrow) return GC_FONT_EYEBROW;
    if (fi == &s_font_tab)     return GC_FONT_TAB;
    if (fi == &s_font_tab_reg) return GC_FONT_TABREG;
    if (fi == &s_font_lockup)  return GC_FONT_LOCKUP;
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

// --- draw-cost instrumentation -------------------------------------------
// tools/spubench measured PPU reads from RSX video memory at 7.7 MB/s against
// 767 MB/s for writes -- a 100x asymmetry -- but it measured it with a
// `volatile` loop, which forces one load and one store per pixel with no
// batching.  This blitter is not volatile, so that figure is a FLOOR on the
// hardware's speed rather than the cost of this path, and multiplying an
// estimated glyph-pixel count by it would be guesswork dressed up as a
// measurement.
//
// So count the real thing instead.  `blend` is the number of pixels that took
// the read-modify-write branch (one uncached VRAM read each); `opaque` is the
// number that hit the a==255 store-only fast path.  The ratio between them is
// the whole question -- if most glyph coverage is fully opaque, the read path
// costs far less than the pixel count suggests.
//
// Accumulated per glyph, not per pixel: a static increment inside the inner
// loop would measurably change what it is trying to measure.
static u32 s_tx_glyphs, s_tx_px_blend, s_tx_px_opaque;

void ui_text_stats_reset(void) { s_tx_glyphs = s_tx_px_blend = s_tx_px_opaque = 0; }
void ui_text_stats_get(u32 *glyphs, u32 *blend_px, u32 *opaque_px) {
    if (glyphs)    *glyphs    = s_tx_glyphs;
    if (blend_px)  *blend_px  = s_tx_px_blend;
    if (opaque_px) *opaque_px = s_tx_px_opaque;
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
    u32  n_blend = 0, n_opaque = 0;

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
            if (rt) { row[sx] = argb_over(row[sx], color, a); n_blend++; continue; }
            if (a == 255) { row[sx] = color; n_opaque++; continue; }
            n_blend++;
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
    s_tx_glyphs++;
    s_tx_px_blend  += n_blend;
    s_tx_px_opaque += n_opaque;
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

// --- UTF-8 ----------------------------------------------------------------
// Jellyfin sends UTF-8.  Every walker below used to read `(unsigned char)*p`
// and step one byte, i.e. treat the stream as Latin-1, so a curly apostrophe
// (U+2019, bytes E2 80 99) drew as `a-circumflex` followed by two .notdef
// boxes, and an accented title (e-acute = C3 A9) drew as two glyphs.  The
// glyphs were never missing -- U+2019 is glyph 304 in Rodin, 110 in
// Microgramma, and present in OpenSans too -- only the decode was.
//
// EVERY walker must use this, and identically.  ttf_run_box()/ttf_run_raster()
// feed the GPU path while drawTTF_face() feeds the CPU one, and ui_text_gpu.h's
// contract is that a run drawn either way lands on exactly the same pixels; a
// decoder applied to one and not the other shows up as text that shifts when
// the gate flips.  ttf_text_width*() has to agree as well or centring and
// clipping go wrong on precisely the strings this fixes.
//
// Nothing downstream needs changing: gc_glyph() keys on the full int codepoint
// (it has to -- icons live in the Private Use Area) and glyph_advance() falls
// through to stb for anything outside its ASCII table.
//
// A malformed, truncated, overlong or surrogate sequence falls back to the
// lead byte as a Latin-1 codepoint and advances one byte -- exactly what this
// code did for every byte before -- so nothing that renders today can start
// rendering as nothing, and a server that really does send Latin-1 still works.
static inline int utf8_next(const char **pp)
{
    const unsigned char *s = (const unsigned char *)*pp;
    unsigned c0 = s[0];
    int n, cp, lo;      // continuation bytes, accumulator, smallest legal value

    if      (c0 < 0x80)                { *pp += 1; return (int)c0;            }
    else if (c0 >= 0xC2 && c0 <= 0xDF) { n = 1; cp = c0 & 0x1F; lo = 0x80;    }
    else if (c0 >= 0xE0 && c0 <= 0xEF) { n = 2; cp = c0 & 0x0F; lo = 0x800;   }
    else if (c0 >= 0xF0 && c0 <= 0xF4) { n = 3; cp = c0 & 0x07; lo = 0x10000; }
    else                               { *pp += 1; return (int)c0;            }

    for (int i = 1; i <= n; i++) {
        unsigned ci = s[i];
        if ((ci & 0xC0) != 0x80) { *pp += 1; return (int)c0; }   // truncated
        cp = (cp << 6) | (int)(ci & 0x3F);
    }
    if (cp < lo || (cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF) {
        *pp += 1; return (int)c0;                                // not a codepoint
    }
    *pp += n + 1;
    return cp;
}

// The PRIMARY face for a UI_FACE_* role, falling back to the bold UI face when
// that face failed to load -- a missing font must degrade to readable text,
// never to nothing on screen.
static stbtt_fontinfo *face_of(int face) {
    switch (face) {
    case UI_FACE_NOTO:     if (s_noto_ok)     return &s_font_noto;     break;
    case UI_FACE_ROBOCOND: if (s_robocond_ok) return &s_font_robocond; break;
    case UI_FACE_DISPLAY:  if (s_display_ok)  return &s_font_display;  break;
    case UI_FACE_SPEC:     if (s_spec_ok)     return &s_font_spec;     break;
    case UI_FACE_EYEBROW:  if (s_eyebrow_ok)  return &s_font_eyebrow;  break;
    case UI_FACE_TAB:      if (s_tab_ok)      return &s_font_tab;      break;
    case UI_FACE_TAB_REG:  if (s_tab_reg_ok)  return &s_font_tab_reg;  break;
    case UI_FACE_LOCKUP:   if (s_lockup_ok)   return &s_font_lockup;   break;
    case UI_FACE_BOLD:     break;
    default:               return &s_font;
    }
    return s_ttf_bold_ok ? &s_font_bold : &s_font;
}

// ---- per-glyph fallback ---------------------------------------------------
//
// The design's font tokens are CSS STACKS, and for v1.0 that stopped being a
// formality.  `--font-display: "GT America Expanded", "Rodin", …` is backed by
// a 99-glyph subset: it carries A-Z, a-z, 0-9 and `! ( ) , . : ; ? _` and
// NOTHING else -- no hyphen, ampersand, slash, apostrophe or accent.  Taken as
// a single face it would render "Spider-Man" as "SpiderMan", "Fast & Furious"
// without its ampersand and "Amelie" for "Amélie", silently, because a missing
// codepoint is just .notdef and .notdef in most faces is a zero-width nothing.
//
// So a role resolves PER CODEPOINT: the primary face if it has the glyph,
// otherwise the next face in its stack.  Rodin anchors every chain because it
// is the system face and covers Latin-1 plus the punctuation Jellyfin sends.
//
// Two things keep this honest:
//
//   * ONE BASELINE for the whole run, taken from the primary.  Each face is
//     scaled to the same pixel height by stbtt_ScaleForPixelHeight, but their
//     ascents differ (GT America 1005/em, Rodin's is its own), so using each
//     glyph's own baseline would make the fallback characters ride up or down
//     mid-word.  The line is the primary's; the glyphs sit on it.
//   * KERNING ONLY WITHIN A FACE.  A kern pair is a property of one font's
//     tables; asking Rodin how to kern a GT America 'r' against its own 'i' is
//     meaningless, so the pair is skipped when the two glyphs resolved to
//     different faces.
//
// Every walker in this file uses this, and identically -- see the note on
// utf8_next() above for why that matters.
#define FACE_CHAIN_MAX 3

typedef struct {
    stbtt_fontinfo *fi[FACE_CHAIN_MAX];
    float           scale[FACE_CHAIN_MAX];
    u8              id[FACE_CHAIN_MAX];
    int             n;
    int             baseline;      // from fi[0]; shared by the whole run
} FaceChain;

// Resolution cache for ASCII, keyed by the role's PRIMARY font id.  Without it
// every measure pass pays a cmap binary search per character, and the measure
// passes run every frame for centring even when the glyph cache means nothing
// is rasterized.  -1 = not yet resolved.
static signed char s_pick[GC_FONT_COUNT][128];
static bool        s_pick_init = false;

static void chain_of(int face, float px, FaceChain *c)
{
    stbtt_fontinfo *primary = face_of(face);
    c->n = 0;
    c->fi[c->n++] = primary;

    // The stacks, straight from the design tokens.  Only the roles whose face
    // is genuinely incomplete need a fallback; the rest are single-entry and
    // cost one comparison.
    switch (face) {
    case UI_FACE_DISPLAY:
    case UI_FACE_EYEBROW:
    case UI_FACE_TAB:
    case UI_FACE_TAB_REG:
    case UI_FACE_SPEC:
    case UI_FACE_LOCKUP:
        if (primary != &s_font) c->fi[c->n++] = &s_font;      // → Rodin
        break;
    default:
        break;
    }

    for (int i = 0; i < c->n; i++) {
        c->id[i]    = (u8)font_id_of(c->fi[i]);
        c->scale[i] = stbtt_ScaleForPixelHeight(c->fi[i], px);
    }
    c->baseline = (int)((float)s_ascent[c->id[0]] * c->scale[0]);
}

// Which link of the chain owns this codepoint.  Falls back to 0 when nothing
// has it, so the .notdef comes from the primary exactly as it did before.
static inline int chain_pick(const FaceChain *c, int cp)
{
    if (c->n == 1) return 0;

    const bool cacheable = s_pick_init && cp >= 0 && cp < 128;
    if (cacheable) {
        signed char v = s_pick[c->id[0]][cp];
        if (v >= 0) return v < c->n ? v : 0;
    }

    int k = 0;
    for (int i = 0; i < c->n; i++) {
        if (stbtt_FindGlyphIndex(c->fi[i], cp)) { k = i; break; }
    }
    if (cacheable) s_pick[c->id[0]][cp] = (signed char)k;
    return k;
}

int ttf_text_width_face(const char *text, float px, int face) {
    if (!s_ttf_ok) return (int)(strlen(text) * px);
    FaceChain ch;
    chain_of(face, px, &ch);
    float xf = 0.0f;
    int prev_cp = 0, prev_k = -1;
    while (*text) {
        int cp = utf8_next(&text);
        int k  = chain_pick(&ch, cp);
        if (prev_cp && k == prev_k)
            xf += stbtt_GetCodepointKernAdvance(ch.fi[k], prev_cp, cp) * ch.scale[k];
        xf += (float)glyph_advance(ch.fi[k], ch.id[k], cp) * ch.scale[k];
        prev_cp = cp; prev_k = k;
    }
    return (int)xf;
}

int ttf_text_width(const char *text, float px, bool bold) {
    return ttf_text_width_face(text, px, bold ? UI_FACE_BOLD : UI_FACE_REGULAR);
}

// -------------------------------------------------------
// Run measurement and rasterization for the GPU path
// -------------------------------------------------------
// These two are the seam described in ui_text_gpu.h: that module owns video
// memory and the RSX, this one owns stb_truetype, the face table and the glyph
// cache.  Both walk the string exactly the way drawTTF_face does below --
// same kerning, same `(int)xf` pen truncation, same cache lookups -- so a run
// drawn through the GPU lands on precisely the pixels the CPU path would have
// used.  Any divergence here shows up as text that shifts by a pixel when the
// gate is flipped, which is why the walk is duplicated rather than
// approximated.

bool ttf_run_box(const char *text, float px, int face, TtfRunBox *box)
{
    if (!s_ttf_ok || !text || !*text) return false;

    FaceChain ch;
    chain_of(face, px, &ch);

    float xf = 0.0f;
    int   prev_cp = 0, prev_k = -1;
    int   x0min = 0, y0min = 0, x1max = 0, y1max = 0;
    bool  any = false;

    for (const char *p = text; *p; ) {
        int cp = utf8_next(&p);
        int k  = chain_pick(&ch, cp);
        if (prev_cp && k == prev_k)
            xf += stbtt_GetCodepointKernAdvance(ch.fi[k], prev_cp, cp) * ch.scale[k];

        int gx, gy, gw, gh;
        const GlyphSlot *g = gc_glyph(ch.fi[k], ch.id[k], px, ch.scale[k], cp);
        if (g) {
            gx = g->xoff; gy = g->yoff; gw = g->w; gh = g->h;
        } else {
            int a0, b0, a1, b1;
            stbtt_GetCodepointBitmapBox(ch.fi[k], cp, ch.scale[k], ch.scale[k],
                                        &a0, &b0, &a1, &b1);
            gx = a0; gy = b0; gw = a1 - a0; gh = b1 - b0;
        }
        if (gw > 0 && gh > 0) {
            int lx = (int)xf + gx, ty = ch.baseline + gy;
            if (!any || lx      < x0min) x0min = lx;
            if (!any || ty      < y0min) y0min = ty;
            if (!any || lx + gw > x1max) x1max = lx + gw;
            if (!any || ty + gh > y1max) y1max = ty + gh;
            any = true;
        }

        xf += (float)glyph_advance(ch.fi[k], ch.id[k], cp) * ch.scale[k];
        prev_cp = cp; prev_k = k;
    }
    if (!any) return false;

    box->ox = x0min;          box->oy = y0min;
    box->w  = x1max - x0min;  box->h  = y1max - y0min;
    return true;
}

void ttf_run_raster(const char *text, float px, int face, u32 colour,
                    u32 *dst, const TtfRunBox *box)
{
    const u32 rgb = colour & 0x00FFFFFFu;
    const int W = box->w, H = box->h;

    memset(dst, 0, (size_t)W * (size_t)H * sizeof(u32));
    if (!s_ttf_ok) return;

    FaceChain ch;
    chain_of(face, px, &ch);

    float xf = 0.0f;
    int   prev_cp = 0, prev_k = -1;

    for (const char *p = text; *p; ) {
        int cp = utf8_next(&p);
        int k = chain_pick(&ch, cp);
        if (prev_cp && k == prev_k)
            xf += stbtt_GetCodepointKernAdvance(ch.fi[k], prev_cp, cp) * ch.scale[k];

        const unsigned char *bm  = NULL;
        unsigned char       *tmp = NULL;
        int gw = 0, gh = 0, gx = 0, gy = 0;

        const GlyphSlot *g = gc_glyph(ch.fi[k], ch.id[k], px, ch.scale[k], cp);
        if (g) {
            if (g->w > 0 && g->h > 0) {
                bm = s_gc_arena + g->pix;
                gw = g->w; gh = g->h; gx = g->xoff; gy = g->yoff;
            }
        } else {
            tmp = stbtt_GetCodepointBitmap(ch.fi[k], ch.scale[k], ch.scale[k], cp,
                                           &gw, &gh, &gx, &gy);
            bm  = tmp;
        }

        if (bm && gw > 0 && gh > 0) {
            int lx = (int)xf + gx - box->ox;
            int ty = ch.baseline + gy - box->oy;
            for (int r = 0; r < gh; r++) {
                int dy = ty + r;
                if (dy < 0 || dy >= H) continue;
                u32                 *drow = dst + (size_t)dy * (size_t)W;
                const unsigned char *srow = bm  + (size_t)r  * (size_t)gw;
                for (int c = 0; c < gw; c++) {
                    u32 a = srow[c];
                    if (!a) continue;
                    int dx = lx + c;
                    if (dx < 0 || dx >= W) continue;
                    // Combine overlapping glyph boxes with OVER in COVERAGE
                    // space.  Because the whole run is one colour, that is
                    // algebraically identical to compositing the glyphs one
                    // after another onto the destination, which is what the
                    // per-glyph path does -- so kerned pairs that share pixels
                    // come out the same rather than one clobbering the other.
                    u32 cur = drow[dx] >> 24;
                    u32 na  = cur ? cur + a - (cur * a) / 255u : a;
                    drow[dx] = (na << 24) | rgb;
                }
            }
        }
        if (tmp) stbtt_FreeBitmap(tmp, NULL);

        xf += (float)glyph_advance(ch.fi[k], ch.id[k], cp) * ch.scale[k];
        prev_cp = cp; prev_k = k;
    }

    // Bake the gamma correction into the stored alpha.  blit_coverage()
    // composites in linear light; RSX fixed-function blending is plain 8-bit.
    // Storing l2g[a] reconciles them exactly over a black background for ANY
    // text colour, since l2g(a * g2l(fg) / 255) == (l2g(a) / 255) * fg, and
    // closely as the background lightens.  See ui_text_gpu.h.
    for (int y = 0; y < H; y++) {
        u32 *row = dst + (size_t)y * (size_t)W;
        for (int x = 0; x < W; x++) {
            u32 a = row[x] >> 24;
            if (a) row[x] = ((u32)s_l2g[a] << 24) | rgb;
        }
    }
}

// --- icon seam ------------------------------------------------------------
// One glyph from the icon face, measured and rasterized the same way a run is
// but WITHOUT the gamma bake: icons have always composited with the plain
// 8-bit blend (drawIcon passes gamma_aa = false), and running them through the
// linear-light correction would change every icon edge on screen.

bool ttf_icon_box(int codepoint, float px, TtfRunBox *box)
{
    if (!s_icons_ok) return false;
    float scale    = stbtt_ScaleForPixelHeight(&s_icons, px);
    int   baseline = (int)((float)s_ascent[GC_FONT_ICONS] * scale);

    int gx, gy, gw, gh;
    const GlyphSlot *g = gc_glyph(&s_icons, GC_FONT_ICONS, px, scale, codepoint);
    if (g) {
        gx = g->xoff; gy = g->yoff; gw = g->w; gh = g->h;
    } else {
        int a0, b0, a1, b1;
        stbtt_GetCodepointBitmapBox(&s_icons, codepoint, scale, scale,
                                    &a0, &b0, &a1, &b1);
        gx = a0; gy = b0; gw = a1 - a0; gh = b1 - b0;
    }
    if (gw <= 0 || gh <= 0) return false;

    // drawIcon()'s anchor is the top of the line and it adds the baseline
    // itself, so oy carries the baseline here for the queued draw to land on
    // exactly the pixels the CPU path used.
    box->ox = gx;  box->oy = baseline + gy;
    box->w  = gw;  box->h  = gh;
    return true;
}

void ttf_icon_raster(int codepoint, float px, u32 colour,
                     u32 *dst, const TtfRunBox *box)
{
    const u32 rgb = colour & 0x00FFFFFFu;
    const int W = box->w, H = box->h;

    memset(dst, 0, (size_t)W * (size_t)H * sizeof(u32));
    if (!s_icons_ok) return;

    float scale = stbtt_ScaleForPixelHeight(&s_icons, px);

    const unsigned char *bm  = NULL;
    unsigned char       *tmp = NULL;
    int gw = 0, gh = 0, gx = 0, gy = 0;

    const GlyphSlot *g = gc_glyph(&s_icons, GC_FONT_ICONS, px, scale, codepoint);
    if (g) {
        if (g->w > 0 && g->h > 0) {
            bm = s_gc_arena + g->pix;
            gw = g->w; gh = g->h; gx = g->xoff; gy = g->yoff;
        }
    } else {
        tmp = stbtt_GetCodepointBitmap(&s_icons, scale, scale, codepoint,
                                       &gw, &gh, &gx, &gy);
        bm  = tmp;
    }

    if (bm && gw > 0 && gh > 0) {
        int baseline = (int)((float)s_ascent[GC_FONT_ICONS] * scale);
        int lx = gx - box->ox;               // 0 by construction, but the box
        int ty = baseline + gy - box->oy;    // is the caller's to choose
        for (int r = 0; r < gh; r++) {
            int dy = ty + r;
            if (dy < 0 || dy >= H) continue;
            u32                 *drow = dst + (size_t)dy * (size_t)W;
            const unsigned char *srow = bm  + (size_t)r  * (size_t)gw;
            for (int c = 0; c < gw; c++) {
                u32 a = srow[c];
                if (!a) continue;
                int dx = lx + c;
                if (dx < 0 || dx >= W) continue;
                drow[dx] = (a << 24) | rgb;   // RAW coverage: no l2g[]
            }
        }
    }
    if (tmp) stbtt_FreeBitmap(tmp, NULL);
}

void drawTTF_face(u32 x, u32 y, const char *text, float px, u32 color, int face) {
    if (!s_ttf_ok) {
        drawTextScaled(x, y, text, (int)px);
        return;
    }

    // Inside the XMB's collecting window this hands the whole string to the
    // RSX as one cached texture and draws nothing here.  Everywhere else --
    // and whenever the run is too long, the atlas is full, or a CPU compose
    // target is bound -- it returns false and the per-glyph path below runs
    // exactly as it always has.
    if (ui_text_gpu_run(x, y, text, px, color, face)) return;

    FaceChain ch;
    chain_of(face, px, &ch);

    float xf      = (float)x;
    int   prev_cp = 0, prev_k = -1;

    while (*text) {
        int cp = utf8_next(&text);
        int k  = chain_pick(&ch, cp);

        if (prev_cp && k == prev_k)
            xf += stbtt_GetCodepointKernAdvance(ch.fi[k], prev_cp, cp) * ch.scale[k];

        draw_glyph(ch.fi[k], ch.id[k], px, ch.scale[k], cp,
                   (int)xf, (int)y + ch.baseline, color, true);

        xf += (float)glyph_advance(ch.fi[k], ch.id[k], cp) * ch.scale[k];
        prev_cp = cp; prev_k = k;
    }
}


// ---- letter-spacing -------------------------------------------------------
//
// v1.0 sets tracking on the labels it puts in caps -- the tab strip at 0.04em,
// eyebrows at 0.18em -- and caps without tracking look cramped in a way that is
// obvious on a TV.  The text stack had no notion of it, so here it is, as an
// extra advance after each glyph rather than as hidden global state.
//
// These deliberately take the PER-GLYPH CPU path instead of the cached GPU run:
// ui_text_gpu.cpp keys a run on (string, px, colour, face), and tracking is not
// in that key, so a tracked and an untracked draw of the same string would
// collide in the cache and one would render as the other.  Widening the key is
// the better long-term fix; until then the callers are a handful of short
// labels -- exactly one tab label is drawn per frame -- and the glyph cache
// still means nothing rasterizes twice.
//
// Costed deliberately: per-glyph blits are the path that can increment `bpx`
// (see UI-BRIEF.md), so this must stay confined to short strings.  It is not a
// replacement for drawTTF_face on body text.
// Sample a ramp of evenly spaced stops at t in [0,1].  The design spells the
// wordmark's stops at 0/34/67/100%, which is even to within a rounding, so
// even spacing is the ramp rather than an approximation of it.
static u32 ramp_at(const u32 *stop, int n, float t)
{
    if (n <= 0)             return 0;
    if (n == 1 || t <= 0.0f) return stop[0];
    if (t >= 1.0f)           return stop[n - 1];
    float f = t * (float)(n - 1);
    int   i = (int)f;
    float g = f - (float)i;
    u32   a = stop[i], b = stop[i + 1];
    u32 r = (u32)((float)((a >> 16) & 0xFF) * (1.0f - g) + (float)((b >> 16) & 0xFF) * g);
    u32 v = (u32)((float)((a >>  8) & 0xFF) * (1.0f - g) + (float)((b >>  8) & 0xFF) * g);
    u32 l = (u32)((float)( a        & 0xFF) * (1.0f - g) + (float)( b        & 0xFF) * g);
    return (r << 16) | (v << 8) | l;
}

static float run_tracked(u32 x, u32 y, const char *text, float px, u32 color,
                         int face, float track, bool draw,
                         const u32 *ramp, int nramp)
{
    if (!s_ttf_ok) {
        if (draw) drawTextScaled(x, y, text, (int)px);
        return (float)(strlen(text) * (size_t)px);
    }
    FaceChain ch;
    chain_of(face, px, &ch);

    // A ramp is positional, so it needs the run's width before the first glyph
    // is drawn.  The measure pass is the same walk with draw off, and it warms
    // the glyph cache for the pass that follows, so it costs a loop and not a
    // second rasterization.
    float total = 0.0f;
    if (draw && ramp && nramp > 0)
        total = run_tracked(0, 0, text, px, 0, face, track, false, NULL, 0);

    float xf      = draw ? (float)x : 0.0f;
    int   prev_cp = 0, prev_k = -1;
    bool  first   = true;

    while (*text) {
        int cp = utf8_next(&text);
        int k  = chain_pick(&ch, cp);

        if (prev_cp && k == prev_k)
            xf += stbtt_GetCodepointKernAdvance(ch.fi[k], prev_cp, cp) * ch.scale[k];
        if (!first) xf += track;          // the space goes BETWEEN glyphs only

        float adv = (float)glyph_advance(ch.fi[k], ch.id[k], cp) * ch.scale[k];

        if (draw) {
            // PER GLYPH, not per pixel.  A per-pixel gradient would have to
            // reach into the coverage blend, and the only string that wants
            // one is eight letters wide -- at which point each letter is ~12%
            // of the ramp and the steps are not visible on a TV.
            u32 col = color;
            if (ramp && nramp > 0 && total > 0.0f)
                col = ramp_at(ramp, nramp, (xf - (float)x + adv * 0.5f) / total);
            draw_glyph(ch.fi[k], ch.id[k], px, ch.scale[k], cp,
                       (int)xf, (int)y + ch.baseline, col, true);
        }

        xf += adv;
        prev_cp = cp; prev_k = k; first = false;
    }
    return draw ? xf - (float)x : xf;
}

void drawTTF_tracked(u32 x, u32 y, const char *text, float px, u32 color,
                     int face, float track)
{
    if (strobe_test_disable_tracked_text()) return;
    run_tracked(x, y, text, px, color, face, track, true, NULL, 0);
}

// The wordmark's gradient.  Same walk as drawTTF_tracked, with the colour
// taken from the ramp at each glyph's centre instead of being constant.
void drawTTF_ramp(u32 x, u32 y, const char *text, float px,
                  const u32 *stops, int nstops, int face, float track)
{
    if (strobe_test_disable_tracked_text()) return;
    run_tracked(x, y, text, px, stops ? stops[0] : 0, face, track, true,
                stops, nstops);
}

int ttf_text_width_tracked(const char *text, float px, int face, float track)
{
    return (int)run_tracked(0, 0, text, px, 0, face, track, false, NULL, 0);
}

// ASCII-only uppercase, in place, for the labels v1.0 sets in caps.
//
// ASCII-only on purpose: the strings are SERVER LIBRARY NAMES and arrive as
// UTF-8, where every byte of a multi-byte sequence is >= 0x80.  Touching only
// a-z leaves those sequences byte-identical, so a library called "Filmes de
// Ação" keeps its cedilla instead of having its encoding broken by a locale
// toupper() that thinks in bytes.  Accented capitals stay lowercase rather
// than being mangled, which is the right trade for a tab label.
void ui_upper_ascii(char *s)
{
    for (; *s; s++)
        if (*s >= 'a' && *s <= 'z') *s = (char)(*s - 'a' + 'A');
}
void drawTTF(u32 x, u32 y, const char *text, float px, u32 color, bool bold) {
    drawTTF_face(x, y, text, px, color, bold ? UI_FACE_BOLD : UI_FACE_REGULAR);
}

// The y a run should be drawn at for its INK to centre on cy, for any face.
//
// This is the whole of what drawTTF_vcentered used to do inline, lifted out
// because the lockup needs the same answer for a face it cannot reach through
// the bold flag -- and because "centre the ink, not the em box" is the only
// way to sit 14px type against a 21px mark without eyeballing an offset that
// then has to be re-eyeballed at every scale.
//
// Returns false when the string has no ink at all (all spaces), which means
// draw nothing -- not draw it at cy.
bool ttf_center_y(const char *text, float px, int face, int cy, int *out_y)
{
    if (!s_ttf_ok) { *out_y = cy - (int)(px * 0.5f); return true; }
    FaceChain ch;
    chain_of(face, px, &ch);

    int y0min = 0, y1max = 0;
    bool any = false;
    for (const char *p = text; *p; ) {
        int cp = utf8_next(&p);
        int gy0, gy1;
        int k = chain_pick(&ch, cp);
        const GlyphSlot *g = gc_glyph(ch.fi[k], ch.id[k], px, ch.scale[k], cp);
        if (g) {
            if (g->w <= 0 && g->h <= 0) continue;   // space etc.
            gy0 = g->yoff;
            gy1 = g->yoff + g->h;
        } else {
            int gx0, gx1;
            stbtt_GetCodepointBitmapBox(ch.fi[k], cp, ch.scale[k], ch.scale[k],
                                        &gx0, &gy0, &gx1, &gy1);
            if (gx1 <= gx0 && gy1 <= gy0) continue;
        }
        if (!any || gy0 < y0min) y0min = gy0;
        if (!any || gy1 > y1max) y1max = gy1;
        any = true;
    }
    if (!any) return false;

    int y = cy - ch.baseline - (y0min + y1max) / 2;
    *out_y = y < 0 ? 0 : y;
    return true;
}

void drawTTF_vcentered(u32 x, int cy, const char *text, float px, u32 color,
                       bool bold) {
    const int face = bold ? UI_FACE_BOLD : UI_FACE_REGULAR;
    int y;
    // ttf_center_y measures through the glyph cache, which warms it for the
    // draw below -- a centred string still rasterizes at most once, not once
    // per frame.
    if (!ttf_center_y(text, px, face, cy, &y)) return;
    drawTTF(x, (u32)y, text, px, color, bold);
}

void drawIcon(u32 x, u32 y, int codepoint, float px, u32 color) {
    if (!s_icons_ok) return;
    // Inside the XMB's collecting window this becomes one cached quad and
    // draws nothing here.  Icons were the largest thing left reading video
    // memory once text moved to the RSX: the frame line's residual bpx=949 is
    // them and the PS button sprites, and every one of those blended pixels
    // is a 4-byte read from VRAM at the measured 7.7 MB/s.
    if (ui_text_gpu_icon(x, y, codepoint, px, color)) return;

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
    // --font-system (handoff section 3.0): SCE-PS3 Rodin LATIN, for everything.
    //
    // BOTH the regular and bold slots point at Rodin, because section 3.0 is
    // explicit that "hierarchy comes from size, weight and opacity, not from
    // adding faces" and that focused items "change scale, ring and brightness
    // — never weight".  Mixing Rodin with OpenSans-Bold would put two
    // different typefaces on one screen, which is worse than losing the weight.
    //
    // If Rodin ever fails to parse we fall back to OpenSans rather than losing
    // text entirely -- the same rule the subtitle faces follow in face_of().
    s_font_buf = (unsigned char*)Rodin_Latin_ttf;
    if (stbtt_InitFont(&s_font, s_font_buf, 0)) {
        s_ttf_ok = true;
    } else {
        s_font_buf = (unsigned char*)OpenSans_Regular_ttf;
        if (stbtt_InitFont(&s_font, s_font_buf, 0)) s_ttf_ok = true;
        plog("ttf: Rodin failed to parse, fell back to OpenSans");
    }
    if (stbtt_InitFont(&s_font_bold, (unsigned char*)Rodin_Latin_ttf, 0))
        s_ttf_bold_ok = true;
    else if (stbtt_InitFont(&s_font_bold, (unsigned char*)OpenSans_Bold_ttf, 0))
        s_ttf_bold_ok = true;

    // The v1.0 type roles.  Three of these are OTTO (CFF outlines) rather than
    // TrueType; stb_truetype parses and rasterizes them, which was verified
    // against THIS copy of stb before they were embedded rather than assumed --
    // a CFF face that parses but rasterizes blank would look like the text
    // simply vanished.
    //
    // Resolution cache starts empty.  -1 means "not looked up yet"; see
    // chain_pick().
    memset(s_pick, -1, sizeof(s_pick));
    s_pick_init = true;

    // --font-display: media titles.  Subset -- chain_of() gives it Rodin.
    if (stbtt_InitFont(&s_font_display, (unsigned char*)GTAmerica_Expanded_ttf, 0))
        s_display_ok = true;
    // --font-eyebrow: section labels and eyebrows.  Microgramma held the
    // display role before v1.0 and is still bundled, so this costs nothing new.
    if (stbtt_InitFont(&s_font_eyebrow, (unsigned char*)Microgramma_ttf, 0))
        s_eyebrow_ok = true;
    // --font-tab: tab labels, the clock, cast names.  Two weights, which the
    // design does use -- 700 for the strip and clock, 400/500 for the date.
    if (stbtt_InitFont(&s_font_tab, (unsigned char*)Satoshi_Bold_otf, 0))
        s_tab_ok = true;
    if (stbtt_InitFont(&s_font_tab_reg, (unsigned char*)Satoshi_Regular_otf, 0))
        s_tab_reg_ok = true;
    // --font-spec: codec / quality values only.
    if (stbtt_InitFont(&s_font_spec, (unsigned char*)Michroma_ttf, 0))
        s_spec_ok = true;
    // The lockup wordmark.  One string, once a frame -- see UI_FACE_LOCKUP.
    if (stbtt_InitFont(&s_font_lockup, (unsigned char*)Mata_Bold_otf, 0))
        s_lockup_ok = true;
    if (stbtt_InitFont(&s_icons, (unsigned char*)TablerIcons_ttf, 0))
        s_icons_ok = true;
    if (stbtt_InitFont(&s_font_noto, (unsigned char*)NotoSans_Bold_ttf, 0))
        s_noto_ok = true;
    if (stbtt_InitFont(&s_font_robocond, (unsigned char*)RobotoCondensed_Bold_ttf, 0))
        s_robocond_ok = true;

    // Ascent is size-independent — read it once here instead of on every call.
    if (s_ttf_ok)
        stbtt_GetFontVMetrics(&s_font,      &s_ascent[GC_FONT_REG],   NULL, NULL);
    if (s_ttf_bold_ok)
        stbtt_GetFontVMetrics(&s_font_bold, &s_ascent[GC_FONT_BOLD],  NULL, NULL);
    if (s_icons_ok)
        stbtt_GetFontVMetrics(&s_icons,     &s_ascent[GC_FONT_ICONS], NULL, NULL);
    if (s_noto_ok)
        stbtt_GetFontVMetrics(&s_font_noto, &s_ascent[GC_FONT_NOTO],  NULL, NULL);
    if (s_robocond_ok)
        stbtt_GetFontVMetrics(&s_font_robocond, &s_ascent[GC_FONT_ROBOCND], NULL, NULL);
    if (s_display_ok)
        stbtt_GetFontVMetrics(&s_font_display, &s_ascent[GC_FONT_DISPLAY], NULL, NULL);
    if (s_eyebrow_ok)
        stbtt_GetFontVMetrics(&s_font_eyebrow, &s_ascent[GC_FONT_EYEBROW], NULL, NULL);
    if (s_tab_ok)
        stbtt_GetFontVMetrics(&s_font_tab,     &s_ascent[GC_FONT_TAB],     NULL, NULL);
    if (s_tab_reg_ok)
        stbtt_GetFontVMetrics(&s_font_tab_reg, &s_ascent[GC_FONT_TABREG],  NULL, NULL);
    if (s_spec_ok)
        stbtt_GetFontVMetrics(&s_font_spec,    &s_ascent[GC_FONT_SPEC],    NULL, NULL);
    if (s_lockup_ok)
        stbtt_GetFontVMetrics(&s_font_lockup,  &s_ascent[GC_FONT_LOCKUP],  NULL, NULL);

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
