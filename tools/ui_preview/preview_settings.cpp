// Host-side preview of the Settings screen: renders the REAL drawing code from
// source/ui/render/ui_settings.cpp (the block between the "The screen" and
// "Overscan calibration" banners, extracted by build_settings_preview.sh) with
// the real Rodin / Noto JP / Tabler fonts, the real model and the real catalog,
// at several resolutions and languages, into PPM frames.
//
//   ./build_settings_preview.sh          (writes settings_*.ppm and .png)
//
// It stands in for a TV only for layout: overlaps, clipping, what fits.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>

#define STB_TRUETYPE_IMPLEMENTATION
#include "../../source/gfx/stb_truetype.h"

extern "C" {
#include "../../source/ui/settings_model.h"
#include "../../source/i18n/i18n.h"
}
#include "../../source/gfx/rodin_latin.h"
#include "../../source/gfx/notosansjp.h"
#include "../../source/ui/fonts/tabler_icons.h"
#include "../../source/ui/icons.h"

typedef uint32_t u32;
typedef uint8_t  u8;

// ---- the screen being rendered ----
static int display_width = 1280, display_height = 720;
static u32 *fb = NULL;
static int g_uis_pct = 0;
static int overscan_x(void) { return 0; }
static int overscan_y(void) { return 0; }

// ---- ui_visuals.h's scale, verbatim (including the low-resolution floor) ----
static inline int uis_safe_w(void) { return display_width  - 2 * overscan_x(); }
static inline int uis_safe_h(void) { return display_height - 2 * overscan_y(); }
static inline int uis_w(int px) { return g_uis_pct ? px * g_uis_pct * uis_safe_w() / (100 * display_width) : px * uis_safe_w() / 1280; }
static inline int uis_h(int px) { return g_uis_pct ? px * g_uis_pct * uis_safe_h() / (100 * display_height) : px * uis_safe_h() / 720; }
static inline float uis_tf(float px) {
    const float s = g_uis_pct ? px * (float)g_uis_pct / 100.0f * (float)uis_safe_h() / (float)display_height
                              : px * (float)uis_safe_h() / 720.0f;
    if (display_height < 720) {
        const float floor_px = px < 11.0f ? px : 11.0f;
        if (s < floor_px) return floor_px;
    }
    return s;
}
#define UIS_W(px)  uis_w((int)(px))
#define UIS_H(px)  uis_h((int)(px))
#define UIS_TF(px) uis_tf((float)(px))

#define XMB_OY          overscan_y()
#define XMB_OX          overscan_x()
#define XMB_DIVIDER_Y   (XMB_OY + UIS_H(214))           // the spine's L2 header divider
#define XMB_CONTENT_Y   (XMB_DIVIDER_Y + UIS_H(30))
#define XMB_BOTTOM_PAD  (UIS_H(70) + XMB_OY)
#define XMB_ITEM_PAD    (UIS_W(40) + XMB_OX)
#define XMB_LIST_W      (display_width - 2 * XMB_ITEM_PAD < UIS_W(780) ? display_width - 2 * XMB_ITEM_PAD : UIS_W(780))

#define XMB_BG          0x000B0E1EUL
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

// ---- fonts ----
static stbtt_fontinfo s_rodin, s_jp, s_icons;
static float s_jp_k;

static int next_cp(const char **p) {
    const unsigned char *s = (const unsigned char *)*p;
    int cp, n;
    if (s[0] < 0x80)      { cp = s[0]; n = 1; }
    else if (s[0] < 0xE0) { cp = s[0] & 0x1F; n = 2; }
    else if (s[0] < 0xF0) { cp = s[0] & 0x0F; n = 3; }
    else                  { cp = s[0] & 0x07; n = 4; }
    for (int i = 1; i < n; i++) cp = (cp << 6) | (s[i] & 0x3F);
    *p += n;
    return cp;
}

static int ttf_text_width(const char *t, float px, bool = false) {
    const float sr = stbtt_ScaleForPixelHeight(&s_rodin, px);
    float w = 0;
    while (*t) {
        const int cp = next_cp(&t);
        int adv = 0;
        if (stbtt_FindGlyphIndex(&s_rodin, cp)) { stbtt_GetCodepointHMetrics(&s_rodin, cp, &adv, NULL); w += adv * sr; }
        else                                    { stbtt_GetCodepointHMetrics(&s_jp, cp, &adv, NULL);    w += adv * px * s_jp_k; }
    }
    return (int)(w + 0.5f);
}

static void blend(int x, int y, u32 colour, int a) {
    if (x < 0 || y < 0 || x >= display_width || y >= display_height || a <= 0) return;
    u32 bg = fb[y * display_width + x];
    u32 r = (((colour >> 16) & 255) * a + ((bg >> 16) & 255) * (255 - a)) / 255;
    u32 g = (((colour >> 8) & 255) * a + ((bg >> 8) & 255) * (255 - a)) / 255;
    u32 b = ((colour & 255) * a + (bg & 255) * (255 - a)) / 255;
    fb[y * display_width + x] = (r << 16) | (g << 8) | b;
}

static void draw_glyph(stbtt_fontinfo *fi, int cp, float scale, float baseline_scale, int x, int y_top, float px, u32 colour) {
    int asc; stbtt_GetFontVMetrics(fi, &asc, NULL, NULL);
    int w, h, xo, yo;
    unsigned char *bm = stbtt_GetCodepointBitmap(fi, scale, scale, cp, &w, &h, &xo, &yo);
    if (!bm) return;
    const int base = y_top + (int)(asc * baseline_scale);
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++) blend(x + xo + i, base + yo + j, colour, bm[j * w + i]);
    stbtt_FreeBitmap(bm, NULL);
    (void)px;
}

static void drawRect(u32 x, u32 y, u32 w, u32 h, u32 colour) {
    for (u32 j = y; j < y + h && (int)j < display_height; j++)
        for (u32 i = x; i < x + w && (int)i < display_width; i++) fb[j * display_width + i] = colour;
}

static void drawTTF(u32 x, u32 y, const char *t, float px, u32 colour, bool = false) {
    const float sr = stbtt_ScaleForPixelHeight(&s_rodin, px);
    float pen = (float)x;
    while (*t) {
        const int cp = next_cp(&t);
        int adv = 0;
        if (stbtt_FindGlyphIndex(&s_rodin, cp)) {
            stbtt_GetCodepointHMetrics(&s_rodin, cp, &adv, NULL);
            draw_glyph(&s_rodin, cp, sr, sr, (int)pen, (int)y, px, colour);
            pen += adv * sr;
        } else {
            const float sj = px * s_jp_k;
            stbtt_GetCodepointHMetrics(&s_jp, cp, &adv, NULL);
            draw_glyph(&s_jp, cp, sj, sj, (int)pen, (int)y, px, colour);
            pen += adv * sj;
        }
    }
}

static void drawIcon(u32 x, u32 y, int cp, float px, u32 colour) {
    const float sc = stbtt_ScaleForPixelHeight(&s_icons, px);
    draw_glyph(&s_icons, cp, sc, sc, (int)x, (int)y, px, colour);
}

// ---- the app's own things the block uses ----
static const char *tr_(const char *s) { return tr(s); }
#define TR(s)  tr(s)
#define TRN(s) (s)
#define APP_VERSION "3.2"
static char g_username[64] = "Ethan";
static char g_server[256]  = "http://192.168.0.47:8096";
static bool g_spine_on = true;
int  g_settings_sel = 0;
bool g_settings_confirm = false;
int  g_settings_cat = -1;
int  g_settings_catsel = 0;

static const char *mock_value(setting_id id, bool *lit);
static setting_id g_val_id;
static void mock_value_fn(char *b, int c, bool *l) { snprintf(b, (size_t)c, "%s", mock_value(g_val_id, l)); }
struct UiRow { setting_id id; int icon; void (*value)(char *buf, int cap, bool *lit); };
static const UiRow *ui_of(setting_id id) {
    static UiRow r;
    r.id = id;
    g_val_id = id;
    r.value = mock_value_fn;
    switch (id) {
    case SET_HD1080: case SET_DIRECT_STREAM: r.icon = ICON_MOVIE; break;
    case SET_24HZ: case SET_UPDATE: case SET_SCREEN_SIZE: case SET_WAKE: r.icon = ICON_TV; break;
    case SET_AUTOSKIP: r.icon = ICON_MOVIE; break;
    case SET_AUDIO_OUT: case SET_DIALOGUE: case SET_WAVE_INT: r.icon = ICON_MUSIC; break;
    case SET_SUB_FONT: case SET_SUB_COLOUR: case SET_LANGUAGE: r.icon = ICON_TV; break;
    case SET_THEME: case SET_PARTICLES: case SET_DAYNIGHT: r.icon = ICON_PHOTO; break;
    case SET_DL_LOCATION: case SET_DOWNLOADS: r.icon = ICON_COLLECTIONS; break;
    case SET_OFFLINE_LIB: r.icon = ICON_PLAY; break;
    case SET_LOGOUT: r.icon = ICON_LOGOUT; break;
    case SET_WOL_MAC: r.icon = ICON_SETTINGS; break;
    default: r.icon = ICON_BUG; break;
    }
    return &r;
}

static const setting_row *row_at(int i) { return settings_row(i); }

#include "settings_block.inc"

// value text for the preview (the app computes these from live state)
static const char *mock_value(setting_id id, bool *lit) {
    *lit = true;
    switch (id) {
    case SET_HD1080: return "On";
    case SET_24HZ: return "Auto";
    case SET_DIRECT_STREAM: return "Auto";
    case SET_AUTOSKIP: *lit = false; return "Off";
    case SET_AUDIO_OUT: return "Dolby Digital";
    case SET_DIALOGUE: return "Medium";
    case SET_SUB_FONT: return "Default";
    case SET_SUB_COLOUR: return "White";
    case SET_DL_LOCATION: return "Internal HDD";
    case SET_DOWNLOADS: return "None";
    case SET_OFFLINE_LIB: return "3 items";
    case SET_SCREEN_SIZE: return "Calibrate";
    case SET_LANGUAGE: return "Auto";
    case SET_THEME: return "Jellywave";
    case SET_DAYNIGHT: return "On";
    case SET_WAVE_INT: return "Normal";
    case SET_PARTICLES: *lit = false; return "Off";
    case SET_UPDATE: return "Up to date (3.2)";
    case SET_SEND_LOG: *lit = false; return "X to send";
    case SET_WOL_MAC: return "00:D8:61:D5:62:58";
    case SET_WAKE: *lit = false; return "X to send";
    case SET_DEBUG_LOG: return "On";
    case SET_STATS: *lit = false; return "Off";
    default: *lit = false; return "";
    }
}

static unsigned char *load_blob(const unsigned char *p) { return (unsigned char *)p; }

static void frame(const char *name, int w, int h, int lang, int cat, int catsel, int sel) {
    display_width = w; display_height = h;
    fb = (u32 *)malloc((size_t)w * (size_t)h * 4);
    for (int i = 0; i < w * h; i++) fb[i] = XMB_BG;
    i18n_set((i18n_lang)lang);
    g_settings_cat = cat; g_settings_catsel = catsel; g_settings_sel = sel;

    // The chrome above the content: the spine's divider and the tab name.
    drawRect(0, (u32)XMB_DIVIDER_Y, (u32)w, 1, XMB_HAIRLINE);
    drawTTF((u32)XMB_ITEM_PAD, (u32)UIS_H(150), "Settings", UIS_TF(30), XMB_WHITE);

    xmb_cpu_draw_settings();
    xmb_draw_settings();

    // The hints bar, as text.
    drawTTF((u32)XMB_ITEM_PAD, (u32)(h - XMB_BOTTOM_PAD + UIS_H(28)),
            cat < 0 ? "X Open     O Back" : "X Select     O Back     L2/R2 Category", UIS_TF(12.5f), XMB_TEXT_DIM);

    char path[160];
    snprintf(path, sizeof path, "%s.ppm", name);
    FILE *f = fopen(path, "wb");
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int i = 0; i < w * h; i++) { u8 px[3] = { (u8)(fb[i] >> 16), (u8)(fb[i] >> 8), (u8)fb[i] }; fwrite(px, 1, 3, f); }
    fclose(f);
    free(fb);
    printf("%s  (%dx%d, lang %d, cat %d)\n", path, w, h, lang, cat);
}

int main() {
    stbtt_InitFont(&s_rodin, load_blob(Rodin_Latin_ttf), 0);
    stbtt_InitFont(&s_jp, load_blob(NotoSansJP_otf), 0);
    stbtt_InitFont(&s_icons, load_blob(TablerIcons_ttf), 0);
    int asc, desc;
    stbtt_GetFontVMetrics(&s_rodin, &asc, &desc, NULL);
    const float rodin_upem = 1.0f / stbtt_ScaleForMappingEmToPixels(&s_rodin, 1.0f);
    const float jp_upem    = 1.0f / stbtt_ScaleForMappingEmToPixels(&s_jp, 1.0f);
    s_jp_k = (rodin_upem / (float)(asc - desc)) / jp_upem;

    frame("settings_720_categories",  1280, 720, 0, -1, 0, 0);
    frame("settings_720_categories_sel3", 1280, 720, 0, -1, 3, 0);
    frame("settings_720_playback",    1280, 720, 0, SEC_PLAYBACK, 0, settings_category_row(SEC_PLAYBACK, 0));
    frame("settings_720_system_sel4", 1280, 720, 0, SEC_SYSTEM, 5, settings_category_row(SEC_SYSTEM, 4));
    frame("settings_720_system_last", 1280, 720, 0, SEC_SYSTEM, 5, settings_category_row(SEC_SYSTEM, settings_category_row_count(SEC_SYSTEM) - 1));
    frame("settings_1080_categories", 1920, 1080, 0, -1, 1, 0);
    frame("settings_480_categories",  720, 480, 0, -1, 2, 0);
    frame("settings_480_audio",       720, 480, 0, SEC_AUDIO, 1, settings_category_row(SEC_AUDIO, 0));
    frame("settings_480_system",      720, 480, 0, SEC_SYSTEM, 5, settings_category_row(SEC_SYSTEM, 3));
    frame("settings_de_categories",   1280, 720, 3, -1, 0, 0);
    frame("settings_de_system",       1280, 720, 3, SEC_SYSTEM, 5, settings_category_row(SEC_SYSTEM, 2));
    frame("settings_fr_downloads",    1280, 720, 4, SEC_DOWNLOADS, 3, settings_category_row(SEC_DOWNLOADS, 0));
    frame("settings_ja_categories",   1280, 720, 1, -1, 4, 0);
    return 0;
}
