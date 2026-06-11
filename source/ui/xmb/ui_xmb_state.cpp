// XMB tab / item / navigation state — the shared globals declared in
// ui_visuals.h.  All mutation happens in the xmb/ input handlers.

#include "ui_internal.h"

XMBTab g_tabs[XMB_TAB_COUNT] = {
    {"SEARCH",            "?",  "", true },
    {"CONTINUE WATCHING", ">",  "", true },
    {"MOVIES",            "#",  "", false},
    {"TV SHOWS",          "=",  "", false},
    {"MUSIC",             "~",  "", true },
    {"COLLECTIONS",       "+",  "", false},
    {"SETTINGS",          "*",  "", true },
};

XMBItem g_items[XMB_TAB_COUNT][XMB_ITEMS_MAX];
int     g_item_count[XMB_TAB_COUNT];
bool    g_items_loaded[XMB_TAB_COUNT];

// UI navigation state
int  g_active_tab = XMB_TAB_MOVIES;
int  g_sel        = 0;
int  g_scroll_top = 0;

// Triangle info overlay sets this so its closing press doesn't re-trigger.
u64 g_info_cooldown_until = 0;

// TV sub-screen state (Series→Seasons→Episodes)
int  g_tv_depth       = 0;
char g_tv_series_id[64];
char g_tv_series_name[128];
char g_tv_season_id[64];
char g_tv_season_name[64];
XMBItem g_tv_sub_items[XMB_ITEMS_MAX];
int     g_tv_sub_count  = 0;
int     g_tv_sub_sel    = 0;
int     g_tv_sub_scroll = 0;

// Collections sub-screen state (Collection→Movies)
int  g_col_depth      = 0;
char g_col_id[64];
char g_col_name[128];
XMBItem g_col_sub_items[XMB_ITEMS_MAX];
int     g_col_sub_count  = 0;
int     g_col_sub_sel    = 0;
int     g_col_sub_scroll = 0;

// Pagination state — sliding window per main tab
int g_tab_start[XMB_TAB_COUNT];
int g_tab_total[XMB_TAB_COUNT];

// Pagination state for TV and collections sub-lists
int g_tv_sub_start  = 0;
int g_tv_sub_total  = 0;
int g_col_sub_start = 0;
int g_col_sub_total = 0;

// Jump bar state
bool g_jumpbar_active = false;
int  g_jumpbar_sel    = 1;
char g_tab_name_filter[XMB_TAB_COUNT][4];

// Settings tab state
int  g_settings_sel     = 0;
bool g_settings_confirm = false;
