// XMB navigation — tab switching, browse/TV/collections input, pagination, fetch.

#include <stdio.h>
#include <string.h>
#include <ctype.h>

#include "ui.h"
#include "ui_visuals.h"
#include "jellyfin_api.h"
#include "player.h"
#include "plog.h"
#include "timing.h"

// Defined in ui.cpp
int xmb_json_str_range(const char *start, int len, const char *key, char *out, int out_size);
int xmb_json_int_range(const char *start, int len, const char *key, int def);
int parse_xmb_items(const char *json, XMBItem *arr, int max);

// Defined in ui/ui_info.cpp
void xmb_show_item_info(const XMBItem *it);

extern u64 g_info_cooldown_until;

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
}

int xmb_next_enabled(int start, int dir) {
    int t = start + dir;
    while (t >= 0 && t < XMB_TAB_COUNT) {
        if (g_tabs[t].enabled) return t;
        t += dir;
    }
    return start;
}

// -------------------------------------------------------
// API fetch helpers
// -------------------------------------------------------

void xmb_detect_tabs(void) {
    char url[512];
    snprintf(url, sizeof(url), "%s/Users/%s/Views", g_server, g_userid);
    int status = http_request(0, url, NULL, g_token, responseBuffer, RESPONSE_SIZE);
    if (status != 200) return;

    const char *p = strstr(responseBuffer, "\"Items\":[");
    if (!p) return;
    p += 9;

    while (*p) {
        while (*p && *p != '{' && *p != ']') p++;
        if (!*p || *p == ']') break;
        const char *obj = p;
        int depth = 0; bool in_str = false, esc = false;
        while (*p) {
            char c = *p;
            if (esc) { esc = false; }
            else if (in_str) { if (c=='\\') esc=true; else if (c=='"') in_str=false; }
            else { if (c=='"') in_str=true; else if (c=='{') depth++; else if (c=='}') { if (--depth==0){p++;break;} } }
            p++;
        }
        int olen = (int)(p - obj);

        char ct[32] = "", id[64] = "";
        xmb_json_str_range(obj, olen, "CollectionType", ct, sizeof(ct));
        xmb_json_str_range(obj, olen, "Id",             id, sizeof(id));

        if      (strcmp(ct, "movies")   == 0) { g_tabs[XMB_TAB_MOVIES].enabled=true;      strncpy(g_tabs[XMB_TAB_MOVIES].library_id,      id, 63); }
        else if (strcmp(ct, "tvshows")  == 0) { g_tabs[XMB_TAB_TV].enabled=true;           strncpy(g_tabs[XMB_TAB_TV].library_id,          id, 63); }
        else if (strcmp(ct, "music")    == 0) { g_tabs[XMB_TAB_MUSIC].enabled=true;        strncpy(g_tabs[XMB_TAB_MUSIC].library_id,       id, 63); }
        else if (strcmp(ct, "boxsets")  == 0) { g_tabs[XMB_TAB_COLLECTIONS].enabled=true;  strncpy(g_tabs[XMB_TAB_COLLECTIONS].library_id, id, 63); }
    }
}

static void xmb_build_items_url(char *url, int url_size, int tab,
                                  int start_index, int limit) {
    const char *filt = g_tab_name_filter[tab];
    const char *lid  = g_tabs[tab].library_id;
    if (filt[0] == '#') {
        snprintf(url, url_size,
            "%s/Users/%s/Items?ParentId=%s&StartIndex=%d&Limit=%d"
            "&SortBy=SortName&SortOrder=Ascending"
            "&NameLessThan=A"
            "&Fields=Genres,RunTimeTicks,ProductionYear,Container",
            g_server, g_userid, lid, start_index, limit);
    } else if (filt[0]) {
        snprintf(url, url_size,
            "%s/Users/%s/Items?ParentId=%s&StartIndex=%d&Limit=%d"
            "&SortBy=SortName&SortOrder=Ascending"
            "&NameStartsWith=%c"
            "&Fields=Genres,RunTimeTicks,ProductionYear,Container",
            g_server, g_userid, lid, start_index, limit,
            (char)toupper((unsigned char)filt[0]));
    } else {
        snprintf(url, url_size,
            "%s/Users/%s/Items?ParentId=%s&StartIndex=%d&Limit=%d"
            "&SortBy=SortName&SortOrder=Ascending"
            "&Fields=Genres,RunTimeTicks,ProductionYear,Container",
            g_server, g_userid, lid, start_index, limit);
    }
}

void xmb_fetch_tab_items(int tab) {
    if (g_items_loaded[tab]) return;
    g_item_count[tab] = 0;
    g_tab_start[tab]  = 0;
    g_tab_total[tab]  = 0;

    const char *lib_id = g_tabs[tab].library_id;
    if (!lib_id[0]) { g_items_loaded[tab] = true; return; }

    char url[512];
    xmb_build_items_url(url, sizeof(url), tab, 0, XMB_ITEMS_MAX);

    int status = http_request(0, url, NULL, g_token, responseBuffer, RESPONSE_SIZE);
    if (status == 200) {
        g_item_count[tab] = parse_xmb_items(responseBuffer, g_items[tab], XMB_ITEMS_MAX);
        g_tab_total[tab]  = xmb_json_int_range(responseBuffer,
            (int)strlen(responseBuffer), "TotalRecordCount", g_item_count[tab]);
    }
    g_items_loaded[tab] = true;
}

static int xmb_fetch_seasons(const char *series_id, XMBItem *arr, int max,
                               int start_index, int *out_total) {
    char url[512];
    snprintf(url, sizeof(url),
        "%s/Shows/%s/Seasons?userId=%s&StartIndex=%d&Limit=%d"
        "&Fields=ProductionYear,RunTimeTicks",
        g_server, series_id, g_userid, start_index, max);
    int status = http_request(0, url, NULL, g_token, responseBuffer, RESPONSE_SIZE);
    if (status != 200) return 0;
    int n = parse_xmb_items(responseBuffer, arr, max);
    if (out_total)
        *out_total = xmb_json_int_range(responseBuffer,
            (int)strlen(responseBuffer), "TotalRecordCount", n + start_index);
    return n;
}

static int xmb_fetch_episodes(const char *series_id, const char *season_id,
                               XMBItem *arr, int max,
                               int start_index, int *out_total) {
    char url[512];
    snprintf(url, sizeof(url),
        "%s/Shows/%s/Episodes?seasonId=%s&userId=%s"
        "&StartIndex=%d&Limit=%d"
        "&Fields=ProductionYear,RunTimeTicks,Genres,Container",
        g_server, series_id, season_id, g_userid, start_index, max);
    int status = http_request(0, url, NULL, g_token, responseBuffer, RESPONSE_SIZE);
    if (status != 200) return 0;
    int count = parse_xmb_items(responseBuffer, arr, max);
    for (int i = 0; i < count; i++)
        strncpy(arr[i].type, "Episode", sizeof(arr[i].type)-1);
    if (out_total)
        *out_total = xmb_json_int_range(responseBuffer,
            (int)strlen(responseBuffer), "TotalRecordCount", count + start_index);
    return count;
}

static int xmb_fetch_collection_items(const char *collection_id, XMBItem *arr, int max,
                                       int start_index, int *out_total) {
    char url[512];
    snprintf(url, sizeof(url),
        "%s/Users/%s/Items?ParentId=%s"
        "&StartIndex=%d&Limit=%d"
        "&Fields=Genres,RunTimeTicks,ProductionYear,Container"
        "&SortBy=SortName&SortOrder=Ascending",
        g_server, g_userid, collection_id, start_index, max);
    int status = http_request(0, url, NULL, g_token, responseBuffer, RESPONSE_SIZE);
    if (status != 200) return 0;
    int n = parse_xmb_items(responseBuffer, arr, max);
    if (out_total)
        *out_total = xmb_json_int_range(responseBuffer,
            (int)strlen(responseBuffer), "TotalRecordCount", n + start_index);
    return n;
}

// -------------------------------------------------------
// Sliding-window pagination helpers
// -------------------------------------------------------

static int xmb_slide_tab_forward(int tab) {
    int keep = g_item_count[tab] - XMB_PAGE_SIZE;
    if (keep < 0) keep = 0;
    if (keep > 0)
        memmove(g_items[tab], g_items[tab] + XMB_PAGE_SIZE, keep * sizeof(XMBItem));
    g_tab_start[tab] += XMB_PAGE_SIZE;
    g_item_count[tab]  = keep;

    int fetch_start = g_tab_start[tab] + g_item_count[tab];
    char url[512];
    xmb_build_items_url(url, sizeof(url), tab, fetch_start, XMB_PAGE_SIZE);

    int new_count = 0;
    int status = http_request(0, url, NULL, g_token, responseBuffer, RESPONSE_SIZE);
    if (status == 200) {
        new_count = parse_xmb_items(responseBuffer,
                                     g_items[tab] + g_item_count[tab], XMB_PAGE_SIZE);
        g_item_count[tab] += new_count;
        g_tab_total[tab] = xmb_json_int_range(responseBuffer,
            (int)strlen(responseBuffer), "TotalRecordCount", g_tab_total[tab]);
    }
    return new_count > 0 ? keep : -1;
}

static int xmb_slide_tv_sub_forward(void) {
    int keep = g_tv_sub_count - XMB_PAGE_SIZE;
    if (keep < 0) keep = 0;
    if (keep > 0)
        memmove(g_tv_sub_items, g_tv_sub_items + XMB_PAGE_SIZE, keep * sizeof(XMBItem));
    g_tv_sub_start  += XMB_PAGE_SIZE;
    g_tv_sub_count   = keep;

    int fetch_start = g_tv_sub_start + g_tv_sub_count;
    int new_total   = g_tv_sub_total;
    int new_count   = 0;
    if (g_tv_depth == 1)
        new_count = xmb_fetch_seasons(g_tv_series_id,
                                       g_tv_sub_items + g_tv_sub_count,
                                       XMB_PAGE_SIZE, fetch_start, &new_total);
    else
        new_count = xmb_fetch_episodes(g_tv_series_id, g_tv_season_id,
                                        g_tv_sub_items + g_tv_sub_count,
                                        XMB_PAGE_SIZE, fetch_start, &new_total);
    g_tv_sub_count += new_count;
    g_tv_sub_total  = new_total;
    return new_count > 0 ? keep : -1;
}

static int xmb_slide_col_sub_forward(void) {
    int keep = g_col_sub_count - XMB_PAGE_SIZE;
    if (keep < 0) keep = 0;
    if (keep > 0)
        memmove(g_col_sub_items, g_col_sub_items + XMB_PAGE_SIZE, keep * sizeof(XMBItem));
    g_col_sub_start += XMB_PAGE_SIZE;
    g_col_sub_count  = keep;

    int fetch_start = g_col_sub_start + g_col_sub_count;
    int new_total   = g_col_sub_total;
    int new_count   = xmb_fetch_collection_items(g_col_id,
                                                  g_col_sub_items + g_col_sub_count,
                                                  XMB_PAGE_SIZE, fetch_start, &new_total);
    g_col_sub_count += new_count;
    g_col_sub_total  = new_total;
    return new_count > 0 ? keep : -1;
}

// -------------------------------------------------------
// Browse input handler
// -------------------------------------------------------

bool xmb_handle_input_browse(void) {
    static bool s_movie_just_exited = false;
    int tab = g_active_tab;
    int vis = XMB_ITEMS_VIS;

    // Settings tab — account actions (Log Out).
    if (tab == XMB_TAB_SETTINGS) {
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

    if (BTN_PRESSED(l1)) { xmb_switch_tab(xmb_next_enabled(g_active_tab, -1)); return false; }
    if (BTN_PRESSED(r1)) { xmb_switch_tab(xmb_next_enabled(g_active_tab, +1)); return false; }

    // TV sub-screen (depth > 0)
    if (tab == XMB_TAB_TV && g_tv_depth > 0) {
        if (BTN_PRESSED(circle)) {
            g_tv_depth--;
            g_tv_sub_sel = 0; g_tv_sub_scroll = 0; g_tv_sub_start = 0; g_tv_sub_total = 0;
            return false;
        }
        if (BTN_REPEAT(up)) {
            if (g_tv_sub_sel > 0) {
                g_tv_sub_sel--;
                if (g_tv_sub_sel < g_tv_sub_scroll) g_tv_sub_scroll = g_tv_sub_sel;
            }
        }
        if (BTN_REPEAT(down)) {
            if (g_tv_sub_sel < g_tv_sub_count - 1) {
                g_tv_sub_sel++;
                if (g_tv_sub_sel >= g_tv_sub_scroll + vis)
                    g_tv_sub_scroll = g_tv_sub_sel - vis + 1;
            } else if (g_tv_sub_start + g_tv_sub_count < g_tv_sub_total) {
                int first = xmb_slide_tv_sub_forward();
                if (first >= 0) { g_tv_sub_sel = first; g_tv_sub_scroll = first; }
            }
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
                JFItem jf; memset(&jf, 0, sizeof(jf));
                strncpy(jf.id,   it->id,   sizeof(jf.id)-1);
                strncpy(jf.name, it->name, sizeof(jf.name)-1);
                strncpy(jf.type, it->type, sizeof(jf.type)-1);
                show_player(&jf);
                g_tv_depth = 0;
                g_tv_sub_sel = 0;
                g_tv_sub_scroll = 0;
            }
        }
        return false;
    }

    // Collections sub-screen (depth > 0)
    if (tab == XMB_TAB_COLLECTIONS && g_col_depth > 0) {
        if (BTN_PRESSED(circle)) {
            g_col_depth = 0;
            g_col_sub_sel = 0; g_col_sub_scroll = 0; g_col_sub_start = 0; g_col_sub_total = 0;
            return false;
        }
        if (BTN_REPEAT(up)) {
            if (g_col_sub_sel > 0) {
                g_col_sub_sel--;
                if (g_col_sub_sel < g_col_sub_scroll) g_col_sub_scroll = g_col_sub_sel;
            }
        }
        if (BTN_REPEAT(down)) {
            if (g_col_sub_sel < g_col_sub_count - 1) {
                g_col_sub_sel++;
                if (g_col_sub_sel >= g_col_sub_scroll + vis)
                    g_col_sub_scroll = g_col_sub_sel - vis + 1;
            } else if (g_col_sub_start + g_col_sub_count < g_col_sub_total) {
                int first = xmb_slide_col_sub_forward();
                if (first >= 0) { g_col_sub_sel = first; g_col_sub_scroll = first; }
            }
        }
        if (BTN_PRESSED(cross) && g_col_sub_count > 0 && g_col_sub_sel < g_col_sub_count) {
            const XMBItem *it = &g_col_sub_items[g_col_sub_sel];
            JFItem jf; memset(&jf, 0, sizeof(jf));
            strncpy(jf.id,   it->id,   sizeof(jf.id)-1);
            strncpy(jf.name, it->name, sizeof(jf.name)-1);
            strncpy(jf.type, it->type, sizeof(jf.type)-1);
            show_player(&jf);
            g_col_depth = 0;
            g_col_sub_sel = 0;
            g_col_sub_scroll = 0;
        }
        return false;
    }

    // Jump bar
    bool jbar_mode = g_jumpbar_active &&
        (tab == XMB_TAB_MOVIES || tab == XMB_TAB_TV ||
         tab == XMB_TAB_MUSIC  || tab == XMB_TAB_COLLECTIONS);

    if (jbar_mode) {
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
        return false;
    }

    // Normal browse
    if (BTN_PRESSED(circle)) return false;

    int count = g_item_count[tab];

    if (BTN_REPEAT(up)) {
        if (g_sel > 0) {
            g_sel--;
            if (g_sel < g_scroll_top) g_scroll_top = g_sel;
        }
    }
    if (BTN_REPEAT(down)) {
        if (g_sel < count - 1) {
            g_sel++;
            if (g_sel >= g_scroll_top + vis) g_scroll_top = g_sel - vis + 1;
        } else if (g_tab_start[tab] + count < g_tab_total[tab]) {
            int first = xmb_slide_tab_forward(tab);
            if (first >= 0) { g_sel = first; g_scroll_top = first; }
        }
    }

    if (BTN_PRESSED(left) && (tab == XMB_TAB_MOVIES || tab == XMB_TAB_TV ||
                               tab == XMB_TAB_MUSIC  || tab == XMB_TAB_COLLECTIONS)) {
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
            JFItem jf; memset(&jf, 0, sizeof(jf));
            strncpy(jf.id,   it->id,   sizeof(jf.id)-1);
            strncpy(jf.name, it->name, sizeof(jf.name)-1);
            strncpy(jf.type, it->type, sizeof(jf.type)-1);
            show_player(&jf);
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
