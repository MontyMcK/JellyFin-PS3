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
    SET_DIRECT_STREAM, SET_DL_LOCATION, SET__COUNT
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
// The first row of the section before / after the one row `i` is in, or -1
// at the first / last section.  L2 and R2 use these.
int                settings_section_jump(int i, int dir);

// ---- layout ---------------------------------------------------------------
//  One pass builds every section header and row in content coordinates (y = 0
//  at the top of the first header).  The scroll offset, in pixels, then keeps
//  the selected row fully inside the visible band.

typedef struct {
    int is_header;   // 1: a section header (`index` is the setting_section)
    int index;       // row index when !is_header
    int y, h;        // content coordinates
} settings_item;

#define SETTINGS_MAX_ITEMS (SET__COUNT + SEC__COUNT)

// Returns the item count.  Sections with no visible row get no header.
// `*total_h` (optional) is the height of the whole content.
int settings_layout(settings_item *out, int max, int header_h, int row_h,
                    int row_pitch, int *total_h);

// The item index of row `row`, or -1.
int settings_item_of_row(const settings_item *items, int n, int row);

// The scroll offset that shows row `sel` completely inside a band `band_h`
// tall, moving as little as possible from `cur`.  When `sel` is the first row
// of its section its header is kept visible too.  Always within
// [0, total_h - band_h] (0 when everything fits).
int settings_scroll_for(const settings_item *items, int n, int sel,
                        int band_h, int cur, int total_h);

#ifdef __cplusplus
}
#endif
