#pragma once
#include <ppu-types.h>
#include "ui.h"
#include "icons.h"
#include "overscan.h"   // overscan_x()/overscan_y() feed the layout anchors below
#include "../build_config.h"   // ENABLE_PLAYER_STATS gates a settings row

// -------------------------------------------------------
// XMB tab indices
// -------------------------------------------------------
// Search, Home and Settings are FIXED slots — they are app screens, not
// libraries, so their indices never move.  Everything from XMB_TAB_LIB0 up is
// built at runtime, ONE TAB PER JELLYFIN LIBRARY (see xmb_detect_tabs).
//
// The old layout hardcoded one slot each for Movies/TV/Music/Collections and
// mapped libraries onto them by CollectionType.  That silently lost content
// two ways: a second library of the same type overwrote the first, and any
// library with another type (homevideos, musicvideos, books, photos, mixed,
// or no CollectionType at all) matched nothing and was dropped entirely.
//
// ARRAY INDEX IS NOT DISPLAY POSITION.  Settings keeps index 2 so it stays a
// compile-time constant, but it renders LAST; xmb_tab_order() defines what
// the user actually sees, and both the tab bar and L1/R1 follow it.
#define XMB_TAB_SEARCH      0
#define XMB_TAB_RESUME      1   // now the Home shelf (Continue Watching is one row of it)
#define XMB_TAB_SETTINGS    2
#define XMB_TAB_LIB0        3   // first runtime library tab
#define XMB_TAB_COUNT       16  // array ceiling, NOT the live tab count
#define XMB_LIB_MAX         (XMB_TAB_COUNT - XMB_TAB_LIB0)

// The Continue Watching tab slot is repurposed as the Home shelf screen.
#define XMB_TAB_HOME        XMB_TAB_RESUME

#define XMB_ITEMS_MAX 50
#define XMB_PAGE_SIZE 25

// What a tab BEHAVES like.  Several tabs can share a kind when the server has
// several libraries of the same collection type, so all the per-type logic
// keys off this rather than off a tab index.
typedef enum {
    TABKIND_SEARCH = 0,
    TABKIND_HOME,
    TABKIND_MOVIES,
    TABKIND_TV,
    TABKIND_MUSIC,
    TABKIND_BOXSETS,
    TABKIND_PLAYLISTS,  // Jellyfin's "playlists" view — its own library
    TABKIND_GENERIC,    // homevideos / musicvideos / mixed / untyped
    TABKIND_SETTINGS,
} XMBTabKind;

typedef struct {
    char label[40];         // server library name ("Kids Movies"), or app screen
    const char *icon;
    char library_id[64];
    XMBTabKind kind;
    bool enabled;
} XMBTab;

// Kind of a tab index (TABKIND_GENERIC if out of range).
XMBTabKind xmb_kind(int tab);

// First ENABLED tab with the given kind, or -1 when the server has no such
// library.  Use for "jump to the music library" style lookups that used to be
// a bare XMB_TAB_MUSIC.  With several libraries of a kind this returns the
// first; callers wanting a specific one already hold its tab index.
int xmb_tab_of_kind(XMBTabKind k);

// Fill order[] with enabled tab indices in DISPLAY order — Search, Home,
// libraries in server order, then Settings.  Returns how many were written;
// order[] must hold at least XMB_TAB_COUNT entries.
int xmb_tab_order(int *order);

// -------------------------------------------------------
// OSK constants
// -------------------------------------------------------
#define OSK_ROWS_N 4

// -------------------------------------------------------
// Palette — deep indigo base (XMB-style), one violet accent
// (Jellyfin brand-adjacent), white reserved for selection.
// Text comes in three fixed levels; never invent new greys.
// -------------------------------------------------------
#define XMB_BG          0x000B0E1EUL   // RSX clear color (sits under the gradient)
#define XMB_BG_TOP      0x00151A38UL   // background gradient, top edge
#define XMB_BG_BOT      0x0005060CUL   // background gradient, bottom edge
#define XMB_ACCENT      0x008F6FE8UL   // violet: progress fills, active marks, brand tag
#define XMB_ACCENT_DEEP 0x005B43A8UL   // latched accent (caps-lock key)
#define XMB_TEXT        0x00E9EBF5UL   // primary text (soft white)
#define XMB_TEXT_DIM    0x0099A0BCUL   // secondary text / metadata
#define XMB_TEXT_FAINT  0x005C6386UL   // tertiary text, section labels, empty states
#define XMB_WHITE       0x00FFFFFFUL   // selected emphasis only
#define XMB_PANEL       0x00191E3CUL   // raised panels (settings card, modals)
#define XMB_PANEL_HI    0x00232950UL   // selected row fill
#define XMB_HAIRLINE    0x002C3258UL   // 1px panel borders
#define XMB_THUMB_DIM   0x0012162CUL   // thumbnail loading placeholder
#define XMB_TRACK       0x0010142AUL   // progress bar track
#define XMB_KEY_NORMAL  0x001A1F3EUL   // OSK key
#define XMB_KEY_SEL     0x00F0F2FAUL   // OSK selected key (white, dark label)
#define XMB_KEY_LABEL_SEL 0x0010142AUL // label color on the selected key
#define XMB_ICON_IDLE   0x00646C96UL   // inactive tab icons / chrome glyphs

// -------------------------------------------------------
// XMB layout constants — pixel values authored at 1280×720.
// UIS_W/UIS_H scale them down proportionally on SD framebuffers
// (576i → 720×576, 480i/p → 720×480); HD modes keep the authored
// sizes.  Runtime expressions: fine everywhere they're used (no
// array sizes), evaluated after init_screen() sets display_*.
// -------------------------------------------------------
#define UIS_W(px) ((int)display_width  < 1280 ? (px) * (int)display_width  / 1280 : (px))
#define UIS_H(px) ((int)display_height <  720 ? (px) * (int)display_height /  720 : (px))

// CRT overscan inset (Settings > Screen Size).  0 by default, so with no
// calibration every anchor below is byte-identical to before.  Folded into the
// top/bottom/side anchors so the WHOLE XMB clears the bezel: top chrome shifts
// down by XMB_OY, bottom hints lift by XMB_OY, and the centered content plus
// edge-anchored brand/clock inset by XMB_OX (via XMB_ITEM_PAD).
#define XMB_OY          overscan_y()
#define XMB_OX          overscan_x()

#define XMB_TOPBAR_H    UIS_H(64)
#define XMB_TABBAR_H    UIS_H(80)
#define XMB_DIVIDER_Y   (XMB_OY + XMB_TOPBAR_H + XMB_TABBAR_H)
#define XMB_CONTENT_Y   (XMB_DIVIDER_Y + UIS_H(30))
#define XMB_BOTTOM_PAD  (UIS_H(70) + XMB_OY)
#define XMB_ITEM_H      UIS_H(90)
#define XMB_THUMB_W     UIS_H(52)
#define XMB_THUMB_H     UIS_H(74)
#define XMB_ITEM_PAD    (UIS_W(40) + XMB_OX)

// Centered narrow list layout (search results keep this style).
// 780 (~60% of 1280) when it fits, otherwise the framebuffer minus margins.
#define XMB_LIST_W      ((int)display_width - 2 * XMB_ITEM_PAD < 780 \
                             ? (int)display_width - 2 * XMB_ITEM_PAD : 780)
#define XMB_ROW_H       UIS_H(88)                   // row visual height
#define XMB_ROW_GAP     UIS_H(16)                   // vertical gap between rows
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
#define XMB_CARD_GAP_X   UIS_W(24)
#define XMB_CARD_TEXT_H  UIS_H(50)                  // text band under each row
// Cards are sized to fill the space between the content area and the hints
// bar at any resolution.  The 26px reserve covers the breadcrumb offset on
// sub-screens so the bottom row's text band never runs into the hints bar.
#define XMB_GRID_AVAIL_H ((int)display_height - XMB_BOTTOM_PAD - XMB_GRID_Y0 - 26)
#define XMB_CARD_H_FIT   (XMB_GRID_AVAIL_H / XMB_GRID_ROWS - XMB_CARD_TEXT_H - 6)
#define XMB_CARD_W_CAP   ((int)display_width * 300 / 1280)
#define XMB_GRID_Y0      (XMB_CONTENT_Y + 8)

// Music tab: square album cards under a sub-tab header row, with a taller
// text band (title + artist + meta for the selected card).
#define XMB_MUSIC_SUBTAB_H UIS_H(44)
#define XMB_MUSIC_TEXT_H   UIS_H(68)
#define XMB_MUSIC_COLS      5

// Music sub-tabs (d-pad: UP from the grid's top row focuses the header,
// LEFT/RIGHT switch, DOWN/X drop back into the grid).
#define MUSIC_ST_ALBUMS    0
#define MUSIC_ST_ARTISTS   1
#define MUSIC_ST_PLAYLISTS 2
#define MUSIC_ST_GENRES    3
#define MUSIC_ST_SONGS     4
#define MUSIC_ST_COUNT     5

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
// Poster-grid geometry not tied to any tab (search list, thumb prefetch).
void xmb_grid_geom_portrait(GridGeom *gg);

// Jump bar (narrow letter column to the left of the item list)
#define JBAR_ENTRIES 27
#define JBAR_W       20
#define JBAR_GAP      8

// OSK constants (pixels).  Keys are sized so the widest keyboard row —
// 11 step units on the login OSK (Caps + zxcvbnm + backspace) — fits the
// framebuffer with a small margin; capped at the 720p-authored 80px so HD
// modes are unchanged.  A 720px-wide SD framebuffer otherwise overflows
// both edges (the 576i "can't enter my server address" report).
#define OSK_UNITS_MAX 11
#define OSK_GAP       8
#define OSK_KEY_W_FIT (((int)display_width - 24) / OSK_UNITS_MAX - OSK_GAP)
#define OSK_KEY_W     (OSK_KEY_W_FIT < 80 ? OSK_KEY_W_FIT : 80)
#define OSK_KEY_H     UIS_H(44)
#define OSK_STEP_X   (OSK_KEY_W + OSK_GAP)
#define OSK_STEP_Y   (OSK_KEY_H + OSK_GAP)

// -------------------------------------------------------
// OSK data (defined in ui/ui_search.cpp, used by draw functions)
// -------------------------------------------------------
extern const char *OSK_LETTERS[OSK_ROWS_N];
extern const char *OSK_SYMBOLS[OSK_ROWS_N];

// Search layout (ui_osk_draw.cpp).  The keyboard collapses to just the search
// field once the results are focused, so the list gets the full content area
// instead of the nothing it was left with below 1080p.  Both the renderer and
// the scroll arithmetic must agree on the row count — use these, not a
// hardcoded guess.
int xmb_search_results_y(void);
int xmb_search_vis_rows(void);

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
extern int     g_music_subtab;
extern bool    g_music_header;
extern int     g_music_depth;
extern char    g_music_parent_id[64];
extern char    g_music_parent_name[128];
extern XMBItem g_music_sub_items[XMB_ITEMS_MAX];
extern int     g_music_sub_count;
extern int     g_music_sub_sel;
extern int     g_music_sub_scroll;
extern int     g_music_sub_total;
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

// Settings tab state (defined in ui_xmb_state.cpp)
// The "Player Stats Overlay" row only exists when the diagnostics module is
// compiled in — a build with ENABLE_PLAYER_STATS 0 has nothing for it to
// toggle, so the row goes away rather than sitting there doing nothing.
#define XMB_SETTINGS_COUNT (4 + ENABLE_PLAYER_STATS)  // selectable settings entries
extern int   g_settings_sel;       // highlighted settings entry
extern bool  g_settings_confirm;   // true while the logout confirm prompt is up
extern bool  g_overscan_calib;     // true while the overscan calibration screen is up
extern float g_overscan_calib_prev; // frac to restore if calibration is cancelled

// Full-screen overscan calibration screen (Settings > Screen Size).  Split into
// CPU (rects) and text (labels) phases like the rest of the XMB.  Defined in
// ui_settings.cpp; driven from the XMB loop when g_overscan_calib is set.
void xmb_overscan_calib_cpu(void);
void xmb_overscan_calib_text(void);

// -------------------------------------------------------
// Hints bar
// -------------------------------------------------------
typedef struct { char glyph; const char *label; } Hint;

// -------------------------------------------------------
// Functions implemented in ui_visuals.cpp
// -------------------------------------------------------
// PS button sprites (Kenney sheet, ui_widgets.cpp).  Same glyph codes the
// old Iconic PSx font used: X/C/S/T face buttons, A=START, B=SELECT,
// D/E=d-pad, L=L2, R=R2.  bright scales RGB (255 = as-authored).
int  ps_btn_width(char glyph, int h);
void draw_ps_button_vcentered(u32 x, int cy, char glyph, int h, u32 bright);
// Decode the sheet now rather than on the first hints bar.  It is a 1536x1536
// PNG: ~9MB of output plus a comparable working set, transient but far bigger
// than anything else the UI asks for.  Left lazy it ran after the thumbnail
// cache had taken the heap down to nothing and would simply fail (glyphs gone,
// permanently — s_ps_state latches -1).  Call it at boot while there is room.
void ps_sprites_preload(void);

void visuals_cleanup(void);
void ttf_init(void);
void ttf_prewarm_hud(void);
int  ttf_text_width(const char *text, float px, bool bold = false);
void xmb_draw_tabs(void);
void xmb_draw_meta(u32 x, u32 y, const XMBItem *it, float px = 14);

// Card grid (library tabs).  Two phases per frame (after rsxSync):
//   cpu   — card images (CPU blit from main RAM), placeholders, selection
//           border, progress strips, next-page prefetch
//   text  — titles under every card (selected emphasized) + scroll arrows
void xmb_grid_cpu(const GridGeom *gg, const XMBItem *items, int count,
                  int sel, int scroll, int y0);

// Colored letter tile — album-art placeholder (loading or artless albums)
// and Up Next queue thumbs.  Color is a stable hash of seed (the item id).
void xmb_draw_letter_tile(const char *seed, const char *name,
                          int x, int y, int size);

// 1:1 blit of a cached thumb at exactly w x h; false while still loading.
bool xmb_cpu_blit_thumb(const char *item_id, int x, int y, int w, int h);

// Scaled blit of an item's Primary poster (requested at the Movies-tab card
// size, so the 2:3 aspect is preserved) into an arbitrary w x h rect; draws a
// dim placeholder while the fetch is still in flight.  Used by the search list
// and the triangle detail overlay's poster.
void xmb_cpu_blit_thumb_scaled(const char *item_id, int x, int y, int w, int h);

// Music sub-tab header row.  active = g_music_subtab; focused = the d-pad
// is on the header (active label pops white instead of just underlined).
void xmb_draw_music_subtabs(int x, int y, int active, bool focused);
// abs_start/abs_total place the loaded window inside the full library so
// the scrollbar reflects the whole list (0 total = use count).
void xmb_grid_text(const GridGeom *gg, const XMBItem *items, int count,
                   int sel, int scroll, int y0, bool more_below,
                   int abs_start, int abs_total);
void xmb_rsx_draw_osk(void);
void xmb_cpu_draw_osk(void);
void xmb_cpu_draw_search_results(void);
void xmb_draw_jumpbar(int tab);
void draw_hints_bar(const Hint *hints, int n);
void xmb_draw_topbar(void);         // brand top-left, clock top-right
void xmb_draw_divider(void);        // faded hairline under the tab bar (CPU phase)
// Centered empty-state: the tab's icon, dimmed, above one line of text.
void xmb_draw_empty_state(int tab, const char *msg);
// Breadcrumb trail: up to two dim parent segments + bright leaf, with
// chevron separators.  Pass NULL for unused segments.
void xmb_draw_breadcrumb(int x, int y, const char *a, const char *b,
                         const char *leaf);
void xmb_cpu_draw_settings(void);   // CPU phase: highlight rect
void xmb_draw_settings(void);       // RSX phase: account info + entries

// Update-available popup (render/ui_update_popup.cpp).  Active from the
// frame the background release check reports a newer version until the user
// dismisses it for the session.
bool xmb_update_popup_active(void);
void xmb_update_popup_input(void);  // owns the frame's input; X dismisses
void xmb_update_popup_draw(void);   // dim + panel; call last, before flip()
