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

// Drawing
void clearScreen(u32 color);
void drawChar(u32 x, u32 y, char c);
void drawText(u32 x, u32 y, const char *text);
void drawTextf(u32 x, u32 y, const char *fmt, ...);
void drawHeader(void);
void decode_unicode_escapes(char *str);

// On-screen keyboard. Returns 1 = confirmed, -1 = cancelled.
int  get_input(char *out, int max_len, const char *prompt, bool is_password);

// One-time setup: upload font bitmap, configure RSX blend.
void ui_init(void);
void ui_cleanup(void);
