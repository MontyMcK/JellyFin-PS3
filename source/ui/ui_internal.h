#pragma once
// Cross-file declarations internal to the UI module.  Public API lives in
// ui.h / ui_visuals.h; everything here is implementation plumbing shared
// between the xmb/ source files.

#include "ui.h"
#include "ui_visuals.h"

// -------------------------------------------------------
// JSON helpers (xmb/ui_json.cpp)
// -------------------------------------------------------
int       xmb_json_str_range(const char *start, int len,
                             const char *key, char *out, int out_size);
int       xmb_json_int_range(const char *start, int len,
                             const char *key, int def);
long long xmb_json_ll_range(const char *start, int len,
                            const char *key, long long def);
int       xmb_json_first_arr_str(const char *start, int len,
                                 const char *key, char *out, int out_size);
int       parse_xmb_items(const char *json, XMBItem *arr, int max);
// The same, calling each(index, object, length, ctx) for every item kept, so
// a caller can read fields XMBItem has no room for (Home's series ids).
typedef void (*XMBItemEach)(int index, const char *obj, int olen, void *ctx);
int       parse_xmb_items_each(const char *json, XMBItem *arr, int max,
                               XMBItemEach each, void *ctx);

// -------------------------------------------------------
// Tab switching + library fetch (xmb/ui_nav.cpp, xmb/ui_fetch.cpp)
// -------------------------------------------------------
void xmb_switch_tab(int new_tab);
int  xmb_next_enabled(int start, int dir);

void xmb_detect_tabs(void);
bool xmb_take_prepared(void);     // see xmb_prepare() in ui.h
// One attempt only — for the XMB's background retry, which must not block
// the render loop inside the full retry set.
bool xmb_detect_tabs_once(void);
void xmb_fetch_tab_items(int tab);
int  xmb_fetch_seasons(const char *series_id, XMBItem *arr, int max,
                       int start_index, int *out_total);
int  xmb_fetch_episodes(const char *series_id, const char *season_id,
                        XMBItem *arr, int max,
                        int start_index, int *out_total);
int  xmb_fetch_collection_items(const char *collection_id, XMBItem *arr,
                                int max, int start_index, int *out_total);
int  xmb_fetch_similar(const char *item_id, XMBItem *arr, int max);
int  xmb_fetch_music_children(const char *id_param, const char *parent_id,
                              XMBItem *arr, int max, int *out_total);
// Tracks of one playlist, in playlist order (see ui_fetch.cpp).
int  xmb_fetch_playlist_items(const char *playlist_id, XMBItem *arr, int max,
                              int *out_total);
bool xmb_fetch_next_episode(const char *episode_id, XMBItem *out);

// Sliding-window pagination: drop the first page, fetch the next one.
// Returns the index of the first newly-visible row, or -1 if nothing came back.
int  xmb_slide_tab_forward(int tab);
int  xmb_slide_tv_sub_forward(void);
int  xmb_slide_col_sub_forward(void);

// Reverse direction: prepend the previous page (dropped by a forward slide)
// so the user can scroll back to the top.  Returns the number of prepended
// items (existing indices shift up by that much), or -1 at the top / on a
// failed fetch.
int  xmb_slide_tab_backward(int tab);
int  xmb_slide_tv_sub_backward(void);
int  xmb_slide_col_sub_backward(void);

// -------------------------------------------------------
// Per-tab input handlers (xmb/ui_nav.cpp, xmb/ui_search.cpp, xmb/ui_home.cpp)
// Return true when the XMB loop should exit (logout / quit).
// -------------------------------------------------------
bool xmb_handle_input_browse(void);
bool xmb_handle_input_search(void);
// Join the search worker.  Call before leaving the XMB.
// (xmb_search_in_flight() lives in ui_visuals.h, beside the search state, so
// the results-list drawing can see it too.)
void xmb_search_shutdown(void);
bool xmb_handle_input_home(void);

// Launch the player for one list item (xmb/ui_nav.cpp).  resume_secs > 0
// starts playback at that saved position.
void xmb_play_item(const XMBItem *it, u32 resume_secs,
                   const char *media_source_id = NULL);

// Counts playback starts and mark-as-watched (ui_nav.cpp, ui_info.cpp).  A
// change means Continue Watching and Next Up are out of date.
extern unsigned g_play_gen;

// Play an episode with the end-of-item NEXT prompt / auto-advance, resolving
// each follower from the server so it works from any launch point (Home rows,
// Continue Watching, search, season lists) and across season boundaries.
void xmb_play_episode_with_next(const XMBItem *first, u32 resume_secs,
                                const char *media_source_id = NULL);

// Triangle detail overlay (xmb/ui_info.cpp)
void xmb_show_item_info(const XMBItem *it);

// Open the Seasons -> Episodes browser for a Series.
//
// A Series is a FOLDER, not something playable, so neither X nor Triangle
// should hand it to the player or to the version overlay -- there is no
// version to pick until an actual episode is chosen.  Every entry point
// (home rows, search results, any library list) routes through here so they
// cannot drift apart again.  Returns false if the TV tab is unavailable.
bool xmb_open_series(const XMBItem *it);

// Resume-or-restart prompt for a partly-watched item (xmb/ui_info.cpp).
// Returns seconds to start at (0 = beginning), or <0 if the user cancelled.
// Returns 0 without prompting when the item is not meaningfully resumable.
int xmb_resume_choice(const XMBItem *it);

// -------------------------------------------------------
// Home shelf screen (xmb/ui_home.cpp) — stacked horizontal rows
// (Continue Watching, Next Up, Recently Added Movies/Shows, Music stub).
// -------------------------------------------------------
void xmb_home_on_enter(void);     // reset focus + mark dynamic rows for refetch
// Spine gate: true when no row above the focus has anything in it, so Up
// leaves for the base layer.  (Empty rows are stepped over.)
bool xmb_home_at_top(void);
// Spine gate: X on the base layer -- open the focused queue item (detail, a
// series' seasons, an album).  False when there is nothing to open.
bool xmb_home_open_focused(void);
// Spine gate: Triangle -- the quick-peek of the focused queue item (an album
// opens instead).  False when there is nothing focused.
bool xmb_home_peek_focused(void);
// Spine gate: Home drawn at every depth, its column swinging into the queue
// (the canvas's "L2 · Category").  Replace the column and xmb_home_*_phase.
void xmb_home_stage_gpu(void);
void xmb_home_stage_cpu(void);
void xmb_home_stage_text(void);

// The ambient screensaver's artwork (xmb/ui_ambient.cpp): posters Home has
// already fetched -- Continue Watching, Next Up, Recently Added Movies and
// Shows -- with the size they are cached at, so the screensaver shares the
// Home queue's cache slots and fetches nothing Home would not.  Strings point
// into Home's rows and are valid until the next fetch.  Returns how many.
typedef struct {
    const char *img_id;
    int         src_w, src_h;
    const char *eyebrow;     // the row's title
    const char *title;       // an episode goes by its series
    const char *meta;        // "2014" / "S1 E4" (may be "")
} AmbientArt;
int  xmb_home_ambient_art(AmbientArt *out, int max);

// The ambient screensaver (xmb/ui_ambient.cpp).  Once per XMB frame, after
// poll_buttons(): ambient_update() advances it and returns true when this
// frame's input must not reach the UI (it woke the screensaver).  `allowed`
// = false over a modal, which counts as activity.  ambient_draw_ui() says
// whether the XMB draws its own phases this frame; when it does not,
// ambient_gpu() / ambient_text() draw in their place.  ambient_cover_over_ui()
// goes after the frame's text flush (the dissolve).  All inert with the spine
// gate off.
bool ambient_update(bool allowed);
bool ambient_draw_ui(void);
void ambient_gpu(void);
void ambient_text(void);
void ambient_cover_over_ui(void);

// The Triangle quick-peek (xmb/ui_peek.cpp): on any focused poster under the
// spine -- Home, the base column, the library grids and sub-grids, Search --
// the poster spins 180 degrees into a 4:3 panel with the synopsis and cast.
// peek_open_item() starts it (src_w/h = the screen's thumbnail size, used when
// the depth stage did not note the focused card's image); `open` is what X
// does from the peek (NULL: a series opens its seasons, anything else detail);
// peek_input() takes the frame's input while it is up (true = consumed);
// peek_draw_over() draws it after the frame's text flush.  Spine gate only.
void peek_open_item(const XMBItem *it, int src_w, int src_h, void (*open)(void) = NULL);
bool peek_input(void);
// Warm the first n posters of another library tab (xmb/ui_depth_lib.cpp).
void depth_lib_prefetch(int tab, int n);
// The same card turn for a Settings row: (x,y,w,h) is the row on screen.
void peek_open_text(const char *title, const char *body, int x, int y, int w, int h);
// Settings: open the highlighted row's description as a peek (ui_settings.cpp).
void settings_open_help_peek(void);
bool peek_active(void);    // opening or open: owns input
bool peek_visible(void);   // anything on screen, including the close
void peek_draw_over(void);

// -------------------------------------------------------
// Library categories on the depth engine (xmb/ui_depth_lib.cpp)
// -------------------------------------------------------
// Every tab whose L2 is the card grid, under the spine: the L1 column and its
// swing into the grid are drawn by the engine's stage; the grid itself takes
// the screen back at the hand-over.  depth_lib_owns() says whether the stage
// (true) or the tab's own screen draws this frame.  Inert with the gate off.
bool depth_lib_owns(int tab);
void depth_lib_gpu(int tab);
void depth_lib_cpu(int tab);
void depth_lib_text(int tab);
// True for a tab whose L1 -> L2 move is a stage's swing (Home and the grid
// tabs), so the content glide is not applied on top of it.
bool depth_stage_tab(int tab);

// The card-grid view of the current tab and sub-screen (ui_xmb.cpp): geometry,
// item array, count, selection, scroll, grid origin, more-below, and the
// window's place in the whole library.  False for Search and Settings.
bool xmb_grid_view(int tab, GridGeom *gg, const XMBItem **items,
                   int *count, int *sel, int *scroll, int *y0,
                   bool *more_below, int *abs_start, int *abs_total);
void xmb_home_prefetch(void);     // fetch every unloaded row now (boot worker)
void xmb_home_gpu_phase(void);    // card images as RSX quads (BEFORE rsxSync)
void xmb_home_cpu_phase(void);    // card images / placeholders / selection (after rsxSync)
void xmb_home_text_phase(void);   // row titles, labels, chevrons
