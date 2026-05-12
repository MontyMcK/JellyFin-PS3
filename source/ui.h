#pragma once
#include <ppu-types.h>
#include <io/pad.h>
#include "rsxutil.h"

#define FONT_SCALE   2
#define CHAR_SIZE    (8 * FONT_SCALE)
#define LINE_HEIGHT  (CHAR_SIZE + 6)

typedef struct {
    u8 up, down, left, right;
    u8 cross, circle, square, triangle;
    u8 start, select;
    u8 l1, r1;
} ButtonState;

// -------------------------------------------------------
// Extended item for the XMB list view
// -------------------------------------------------------
typedef struct {
    char id[64];
    char name[128];
    char type[32];
    char year_str[8];       // "2014"
    char duration_str[16];  // "2h 49m"
    char genre[32];         // "Sci-Fi"
    char codec[12];         // "H.264"
} XMBItem;

// Defined in main.cpp; every input loop reads this.
extern u32 running;

// Defined in ui.cpp
extern ButtonState btn_cur;
extern ButtonState btn_prev;

// True only on the frame the button transitions 0→1
#define BTN_PRESSED(b) (btn_cur.b && !btn_prev.b)

void update_buttons(padData *pad);

// Seed btn_prev = btn_cur from the live pad so held buttons
// from a previous screen don't fire as new presses.
void init_btns(void);

// -------------------------------------------------------
// RSX drawing — all write into color_buffer[curr_fb] via
// the RSX command buffer (runs after rsxSync + CPU writes).
// -------------------------------------------------------
void clearScreen(u32 color);
void drawChar(u32 x, u32 y, char c);
void drawText(u32 x, u32 y, const char *text);
void drawTextf(u32 x, u32 y, const char *fmt, ...);
void drawTextScaled(u32 x, u32 y, const char *text, int px);
void drawHeader(void);
void decode_unicode_escapes(char *str);

// -------------------------------------------------------
// CPU drawing — write directly to color_buffer[curr_fb].
// Must only be called AFTER rsxSync() and BEFORE any RSX
// draw commands are queued for the current frame.
// color is 0x00RRGGBB (X8R8G8B8 framebuffer format).
// -------------------------------------------------------
void drawRect(u32 x, u32 y, u32 w, u32 h, u32 color);
void cpuClearFb(u32 color);   // clear entire framebuffer

// -------------------------------------------------------
// XMB main screen — replaces the old show_main_menu().
// -------------------------------------------------------
void ui_run_xmb(void);

// -------------------------------------------------------
// Legacy on-screen keyboard (used by login flow).
// Returns 1 = confirmed, -1 = cancelled.
// -------------------------------------------------------
int  get_input(char *out, int max_len, const char *prompt, bool is_password);

// One-time setup: upload font bitmap, configure RSX blend.
void ui_init(void);
void ui_cleanup(void);
