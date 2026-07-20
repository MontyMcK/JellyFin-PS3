// XMB main screen — per-frame loop: input dispatch, CPU draw phase,
// RSX/TTF draw phase, contextual hints.

#include <stdio.h>
#include <string.h>

#include <rsx/rsx.h>
#include <sysutil/sysutil.h>

#include "ui_internal.h"
#include "ui_wave.h"
#include "thumbnail_cache.h"
#include "slog.h"

extern void crash_log(const char *msg);

static void xmb_reset_state(void) {
    memset(g_items, 0, sizeof(g_items));
    memset(g_item_count, 0, sizeof(g_item_count));
    memset(g_items_loaded, 0, sizeof(g_items_loaded));
    memset(g_tab_start, 0, sizeof(g_tab_start));
    memset(g_tab_total, 0, sizeof(g_tab_total));
    memset(g_tab_name_filter, 0, sizeof(g_tab_name_filter));
    g_jumpbar_active = false;
    g_jumpbar_sel    = 1;
    g_settings_sel     = 0;
    g_settings_confirm = false;
    memset(g_search_buf, 0, sizeof(g_search_buf));
    g_search_results_count = 0;
    g_active_tab = XMB_TAB_HOME;
    g_sel = 0; g_scroll_top = 0;
    g_tv_sub_start = 0; g_tv_sub_total = 0;
    g_col_sub_start = 0; g_col_sub_total = 0;
    g_music_subtab = MUSIC_ST_ALBUMS;
    g_music_header = false;
    g_music_depth  = 0;
    g_music_sub_count = 0; g_music_sub_sel = 0;
    g_music_sub_scroll = 0; g_music_sub_total = 0;
    g_osk_row = 0; g_osk_col = 0; g_osk_sym = false;
}

// Resolve the card-grid view for the current tab: grid geometry, which item
// array, count, selection, scroll, grid origin, and whether more rows exist
// below.  Returns false for tabs that don't use the grid
// (search/settings).
static bool xmb_grid_view(int tab, GridGeom *gg, const XMBItem **items,
                          int *count, int *sel, int *scroll, int *y0,
                          bool *more_below, int *abs_start, int *abs_total) {
    if (tab == XMB_TAB_SEARCH || tab == XMB_TAB_SETTINGS)
        return false;
    xmb_grid_geom(tab, gg);
    if (tab == XMB_TAB_TV && g_tv_depth > 0) {
        *items = g_tv_sub_items;  *count = g_tv_sub_count;
        *sel   = g_tv_sub_sel;    *scroll = g_tv_sub_scroll;
        *y0    = XMB_GRID_Y0 + 26;
        *more_below = g_tv_sub_scroll + gg->vis < g_tv_sub_count ||
                      g_tv_sub_start + g_tv_sub_count < g_tv_sub_total;
        *abs_start = g_tv_sub_start;  *abs_total = g_tv_sub_total;
        return true;
    }
    if (tab == XMB_TAB_COLLECTIONS && g_col_depth > 0) {
        *items = g_col_sub_items; *count = g_col_sub_count;
        *sel   = g_col_sub_sel;   *scroll = g_col_sub_scroll;
        *y0    = XMB_GRID_Y0 + 26;
        *more_below = g_col_sub_scroll + gg->vis < g_col_sub_count ||
                      g_col_sub_start + g_col_sub_count < g_col_sub_total;
        *abs_start = g_col_sub_start; *abs_total = g_col_sub_total;
        return true;
    }
    if (tab == XMB_TAB_MUSIC && g_music_depth > 0) {
        *items = g_music_sub_items; *count = g_music_sub_count;
        *sel   = g_music_sub_sel;   *scroll = g_music_sub_scroll;
        *y0    = XMB_GRID_Y0 + 26;
        *more_below = g_music_sub_scroll + gg->vis < g_music_sub_count;
        *abs_start = 0; *abs_total = g_music_sub_count;  // single page
        return true;
    }
    *items = g_items[tab];  *count = g_item_count[tab];
    *sel   = g_sel;         *scroll = g_scroll_top;
    *y0    = XMB_GRID_Y0
           + (tab == XMB_TAB_MUSIC ? XMB_MUSIC_SUBTAB_H : 0);
    *more_below = g_scroll_top + gg->vis < g_item_count[tab] ||
                  g_tab_start[tab] + g_item_count[tab] < g_tab_total[tab];
    *abs_start = g_tab_start[tab]; *abs_total = g_tab_total[tab];
    return true;
}

#if BUILD_FOR_RPCS3
// STATE breadcrumb for menu navigation.  Called once per frame from the XMB
// loop, but only emits when the *effective* (tab, depth, selection, count)
// actually changes — so the host log-assert harness sees exactly one MENU
// line per navigation step rather than one per frame.  Reuses xmb_grid_view()
// (above) to resolve sel/total for whichever grid/sub-screen is active.
static void slog_menu_tick(void) {
    int         tab   = g_active_tab;
    const char *ctx   = g_tabs[tab].label;
    int         sel   = -1, total = 0, depth = 0;

    if (tab == XMB_TAB_SETTINGS) {
        sel = g_settings_sel; total = XMB_SETTINGS_COUNT;
    } else if (tab == XMB_TAB_SEARCH) {
        sel = g_search_focus_results ? 1 : 0; total = g_search_results_count;
    } else {
        GridGeom gg; const XMBItem *items;
        int count, s, scroll, y0, a_start, a_total; bool more;
        if (xmb_grid_view(tab, &gg, &items, &count, &s, &scroll, &y0,
                          &more, &a_start, &a_total)) {
            sel   = s;
            total = (a_total > count) ? a_total : count;   // paged vs. loaded
            depth = (tab == XMB_TAB_TV)          ? g_tv_depth
                  : (tab == XMB_TAB_COLLECTIONS) ? g_col_depth
                  : (tab == XMB_TAB_MUSIC)       ? g_music_depth : 0;
        }
        // else: Home — no grid; just track the tab (sel stays 0).
        else { sel = 0; total = 0; }
    }

    bool header = (tab == XMB_TAB_MUSIC && g_music_header);

    static int  s_tab = -1, s_sel = -2, s_total = -1, s_depth = -1;
    static bool s_header = false;
    if (tab != s_tab || sel != s_sel || total != s_total ||
        depth != s_depth || header != s_header) {
        s_tab = tab; s_sel = sel; s_total = total; s_depth = depth;
        s_header = header;
        slog_state("MENU tab=%s depth=%d sel=%d total=%d%s",
                   ctx, depth, sel, total, header ? " header=1" : "");
    }
}
#else
static inline void slog_menu_tick(void) {}
#endif

// CPU draw phase — direct framebuffer writes, runs after rsxSync().
static void xmb_draw_cpu_phase(int tab) {
    xmb_draw_divider();

    if (tab == XMB_TAB_SEARCH) {
        xmb_cpu_draw_osk();
        xmb_cpu_draw_search_results();
    } else if (tab == XMB_TAB_SETTINGS) {
        xmb_cpu_draw_settings();
    } else if (tab == XMB_TAB_HOME) {
        xmb_home_cpu_phase();
    } else {
        GridGeom gg; const XMBItem *items;
        int count, sel, scroll, y0, a_start, a_total; bool more;
        if (xmb_grid_view(tab, &gg, &items, &count, &sel, &scroll, &y0,
                          &more, &a_start, &a_total))
            xmb_grid_cpu(&gg, items, count, sel, scroll, y0);
    }
}

// TTF/RSX draw phase — text and icons on top of the CPU-drawn layer.
static void xmb_draw_text_phase(int tab) {
    xmb_draw_topbar();

    if (tab == XMB_TAB_SEARCH) {
        xmb_rsx_draw_osk();
    } else if (tab == XMB_TAB_SETTINGS) {
        xmb_draw_settings();
    } else if (tab == XMB_TAB_HOME) {
        xmb_home_text_phase();
    } else {
        // Card-grid tabs: breadcrumb (sub-screens), empty text, grid labels.
        if (tab == XMB_TAB_MUSIC) {
            GridGeom mg;
            xmb_grid_geom(XMB_TAB_MUSIC, &mg);
            if (g_music_depth == 0)
                xmb_draw_music_subtabs(mg.x0, XMB_CONTENT_Y + 2,
                                       g_music_subtab, g_music_header);
            else
                xmb_draw_breadcrumb(XMB_ITEM_PAD, XMB_CONTENT_Y + 2,
                                    g_music_parent_name, "Albums", NULL);
        }
        if (tab == XMB_TAB_TV && g_tv_depth > 0) {
            if (g_tv_depth == 1)
                xmb_draw_breadcrumb(XMB_ITEM_PAD, XMB_CONTENT_Y + 2,
                                    g_tv_series_name, "Seasons", NULL);
            else
                xmb_draw_breadcrumb(XMB_ITEM_PAD, XMB_CONTENT_Y + 2,
                                    g_tv_series_name, g_tv_season_name,
                                    "Episodes");
        } else if (tab == XMB_TAB_COLLECTIONS && g_col_depth > 0) {
            xmb_draw_breadcrumb(XMB_ITEM_PAD, XMB_CONTENT_Y + 2,
                                g_col_name, "Movies", NULL);
        }

        GridGeom gg; const XMBItem *items;
        int count, sel, scroll, y0, a_start, a_total; bool more;
        if (xmb_grid_view(tab, &gg, &items, &count, &sel, &scroll, &y0,
                          &more, &a_start, &a_total)) {
            bool sub = (tab == XMB_TAB_TV && g_tv_depth > 0) ||
                       (tab == XMB_TAB_COLLECTIONS && g_col_depth > 0) ||
                       (tab == XMB_TAB_MUSIC && g_music_depth > 0);
            if (count == 0) {
                bool loaded = sub || g_items_loaded[tab];
                if (loaded)
                    xmb_draw_empty_state(tab,
                            tab == XMB_TAB_RESUME ? "Nothing in progress"
                                                  : "No items in this library");
            } else {
                xmb_grid_text(&gg, items, count, sel, scroll, y0, more,
                              a_start, a_total);
            }
            // Letter jump bar on library tabs at depth 0 (not the resume
            // list — it isn't alphabetical).
            if (!sub && tab != XMB_TAB_RESUME)
                xmb_draw_jumpbar(tab);
        }
    }
}

// Contextual hints bar for the current tab / mode.
static void xmb_draw_hints(int tab) {
    bool in_tv_sub  = (tab == XMB_TAB_TV          && g_tv_depth  > 0);
    bool in_col_sub = (tab == XMB_TAB_COLLECTIONS && g_col_depth > 0);

    if (tab == XMB_TAB_SETTINGS) {
        if (g_settings_confirm) {
            static const Hint h[] = {{'X',"Confirm"},{'C',"Cancel"}};
            draw_hints_bar(h, 2);
        } else {
            static const Hint h[] = {{'X',"Select"}};
            draw_hints_bar(h, 1);
        }
    } else if (tab == XMB_TAB_SEARCH) {
        if (g_search_focus_results) {
            static const Hint h[] = {{'X',"Play"},{'C',"Back"}};
            draw_hints_bar(h, 2);
        } else {
            static const Hint h[] = {{'X',"Type"},{'C',"Clear"}};
            draw_hints_bar(h, 2);
        }
    } else if (tab == XMB_TAB_MUSIC && g_music_header) {
        static const Hint h[] = {{'D',"Switch"},{'X',"Select"}};
        draw_hints_bar(h, 2);
    } else if (in_tv_sub || in_col_sub ||
               (tab == XMB_TAB_MUSIC && g_music_depth > 0)) {
        static const Hint h[] = {{'X',"Select"},{'C',"Back"}};
        draw_hints_bar(h, 2);
    } else if (g_jumpbar_active) {
        static const Hint h[] = {{'X',"Jump"},{'C',"Cancel"}};
        draw_hints_bar(h, 2);
    } else if (tab == XMB_TAB_HOME) {
        static const Hint h[] = {{'X',"Open"},{'T',"Info"}};
        draw_hints_bar(h, 2);
    } else {
        static const Hint h[] = {{'E',"Nav"},{'X',"Select"},{'T',"Info"}};
        draw_hints_bar(h, 3);
    }
}

void ui_run_xmb(void) {
    crash_log("13.1 reset_state");
    xmb_reset_state();
    wave_reset();

    crash_log("13.2 detect_tabs");
    xmb_detect_tabs();
    crash_log("13.3 detect_tabs done");

    if (!g_tabs[g_active_tab].enabled) {
        for (int t = 0; t < XMB_TAB_COUNT; t++) {
            if (g_tabs[t].enabled) { g_active_tab = t; break; }
        }
    }

    OSK_Y0 = XMB_CONTENT_Y + UIS_H(80);

    crash_log("13.4 init_btns");
    init_btns();
    crash_log("13.5 loop enter");

    // Breadcrumbs inside the loop fire only on the first pass so the log
    // doesn't grow unbounded once the UI is actually running.
    bool first_iter = true;
    while (running) {
        if (first_iter) crash_log("13.5a waitflip");
        waitflip();
        if (first_iter) crash_log("13.5b syscb");
        sysUtilCheckCallback();
        thumb_cache_tick();   // age out thumbs nothing on screen still wants
        if (first_iter) crash_log("13.5c clearScreen");
        clearScreen(XMB_BG);
        if (first_iter) crash_log("13.5d wave_draw");
        wave_draw();
        if (first_iter) crash_log("13.6 wave_draw done");

        int tab = g_active_tab;
        if (tab != XMB_TAB_SEARCH && tab != XMB_TAB_SETTINGS
            && tab != XMB_TAB_HOME)
            if (!g_items_loaded[tab]) {
                if (first_iter) crash_log("13.7 fetch_tab_items");
                xmb_fetch_tab_items(tab);
                if (first_iter) crash_log("13.7b fetch done");
            }

        poll_buttons();
        bool should_exit = false;
        if (xmb_update_popup_active())
            xmb_update_popup_input();   // modal: the screen below keeps focus state
        else if (tab == XMB_TAB_SEARCH)
            should_exit = xmb_handle_input_search();
        else if (tab == XMB_TAB_HOME)
            should_exit = xmb_handle_input_home();
        else
            should_exit = xmb_handle_input_browse();
        if (should_exit) break;

        slog_menu_tick();   // STATE: MENU ... (emulator-only, emits on change)

        rsxSync();

        if (first_iter) crash_log("13.8 cpu_phase");
        xmb_draw_cpu_phase(tab);
        if (first_iter) crash_log("13.8b text_phase");
        xmb_draw_text_phase(tab);
        if (first_iter) crash_log("13.8c hints");
        bool popup = xmb_update_popup_active();
        if (!popup) xmb_draw_hints(tab);   // the popup swaps in its own hint
        if (first_iter) crash_log("13.8d tabs");
        xmb_draw_tabs();
        if (popup) xmb_update_popup_draw();

        if (first_iter) crash_log("13.9 first flip");
        flip();
        if (first_iter) { crash_log("13.10 first frame done"); first_iter = false; }
        sysUtilCheckCallback();
    }
}
