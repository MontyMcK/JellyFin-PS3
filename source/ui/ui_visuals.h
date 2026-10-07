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
// Palette — now a RUNTIME struct, not #defines.  See theme.h.
// -------------------------------------------------------
// Phase 1 of the XMB revamp moved every colour below into `Theme`, so the
// palette can be swapped at runtime (Settings > Theme) and shipped as .ini
// files.  theme.h defines one shim macro per colour that used to live here —
// XMB_ACCENT and friends still resolve, now to g_theme fields — so every
// existing draw call site compiles and reads exactly as before.
//
// Adding a colour: put it in the Theme struct, give both built-ins a value,
// add an [colors] key in theme_load(), and add its shim to theme.h.  Do NOT
// reintroduce a bare #define here — it would silently ignore the theme.
//
// XMB_ACCENT changed value in this phase: the old 8F6FE8 violet became
// AA5CC3, Jellyfin's official purple, per handoff section 2.1.  XMB_ACCENT_DEEP
// likewise changed role, from the OSK caps-lock latch to the primary-action
// ramp start; the caps-lock key still uses it and still reads as a deeper
// accent, so nothing needed re-pointing.
#include "theme.h"

// -------------------------------------------------------
// XMB layout constants -- pixel values authored at 1280x720, scaled to the
// framebuffer.
// -------------------------------------------------------
// ONE scale, applied to everything the XMB draws: geometry through UIS_W/UIS_H
// and type through UIS_TF.  That uniformity is the whole point, and it was not
// always true -- see below.
//
// These used to scale DOWN only:
//
//     #define UIS_H(px) ((int)display_height < 720 ? (px) * h / 720 : (px))
//
// i.e. SD framebuffers shrank, and 720p and above got the authored number as a
// literal pixel count.  On this console's 1920x1080 that made the whole UI
// two-thirds of its intended size, which is why a 12.5px label taken straight
// from the design handoff was reported unreadable on the TV.
//
// Fixing that for the chrome alone (a separate UIS_T that scaled both ways) was
// worse than either extreme: the top bar, tab strip and hints bar rendered at
// 1.5x while the cards, rows, jump bar and every literal font size stayed at
// 1.0x, so the UI was at TWO scales at once -- reported as "a lot of the sizing
// in general seems off", with the A-Z jump bar as the loudest symptom (letters
// sized from the rescaled grid, drawn in a column that had not moved).
//
// So UIS_W/UIS_H now scale in both directions and UIS_T is gone.  Anything
// authored against the handoff's 1280x720 canvas goes through one of these
// three macros -- no bare pixel literals in XMB draw code, including font
// sizes, or that thing alone stays at 1.0x and the mismatch is back.
//
// Width and height scale independently on purpose: SD is 720x576 and 720x480,
// which are not 16:9 pixel grids, and the previous code already treated the
// two axes separately.  At 720p and 1080p the two factors are equal.
//
// NOT in scope: the player HUD (player/hud/) and the boot/auth screens, whose
// geometry is raw pixels throughout and self-consistent that way.  Scaling
// their type without their boxes would overflow rows that are 30px tall.
//
// Runtime expressions -- fine everywhere they are used (no array sizes),
// evaluated after init_screen() has set display_*.
// The live scale, as a percentage of the authoring canvas.  0 means AUTO:
// track the framebuffer, which is the default and what the paragraphs above
// describe.  Any other value overrides it on both axes, so 100 reproduces the
// pre-2026-09-20 look exactly (authored numbers as literal pixels) and 120 is
// the "increase by 10-20%" that prompted this work.
//
// Settable without a rebuild -- it is read from jellyfin_uiscale.txt at boot
// (ui_scale.cpp), which is an ordinary text file in the app data dir and can be
// dropped in over FTP.  That matters because this is the one change in the UI
// whose only real test is looking at it on a TV: if the proportional default is
// wrong for a given set, the fix is one line in a text file rather than a
// rebuild and a reflash.
extern int g_uis_pct;

// THE SAFE AREA (2026-09-24).  The 1280x720 authoring canvas maps onto the
// overscan-SAFE rect -- the framebuffer minus the CRT inset on every edge --
// not onto the whole framebuffer.  Before this, the scale came from the full
// display and the inset was only ADDED as a translation (XMB_OX / XMB_OY), so
// the left and top edges moved in by the inset while the right and bottom
// edges moved OUT by the same amount.  No overscan value could fit both
// sides: raising it to pull the right edge in pushed the left edge further
// from the bezel than the right one, and anything drawn at an authored
// absolute position (the spine, the depth stage, item detail, the redesigned
// player HUD) overflowed right and bottom by one inset while pad-anchored
// chrome (XMB_ITEM_PAD, XMB_BOTTOM_PAD) did not -- two geometries on one
// screen.
//
// Now: screen = inset + authored * (safe size / canvas size), on both axes,
// geometry AND type.  So XMB_OX + UIS_W(1240) == display_width - XMB_ITEM_PAD
// and XMB_OY + UIS_H(698) == display_height - XMB_OY - UIS_H(22): an authored
// position and a pad-anchored one land on the same pixel, and margins are
// symmetric at every calibration (tests/test_layout, test_safe_area()).
//
// With no calibration the inset is 0, the safe rect is the framebuffer, and
// every expression below is integer-identical to what it was.
int overscan_x(void);
int overscan_y(void);
static inline int uis_safe_w(void) { return (int)display_width  - 2 * overscan_x(); }
static inline int uis_safe_h(void) { return (int)display_height - 2 * overscan_y(); }

// The override is a percentage of the authoring canvas; under overscan it is
// shrunk by the same safe fraction, so a calibrated screen still fits.  The
// divide is exact when the inset is 0: floor(px*pct*W / (100*W)) is
// floor(px*pct/100).
static inline int uis_w(int px) {
    return g_uis_pct ? px * g_uis_pct * uis_safe_w() / (100 * (int)display_width)
                     : px * uis_safe_w() / 1280;
}
static inline int uis_h(int px) {
    return g_uis_pct ? px * g_uis_pct * uis_safe_h() / (100 * (int)display_height)
                     : px * uis_safe_h() / 720;
}
static inline float uis_tf(float px) {
    return g_uis_pct ? px * (float)g_uis_pct / 100.0f
                          * (float)uis_safe_h() / (float)display_height
                     : px * (float)uis_safe_h() / 720.0f;
}

#define UIS_W(px)  uis_w((int)(px))
#define UIS_H(px)  uis_h((int)(px))

// Float form, for type sizes and the handoff's fractional values -- drawTTF()
// and ttf_text_width() both take a float, and rounding a 12.5px label to 12
// before scaling loses more than it looks like it should.
#define UIS_TF(px) uis_tf((float)(px))

// CRT overscan inset (Settings > Screen Size).  0 by default, so with no
// calibration every anchor below is byte-identical to before.  Folded into the
// top/bottom/side anchors so the WHOLE XMB clears the bezel: top chrome shifts
// down by XMB_OY, bottom hints lift by XMB_OY, and the centered content plus
// edge-anchored brand/clock inset by XMB_OX (via XMB_ITEM_PAD).
#define XMB_OY          overscan_y()
#define XMB_OX          overscan_x()

// The chrome band, from the handoff's measured numbers.
//
// Section 3.1 and the Phase 2 block in section 8 put the tab icons at y=61, the
// active label at y=96, its underline at y=110 and the divider at y=144, all on
// the 1280x720 authoring canvas.  Those four have to keep their spacing
// relative to each other, so they go through one scale -- and now so does
// everything below them, which is the fix described at the top of this file.
// At 1080p the divider lands at 216, which IS 144 on this screen.
#define XMB_TOPBAR_H    UIS_H(64)
#define XMB_TABBAR_H    UIS_H(80)

// The spine (README 2.9, render/spine.h) replaces the tab strip when
// jellyfin_spine.txt says 1 -- read once at boot by spine_load().  Every XMB
// screen is currently L2, where the spine's label stack runs down to y~205
// and the header divider moves from 144 to 214.  Everything below the chrome
// is positioned from XMB_CONTENT_Y, so moving the divider moves every screen
// with it; nothing else needs to know the spine exists.
//
// A runtime choice rather than a rebuild, like the other gates: a spine that
// reads badly on the TV is one character over FTP from the old strip.
extern bool g_spine_on;
#include "render/spine.h"
#define XMB_DIVIDER_Y   (g_spine_on                                              \
        ? (XMB_OY + UIS_H((int)SPINE_LEVEL[SPINE_L2].divider_y))                \
        : (XMB_OY + XMB_TOPBAR_H + XMB_TABBAR_H))
// Where content sits AT REST, and where it is drawn this frame.  They differ
// only while the spine is gliding a tab's content up into place (0 with the
// gate off and at rest; see spine_content_dy() in render/ui_spine.cpp).
// POSITIONS use XMB_CONTENT_Y.  SIZES -- card heights, visible row counts,
// the jump rail's pitch -- must use XMB_CONTENT_Y_REST, or every card would
// resize on every frame of the glide and its thumbnail (cached per size)
// would be refetched.
int spine_content_dy(void);
#define XMB_CONTENT_Y_REST (XMB_DIVIDER_Y + UIS_H(30))
#define XMB_CONTENT_Y      (XMB_CONTENT_Y_REST + spine_content_dy())
#define XMB_BOTTOM_PAD  (UIS_H(70) + XMB_OY)
#define XMB_ITEM_H      UIS_H(90)
#define XMB_THUMB_W     UIS_H(52)
#define XMB_THUMB_H     UIS_H(74)
#define XMB_ITEM_PAD    (UIS_W(40) + XMB_OX)

// Centered narrow list layout (search results keep this style).
// 780 (~60% of 1280) when it fits, otherwise the framebuffer minus margins.
#define XMB_LIST_W      ((int)display_width - 2 * XMB_ITEM_PAD  < UIS_W(780) \
                             ? (int)display_width - 2 * XMB_ITEM_PAD : UIS_W(780))
#define XMB_ROW_H       UIS_H(88)                   // row visual height
#define XMB_ROW_GAP     UIS_H(16)                   // vertical gap between rows
#define XMB_ROW_STRIDE  (XMB_ROW_H + XMB_ROW_GAP)
#define XMB_ROW_RADIUS  UIS_W(8)                         // reserved for future rounded corners

// Items visible simultaneously in the list area
#define XMB_ITEMS_VIS ((int)((display_height - XMB_CONTENT_Y_REST - XMB_BOTTOM_PAD) / XMB_ROW_STRIDE))

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
#define XMB_GRID_AVAIL_H ((int)display_height - XMB_BOTTOM_PAD - XMB_GRID_Y0_REST - UIS_H(26))
#define XMB_CARD_H_FIT   (XMB_GRID_AVAIL_H / XMB_GRID_ROWS - XMB_CARD_TEXT_H - UIS_H(6))
#define XMB_CARD_W_CAP   UIS_W(300)
#define XMB_GRID_Y0      (XMB_CONTENT_Y + UIS_H(8))
#define XMB_GRID_Y0_REST (XMB_CONTENT_Y_REST + UIS_H(8))   // for sizes

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

// The one size the spine's Home queue fetches every poster at (ui_home.cpp):
// its focused 200x300, capped to a pixel budget the thumbnail cache can hold.
// 0 x 0 with the gate off.  The cache sizes its slots to fit it.
void xmb_queue_src(int *w, int *h);

// Jump bar (narrow letter column to the left of the item list).  The column
// scales with the letters in it -- when it did not, the font size derived from
// the grid outgrew a fixed 20px column and the A-Z strip overflowed.
#define JBAR_ENTRIES 27
#define JBAR_W       UIS_W(20)
#define JBAR_GAP     UIS_W(8)

// OSK constants, authored at 1280x720 like everything else here.  Keys are
// sized so the widest keyboard row -- 11 step units on the login OSK (Caps +
// zxcvbnm + backspace) -- fits the framebuffer with a small margin, capped at
// the authored 80px.  A 720px-wide SD framebuffer otherwise overflows both
// edges (the 576i "can't enter my server address" report), and the FIT term is
// what catches that: the cap alone would not, because the cap scales with the
// screen and the row's total width does not fit at every aspect.
#define OSK_UNITS_MAX 11
#define OSK_GAP      UIS_W(8)
#define OSK_KEY_W_FIT (((int)display_width - UIS_W(24)) / OSK_UNITS_MAX - OSK_GAP)
#define OSK_KEY_W     (OSK_KEY_W_FIT < UIS_W(80) ? OSK_KEY_W_FIT : UIS_W(80))
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
// True while a search is out on its worker thread (ui_search.cpp).  Declared
// here beside the rest of the search state because BOTH sides need it: the
// input handler starts and collects the query, and the results list uses it to
// show "Searching..." instead of "No results" for the several seconds the
// server takes to answer.
bool xmb_search_in_flight(void);
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
#define XMB_SETTINGS_COUNT (15 + ENABLE_PLAYER_STATS)  // selectable settings entries
extern int   g_settings_sel;       // highlighted settings entry
extern bool  g_settings_confirm;   // true while the logout confirm prompt is up
extern bool  g_settings_help;      // Triangle: the highlighted row's description is up
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
// glyph: X/C/S/T face buttons, A=START, B=SELECT, D/E=d-pad, L=L2, R=R2 --
// all drawn from the Kenney sprite sheet -- plus lowercase 'l'/'r' for L1/R1,
// which are drawn instead as outlined badges (handoff section 3.1).
//
// An EMPTY label pairs a hint with the one after it, sharing that one's label:
// {{'l',""},{'r',"Tab"}} renders as a single "L1 R1 Tab" cluster.
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
// Typefaces available to drawTTF_face()/ttf_text_width_face().  The two
// subtitle faces are bold-only and subset to Latin plus SubRip's punctuation;
// see source/gfx/fonts/LICENSES.md for provenance and licences.
#define UI_FACE_REGULAR   0
#define UI_FACE_BOLD      1
#define UI_FACE_NOTO      2   // Noto Sans Bold        (SIL OFL 1.1)
#define UI_FACE_ROBOCOND  3   // Roboto Condensed Bold (Apache 2.0)
// The v1.0 type roles, one per design token.  Each resolves to a FALLBACK
// CHAIN, not a single face -- see face_chain() in render/ui_text.cpp.  The
// display face in particular is a 99-glyph subset with no hyphen or accents,
// so "Spider-Man" only renders because Rodin backs it up mid-word.
//
// REGULAR/BOLD are SCE-PS3 Rodin LATIN: --font-system and --font-tech, the
// face for every string that is not one of the four roles below.
#define UI_FACE_DISPLAY   4   // GT America Expanded Bold - media titles
#define UI_FACE_SPEC      5   // Michroma - codec / quality values
#define UI_FACE_EYEBROW   6   // Microgramma - eyebrows and section labels
#define UI_FACE_TAB       7   // Satoshi Bold - tab labels, clock
#define UI_FACE_TAB_REG   8   // Satoshi Regular - date, cast names
// The wordmark, and nothing else.  The design gives the lockup its own stack
// ('Mata','Microgramma','Michroma') because the brand is not body type: it is
// one eight-letter string drawn once a frame, so it can afford a face that
// exists purely for it.
#define UI_FACE_LOCKUP    9   // Mata Bold - the "JELLYFIN" wordmark

int  ttf_text_width(const char *text, float px, bool bold = false);
int  ttf_text_width_face(const char *text, float px, int face);
void drawTTF_face(u32 x, u32 y, const char *text, float px, u32 color, int face);

// Letter-spacing, in pixels ADDED BETWEEN glyphs (the design spells it in em:
// 0.04em at 11.5px is 0.46px).  See the note in render/ui_text.cpp -- these
// take the per-glyph path, so they are for short labels, not body text.
void drawTTF_tracked(u32 x, u32 y, const char *text, float px, u32 color,
                     int face, float track);
int  ttf_text_width_tracked(const char *text, float px, int face, float track);

// drawTTF_tracked with a horizontal colour ramp instead of one colour: `stops`
// are evenly spaced across the run and each glyph takes the ramp at its own
// centre.  Width is ttf_text_width_tracked -- the ramp costs no space.
// This exists for the lockup wordmark.  It is not a general gradient-text
// facility, and it takes the same per-glyph path its tracking does.
void drawTTF_ramp(u32 x, u32 y, const char *text, float px,
                  const u32 *stops, int nstops, int face, float track);
// drawTTF_ramp with each glyph posed, for the boot animation's wordmark:
// glyph i slides into its home from up to max_slide px to its left (never
// from left of x; max_slide <= 0 means always from x) by reveal[i]
// (1 = home), at opacity alpha[i].  At all-ones it draws exactly
// drawTTF_ramp's pixels.  stops may be NULL for a flat black run (the
// lockup's drop shadow).
void drawTTF_ramp_posed(u32 x, u32 y, const char *text, float px,
                        const u32 *stops, int nstops, int face, float track,
                        const float *reveal, const float *alpha, int npose,
                        float max_slide = 0.0f);

// The y that centres a run's INK box (not its em box) on cy, for any face.
// false = the string has no ink, so draw nothing.
bool ttf_center_y(const char *text, float px, int face, int cy, int *out_y);

// ASCII-only uppercase, in place.  Safe on the UTF-8 the server sends.
void ui_upper_ascii(char *s);

// Glyph-blit counters (ui_text.cpp).  blend_px counts pixels that took the
// read-modify-write branch -- one uncached read of RSX video memory each,
// which tools/spubench measured at 100x the cost of a write -- and opaque_px
// counts the ones that hit the store-only fast path.  Read by the XMB's
// per-frame cost line; see docs/spu-feasibility.md.
void ui_text_stats_reset(void);
void ui_text_stats_get(u32 *glyphs, u32 *blend_px, u32 *opaque_px);
int  xmb_nav_depth(void);
int  xmb_tab_focus_center(void);
void xmb_draw_tabs(void);
void xmb_draw_meta(u32 x, u32 y, const XMBItem *it, float px = UIS_TF(14));

// Card grid (library tabs).  Two phases per frame (after rsxSync):
//   cpu   — card images (CPU blit from main RAM), placeholders, selection
//           border, progress strips, next-page prefetch
//   text  — titles under every card (selected emphasized) + scroll arrows
// GPU phase (BEFORE rsxSync): card images as RSX textured quads, drawn from
// each cache slot's VRAM mirror.  The matching CPU call then skips its blit.
// See ui/render/ui_card_gpu.h for why this exists and what it costs.
void xmb_grid_gpu(const GridGeom *gg, const XMBItem *items, int count,
                  int sel, int scroll, int y0);

// Which rows the three grid walks DRAW, relative to `scroll`'s row, and a
// pixel offset for all of them -- the grid's scroll easing (render/ui_spine.cpp
// spine_grid_motion).  Rest is (0, XMB_GRID_ROWS, 0); callers put it back.
void xmb_grid_motion(int row0, int rows, int dy);

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

// The same image as a CIRCLE of diameter d, with a one-pixel rim.  v1.0 uses
// this for the cast headshots.  Write-only, including its anti-aliasing -- see
// the note on cpu_blit_bitmap_circle() in render/ui_lists.cpp for why that
// matters and what it costs to get wrong.
void xmb_cpu_blit_thumb_circle(const char *item_id, int x, int y, int d,
                               u32 rim);

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
// The lockup's mark, with its BELL bell_px wide and its top-left at (x,y).
// The raster carries the design's drop shadow in a padded box, so it draws
// slightly larger than bell_px and slightly above/left of (x,y).
void xmb_draw_mark(int x, int y, int bell_px);
void xmb_draw_mark_a(int x, int y, int bell_px, u8 alpha);
// The top bar's lockup opacity, 0..1 (the music screen fades it in focus mode).
extern float g_topbar_logo_a;
// Which mark raster the loaded theme takes: 0 = cool, 1 = gold.  The boot
// animation's large mark follows the same choice.
int  xmb_mark_variant(void);

// Where the lockup sits.  ONE source for the top bar and for the boot
// animation, which docks its mark onto exactly these numbers -- so the
// animation's last frame and the static lockup are the same pixels.
typedef struct {
    int   mark_x, mark_y, mark_px;   // the mark's bell box (xmb_draw_mark args)
    int   cy;                        // the shared row's centre line
    int   word_x, word_y;            // "JELLYFIN" pen position (ink-centred)
    float word_px, track;
    bool  word_ok;                   // false: the face has no ink, skip it
} XmbLockupGeom;
void xmb_lockup_geom(XmbLockupGeom *g);

// Section eyebrow: the small uppercase label above a row (--font-eyebrow,
// 11px, 0.18em tracking).  Returns its advance so a count can follow it.
int  xmb_draw_eyebrow(int x, int y, const char *text, u32 color);
int  xmb_eyebrow_width(const char *text);
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
// Settings > Software Update: show the popup again (after a dismissal, or
// when a re-run check finds a release).
void xmb_update_popup_reopen(void);
