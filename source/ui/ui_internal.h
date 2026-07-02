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

// -------------------------------------------------------
// Tab switching + library fetch (xmb/ui_nav.cpp, xmb/ui_fetch.cpp)
// -------------------------------------------------------
void xmb_switch_tab(int new_tab);
int  xmb_next_enabled(int start, int dir);

void xmb_detect_tabs(void);
void xmb_fetch_tab_items(int tab);
int  xmb_fetch_seasons(const char *series_id, XMBItem *arr, int max,
                       int start_index, int *out_total);
int  xmb_fetch_episodes(const char *series_id, const char *season_id,
                        XMBItem *arr, int max,
                        int start_index, int *out_total);
int  xmb_fetch_collection_items(const char *collection_id, XMBItem *arr,
                                int max, int start_index, int *out_total);

// Sliding-window pagination: drop the first page, fetch the next one.
// Returns the index of the first newly-visible row, or -1 if nothing came back.
int  xmb_slide_tab_forward(int tab);
int  xmb_slide_tv_sub_forward(void);
int  xmb_slide_col_sub_forward(void);

// -------------------------------------------------------
// Per-tab input handlers (xmb/ui_nav.cpp, xmb/ui_search.cpp, xmb/ui_home.cpp)
// Return true when the XMB loop should exit (logout / quit).
// -------------------------------------------------------
bool xmb_handle_input_browse(void);
bool xmb_handle_input_search(void);
bool xmb_handle_input_home(void);

// Launch the player for one list item (xmb/ui_nav.cpp).  resume_secs > 0
// starts playback at that saved position.
void xmb_play_item(const XMBItem *it, u32 resume_secs);

// Triangle detail overlay (xmb/ui_info.cpp)
void xmb_show_item_info(const XMBItem *it);

// -------------------------------------------------------
// Home shelf screen (xmb/ui_home.cpp) — stacked horizontal rows
// (Continue Watching, Next Up, Recently Added Movies/Shows, Music stub).
// -------------------------------------------------------
void xmb_home_on_enter(void);     // reset focus + mark dynamic rows for refetch
void xmb_home_cpu_phase(void);    // card images / placeholders / selection (after rsxSync)
void xmb_home_text_phase(void);   // row titles, labels, chevrons
