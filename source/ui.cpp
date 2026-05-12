#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>

#include <rsx/rsx.h>
#include <sysutil/sysutil.h>

#include "ui.h"
#include "bitmap.h"
#include "font8x8.xpm"
#include "jellyfin_api.h"
#include "player.h"
#include "timing.h"

static Bitmap fontBitmap;

ButtonState btn_cur  = {0};
ButtonState btn_prev = {0};

void update_buttons(padData *pad) {
    btn_prev         = btn_cur;
    btn_cur.up       = pad->BTN_UP;
    btn_cur.down     = pad->BTN_DOWN;
    btn_cur.left     = pad->BTN_LEFT;
    btn_cur.right    = pad->BTN_RIGHT;
    btn_cur.cross    = pad->BTN_CROSS;
    btn_cur.circle   = pad->BTN_CIRCLE;
    btn_cur.square   = pad->BTN_SQUARE;
    btn_cur.triangle = pad->BTN_TRIANGLE;
    btn_cur.start    = pad->BTN_START;
    btn_cur.select   = pad->BTN_SELECT;
    btn_cur.l1       = pad->BTN_L1;
    btn_cur.r1       = pad->BTN_R1;
}

void init_btns(void) {
    padInfo pi; padData pd;
    ioPadGetInfo(&pi);
    for (int i = 0; i < MAX_PADS; i++) {
        if (!pi.status[i]) continue;
        ioPadGetData(i, &pd);
        update_buttons(&pd);
    }
    btn_prev = btn_cur;
}

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

// Render text at a custom pixel size (px = output pixels per 8x8 glyph).
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
    drawText(40, 30, "JELLYFIN PS3");
    drawText(40, 30+LINE_HEIGHT, "------------");
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
// Legacy on-screen keyboard (used by login / server-url screens)
// -------------------------------------------------------

static const char *KB_ROWS[] = {
    "1234567890",
    "qwertyuiop",
    "asdfghjkl",
    "zxcvbnm",
};
#define KB_LETTER_ROWS 4

typedef struct { const char *label; char value; } SpecialKey;
static const SpecialKey SPECIAL[] = {
    {"SPC",' '}, {".",'.'},  {":",':'}, {"/",'/'},
    {"@",'@'},   {"-",'-'},  {"_",'_'}, {"DEL",'\b'}, {"OK",'\r'},
};
#define SPECIAL_N  9
#define TOTAL_ROWS (KB_LETTER_ROWS + 1)

static int  kb_row  = 0;
static int  kb_col  = 0;
static bool kb_caps = false;

static int row_len(int r) {
    return (r < KB_LETTER_ROWS) ? (int)strlen(KB_ROWS[r]) : SPECIAL_N;
}

static char current_key_value(void) {
    if (kb_row < KB_LETTER_ROWS) {
        char c = KB_ROWS[kb_row][kb_col];
        return kb_caps ? (char)toupper(c) : c;
    }
    return SPECIAL[kb_col].value;
}

static void draw_keyboard(const char *prompt, const char *input, bool is_password) {
    drawHeader();
    drawTextf(40, 85, "%s", prompt);

    char display[256] = "";
    int ilen = strlen(input);
    if (is_password) {
        memset(display, '*', ilen);
        display[ilen] = '\0';
    } else {
        if (ilen > 34) strncpy(display, input + ilen - 34, 35);
        else           strncpy(display, input, 255);
    }
    drawTextf(40, 115, "> %s_", display);
    drawText(40, 115+LINE_HEIGHT, "----------------------------------------");
    if (kb_caps) drawText(40, 115+LINE_HEIGHT*2, "CAPS ON");

    int kb_x = 50, kb_y = 175;
    int key_h = LINE_HEIGHT + 8;

    for (int r = 0; r < TOTAL_ROWS; r++) {
        int key_w = (r < KB_LETTER_ROWS) ? (CHAR_SIZE + 16) : (CHAR_SIZE * 3 + 8);
        int rlen  = row_len(r);
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
                drawText(xk - CHAR_SIZE, yk, sel_buf);
            } else {
                drawText(xk, yk, label);
            }
        }
    }

    drawText(40, 660, "Dpad:move  X:type  Tri:caps  Sq:del  START:done  SEL:cancel");
    flip();
}

int get_input(char *out, int max_len, const char *prompt, bool is_password) {
    out[0]  = '\0';
    kb_row  = 0; kb_col = 0; kb_caps = false;

    padInfo padinfo; padData paddata;
    init_btns();

    while (running) {
        sysUtilCheckCallback();
        ioPadGetInfo(&padinfo);
        for (int i = 0; i < MAX_PADS; i++) {
            if (!padinfo.status[i]) continue;
            ioPadGetData(i, &paddata);
            update_buttons(&paddata);

            int rlen = row_len(kb_row);

            if (BTN_PRESSED(up)) {
                kb_row = (kb_row - 1 + TOTAL_ROWS) % TOTAL_ROWS;
                int nl = row_len(kb_row); if (kb_col >= nl) kb_col = nl - 1;
            }
            if (BTN_PRESSED(down)) {
                kb_row = (kb_row + 1) % TOTAL_ROWS;
                int nl = row_len(kb_row); if (kb_col >= nl) kb_col = nl - 1;
            }
            if (BTN_PRESSED(left))  kb_col = (kb_col - 1 + rlen) % rlen;
            if (BTN_PRESSED(right)) kb_col = (kb_col + 1) % rlen;

            if (BTN_PRESSED(triangle)) kb_caps = !kb_caps;

            if (BTN_PRESSED(cross)) {
                char ch = current_key_value();
                if (ch == '\r') return 1;
                if (ch == '\b') { int len = strlen(out); if (len > 0) out[len-1] = '\0'; }
                else            { int len = strlen(out); if (len < max_len-1) { out[len]=ch; out[len+1]='\0'; } }
            }
            if (BTN_PRESSED(square)) { int len = strlen(out); if (len > 0) out[len-1] = '\0'; }
            if (BTN_PRESSED(start))  return 1;
            if (BTN_PRESSED(select)) return -1;
        }

        draw_keyboard(prompt, out, is_password);
    }
    return -1;
}

// -------------------------------------------------------
// Wave background
// -------------------------------------------------------

// Pre-blended against background 0x0008060F:
//   wave_blended = round(alpha * wave_rgb + (1-alpha) * bg_rgb)
static const u32 WAVE_COLOR[3] = {
    0x00221641,  // rgba(55,35,105,0.55)  → R=34 G=22 B=65
    0x001A1032,  // rgba(44,26,84, 0.50)  → R=26 G=16 B=50
    0x00130B23,  // rgba(32,16,60, 0.45)  → R=19 G=11 B=35
};

static const float WAVE_AMP[3]   = { 16.0f, 12.0f, 8.0f  };
static const float WAVE_FREQ[3]  = { 0.008f, 0.013f, 0.019f };
static const float WAVE_SPEED[3] = { 0.6f,  0.9f,  1.5f  };
// y-frac measured from BOTTOM → center_y (from top) = H * (1 - frac)
static const float WAVE_YFRAC[3] = { 0.08f, 0.14f, 0.22f };

static float g_wave_phase = 0.0f;

static void wave_draw(void) {
    int W = (int)display_width;
    int H = (int)display_height;

    // Wave center Y positions (from top of screen)
    float cy[3];
    for (int w = 0; w < 3; w++)
        cy[w] = H * (1.0f - WAVE_YFRAC[w]);

    // For each x column, compute the wave Y for each layer.
    // Reuse a static buffer to avoid stack pressure on PPU.
    static float wy[3][1920];
    int cols = (W < 1920) ? W : 1920;

    for (int x = 0; x < cols; x++) {
        for (int wi = 0; wi < 3; wi++) {
            float p = g_wave_phase * WAVE_SPEED[wi];
            float a = WAVE_AMP[wi];
            float f = WAVE_FREQ[wi];
            wy[wi][x] = cy[wi]
                + sinf(x * f + p)           * a
                + sinf(x * f * 1.65f + p * 0.6f) * a * 0.42f;
        }
    }

    // Find the topmost scanline any wave reaches.
    int strip_top = H;
    for (int wi = 0; wi < 3; wi++)
        for (int x = 0; x < cols; x++) {
            int iy = (int)wy[wi][x];
            if (iy < strip_top) strip_top = iy;
        }
    if (strip_top < 0) strip_top = 0;

    // Fill scanlines back-to-front (layer 2 first, layer 0 last/on-top)
    for (int y = strip_top; y < H; y++) {
        u32 *row = color_buffer[curr_fb] + y * W;
        for (int x = 0; x < cols; x++) {
            u32 px = row[x]; // already cleared to BG by cpuClearFb
            if (y > (int)wy[2][x]) px = WAVE_COLOR[2];
            if (y > (int)wy[1][x]) px = WAVE_COLOR[1];
            if (y > (int)wy[0][x]) px = WAVE_COLOR[0];
            row[x] = px;
        }
    }
}

// -------------------------------------------------------
// XMB tab / item data
// -------------------------------------------------------

#define XMB_TAB_SEARCH      0
#define XMB_TAB_MOVIES      1
#define XMB_TAB_TV          2
#define XMB_TAB_MUSIC       3
#define XMB_TAB_COLLECTIONS 4
#define XMB_TAB_SETTINGS    5
#define XMB_TAB_COUNT       6

typedef struct {
    const char *label;
    const char *icon;   // short ASCII glyph drawn as the tab icon
    char library_id[64];
    bool enabled;
} XMBTab;

static XMBTab g_tabs[XMB_TAB_COUNT] = {
    {"SEARCH",      "?",  "", true },
    {"MOVIES",      "#",  "", false},
    {"TV SHOWS",    "=",  "", false},
    {"MUSIC",       "~",  "", true },
    {"COLLECTIONS", "+",  "", false},
    {"SETTINGS",    "*",  "", true },
};

#define XMB_ITEMS_MAX 50

static XMBItem g_items[XMB_TAB_COUNT][XMB_ITEMS_MAX];
static int     g_item_count[XMB_TAB_COUNT];
static bool    g_items_loaded[XMB_TAB_COUNT];

// UI state
static int  g_active_tab = XMB_TAB_MOVIES;
static int  g_sel        = 0;
static int  g_scroll_top = 0;

// Search OSK state
#define OSK_ROWS_N 4
static const char *OSK_LETTERS[OSK_ROWS_N] = {
    "1234567890",
    "QWERTYUIOP",
    "ASDFGHJKL",
    "ZXCVBNM",  // index 7 = "#+=" toggle key
};
static const char *OSK_SYMBOLS[OSK_ROWS_N] = {
    "!@#$%^&*()",
    "-_=+[]{}|\\",
    ":;\"'`~<>?",
    ".,/!",       // index 4 = "ABC" back key
};
static int  g_osk_row    = 0;
static int  g_osk_col    = 0;
static bool g_osk_sym    = false;  // false=letters true=symbols
static char g_search_buf[64];
static int  g_search_results_count = 0;
static XMBItem g_search_results[XMB_ITEMS_MAX];

// -------------------------------------------------------
// XMB JSON parsing helpers (local, avoids changing jellyfin_api.cpp)
// -------------------------------------------------------

static int xmb_json_str_range(const char *start, int len,
                               const char *key, char *out, int out_size) {
    char search[64];
    snprintf(search, sizeof(search), "\"%s\":\"", key);
    int slen = strlen(search);
    const char *p = start, *end = start + len;
    while (p + slen <= end) {
        if (memcmp(p, search, slen) == 0) {
            p += slen;
            int i = 0;
            while (p < end && *p != '"' && i < out_size-1) out[i++] = *p++;
            out[i] = '\0';
            return 1;
        }
        p++;
    }
    out[0] = '\0';
    return 0;
}

static int xmb_json_int_range(const char *start, int len,
                               const char *key, int def) {
    char search[64];
    snprintf(search, sizeof(search), "\"%s\":", key);
    int slen = strlen(search);
    const char *p = start, *end = start + len;
    while (p + slen <= end) {
        if (memcmp(p, search, slen) == 0) {
            p += slen;
            while (p < end && *p == ' ') p++;
            if (*p < '0' || *p > '9') return def;
            return atoi(p);
        }
        p++;
    }
    return def;
}

static long long xmb_json_ll_range(const char *start, int len,
                                    const char *key, long long def) {
    char search[64];
    snprintf(search, sizeof(search), "\"%s\":", key);
    int slen = strlen(search);
    const char *p = start, *end = start + len;
    while (p + slen <= end) {
        if (memcmp(p, search, slen) == 0) {
            p += slen;
            while (p < end && *p == ' ') p++;
            if (*p < '0' || *p > '9') return def;
            long long v = 0;
            while (p < end && *p >= '0' && *p <= '9') v = v * 10 + (*p++ - '0');
            return v;
        }
        p++;
    }
    return def;
}

// Parse first element of a JSON string array: "Key":["Value",...]
static int xmb_json_first_arr_str(const char *start, int len,
                                   const char *key, char *out, int out_size) {
    char search[64];
    snprintf(search, sizeof(search), "\"%s\":[\"", key);
    int slen = strlen(search);
    const char *p = start, *end = start + len;
    while (p + slen <= end) {
        if (memcmp(p, search, slen) == 0) {
            p += slen;
            int i = 0;
            while (p < end && *p != '"' && i < out_size-1) out[i++] = *p++;
            out[i] = '\0';
            return 1;
        }
        p++;
    }
    out[0] = '\0';
    return 0;
}

// Parse full list of XMBItems from a Jellyfin JSON Items response.
// Expects Fields=Genres,RunTimeTicks,ProductionYear,Container in the request.
static int parse_xmb_items(const char *json, XMBItem *arr, int max) {
    const char *p = strstr(json, "\"Items\":[");
    if (!p) return 0;
    p += 9;

    int count = 0;
    while (count < max && *p) {
        while (*p && *p != '{' && *p != ']') p++;
        if (!*p || *p == ']') break;

        const char *obj = p;
        int  depth = 0;
        bool in_str = false, esc = false;
        while (*p) {
            char c = *p;
            if (esc) { esc = false; }
            else if (in_str) { if (c == '\\') esc = true; else if (c == '"') in_str = false; }
            else { if (c == '"') in_str = true; else if (c == '{') depth++; else if (c == '}') { if (--depth == 0) { p++; break; } } }
            p++;
        }
        int olen = (int)(p - obj);

        XMBItem it; memset(&it, 0, sizeof(it));
        xmb_json_str_range(obj, olen, "Id",   it.id,   sizeof(it.id));
        xmb_json_str_range(obj, olen, "Name", it.name, sizeof(it.name));
        xmb_json_str_range(obj, olen, "Type", it.type, sizeof(it.type));
        if (!it.id[0]) continue;

        // Year
        int year = xmb_json_int_range(obj, olen, "ProductionYear", 0);
        if (year > 0) snprintf(it.year_str, sizeof(it.year_str), "%d", year);

        // Duration from RunTimeTicks (100ns units)
        long long ticks = xmb_json_ll_range(obj, olen, "RunTimeTicks", 0);
        if (ticks > 0) {
            int total_min = (int)(ticks / 600000000LL);
            int h = total_min / 60, m = total_min % 60;
            if (h > 0) snprintf(it.duration_str, sizeof(it.duration_str), "%dh %dm", h, m);
            else        snprintf(it.duration_str, sizeof(it.duration_str), "%dm", m);
        }

        // First genre
        xmb_json_first_arr_str(obj, olen, "Genres", it.genre, sizeof(it.genre));

        // Codec badge — Container tells us the mux, not codec, but most MKV/MP4
        // content in typical Jellyfin setups is H.264.  Proper codec detection
        // requires MediaSources; improve here if needed.
        char container[16] = "";
        xmb_json_str_range(obj, olen, "Container", container, sizeof(container));
        if (strstr(container, "hevc") || strstr(container, "h265") ||
            strstr(container, "265"))
            strncpy(it.codec, "H.265", sizeof(it.codec)-1);
        else
            strncpy(it.codec, "H.264", sizeof(it.codec)-1);

        decode_unicode_escapes(it.name);
        decode_unicode_escapes(it.genre);
        arr[count++] = it;
    }
    return count;
}

// -------------------------------------------------------
// XMB layout constants (pixel values, assume 720p minimum)
// -------------------------------------------------------

#define XMB_BG          0x0008060FUL   // dark near-black navy
#define XMB_DIVIDER_CLR 0x00554077UL   // muted purple divider
#define XMB_ACCENT      0x007C3CEAUL   // bright purple accent
#define XMB_HIGHLIGHT   0x001D1040UL   // selected row bg
#define XMB_THUMB_DIM   0x00241444UL   // thumb placeholder (dark purple)
#define XMB_BADGE_BG    0x00302050UL   // codec badge background
#define XMB_KEY_NORMAL  0x001A103AUL   // unselected OSK key bg
#define XMB_KEY_SEL     0x005A3AB0UL   // selected OSK key bg

#define XMB_TOPBAR_H    64
#define XMB_TABBAR_H    80
#define XMB_DIVIDER_Y   (XMB_TOPBAR_H + XMB_TABBAR_H)
#define XMB_CONTENT_Y   (XMB_DIVIDER_Y + 2)
#define XMB_BOTTOM_PAD  70
#define XMB_ITEM_H      90
#define XMB_THUMB_W     52
#define XMB_THUMB_H     74
#define XMB_ITEM_PAD    40

// Items visible simultaneously in the list area
#define XMB_ITEMS_VIS ((int)((display_height - XMB_CONTENT_Y - XMB_BOTTOM_PAD) / XMB_ITEM_H))

// OSK constants (pixels)
#define OSK_KEY_W    80
#define OSK_KEY_H    44
#define OSK_GAP       8
#define OSK_STEP_X   (OSK_KEY_W + OSK_GAP)  // 88
#define OSK_STEP_Y   (OSK_KEY_H + OSK_GAP)  // 52

// -------------------------------------------------------
// XMB API fetch helpers
// -------------------------------------------------------

static void xmb_detect_tabs(void) {
    char url[512];
    snprintf(url, sizeof(url), "%s/Users/%s/Views", g_server, g_userid);
    int status = http_request(0, url, NULL, g_token, responseBuffer, RESPONSE_SIZE);
    if (status != 200) return;

    // Walk items in the Views response to find which library types exist.
    const char *p = strstr(responseBuffer, "\"Items\":[");
    if (!p) return;
    p += 9;

    while (*p) {
        while (*p && *p != '{' && *p != ']') p++;
        if (!*p || *p == ']') break;
        const char *obj = p;
        int depth = 0; bool in_str = false, esc = false;
        while (*p) {
            char c = *p;
            if (esc) { esc = false; }
            else if (in_str) { if (c=='\\') esc=true; else if (c=='"') in_str=false; }
            else { if (c=='"') in_str=true; else if (c=='{') depth++; else if (c=='}') { if (--depth==0){p++;break;} } }
            p++;
        }
        int olen = (int)(p - obj);

        char ct[32] = "", id[64] = "";
        xmb_json_str_range(obj, olen, "CollectionType", ct, sizeof(ct));
        xmb_json_str_range(obj, olen, "Id",             id, sizeof(id));

        if      (strcmp(ct, "movies")   == 0) { g_tabs[XMB_TAB_MOVIES].enabled=true;      strncpy(g_tabs[XMB_TAB_MOVIES].library_id,      id, 63); }
        else if (strcmp(ct, "tvshows")  == 0) { g_tabs[XMB_TAB_TV].enabled=true;           strncpy(g_tabs[XMB_TAB_TV].library_id,          id, 63); }
        else if (strcmp(ct, "music")    == 0) { g_tabs[XMB_TAB_MUSIC].enabled=true;        strncpy(g_tabs[XMB_TAB_MUSIC].library_id,       id, 63); }
        else if (strcmp(ct, "boxsets")  == 0) { g_tabs[XMB_TAB_COLLECTIONS].enabled=true;  strncpy(g_tabs[XMB_TAB_COLLECTIONS].library_id, id, 63); }
    }
}

static void xmb_fetch_tab_items(int tab) {
    if (g_items_loaded[tab]) return;
    g_item_count[tab] = 0;

    const char *lib_id = g_tabs[tab].library_id;
    if (!lib_id[0]) { g_items_loaded[tab] = true; return; }

    char url[512];
    snprintf(url, sizeof(url),
        "%s/Users/%s/Items?ParentId=%s&Limit=%d"
        "&SortBy=SortName&SortOrder=Ascending"
        "&Fields=Genres,RunTimeTicks,ProductionYear,Container",
        g_server, g_userid, lib_id, XMB_ITEMS_MAX);

    int status = http_request(0, url, NULL, g_token, responseBuffer, RESPONSE_SIZE);
    if (status == 200)
        g_item_count[tab] = parse_xmb_items(responseBuffer, g_items[tab], XMB_ITEMS_MAX);
    g_items_loaded[tab] = true;
}

static void xmb_do_search(void) {
    g_search_results_count = 0;
    if (!g_search_buf[0]) return;

    char encoded[192];
    url_encode_query(g_search_buf, encoded, sizeof(encoded));

    char url[512];
    snprintf(url, sizeof(url),
        "%s/Users/%s/Items?searchTerm=%s&Recursive=true"
        "&IncludeItemTypes=Movie,Series,Episode&Limit=%d"
        "&SortBy=SortName&SortOrder=Ascending"
        "&Fields=Genres,RunTimeTicks,ProductionYear,Container",
        g_server, g_userid, encoded, XMB_ITEMS_MAX);

    int status = http_request(0, url, NULL, g_token, responseBuffer, RESPONSE_SIZE);
    if (status == 200)
        g_search_results_count = parse_xmb_items(responseBuffer, g_search_results, XMB_ITEMS_MAX);
}

// -------------------------------------------------------
// XMB rendering helpers
// -------------------------------------------------------

// Returns the display-width x-centre of tab slot t.
static int xmb_tab_cx(int t) {
    return (int)(((t + 0.5f) * display_width) / XMB_TAB_COUNT);
}

static void xmb_draw_tabs(void) {
    for (int t = 0; t < XMB_TAB_COUNT; t++) {
        if (!g_tabs[t].enabled) continue;

        int cx = xmb_tab_cx(t);
        int dist = (t == g_active_tab) ? 0 : abs(t - g_active_tab);
        int icon_px = (dist == 0) ? 24 : (dist == 1) ? 16 : 8;
        int icon_x  = cx - icon_px / 2;
        int icon_y  = XMB_TOPBAR_H + (XMB_TABBAR_H - icon_px) / 2 - 8;

        drawTextScaled((u32)icon_x, (u32)icon_y, g_tabs[t].icon, icon_px);

        if (t == g_active_tab) {
            int lx = cx - (int)(strlen(g_tabs[t].label) * 8) / 2;
            int ly = XMB_TOPBAR_H + XMB_TABBAR_H - 18;
            drawTextScaled((u32)(lx > 0 ? lx : 0), (u32)ly, g_tabs[t].label, 8);
        }
    }
}

// Draw the meta string "year · duration · genre" at (x, y) in 8px text.
static void xmb_draw_meta(u32 x, u32 y, const XMBItem *it) {
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
    if (meta[0]) drawTextScaled(x, y, meta, 8);
}

static void xmb_draw_item_list(int tab) {
    int count = g_item_count[tab];
    int vis   = XMB_ITEMS_VIS;

    for (int i = 0; i < vis; i++) {
        int idx = g_scroll_top + i;
        if (idx >= count) break;
        const XMBItem *it = &g_items[tab][idx];

        int iy = XMB_CONTENT_Y + i * XMB_ITEM_H;

        // Title  (16px font)
        int tx = XMB_ITEM_PAD + XMB_THUMB_W + 16;
        drawTextScaled((u32)tx, (u32)(iy + 8), it->name, 16);

        // Meta line (8px font)
        xmb_draw_meta((u32)tx, (u32)(iy + 32), it);

        // Codec badge text
        if (it->codec[0]) {
            int bx = (int)display_width - XMB_ITEM_PAD - 60;
            drawTextScaled((u32)bx, (u32)(iy + 10), it->codec, 8);
        }

        // Scroll indicator dots on right edge when there are more items
        if (i == 0 && g_scroll_top > 0)
            drawTextScaled(display_width - 20, (u32)iy, "^", 8);
        if (i == vis-1 && g_scroll_top + vis < count)
            drawTextScaled(display_width - 20, (u32)iy, "v", 8);
    }
}

// CPU draws for item list (highlight, thumb, badge bg) - called in CPU phase
static void xmb_cpu_draw_items(int tab) {
    int count = g_item_count[tab];
    int vis   = XMB_ITEMS_VIS;
    int W     = (int)display_width;

    for (int i = 0; i < vis; i++) {
        int idx = g_scroll_top + i;
        if (idx >= count) break;

        int iy = XMB_CONTENT_Y + i * XMB_ITEM_H;

        if (idx == g_sel) {
            // Highlight row background
            drawRect((u32)XMB_ITEM_PAD, (u32)iy,
                     (u32)(W - XMB_ITEM_PAD * 2), (u32)(XMB_ITEM_H - 2),
                     XMB_HIGHLIGHT);
            // Left accent bar
            drawRect((u32)(XMB_ITEM_PAD - 5), (u32)iy,
                     4, (u32)(XMB_ITEM_H - 2), XMB_ACCENT);
        }

        // Thumbnail placeholder
        drawRect((u32)(XMB_ITEM_PAD + 4), (u32)(iy + 8),
                 XMB_THUMB_W, XMB_THUMB_H, XMB_THUMB_DIM);

        // Codec badge background
        drawRect((u32)(W - XMB_ITEM_PAD - 70), (u32)(iy + 6),
                 66, 22, XMB_BADGE_BG);
    }
}

// -------------------------------------------------------
// Search / OSK rendering
// -------------------------------------------------------

static int osk_row_len(int r) {
    if (r >= OSK_ROWS_N) {
        // bottom row: SPACE(0), BACKSPACE(1), CLEAR(2)
        return 3;
    }
    const char **rows = g_osk_sym ? OSK_SYMBOLS : OSK_LETTERS;
    int base = strlen(rows[r]);
    if (!g_osk_sym && r == OSK_ROWS_N - 1) base++; // #+=  toggle key
    if  (g_osk_sym && r == OSK_ROWS_N - 1) base++; // ABC toggle key
    return base;
}

// Returns char to type for current OSK key, 0 for action keys.
static char osk_current_char(void) {
    if (g_osk_row >= OSK_ROWS_N) {
        // bottom row actions handled separately
        return 0;
    }
    const char **rows = g_osk_sym ? OSK_SYMBOLS : OSK_LETTERS;
    const char *row   = rows[g_osk_row];
    int base_len = strlen(row);
    // last key in letter row 3 = toggle, last in sym row 3 = toggle
    bool is_toggle = (g_osk_row == OSK_ROWS_N-1 && g_osk_col == base_len);
    if (is_toggle) return 0;
    if (g_osk_col < base_len) return row[g_osk_col];
    return 0;
}

static int OSK_Y0 = 0; // set at runtime based on display_height

static void xmb_cpu_draw_osk(void) {
    int W = (int)display_width;
    // All rows anchored to the width of the widest row (10 keys)
    int total_w = 10 * OSK_STEP_X - OSK_GAP;
    int osk_x0  = (W - total_w) / 2;

    // Input bar background
    drawRect((u32)XMB_ITEM_PAD, (u32)(XMB_CONTENT_Y + 8),
             (u32)(W - XMB_ITEM_PAD * 2), 40, 0x001A1040);

    // Keyboard key backgrounds
    for (int r = 0; r <= OSK_ROWS_N; r++) {
        if (r == OSK_ROWS_N) {
            // Bottom row: wide SPACE, BACKSPACE, CLEAR — left edge = osk_x0
            int space_w = 5 * OSK_STEP_X - OSK_GAP;  // 5 key-slots wide
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
            if (r == OSK_ROWS_N - 1) rlen++; // toggle key
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

static void xmb_rsx_draw_osk(void) {
    int W = (int)display_width;
    // Must match xmb_cpu_draw_osk exactly so labels land on their backgrounds
    int total_w = 10 * OSK_STEP_X - OSK_GAP;
    int osk_x0  = (W - total_w) / 2;

    // Cursor blink: show cursor on even half-seconds
    u64 us = timing_get_us();
    bool cursor = ((us / 500000) & 1) == 0;

    // Input bar text
    char disp[68];
    snprintf(disp, sizeof(disp), "%s%s", g_search_buf, cursor ? "_" : " ");
    drawTextScaled((u32)(XMB_ITEM_PAD + 12), (u32)(XMB_CONTENT_Y + 18), disp, 14);

    // Keyboard labels
    for (int r = 0; r <= OSK_ROWS_N; r++) {
        if (r == OSK_ROWS_N) {
            // Bottom row: SPACE  <  CLEAR
            int space_w = 5 * OSK_STEP_X - OSK_GAP;
            int sy = OSK_Y0 + r * OSK_STEP_Y + (OSK_KEY_H - 8) / 2;
            int cx_space = osk_x0 + space_w / 2 - 20;
            drawTextScaled((u32)(cx_space > 0 ? cx_space : 0), (u32)sy, "SPACE", 8);

            int bsx = osk_x0 + space_w + OSK_GAP;
            drawTextScaled((u32)(bsx + (OSK_KEY_W - 8) / 2), (u32)sy, "<", 8);

            int clx = bsx + OSK_KEY_W + OSK_GAP;
            drawTextScaled((u32)(clx + (OSK_KEY_W - 40) / 2), (u32)sy, "CLEAR", 8);
        } else {
            const char **rows = g_osk_sym ? OSK_SYMBOLS : OSK_LETTERS;
            const char  *row  = rows[r];
            int base_len = strlen(row);
            int ry = OSK_Y0 + r * OSK_STEP_Y + (OSK_KEY_H - 8) / 2;

            for (int c = 0; c < base_len; c++) {
                char label[3] = { row[c], '\0', '\0' };
                int kx = osk_x0 + c * OSK_STEP_X + (OSK_KEY_W - 8) / 2;
                drawTextScaled((u32)kx, (u32)ry, label, 8);
            }
            // Toggle key
            {
                const char *toggle_lbl = g_osk_sym ? "ABC" : "#+=";
                int kx = osk_x0 + base_len * OSK_STEP_X + (OSK_KEY_W - 24) / 2;
                drawTextScaled((u32)(kx > 0 ? kx : 0), (u32)ry, toggle_lbl, 8);
            }
        }
    }

    // Search results below keyboard
    int results_y = OSK_Y0 + (OSK_ROWS_N + 1) * OSK_STEP_Y + 8;
    int count = g_search_results_count;
    int vis_r = ((int)display_height - XMB_BOTTOM_PAD - results_y) / XMB_ITEM_H;
    if (vis_r < 0) vis_r = 0;
    for (int i = 0; i < vis_r && i < count; i++) {
        const XMBItem *it = &g_search_results[i];
        int iy = results_y + i * XMB_ITEM_H;
        drawTextScaled((u32)(XMB_ITEM_PAD + XMB_THUMB_W + 16), (u32)(iy + 8), it->name, 14);
        xmb_draw_meta((u32)(XMB_ITEM_PAD + XMB_THUMB_W + 16), (u32)(iy + 30), it);
    }
    if (count == 0 && g_search_buf[0]) {
        drawTextScaled((u32)XMB_ITEM_PAD, (u32)results_y, "No results.", 8);
    }
}

// CPU draws for search results list (thumbs, highlights)
static void xmb_cpu_draw_search_results(void) {
    int results_y = OSK_Y0 + (OSK_ROWS_N + 1) * OSK_STEP_Y + 8;
    int count = g_search_results_count;
    int vis_r = ((int)display_height - XMB_BOTTOM_PAD - results_y) / XMB_ITEM_H;
    if (vis_r < 0) vis_r = 0;
    for (int i = 0; i < vis_r && i < count; i++) {
        int iy = results_y + i * XMB_ITEM_H;
        drawRect((u32)(XMB_ITEM_PAD + 4), (u32)(iy + 8), XMB_THUMB_W, XMB_THUMB_H, XMB_THUMB_DIM);
    }
}

// -------------------------------------------------------
// XMB input
// -------------------------------------------------------

static void xmb_switch_tab(int new_tab) {
    if (new_tab < 0 || new_tab >= XMB_TAB_COUNT) return;
    if (!g_tabs[new_tab].enabled) return;
    g_active_tab = new_tab;
    g_sel = 0;
    g_scroll_top = 0;
    // Reset search state when entering Search tab
    if (new_tab == XMB_TAB_SEARCH) {
        g_osk_row = 0; g_osk_col = 0; g_osk_sym = false;
    }
}

// Find next enabled tab in direction dir (+1 or -1)
static int xmb_next_enabled(int start, int dir) {
    int t = start + dir;
    while (t >= 0 && t < XMB_TAB_COUNT) {
        if (g_tabs[t].enabled) return t;
        t += dir;
    }
    return start;
}

// Returns true if we should exit the XMB loop
static bool xmb_handle_input_browse(void) {
    int tab   = g_active_tab;
    int count = g_item_count[tab];
    int vis   = XMB_ITEMS_VIS;

    if (BTN_PRESSED(l1)) { xmb_switch_tab(xmb_next_enabled(g_active_tab, -1)); return false; }
    if (BTN_PRESSED(r1)) { xmb_switch_tab(xmb_next_enabled(g_active_tab, +1)); return false; }
    if (BTN_PRESSED(circle)) return true; // exit

    if (BTN_PRESSED(up)) {
        if (g_sel > 0) {
            g_sel--;
            if (g_sel < g_scroll_top) g_scroll_top = g_sel;
        }
    }
    if (BTN_PRESSED(down)) {
        if (g_sel < count - 1) {
            g_sel++;
            if (g_sel >= g_scroll_top + vis) g_scroll_top = g_sel - vis + 1;
        }
    }

    if (BTN_PRESSED(cross) && count > 0 && g_sel < count) {
        const XMBItem *it = &g_items[tab][g_sel];
        JFItem jf; memset(&jf, 0, sizeof(jf));
        strncpy(jf.id,   it->id,   sizeof(jf.id)-1);
        strncpy(jf.name, it->name, sizeof(jf.name)-1);
        strncpy(jf.type, it->type, sizeof(jf.type)-1);
        show_player(&jf);
        init_btns();
    }

    if (BTN_PRESSED(triangle) && count > 0 && g_sel < count) {
        // Info overlay: show item details until O pressed
        const XMBItem *it = &g_items[tab][g_sel];
        init_btns();
        while (running) {
            sysUtilCheckCallback();
            padInfo pi; padData pd;
            ioPadGetInfo(&pi);
            for (int i = 0; i < MAX_PADS; i++) {
                if (!pi.status[i]) continue;
                ioPadGetData(i, &pd); update_buttons(&pd);
                if (BTN_PRESSED(circle) || BTN_PRESSED(triangle)) goto info_done;
            }
            // Render info screen (CPU clear + RSX text)
            rsxSync();
            cpuClearFb(XMB_BG);
            //wave_draw();
            drawTextScaled(XMB_ITEM_PAD, XMB_TOPBAR_H + 10, it->name, 24);
            drawTextScaled(XMB_ITEM_PAD, XMB_TOPBAR_H + 46, it->year_str, 14);
            xmb_draw_meta(XMB_ITEM_PAD, XMB_TOPBAR_H + 68, it);
            drawTextScaled(XMB_ITEM_PAD, (u32)(display_height - XMB_BOTTOM_PAD + 16), "O BACK", 8);
            flip();
        }
        info_done:
        init_btns();
    }
    return false;
}

static bool xmb_handle_input_search(void) {
    if (BTN_PRESSED(l1)) { xmb_switch_tab(xmb_next_enabled(g_active_tab, -1)); return false; }
    if (BTN_PRESSED(r1)) { xmb_switch_tab(xmb_next_enabled(g_active_tab, +1)); return false; }
    if (BTN_PRESSED(circle) && !g_search_buf[0]) return true;
    if (BTN_PRESSED(circle)) { g_search_buf[0] = '\0'; g_search_results_count = 0; return false; }

    int row_count = OSK_ROWS_N + 1; // letter rows + bottom row

    if (BTN_PRESSED(up)) {
        g_osk_row = (g_osk_row - 1 + row_count) % row_count;
        int ml = osk_row_len(g_osk_row);
        if (g_osk_col >= ml) g_osk_col = ml - 1;
    }
    if (BTN_PRESSED(down)) {
        g_osk_row = (g_osk_row + 1) % row_count;
        int ml = osk_row_len(g_osk_row);
        if (g_osk_col >= ml) g_osk_col = ml - 1;
    }
    if (BTN_PRESSED(left)) {
        int ml = osk_row_len(g_osk_row);
        g_osk_col = (g_osk_col - 1 + ml) % ml;
    }
    if (BTN_PRESSED(right)) {
        int ml = osk_row_len(g_osk_row);
        g_osk_col = (g_osk_col + 1) % ml;
    }

    if (BTN_PRESSED(cross)) {
        if (g_osk_row == OSK_ROWS_N) {
            // Bottom row: SPACE / BACKSPACE / CLEAR
            if (g_osk_col == 0) { // SPACE
                int len = strlen(g_search_buf);
                if (len < (int)sizeof(g_search_buf)-1) { g_search_buf[len]=' '; g_search_buf[len+1]='\0'; }
            } else if (g_osk_col == 1) { // BACKSPACE
                int len = strlen(g_search_buf);
                if (len > 0) g_search_buf[len-1] = '\0';
            } else { // CLEAR
                g_search_buf[0] = '\0';
                g_search_results_count = 0;
            }
        } else {
            // Check toggle key
            const char **rows = g_osk_sym ? OSK_SYMBOLS : OSK_LETTERS;
            int base_len = strlen(rows[g_osk_row]);
            if (g_osk_row == OSK_ROWS_N - 1 && g_osk_col == base_len) {
                g_osk_sym = !g_osk_sym;
                g_osk_col = 0;
            } else {
                char ch = osk_current_char();
                if (ch) {
                    int len = strlen(g_search_buf);
                    if (len < (int)sizeof(g_search_buf)-1) {
                        g_search_buf[len] = ch;
                        g_search_buf[len+1] = '\0';
                    }
                }
            }
        }
    }

    // Triangle = submit search
    if (BTN_PRESSED(triangle) || BTN_PRESSED(start)) {
        xmb_do_search();
    }

    // Cross on search result: play it
    if (BTN_PRESSED(cross) && g_osk_row == OSK_ROWS_N) {
        // handled above
    }

    return false;
}

// -------------------------------------------------------
// XMB main loop
// -------------------------------------------------------

void ui_run_xmb(void) {
    // Reset state
    memset(g_items, 0, sizeof(g_items));
    memset(g_item_count, 0, sizeof(g_item_count));
    memset(g_items_loaded, 0, sizeof(g_items_loaded));
    memset(g_search_buf, 0, sizeof(g_search_buf));
    g_search_results_count = 0;
    g_active_tab = XMB_TAB_MOVIES;
    g_sel = 0; g_scroll_top = 0;
    g_osk_row = 0; g_osk_col = 0; g_osk_sym = false;
    g_wave_phase = 0.0f;

    // Detect which library tabs exist on the server
    xmb_detect_tabs();

    // Start on first enabled content tab (prefer Movies)
    if (!g_tabs[XMB_TAB_MOVIES].enabled) {
        for (int t = 0; t < XMB_TAB_COUNT; t++) {
            if (g_tabs[t].enabled) { g_active_tab = t; break; }
        }
    }

    // Pre-compute OSK Y start (depends on display height)
    OSK_Y0 = XMB_CONTENT_Y + 58; // 8px padding + 40px input bar + 10px gap

    init_btns();
    u64 prev_us = timing_get_us();
    padInfo padinfo; padData paddata;

    while (running) {
        waitflip();
        sysUtilCheckCallback();
        clearScreen(XMB_BG);

        // Timing
        u64 now_us = timing_get_us();
        float dt = (float)(now_us - prev_us) / 1000000.0f;
        if (dt > 0.1f) dt = 0.1f; // clamp on first frame / hiccups
        prev_us = now_us;
        g_wave_phase += dt;
        if (g_wave_phase > 6283.0f) g_wave_phase -= 6283.0f;

        // Lazy-load items for the current browse tab
        int tab = g_active_tab;
        if (tab != XMB_TAB_SEARCH && tab != XMB_TAB_MUSIC && tab != XMB_TAB_SETTINGS)
            if (!g_items_loaded[tab]) xmb_fetch_tab_items(tab);

        // Input — always poll every connected pad to keep btn_prev current,
        // then handle input once after all pads are updated.
        ioPadGetInfo(&padinfo);
        for (int i = 0; i < MAX_PADS; i++) {
            if (!padinfo.status[i]) continue;
            ioPadGetData(i, &paddata);
            update_buttons(&paddata);
        }
        bool should_exit = false;
        if (tab == XMB_TAB_SEARCH)
            should_exit = xmb_handle_input_search();
        else
            should_exit = xmb_handle_input_browse();
        if (should_exit) break;

        // Render — CPU phase first, then RSX text
        rsxSync();

        //wave_draw();

        // Divider line
        u32 *div = color_buffer[curr_fb] + XMB_DIVIDER_Y * display_width;
        for (u32 x = 0; x < display_width; x++) div[x] = XMB_DIVIDER_CLR;

        // Tab-specific CPU draws
        if (tab == XMB_TAB_SEARCH) {
            xmb_cpu_draw_osk();
            xmb_cpu_draw_search_results();
        } else if (tab != XMB_TAB_MUSIC && tab != XMB_TAB_SETTINGS) {
            xmb_cpu_draw_items(tab);
        }

        // RSX phase
        // Top bar
        drawTextScaled(XMB_ITEM_PAD, 16, "JELLYFIN-PS3", 20);
        {
            // Right side: L1/R1 nav hint + time
            char hints[32];
            snprintf(hints, sizeof(hints), "< L1   R1 >");
            int hx = (int)display_width - (int)(strlen(hints) * 8) - XMB_ITEM_PAD;
            drawTextScaled((u32)(hx > 0 ? hx : 0), 24, hints, 8);
        }

        // Tab bar
        xmb_draw_tabs();

        // Content
        if (tab == XMB_TAB_SEARCH) {
            xmb_rsx_draw_osk();
        } else if (tab == XMB_TAB_MUSIC || tab == XMB_TAB_SETTINGS) {
            drawTextScaled(XMB_ITEM_PAD, (u32)(XMB_CONTENT_Y + 40), "Coming soon", 16);
        } else {
            if (g_items_loaded[tab] && g_item_count[tab] == 0)
                drawTextScaled(XMB_ITEM_PAD, (u32)(XMB_CONTENT_Y + 20), "No items.", 8);
            else
                xmb_draw_item_list(tab);
        }

        // Bottom hints
        drawTextScaled(XMB_ITEM_PAD,
                       (u32)(display_height - XMB_BOTTOM_PAD + 16),
                       "X SELECT   O BACK   /\\ INFO", 8);

        flip();
        sysUtilCheckCallback();
    }
}

// -------------------------------------------------------
// Lifecycle
// -------------------------------------------------------

void ui_init(void) {
    bitmapSetXpm(&fontBitmap, font8x8_xpm);
    rsxSetBlendFunc(context,
        GCM_SRC_ALPHA, GCM_ONE_MINUS_SRC_ALPHA,
        GCM_SRC_ALPHA, GCM_ONE_MINUS_SRC_ALPHA);
    rsxSetBlendEquation(context, GCM_FUNC_ADD, GCM_FUNC_ADD);
    rsxSetBlendEnable(context, GCM_TRUE);
    setRenderTarget(curr_fb);
}

void ui_cleanup(void) {
    bitmapDestroy(&fontBitmap);
}
