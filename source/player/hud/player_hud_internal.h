#pragma once
// Shared state and constants internal to the HUD (hud_core / hud_dim /
// hud_draw).  The public API is player_hud.h.

#include <ppu-types.h>

// -------------------------------------------------------
// Visual constants
// -------------------------------------------------------
#define HUD_ACCENT          0x007C3CEAUL
#define HUD_ACCENT_DIM      0x00261260UL
#define HUD_DIMMED          0x00402070UL   // unfocused when another slot is active
#define HUD_FOCUSED         0x00FFFFFFUL   // white for focused control
#define HUD_SHOW_US         4000000ULL

#define HUD_STRIP_H          64
#define CTRL_Y_OFF  (HUD_STRIP_H / 2)      // control row centred in the strip
#define TRACK_H               4
#define SCRUB_R               6
#define LEFT_PAD             50
#define RIGHT_PAD            50
#define CTRL_GAP             10            // gap between transport control glyphs
#define TIME_GAP             12            // gap between time label edge and seek bar
#define RCTRL_GAP            12            // gap between remaining time and right controls

#define ROW_ICON_PX          36.0f         // L2/R2 iconic glyph size
#define ROW_TEXT_PX          18.0f         // time + audio label text size
#define MUSIC_ICON_PX        24.0f
#define CC_TEXT_PX           20.0f
#define ICON_LABEL_GAP        6
#define AUDIO_SEP            16            // horizontal gap between audio btn and CC btn

#define PP_H                 22            // play/pause primitive bounding height (px)
#define PP_W                 22            // play/pause primitive bounding width  (px)

// Title overlay (top-left while paused)
#define TITLE_PX           24.0f           // a touch under the subtitle size
#define TITLE_TOP_PAD        34

// Popup menu (track selection)
#define MENU_MAX              9            // JF_MAX_STREAMS subs + "Off"
#define MENU_TITLE_PX      22.0f
#define MENU_TITLE_H         38            // title row height incl. padding
#define MENU_ROW_H           30
#define MENU_PAD             16            // popup inner padding
#define MENU_DOT_COL         22            // width of the current-entry dot column

#define MATERIAL_MUSIC_NOTE  0xE405

// Focus slot indices
#define FOCUS_REW    0
#define FOCUS_PP     1
#define FOCUS_FF     2
#define FOCUS_AUDIO  3
#define FOCUS_CC     4
#define FOCUS_COUNT  5

// -------------------------------------------------------
// State (defined in hud_core.cpp)
// -------------------------------------------------------

struct HudState {
    u32  total_secs;
    char audio_label[64];
    char title[128];
    bool visible;
    u64  show_us;
    int  seek_delta;
    int  focus;        // -1=none, 0..FOCUS_COUNT-1=focused slot
    int  incr_idx;     // 0=10s  1=30s  2=5min
    bool cc_active;    // subtitles on -> underline the CC button

    // Popup menu state.  Items are caller-owned pointers (track labels live
    // in the player's static JFTracks, so they outlive the menu).
    bool        menu_visible;
    char        menu_title[24];
    const char *menu_items[MENU_MAX];
    int         menu_n;
    int         menu_sel;      // cursor row
    int         menu_cur;      // active entry (accent dot)
    int         menu_choice;   // last X-selected row
};

extern HudState g_hud;

// Make the HUD visible and restart its auto-hide timer (hud_core.cpp).
void hud_show(void);

// Darken the rectangle on the framebuffer.  Selects the configured dim path
// (inline GPU quad by default) and fences the GPU paths with rsxSync before
// returning, so CPU pixel writes may follow immediately (hud_dim.cpp).
void hud_dim_rect(u32 rx, u32 ry, u32 rw, u32 rh, u8 alpha);
