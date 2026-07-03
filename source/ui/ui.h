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
    u8 l1, r1, l2, r2;
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
    u32  resume_secs;       // saved playback position (UserData), 0 = none
    u8   progress_pct;      // 0-100 watched percentage (thumbnail bar)
} XMBItem;

// Defined in main.cpp; every input loop reads this.
extern u32 running;

// Defined in ui.cpp
extern ButtonState btn_cur;
extern ButtonState btn_prev;

// True only on the frame the button transitions 0→1
#define BTN_PRESSED(b) (btn_cur.b && !btn_prev.b)

// Auto-repeat for held navigation (menus).  True on the initial press, then again
// at a steady rate while the button stays held — for scrolling lists/grids.
enum { NAV_up, NAV_down, NAV_left, NAV_right, NAV_REPEAT_SLOTS };
#define NAV_DELAY_US   350000ULL   // hold this long before repeat kicks in
#define NAV_REPEAT_US  140000ULL   // then ~7 steps/sec (raise to slow it down)
bool btn_nav_repeat(bool held, int slot);
#define BTN_REPEAT(b)  btn_nav_repeat(btn_cur.b, NAV_##b)

void update_buttons(padData *pad);

// Reads all active pad slots, ORs their button data into one merged padData,
// calls update_buttons() exactly once. Returns true if any pad was active.
bool poll_buttons(void);

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
void drawTTF(u32 x, u32 y, const char *text, float px, u32 color, bool bold = false);
void drawIcon(u32 x, u32 y, int codepoint, float px, u32 color);
void drawHeader(void);
void decode_unicode_escapes(char *str);

// -------------------------------------------------------
// CPU drawing — write directly to color_buffer[curr_fb].
// Must only be called AFTER rsxSync() and BEFORE any RSX
// draw commands are queued for the current frame.
// color is 0x00RRGGBB (X8R8G8B8 framebuffer format).
// -------------------------------------------------------
void drawRect(u32 x, u32 y, u32 w, u32 h, u32 color);
// Alpha-blend a rect over the framebuffer (alpha 0-255).  Reads back the
// framebuffer per pixel, which is slow on RSX local memory — keep the area
// small (hairlines, thin frames); never large fills.
void drawRectBlend(u32 x, u32 y, u32 w, u32 h, u32 color, u8 alpha);
void cpuClearFb(u32 color);   // clear entire framebuffer

// Vertical scissor for the CPU draw primitives (drawRect/drawRectBlend,
// drawTTF/drawIcon, card blits).  Framebuffer rows outside
// [g_cpu_clip_top, g_cpu_clip_bot) are skipped; g_cpu_clip_bot == 0 means
// "to the bottom edge".  Defaults span the whole screen, so most callers
// never touch these — the Home shelf sets them to keep smoothly-scrolling
// cards out of the top bar / hints bar, and restores them afterward.
extern int g_cpu_clip_top;
extern int g_cpu_clip_bot;
bool cpu_row_clipped(int sy);   // true when framebuffer row sy is scissored out

// -------------------------------------------------------
// CPU compose target.  While begun, the CPU primitives (drawRect,
// drawRectBlend, drawTTF, drawIcon, iconic glyphs) render into this
// main-RAM A8R8G8B8 buffer with straight-alpha OVER compositing instead of
// writing to the framebuffer.  Used by the player HUD to build its overlay
// texture off-screen; the vertical scissor does not apply while active.
// -------------------------------------------------------
void cpu_rt_begin(u32 *buf, u32 w, u32 h);
void cpu_rt_end(void);
bool cpu_rt_on(void);
u32  cpu_draw_w(void);          // active target width  (framebuffer or RT)
u32  cpu_draw_h(void);          // active target height
u32 *cpu_draw_row(u32 y);       // row pointer in the active target

// Straight-alpha OVER: coverage `a` of `color` (0x00RRGGBB) onto an ARGB
// destination pixel.  Only meaningful for the compose target.
static inline u32 argb_over(u32 dst, u32 color, u32 a) {
    if (a == 0) return dst;
    u32 da = (dst >> 24) & 0xFF;
    if (a == 255 || da == 0)
        return (a << 24) | (color & 0x00FFFFFF);
    u32 k    = (da * (255 - a)) / 255;   // remaining dst weight
    u32 outa = a + k;
    u32 r = (((color >> 16) & 0xFF) * a + ((dst >> 16) & 0xFF) * k) / outa;
    u32 g = (((color >>  8) & 0xFF) * a + ((dst >>  8) & 0xFF) * k) / outa;
    u32 b = (( color        & 0xFF) * a + ( dst        & 0xFF) * k) / outa;
    return (outa << 24) | (r << 16) | (g << 8) | b;
}

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

// Restore the RSX pipeline state that ui_init() configured.
// Call after the player tears down RSX so the UI resumes in a known state.
void ui_restore_rsx_state(void);
