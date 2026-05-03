#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include <rsx/rsx.h>
#include <sysutil/sysutil.h>

#include "ui.h"
#include "bitmap.h"
#include "font8x8.xpm"

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
// Drawing
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
// On-screen keyboard
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
            int  x   = kb_x + c * key_w;
            int  y   = kb_y + r * key_h;
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
                drawText(x - CHAR_SIZE, y, sel_buf);
            } else {
                drawText(x, y, label);
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
