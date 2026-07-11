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

    // Letter-jump filter suffix (shared by every tab).
    char name_filt[32] = "";
    if (filt[0] == '#')
        snprintf(name_filt, sizeof(name_filt), "&NameLessThan=A");
    else if (filt[0])
        snprintf(name_filt, sizeof(name_filt), "&NameStartsWith=%c",
                 (char)toupper((unsigned char)filt[0]));

    // Music: each sub-tab is its own query.  Albums/Songs are recursive
    // typed item queries (the library nests them under artist folders);
    // Artists and Genres have dedicated endpoints; Playlists live in their
    // own library, so that query runs from the root with no ParentId.
    if (tab == XMB_TAB_MUSIC) {
        switch (g_music_subtab) {
        case MUSIC_ST_ARTISTS:
            snprintf(url, url_size,
                "%s/Artists/AlbumArtists?userId=%s&ParentId=%s"
                "&StartIndex=%d&Limit=%d"
                "&SortBy=SortName&SortOrder=Ascending%s",
                g_server, g_userid, lid, start_index, limit, name_filt);
            return;
        case MUSIC_ST_PLAYLISTS:
            snprintf(url, url_size,
                "%s/Users/%s/Items?IncludeItemTypes=Playlist&Recursive=true"
                "&StartIndex=%d&Limit=%d"
                "&SortBy=SortName&SortOrder=Ascending%s"
                "&Fields=ChildCount",
                g_server, g_userid, start_index, limit, name_filt);
            return;
        case MUSIC_ST_GENRES:
            snprintf(url, url_size,
                "%s/MusicGenres?userId=%s&ParentId=%s"
                "&StartIndex=%d&Limit=%d"
                "&SortBy=SortName&SortOrder=Ascending%s",
                g_server, g_userid, lid, start_index, limit, name_filt);
            return;
        case MUSIC_ST_SONGS:
            snprintf(url, url_size,
                "%s/Users/%s/Items?ParentId=%s&StartIndex=%d&Limit=%d"
                "&IncludeItemTypes=Audio&Recursive=true"
                "&SortBy=SortName&SortOrder=Ascending%s"
                "&Fields=Genres,RunTimeTicks",
                g_server, g_userid, lid, start_index, limit, name_filt);
            return;
        default:   // MUSIC_ST_ALBUMS
            snprintf(url, url_size,
                "%s/Users/%s/Items?ParentId=%s&StartIndex=%d&Limit=%d"
                "&IncludeItemTypes=MusicAlbum&Recursive=true"
                "&SortBy=SortName&SortOrder=Ascending%s"
                "&Fields=Genres,ProductionYear,ChildCount",
                g_server, g_userid, lid, start_index, limit, name_filt);
            return;
        }
    }

    snprintf(url, url_size,
        "%s/Users/%s/Items?ParentId=%s&StartIndex=%d&Limit=%d"
        "&SortBy=SortName&SortOrder=Ascending%s"
        "&Fields=Genres,RunTimeTicks,ProductionYear,Container",
        g_server, g_userid, lid, start_index, limit, name_filt);
}

void xmb_fetch_tab_items(int tab) {
    if (g_items_loaded[tab]) return;
    g_item_count[tab] = 0;
    g_tab_start[tab]  = 0;
    g_tab_total[tab]  = 0;

    // Continue Watching — the server's resume list (no library / pagination;
    // items carry UserData with PlaybackPositionTicks + PlayedPercentage).
    if (tab == XMB_TAB_RESUME) {
        char url[512];
        snprintf(url, sizeof(url),
            "%s/Users/%s/Items/Resume?Limit=%d&Recursive=true"
            "&MediaTypes=Video"
            "&Fields=Genres,RunTimeTicks,ProductionYear,Container",
            g_server, g_userid, XMB_ITEMS_MAX);
        int status = http_request(0, url, NULL, g_token,
                                  responseBuffer, RESPONSE_SIZE);
        if (status == 200) {
            g_item_count[tab] = parse_xmb_items(responseBuffer,
                                                g_items[tab], XMB_ITEMS_MAX);
            g_tab_total[tab]  = g_item_count[tab];
        }
        g_items_loaded[tab] = true;
        return;
    }

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

// Resolve the episode that follows episode_id in its series, crossing season
// boundaries (the series-wide episode list is used, not the season's).  Two
// requests: the item detail for SeriesId, then the episode list starting at
// the current episode (StartItemId) so the follower is the second item.
// Returns true and fills *out when a follower exists.
bool xmb_fetch_next_episode(const char *episode_id, XMBItem *out) {
    char url[512];
    snprintf(url, sizeof(url), "%s/Users/%s/Items/%s",
             g_server, g_userid, episode_id);
    int status = http_request(0, url, NULL, g_token, responseBuffer, RESPONSE_SIZE);
    if (status != 200) return false;

    char series_id[64] = "";
    json_get_string(responseBuffer, "SeriesId", series_id, sizeof(series_id));
    if (!series_id[0]) return false;

    snprintf(url, sizeof(url),
        "%s/Shows/%s/Episodes?userId=%s&StartItemId=%s&Limit=2"
        "&Fields=ProductionYear,RunTimeTicks,Genres,Container",
        g_server, series_id, g_userid, episode_id);
    status = http_request(0, url, NULL, g_token, responseBuffer, RESPONSE_SIZE);
    if (status != 200) return false;

    XMBItem two[2];
    if (parse_xmb_items(responseBuffer, two, 2) < 2) return false;
    *out = two[1];
    strncpy(out->type, "Episode", sizeof(out->type)-1);
    return true;
}

// Albums belonging to one artist or genre (the music sub-screen).
// id_param is the server-side filter key: "AlbumArtistIds" or "GenreIds".
// Sorted oldest-first so an artist's grid reads as a discography.
int xmb_fetch_music_children(const char *id_param, const char *parent_id,
                             XMBItem *arr, int max, int *out_total) {
    char url[512];
    snprintf(url, sizeof(url),
        "%s/Users/%s/Items?%s=%s"
        "&IncludeItemTypes=MusicAlbum&Recursive=true"
        "&StartIndex=0&Limit=%d"
        "&SortBy=ProductionYear,SortName&SortOrder=Ascending"
        "&Fields=Genres,ProductionYear,ChildCount",
        g_server, g_userid, id_param, parent_id, max);
    int status = http_request(0, url, NULL, g_token, responseBuffer, RESPONSE_SIZE);
    if (status != 200) return 0;
    int n = parse_xmb_items(responseBuffer, arr, max);
    if (out_total)
        *out_total = xmb_json_int_range(responseBuffer,
            (int)strlen(responseBuffer), "TotalRecordCount", n);
    return n;
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

// Backward sliding: re-fetch the page just before the window and prepend it,
// dropping trailing items to stay within XMB_ITEMS_MAX.  The page is fetched
// into a scratch buffer first so a failed or short fetch leaves the window
// untouched (a short page would misalign the window against server indices).
// Returns the number of prepended items — existing indices shift up by that
// much — or -1 when already at the top / on fetch failure.

static XMBItem s_slide_page[XMB_PAGE_SIZE];

int xmb_slide_tab_backward(int tab) {
    if (g_tab_start[tab] <= 0) return -1;
    int fetch_start = g_tab_start[tab] - XMB_PAGE_SIZE;
    if (fetch_start < 0) fetch_start = 0;
    int want = g_tab_start[tab] - fetch_start;

    char url[512];
    xmb_build_items_url(url, sizeof(url), tab, fetch_start, want);
    int status = http_request(0, url, NULL, g_token, responseBuffer, RESPONSE_SIZE);
    if (status != 200) return -1;
    if (parse_xmb_items(responseBuffer, s_slide_page, want) != want) return -1;

    int keep = g_item_count[tab];
    if (keep > XMB_ITEMS_MAX - want) keep = XMB_ITEMS_MAX - want;
    memmove(g_items[tab] + want, g_items[tab], keep * sizeof(XMBItem));
    memcpy(g_items[tab], s_slide_page, want * sizeof(XMBItem));
    g_tab_start[tab]  = fetch_start;
    g_item_count[tab] = keep + want;
    return want;
}

int xmb_slide_tv_sub_backward(void) {
    if (g_tv_sub_start <= 0) return -1;
    int fetch_start = g_tv_sub_start - XMB_PAGE_SIZE;
    if (fetch_start < 0) fetch_start = 0;
    int want = g_tv_sub_start - fetch_start;

    int new_total = g_tv_sub_total;
    int n = (g_tv_depth == 1)
        ? xmb_fetch_seasons(g_tv_series_id, s_slide_page, want,
                            fetch_start, &new_total)
        : xmb_fetch_episodes(g_tv_series_id, g_tv_season_id, s_slide_page,
                             want, fetch_start, &new_total);
    if (n != want) return -1;

    int keep = g_tv_sub_count;
    if (keep > XMB_ITEMS_MAX - want) keep = XMB_ITEMS_MAX - want;
    memmove(g_tv_sub_items + want, g_tv_sub_items, keep * sizeof(XMBItem));
    memcpy(g_tv_sub_items, s_slide_page, want * sizeof(XMBItem));
    g_tv_sub_start = fetch_start;
    g_tv_sub_count = keep + want;
    g_tv_sub_total = new_total;
    return want;
}

int xmb_slide_col_sub_backward(void) {
    if (g_col_sub_start <= 0) return -1;
    int fetch_start = g_col_sub_start - XMB_PAGE_SIZE;
    if (fetch_start < 0) fetch_start = 0;
    int want = g_col_sub_start - fetch_start;

    int new_total = g_col_sub_total;
    int n = xmb_fetch_collection_items(g_col_id, s_slide_page, want,
                                       fetch_start, &new_total);
    if (n != want) return -1;

    int keep = g_col_sub_count;
    if (keep > XMB_ITEMS_MAX - want) keep = XMB_ITEMS_MAX - want;
    memmove(g_col_sub_items + want, g_col_sub_items, keep * sizeof(XMBItem));
    memcpy(g_col_sub_items, s_slide_page, want * sizeof(XMBItem));
    g_col_sub_start = fetch_start;
    g_col_sub_count = keep + want;
    g_col_sub_total = new_total;
    return want;
}
