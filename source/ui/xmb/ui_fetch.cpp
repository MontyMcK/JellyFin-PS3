// Jellyfin library fetch + sliding-window pagination for the XMB lists.

#include <stdio.h>
#include <string.h>
#include <ctype.h>

#include "ui_internal.h"
#include "jellyfin_api.h"

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

int xmb_fetch_seasons(const char *series_id, XMBItem *arr, int max,
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

int xmb_fetch_episodes(const char *series_id, const char *season_id,
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

int xmb_fetch_collection_items(const char *collection_id, XMBItem *arr, int max,
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

int xmb_slide_tab_forward(int tab) {
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

int xmb_slide_tv_sub_forward(void) {
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

int xmb_slide_col_sub_forward(void) {
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
