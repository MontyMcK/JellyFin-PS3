#pragma once
#include <stdint.h>
#include "dl_manager.h"

// -------------------------------------------------------------------------
//  Offline downloads -- what the screens say and do (Stage 5)
// -------------------------------------------------------------------------
//  The item page's DOWNLOAD button, the Downloads list and the Offline
//  library are drawn by ui/xmb/ui_downloads.cpp and ui/xmb/ui_info.cpp, but
//  every word they show and every decision about what a button press does
//  lives here, pure, so tests/test_offline.cpp pins it.  The screens only
//  draw and forward the result to dl_manager / show_player_offline.
// -------------------------------------------------------------------------

// Context the labels depend on besides the item itself.
typedef struct {
    bool playback_block;   // dl_playback_blocking(): a heavy stream is on
    bool auth_held;        // dl_auth_held(): signed out / sign-in expired
    bool ready;            // dl_manager_ready(): the store is usable at all
} DlUiContext;

// ---- the item page ------------------------------------------------------

typedef enum {
    DL_UI_NONE = 0,        // nothing to do (e.g. no version to download)
    DL_UI_START,           // enqueue: dl_download_item()
    DL_UI_PAUSE,
    DL_UI_RESUME,
    DL_UI_RETRY,           // failed/cancelled: enqueue again (fresh url)
    DL_UI_PLAY_OFFLINE,    // completed: show_player_offline()
    DL_UI_CANCEL,          // Downloads list, Square on an unfinished item
    DL_UI_REMOVE,          // Square on a finished/failed/cancelled item
} DlUiAction;

// st NULL = not in the queue at all.  can_download: the page has a version
// to download (a Movie/Episode/Video with its versions list loaded).
DlUiAction dl_ui_item_action(const DlStatus *st, bool can_download,
                             const DlUiContext *cx);
// The button's text for the same inputs: "Download", "Queued",
// "Downloading 42%", "Paused 42%", "Retry download", "Play offline", ...
void dl_ui_item_label(const DlStatus *st, bool can_download,
                      const DlUiContext *cx, char *out, int cap);
// A one-line message for a request that could not be made (shown under the
// button for a few seconds).  "" for DL_OK.
const char *dl_ui_result_text(int dl_result);
// The same, with the numbers when there are any to give: a refusal for lack
// of space says what was needed and what is available (rep from
// dl_last_space_report()).  Falls back to dl_ui_result_text().
void dl_ui_result_message(int dl_result, const DlSpaceReport *rep,
                          char *out, int cap);

// ---- the Downloads list ---------------------------------------------------

typedef struct {
    char title[DL_TITLE_MAX];
    char status[96];        // "Downloading", "Waiting...", "Retrying in 8s -- Server unreachable"
    char size[64];          // "1.2 GB of 3.4 GB", "340 MB", ""
    int  permille;          // progress 0..1000, or -1 = no bar (unknown/none)
    bool emphasis;          // draw in the accent colour (active transfer)
    bool warning;           // draw as a problem (failed / held)
} DlUiRow;

void dl_ui_row(const DlStatus *st, const DlUiContext *cx, DlUiRow *out);

// Primary (Cross) and secondary (Square) actions on a row, and the hint
// labels for them ("" = no action, hide the hint).
DlUiAction dl_ui_row_primary(const DlStatus *st);
DlUiAction dl_ui_row_secondary(const DlStatus *st);
const char *dl_ui_action_label(DlUiAction a);
// Whether a secondary action should ask first: removing a finished film
// deletes gigabytes that took an hour to fetch.
bool dl_ui_action_needs_confirm(DlUiAction a, const DlStatus *st);

// A banner above the list when the queue as a whole is held ("Paused while
// streaming", "Sign in to continue downloads"), or "".
const char *dl_ui_queue_banner(const DlUiContext *cx);
// The same, plus the two space holds, which carry numbers: "Paused: the HDD
// is down to 10 GB free".  `space` is dl_space_report(0) and held is
// dl_space_held().
void dl_ui_banner(const DlUiContext *cx, const DlSpaceReport *space, bool held,
                  char *out, int cap);
// The Downloads screen's one-line header, e.g. "HDD: 42.0 GB free, downloads
// keep 10 GB free" with a middle dot between the two; it says the free space
// is unknown when it cannot be read.
void dl_ui_hdd_header(const DlSpaceReport *space, char *out, int cap);
// The same for the store wherever it is: `drive` is the USB drive's label, or "" / NULL for the HDD.
void dl_ui_store_header(const DlSpaceReport *space, const char *drive, char *out, int cap);

// ---- the Offline library --------------------------------------------------

// "Pilot" / "Some Show  S1 E3" and "47 min  1.2 GB" style lines for a
// library entry; works from a stale (meta_ok=false) entry too.
void dl_ui_offline_lines(const DlMeta *m, bool meta_ok, uint64_t bytes,
                         char *title, int tcap, char *sub, int scap);

// ---- helpers ----------------------------------------------------------------

// "812 KB", "340 MB", "1.2 GB", "18 GB".
void dl_ui_format_bytes(uint64_t bytes, char *out, int cap);
// Keep the selection and scroll window valid after the list changed size.
// Returns the clamped selection; *top is the first visible row.
int  dl_ui_clamp_selection(int sel, int count, int visible, int *top);
