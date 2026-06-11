#pragma once
#include <ppu-types.h>
#include "ui.h"

// -------------------------------------------------------
// XMB tab indices
// -------------------------------------------------------
#define XMB_TAB_SEARCH      0
#define XMB_TAB_RESUME      1   // Continue Watching
#define XMB_TAB_MOVIES      2
#define XMB_TAB_TV          3
#define XMB_TAB_MUSIC       4
#define XMB_TAB_COLLECTIONS 5
#define XMB_TAB_SETTINGS    6
#define XMB_TAB_COUNT       7

#define XMB_ITEMS_MAX 50
#define XMB_PAGE_SIZE 25

typedef struct {
    const char *label;
    const char *icon;
    char library_id[64];
    bool enabled;
} XMBTab;

// -------------------------------------------------------
// OSK constants
// -------------------------------------------------------
#define OSK_ROWS_N 4

// -------------------------------------------------------
// XMB layout constants (pixel values, assume 720p minimum)
// -------------------------------------------------------
#define XMB_BG          0x000d0d1aUL
#define XMB_DIVIDER_CLR 0x00554077UL
#define XMB_ACCENT      0x007C3CEAUL
#define XMB_HIGHLIGHT   0x001D1040UL
#define XMB_THUMB_DIM   0x00241444UL
#define XMB_BADGE_BG    0x00302050UL
#define XMB_KEY_NORMAL  0x001A103AUL
#define XMB_KEY_SEL     0x005A3AB0UL

#define XMB_TOPBAR_H    64
#define XMB_TABBAR_H    80
#define XMB_DIVIDER_Y   (XMB_TOPBAR_H + XMB_TABBAR_H)
#define XMB_CONTENT_Y   (XMB_DIVIDER_Y + 52)
#define XMB_BOTTOM_PAD  70
#define XMB_ITEM_H      90
#define XMB_THUMB_W     52
#define XMB_THUMB_H     74
#define XMB_ITEM_PAD    40

// Centered narrow list layout (search results keep this style)
#define XMB_LIST_W      780                         // ~60% of 1280
#define XMB_ROW_H        88                         // row visual height
#define XMB_ROW_GAP      16                         // vertical gap between rows
#define XMB_ROW_STRIDE  (XMB_ROW_H + XMB_ROW_GAP)
#define XMB_ROW_RADIUS    8                         // reserved for future rounded corners

// Items visible simultaneously in the list area
#define XMB_ITEMS_VIS ((int)((display_height - XMB_CONTENT_Y - XMB_BOTTOM_PAD) / XMB_ROW_STRIDE))

// -------------------------------------------------------
// Card grid layout (library tabs: Continue/Movies/TV/Collections)
// 2 rows of cards.  Poster screens (Movies, TV series/seasons,
// Collections) use portrait 2:3 cards; Continue Watching and episode
// lists use landscape 16:9 stills — matching the image type Jellyfin
// serves for each, so nothing gets cropped or stretched.  Every card
// shows its title in the band below it, with the SELECTED card's
// title drawn bigger/bold plus a meta line.
// -------------------------------------------------------
#define XMB_GRID_ROWS       2
#define XMB_PORTRAIT_COLS   5
#define XMB_LANDSCAPE_COLS  3
#define XMB_CARD_GAP_X   24
#define XMB_CARD_TEXT_H  50                         // text band under each row
// Cards are sized to fill the space between the content area and the hints
// bar at any resolution.  The 26px reserve covers the breadcrumb offset on
// sub-screens so the bottom row's text band never runs into the hints bar.
#define XMB_GRID_AVAIL_H ((int)display_height - XMB_BOTTOM_PAD - XMB_GRID_Y0 - 26)
#define XMB_CARD_H_FIT   (XMB_GRID_AVAIL_H / XMB_GRID_ROWS - XMB_CARD_TEXT_H - 6)
#define XMB_CARD_W_CAP   ((int)display_width * 300 / 1280)
#define XMB_GRID_Y0      (XMB_CONTENT_Y + 8)

// Resolved grid geometry for one tab (depends on tab + sub-screen depth).
typedef struct {
    bool portrait;      // 2:3 poster cards vs 16:9 landscape stills
    int  cols, vis;     // columns and visible card count (cols * rows)
    int  card_w, card_h;
    int  stride;        // row stride incl. text band
    int  grid_w, x0;    // total grid width and centered left edge
} GridGeom;

bool xmb_tab_uses_portrait(int tab);
void xmb_grid_geom(int tab, GridGeom *gg);

// Jump bar (narrow letter column to the left of the item list)
#define JBAR_ENTRIES 27
#define JBAR_W       20
#define JBAR_GAP      8

// OSK constants (pixels)
#define OSK_KEY_W    80
#define OSK_KEY_H    44
#define OSK_GAP       8
#define OSK_STEP_X   (OSK_KEY_W + OSK_GAP)
#define OSK_STEP_Y   (OSK_KEY_H + OSK_GAP)

// -------------------------------------------------------
// OSK data (defined in ui/ui_search.cpp, used by draw functions)
// -------------------------------------------------------
extern const char *OSK_LETTERS[OSK_ROWS_N];
extern const char *OSK_SYMBOLS[OSK_ROWS_N];

// -------------------------------------------------------
// Navigation/UI state defined in ui.cpp, read by ui_visuals.cpp
// -------------------------------------------------------
extern XMBTab  g_tabs[XMB_TAB_COUNT];
extern XMBItem g_items[XMB_TAB_COUNT][XMB_ITEMS_MAX];
extern int     g_item_count[XMB_TAB_COUNT];
extern bool    g_items_loaded[XMB_TAB_COUNT];
extern int     g_active_tab;
extern int     g_sel;
extern int     g_scroll_top;
extern u64     g_info_cooldown_until;
extern int     g_tv_depth;
extern char    g_tv_series_id[64];
extern char    g_tv_series_name[128];
extern char    g_tv_season_id[64];
extern char    g_tv_season_name[64];
extern int     g_tv_sub_sel;
extern int     g_tv_sub_scroll;
extern int     g_tv_sub_count;
extern XMBItem g_tv_sub_items[XMB_ITEMS_MAX];
extern int     g_tv_sub_start;
extern int     g_tv_sub_total;
extern int     g_col_depth;
extern char    g_col_id[64];
extern char    g_col_name[128];
extern int     g_col_sub_sel;
extern int     g_col_sub_scroll;
extern int     g_col_sub_count;
extern XMBItem g_col_sub_items[XMB_ITEMS_MAX];
extern int     g_col_sub_start;
extern int     g_col_sub_total;
extern int     g_tab_start[XMB_TAB_COUNT];
extern int     g_tab_total[XMB_TAB_COUNT];
extern int     g_osk_row;
extern int     g_osk_col;
extern bool    g_osk_sym;
extern bool    g_search_focus_results;
extern int     g_search_sel;
extern int     g_search_scroll;
extern char    g_search_buf[64];
extern int     g_search_results_count;
extern XMBItem g_search_results[XMB_ITEMS_MAX];
extern int     OSK_Y0;

// Jump bar state (defined in ui.cpp)
extern bool g_jumpbar_active;
extern int  g_jumpbar_sel;
extern char g_tab_name_filter[XMB_TAB_COUNT][4];

// Settings tab state (defined in ui.cpp)
#define XMB_SETTINGS_COUNT 1   // number of selectable settings entries
extern int  g_settings_sel;        // highlighted settings entry
extern bool g_settings_confirm;    // true while the logout confirm prompt is up

// -------------------------------------------------------
// Hints bar
// -------------------------------------------------------
typedef struct { char glyph; const char *label; } Hint;

// -------------------------------------------------------
// Functions implemented in ui_visuals.cpp
// -------------------------------------------------------
void draw_iconic_glyph(u32 x, u32 y, char glyph, float px, u32 color);
void draw_iconic_glyph_vcentered(u32 x, int cy, char glyph, float px, u32 color);
int  iconic_adv_px(char glyph, float px);

void visuals_cleanup(void);
void ttf_init(void);
void ttf_prewarm_hud(void);
int  ttf_text_width(const char *text, float px);
void xmb_draw_tabs(void);
void xmb_draw_meta(u32 x, u32 y, const XMBItem *it, float px = 14);

// Card grid (library tabs).  Two phases per frame (after rsxSync):
//   cpu   — card images (CPU blit from main RAM), placeholders, selection
//           border, progress strips, next-page prefetch
//   text  — titles under every card (selected emphasized) + scroll arrows
void xmb_grid_cpu(const GridGeom *gg, const XMBItem *items, int count,
                  int sel, int scroll, int y0);
void xmb_grid_text(const GridGeom *gg, const XMBItem *items, int count,
                   int sel, int scroll, int y0, bool more_below);
void xmb_rsx_draw_osk(void);
void xmb_cpu_draw_osk(void);
void xmb_cpu_draw_search_results(void);
void xmb_draw_jumpbar(int tab);
void draw_hints_bar(const Hint *hints, int n);
void draw_topbar_lr(void);
void xmb_cpu_draw_settings(void);   // CPU phase: highlight rect
void xmb_draw_settings(void);       // RSX phase: account info + entries
