// XMB navigation — tab switching and the browse-mode input handlers
// (settings, TV sub-screens, collections sub-screens, jump bar, item lists).

#include <stdio.h>
#include <string.h>
#include <ctype.h>

#include "ui_internal.h"
#include "jellyfin_api.h"
#include "player.h"
#include "plog.h"
#include "timing.h"

// -------------------------------------------------------
// Tab switching
// -------------------------------------------------------

void xmb_switch_tab(int new_tab) {
    if (new_tab < 0 || new_tab >= XMB_TAB_COUNT) return;
    if (!g_tabs[new_tab].enabled) return;
    int old = g_active_tab;
    if (old != XMB_TAB_SEARCH && old != XMB_TAB_MUSIC && old != XMB_TAB_SETTINGS
        && (g_tab_start[old] > 0 || g_tab_name_filter[old][0])) {
        g_items_loaded[old]       = false;
        g_item_count[old]         = 0;
        g_tab_start[old]          = 0;
        g_tab_total[old]          = 0;
        g_tab_name_filter[old][0] = '\0';
    }
    g_jumpbar_active = false;
    g_active_tab = new_tab;
    g_sel = 0;
    g_scroll_top = 0;
    g_tv_depth = 0; g_tv_sub_sel = 0; g_tv_sub_scroll = 0; g_tv_sub_start = 0; g_tv_sub_total = 0;
    g_col_depth = 0; g_col_sub_sel = 0; g_col_sub_scroll = 0; g_col_sub_start = 0; g_col_sub_total = 0;
    if (new_tab == XMB_TAB_SEARCH) {
        g_osk_row = 0; g_osk_col = 0; g_osk_sym = false;
    }
    if (new_tab == XMB_TAB_SETTINGS) {
        g_settings_sel = 0; g_settings_confirm = false;
    }
    // Home: reset focus and refetch the dynamic rows (Continue Watching /
    // Next Up change after every playback).
    if (new_tab == XMB_TAB_HOME)
        xmb_home_on_enter();
}

int xmb_next_enabled(int start, int dir) {
    int t = start + dir;
    while (t >= 0 && t < XMB_TAB_COUNT) {
        if (g_tabs[t].enabled) return t;
        t += dir;
    }
    return start;
}

// Launch the player for one list item, mapping XMBItem -> JFItem.
// resume_secs > 0 starts playback at that position (Continue Watching).
void xmb_play_item(const XMBItem *it, u32 resume_secs) {
    JFItem jf; memset(&jf, 0, sizeof(jf));
    strncpy(jf.id,   it->id,   sizeof(jf.id)-1);
    strncpy(jf.name, it->name, sizeof(jf.name)-1);
    strncpy(jf.type, it->type, sizeof(jf.type)-1);
    show_player(&jf, resume_secs);
}

// Play items[idx] and keep advancing through the list while the user
// accepts the player's end-of-item NEXT prompt (SELECT during the last
// 30 s).  Used for episodes in a season and movies in a collection.
static void xmb_play_list_with_next(const XMBItem *items, int count, int idx,
                                    const char *label, const char *hint) {
    for (;;) {
        if (idx + 1 < count) player_arm_next(label, hint);
        xmb_play_item(&items[idx], 0);
        if (idx + 1 >= count || !player_take_next_request()) break;
        idx++;
    }
}

// -------------------------------------------------------
// Per-screen input handlers.  Each returns true when the XMB
// loop should exit (only the settings Log Out path does).
// -------------------------------------------------------

// Settings tab — account actions (Log Out).
static bool xmb_input_settings(void) {
    if (g_settings_confirm) {
        // Modal confirm: swallow all other input until resolved.
        if (BTN_PRESSED(cross)) {
            jellyfin_logout();
            return true;   // exit the XMB; main loop returns to the login screen
        }
        if (BTN_PRESSED(circle)) g_settings_confirm = false;
        return false;
    }
    if (BTN_PRESSED(l1)) { xmb_switch_tab(xmb_next_enabled(g_active_tab, -1)); return false; }
    if (BTN_PRESSED(r1)) { xmb_switch_tab(xmb_next_enabled(g_active_tab, +1)); return false; }
    if (BTN_REPEAT(up)   && g_settings_sel > 0)                      g_settings_sel--;
    if (BTN_REPEAT(down) && g_settings_sel < XMB_SETTINGS_COUNT - 1) g_settings_sel++;
    if (BTN_PRESSED(cross) && g_settings_sel == 0) g_settings_confirm = true;  // Log Out
    return false;
}

// TV sub-screen (Series -> Seasons -> Episodes) — card grid.
static void xmb_input_tv_sub(void) {
    GridGeom gg;
    xmb_grid_geom(XMB_TAB_TV, &gg);
    const int C   = gg.cols;
    const int VIS = gg.vis;
    if (BTN_PRESSED(circle)) {
        g_tv_depth--;
        g_tv_sub_sel = 0; g_tv_sub_scroll = 0; g_tv_sub_start = 0; g_tv_sub_total = 0;
        return;
    }
    if (BTN_REPEAT(up)) {
        if (g_tv_sub_sel >= C) {
            g_tv_sub_sel -= C;
            if (g_tv_sub_sel < g_tv_sub_scroll)
                g_tv_sub_scroll = (g_tv_sub_sel / C) * C;
        }
    }
    if (BTN_REPEAT(down)) {
        if (g_tv_sub_sel + C < g_tv_sub_count) {
            g_tv_sub_sel += C;
            if (g_tv_sub_sel >= g_tv_sub_scroll + VIS)
                g_tv_sub_scroll += C;
        } else if (g_tv_sub_sel / C < (g_tv_sub_count - 1) / C) {
            g_tv_sub_sel = g_tv_sub_count - 1;
            if (g_tv_sub_sel >= g_tv_sub_scroll + VIS)
                g_tv_sub_scroll += C;
        } else if (g_tv_sub_start + g_tv_sub_count < g_tv_sub_total) {
            int first = xmb_slide_tv_sub_forward();
            if (first >= 0) {
                g_tv_sub_sel    = first;
                g_tv_sub_scroll = (first / C) * C;
            }
        }
    }
    if (BTN_REPEAT(right)) {
        if ((g_tv_sub_sel % C) < C - 1 && g_tv_sub_sel + 1 < g_tv_sub_count)
            g_tv_sub_sel++;
    }
    if (BTN_REPEAT(left)) {
        if ((g_tv_sub_sel % C) > 0) g_tv_sub_sel--;
    }
    if (BTN_PRESSED(cross) && g_tv_sub_count > 0 && g_tv_sub_sel < g_tv_sub_count) {
        const XMBItem *it = &g_tv_sub_items[g_tv_sub_sel];
        if (g_tv_depth == 1) {
            strncpy(g_tv_season_id,   it->id,   sizeof(g_tv_season_id)-1);
            strncpy(g_tv_season_name, it->name, sizeof(g_tv_season_name)-1);
            g_tv_sub_start = 0; g_tv_sub_total = 0;
            g_tv_sub_count = xmb_fetch_episodes(g_tv_series_id, g_tv_season_id,
                                                 g_tv_sub_items, XMB_ITEMS_MAX,
                                                 0, &g_tv_sub_total);
            g_tv_depth = 2; g_tv_sub_sel = 0; g_tv_sub_scroll = 0;
        } else {
            xmb_play_list_with_next(g_tv_sub_items, g_tv_sub_count,
                                    g_tv_sub_sel, "NEXT EPISODE",
                                    "Press SELECT for next episode");
            g_tv_depth = 0;
            g_tv_sub_sel = 0;
            g_tv_sub_scroll = 0;
        }
    }
}

// Collections sub-screen (Collection -> Movies) — card grid.
static void xmb_input_col_sub(void) {
    GridGeom gg;
    xmb_grid_geom(XMB_TAB_COLLECTIONS, &gg);
    const int C   = gg.cols;
    const int VIS = gg.vis;
    if (BTN_PRESSED(circle)) {
        g_col_depth = 0;
        g_col_sub_sel = 0; g_col_sub_scroll = 0; g_col_sub_start = 0; g_col_sub_total = 0;
        return;
    }
    if (BTN_REPEAT(up)) {
        if (g_col_sub_sel >= C) {
            g_col_sub_sel -= C;
            if (g_col_sub_sel < g_col_sub_scroll)
                g_col_sub_scroll = (g_col_sub_sel / C) * C;
        }
    }
    if (BTN_REPEAT(down)) {
        if (g_col_sub_sel + C < g_col_sub_count) {
            g_col_sub_sel += C;
            if (g_col_sub_sel >= g_col_sub_scroll + VIS)
                g_col_sub_scroll += C;
        } else if (g_col_sub_sel / C < (g_col_sub_count - 1) / C) {
            g_col_sub_sel = g_col_sub_count - 1;
            if (g_col_sub_sel >= g_col_sub_scroll + VIS)
                g_col_sub_scroll += C;
        } else if (g_col_sub_start + g_col_sub_count < g_col_sub_total) {
            int first = xmb_slide_col_sub_forward();
            if (first >= 0) {
                g_col_sub_sel    = first;
                g_col_sub_scroll = (first / C) * C;
            }
        }
    }
    if (BTN_REPEAT(right)) {
        if ((g_col_sub_sel % C) < C - 1 && g_col_sub_sel + 1 < g_col_sub_count)
            g_col_sub_sel++;
    }
    if (BTN_REPEAT(left)) {
        if ((g_col_sub_sel % C) > 0) g_col_sub_sel--;
    }
    if (BTN_PRESSED(cross) && g_col_sub_count > 0 && g_col_sub_sel < g_col_sub_count) {
        xmb_play_list_with_next(g_col_sub_items, g_col_sub_count,
                                g_col_sub_sel, "NEXT MOVIE",
                                "Press SELECT for next movie");
        g_col_depth = 0;
        g_col_sub_sel = 0;
        g_col_sub_scroll = 0;
    }
}

// Jump bar — alphabetical letter filter.
static void xmb_input_jumpbar(int tab) {
    if (BTN_REPEAT(up))
        g_jumpbar_sel = (g_jumpbar_sel - 1 + JBAR_ENTRIES) % JBAR_ENTRIES;
    if (BTN_REPEAT(down))
        g_jumpbar_sel = (g_jumpbar_sel + 1) % JBAR_ENTRIES;
    if (BTN_PRESSED(right) || BTN_PRESSED(circle))
        g_jumpbar_active = false;
    if (BTN_PRESSED(cross)) {
        if (g_jumpbar_sel == 0) {
            g_tab_name_filter[tab][0] = '#';
            g_tab_name_filter[tab][1] = '\0';
        } else {
            g_tab_name_filter[tab][0] = (char)('A' + g_jumpbar_sel - 1);
            g_tab_name_filter[tab][1] = '\0';
        }
        g_items_loaded[tab] = false;
        g_item_count[tab]   = 0;
        g_tab_start[tab]    = 0;
        g_tab_total[tab]    = 0;
        g_sel               = 0;
        g_scroll_top        = 0;
        g_jumpbar_active    = false;
    }
}

// -------------------------------------------------------
// Browse input dispatcher
// -------------------------------------------------------

bool xmb_handle_input_browse(void) {
    static bool s_movie_just_exited = false;
    int tab = g_active_tab;

    if (tab == XMB_TAB_SETTINGS) return xmb_input_settings();

    if (BTN_PRESSED(l1)) { xmb_switch_tab(xmb_next_enabled(g_active_tab, -1)); return false; }
    if (BTN_PRESSED(r1)) { xmb_switch_tab(xmb_next_enabled(g_active_tab, +1)); return false; }

    if (tab == XMB_TAB_TV && g_tv_depth > 0)           { xmb_input_tv_sub();  return false; }
    if (tab == XMB_TAB_COLLECTIONS && g_col_depth > 0) { xmb_input_col_sub(); return false; }

    bool jbar_mode = g_jumpbar_active &&
        (tab == XMB_TAB_MOVIES || tab == XMB_TAB_TV ||
         tab == XMB_TAB_MUSIC  || tab == XMB_TAB_COLLECTIONS);
    if (jbar_mode) { xmb_input_jumpbar(tab); return false; }

    // Normal browse — 3-column card grid.
    if (BTN_PRESSED(circle)) return false;

    int count = g_item_count[tab];
    GridGeom gg;
    xmb_grid_geom(tab, &gg);
    const int C   = gg.cols;
    const int VIS = gg.vis;

    if (BTN_REPEAT(up)) {
        if (g_sel >= C) {
            g_sel -= C;
            if (g_sel < g_scroll_top) g_scroll_top = (g_sel / C) * C;
        }
    }
    if (BTN_REPEAT(down)) {
        if (g_sel + C < count) {
            g_sel += C;
            if (g_sel >= g_scroll_top + VIS) g_scroll_top += C;
        } else if (g_sel / C < (count - 1) / C) {
            g_sel = count - 1;          // partial last row
            if (g_sel >= g_scroll_top + VIS) g_scroll_top += C;
        } else if (g_tab_start[tab] + count < g_tab_total[tab]) {
            // Infinite scroll: slide the window forward one page.
            int first = xmb_slide_tab_forward(tab);
            if (first >= 0) {
                g_sel        = first;
                g_scroll_top = (first / C) * C;
            }
        }
    }
    if (BTN_REPEAT(right)) {
        if ((g_sel % C) < C - 1 && g_sel + 1 < count) g_sel++;
    }
    if (BTN_REPEAT(left) && (g_sel % C) > 0) {
        g_sel--;
    } else if (BTN_PRESSED(left) && (g_sel % C) == 0 &&
               (tab == XMB_TAB_MOVIES || tab == XMB_TAB_TV ||
                tab == XMB_TAB_MUSIC  || tab == XMB_TAB_COLLECTIONS)) {
        // Left at the first column opens the letter jump bar.
        const char *filt = g_tab_name_filter[tab];
        if (filt[0] == '#') {
            g_jumpbar_sel = 0;
        } else if (filt[0] >= 'A' && filt[0] <= 'Z') {
            g_jumpbar_sel = 1 + (filt[0] - 'A');
        } else if (count > 0) {
            int idx = (g_sel < count) ? g_sel : 0;
            char c = (char)toupper((unsigned char)g_items[tab][idx].name[0]);
            g_jumpbar_sel = (c >= 'A' && c <= 'Z') ? 1 + (c - 'A') : 0;
        } else {
            g_jumpbar_sel = 1;
        }
        g_jumpbar_active = true;
        return false;
    }

    if (s_movie_just_exited) { s_movie_just_exited = false; return false; }

    if (BTN_PRESSED(cross) && count > 0 && g_sel < count) {
        const XMBItem *it = &g_items[tab][g_sel];
        if (tab == XMB_TAB_TV && strcmp(it->type, "Series") == 0) {
            strncpy(g_tv_series_id,   it->id,   sizeof(g_tv_series_id)-1);
            strncpy(g_tv_series_name, it->name, sizeof(g_tv_series_name)-1);
            g_tv_sub_start = 0; g_tv_sub_total = 0;
            g_tv_sub_count = xmb_fetch_seasons(g_tv_series_id, g_tv_sub_items, XMB_ITEMS_MAX,
                                                0, &g_tv_sub_total);
            g_tv_depth = 1; g_tv_sub_sel = 0; g_tv_sub_scroll = 0;
        } else if (tab == XMB_TAB_COLLECTIONS) {
            strncpy(g_col_id,   it->id,   sizeof(g_col_id)-1);
            strncpy(g_col_name, it->name, sizeof(g_col_name)-1);
            g_col_sub_start = 0; g_col_sub_total = 0;
            g_col_sub_count = xmb_fetch_collection_items(g_col_id, g_col_sub_items, XMB_ITEMS_MAX,
                                                          0, &g_col_sub_total);
            g_col_depth = 1; g_col_sub_sel = 0; g_col_sub_scroll = 0;
        } else {
            // Continue Watching launches at the saved position.
            xmb_play_item(it, tab == XMB_TAB_RESUME ? it->resume_secs : 0);
            s_movie_just_exited = true;
            init_btns();
            return false;
        }
    }

    // Triangle — detail overlay
    if (btn_cur.triangle || btn_prev.triangle) {
        char dbg[200];
        snprintf(dbg, sizeof(dbg),
            "outer: triangle state cur=%d prev=%d (BTN_PRESSED would be %d)",
            btn_cur.triangle, btn_prev.triangle,
            (btn_cur.triangle && !btn_prev.triangle) ? 1 : 0);
        plog(dbg);
    }
    u64 now_us = timing_get_us();
    if (BTN_PRESSED(triangle) && count > 0 && g_sel < count
        && now_us >= g_info_cooldown_until) {
        xmb_show_item_info(&g_items[tab][g_sel]);
    }
    return false;
}
