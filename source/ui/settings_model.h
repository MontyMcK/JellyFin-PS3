#pragma once

// -------------------------------------------------------------------------
//  Settings screen model
// -------------------------------------------------------------------------
//  What the Settings screen lists, in which section and in what order, plus
//  the layout and scrolling rules.  Pure C with no PS3 or RSX includes, so
//  tests/test_settings_model.c compiles this exact file.  Nothing here knows
//  how a row is drawn or what activating it does: ui/render/ui_settings.cpp
//  keeps a parallel table keyed by setting_id for that.
//
//  The row INDEX is only a position in the displayed order.  Nothing may
//  hard-code one: look the row up by its setting_id.

#ifdef __cplusplus
extern "C" {
#endif

// Stable ids.  A row's id never changes when rows are added or reordered.
typedef enum {
    SET_LOGOUT, SET_DEBUG_LOG, SET_SCREEN_SIZE, SET_HD1080, SET_AUDIO_OUT,
    SET_DIALOGUE, SET_SUB_FONT, SET_SUB_COLOUR, SET_THEME, SET_PARTICLES,
    SET_DAYNIGHT, SET_WAVE_INT, SET_AUTOSKIP, SET_24HZ, SET_UPDATE,
    SET_SEND_LOG, SET_STATS, SET_DOWNLOADS, SET_OFFLINE_LIB, SET_LANGUAGE,
    SET_DIRECT_STREAM, SET_DL_LOCATION, SET_WOL_MAC, SET_WAKE, SET__COUNT
} setting_id;

typedef enum {
    SEC_PLAYBACK, SEC_AUDIO, SEC_SUBTITLES, SEC_DOWNLOADS, SEC_DISPLAY,
    SEC_SYSTEM, SEC__COUNT
} setting_section;

typedef struct {
    setting_id      id;
    setting_section section;
    const char     *label;
    const char     *help;     // exactly two lines, '\n'-separated
} setting_row;

// Visible rows, in display order.  SET_STATS is absent when the player
// statistics module is compiled out.
int                settings_count(void);
const setting_row *settings_row(int i);                  // i in [0, count)
int                settings_index_of(setting_id id);     // -1 if absent
const char        *settings_section_label(setting_section s);
int                settings_section_first(setting_section s);   // row index, -1 if empty

// ---- categories ---------------------------------------------------------------
//  The screen has two levels, like the PS3's own Settings column: the list of
//  categories (the sections), and inside one of them its rows.  A category
//  with no visible row is not offered.

int             settings_category_count(void);
setting_section settings_category_at(int i);             // the i-th, in order; SEC__COUNT if out of range
int             settings_category_index(setting_section s);   // its position in that list, -1 if absent
int             settings_category_row_count(setting_section s);
int             settings_category_row(setting_section s, int k);   // row index of its k-th row, -1
int             settings_row_in_category(int row);       // the row's position inside its category, -1

// The scroll offset (pixels) that shows item `sel` completely inside a band
// `band_h` tall, moving as little as possible from `cur`.  The `n` items are
// `item_h` tall with `gap` between them.  0 when everything fits.
int             settings_scroll_to(int n, int sel, int item_h, int gap, int band_h, int cur);

#ifdef __cplusplus
}
#endif
