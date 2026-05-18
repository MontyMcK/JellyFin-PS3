#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>

#include <rsx/rsx.h>
#include <sysutil/sysutil.h>

#include "ui.h"
#include "ui_wave.h"
#include "ui_visuals.h"
#include "jellyfin_api.h"
#include "player.h"
#include "timing.h"
#include "plog.h"

ButtonState btn_cur  = {0};
ButtonState btn_prev = {0};

void update_buttons(padData *pad) {
    btn_prev         = btn_cur;
    btn_cur.up       = pad->BTN_UP;
    btn_cur.down     = pad->BTN_DOWN;
    btn_cur.left     = pad->BTN_LEFT;
    btn_cur.right    = pad->BTN_RIGHT;
    btn_cur.cross    = pad->BTN_CROSS;
    btn_cur.circle   = pad->BTN_CIRCLE;
    btn_cur.square   = pad->BTN_SQUARE;
    btn_cur.triangle = pad->BTN_TRIANGLE;
    btn_cur.start    = pad->BTN_START;
    btn_cur.select   = pad->BTN_SELECT;
    btn_cur.l1       = pad->BTN_L1;
    btn_cur.r1       = pad->BTN_R1;
}

void init_btns(void) {
    padInfo pi; padData pd;
    ioPadGetInfo(&pi);
    for (int i = 0; i < MAX_PADS; i++) {
        if (!pi.status[i]) continue;
        ioPadGetData(i, &pd);
        update_buttons(&pd);
    }
    btn_prev = btn_cur;
}

// -------------------------------------------------------
// Legacy on-screen keyboard state
// -------------------------------------------------------

int  kb_row  = 0;
int  kb_col  = 0;
bool kb_caps = false;

static int row_len(int r) {
    return (r < KB_LETTER_ROWS) ? (int)strlen(KB_ROWS[r]) : SPECIAL_N;
}

static char current_key_value(void) {
    if (kb_row < KB_LETTER_ROWS) {
        char c = KB_ROWS[kb_row][kb_col];
        return kb_caps ? (char)toupper(c) : c;
    }
    return SPECIAL[kb_col].value;
}

int get_input(char *out, int max_len, const char *prompt, bool is_password) {
    out[0]  = '\0';
    kb_row  = 0; kb_col = 0; kb_caps = false;

    padInfo padinfo; padData paddata;
    init_btns();

    while (running) {
        sysUtilCheckCallback();
        ioPadGetInfo(&padinfo);
        for (int i = 0; i < MAX_PADS; i++) {
            if (!padinfo.status[i]) continue;
            ioPadGetData(i, &paddata);
            update_buttons(&paddata);

            int rlen = row_len(kb_row);

            if (BTN_PRESSED(up)) {
                kb_row = (kb_row - 1 + TOTAL_ROWS) % TOTAL_ROWS;
                int nl = row_len(kb_row); if (kb_col >= nl) kb_col = nl - 1;
            }
            if (BTN_PRESSED(down)) {
                kb_row = (kb_row + 1) % TOTAL_ROWS;
                int nl = row_len(kb_row); if (kb_col >= nl) kb_col = nl - 1;
            }
            if (BTN_PRESSED(left))  kb_col = (kb_col - 1 + rlen) % rlen;
            if (BTN_PRESSED(right)) kb_col = (kb_col + 1) % rlen;

            if (BTN_PRESSED(triangle)) kb_caps = !kb_caps;

            if (BTN_PRESSED(cross)) {
                char ch = current_key_value();
                if (ch == '\r') return 1;
                if (ch == '\b') { int len = strlen(out); if (len > 0) out[len-1] = '\0'; }
                else            { int len = strlen(out); if (len < max_len-1) { out[len]=ch; out[len+1]='\0'; } }
            }
            if (BTN_PRESSED(square)) { int len = strlen(out); if (len > 0) out[len-1] = '\0'; }
            if (BTN_PRESSED(start))  return 1;
            if (BTN_PRESSED(select)) return -1;
        }

        draw_keyboard(prompt, out, is_password);
    }
    return -1;
}

// -------------------------------------------------------
// XMB tab / item data
// -------------------------------------------------------

XMBTab g_tabs[XMB_TAB_COUNT] = {
    {"SEARCH",      "?",  "", true },
    {"MOVIES",      "#",  "", false},
    {"TV SHOWS",    "=",  "", false},
    {"MUSIC",       "~",  "", true },
    {"COLLECTIONS", "+",  "", false},
    {"SETTINGS",    "*",  "", true },
};

XMBItem g_items[XMB_TAB_COUNT][XMB_ITEMS_MAX];
int     g_item_count[XMB_TAB_COUNT];
bool    g_items_loaded[XMB_TAB_COUNT];

// UI navigation state
int  g_active_tab = XMB_TAB_MOVIES;
int  g_sel        = 0;
int  g_scroll_top = 0;

// TV sub-screen state (Series→Seasons→Episodes)
static int  g_tv_depth       = 0;
static char g_tv_series_id[64];
static char g_tv_series_name[128];
static char g_tv_season_id[64];
static char g_tv_season_name[64];
XMBItem g_tv_sub_items[XMB_ITEMS_MAX];
int     g_tv_sub_count  = 0;
int     g_tv_sub_sel    = 0;
int     g_tv_sub_scroll = 0;

// Collections sub-screen state (Collection→Movies)
static int  g_col_depth      = 0;
static char g_col_id[64];
static char g_col_name[128];
XMBItem g_col_sub_items[XMB_ITEMS_MAX];
int     g_col_sub_count  = 0;
int     g_col_sub_sel    = 0;
int     g_col_sub_scroll = 0;

// Search OSK state
const char *OSK_LETTERS[OSK_ROWS_N] = {
    "1234567890",
    "QWERTYUIOP",
    "ASDFGHJKL",
    "ZXCVBNM",
};
const char *OSK_SYMBOLS[OSK_ROWS_N] = {
    "!@#$%^&*()",
    "-_=+[]{}|\\",
    ":;\"'`~<>?",
    ".,/!",
};
int  g_osk_row    = 0;
int  g_osk_col    = 0;
bool g_osk_sym    = false;
char g_search_buf[64];
int  g_search_results_count = 0;
XMBItem g_search_results[XMB_ITEMS_MAX];

int OSK_Y0 = 0;

// -------------------------------------------------------
// XMB JSON parsing helpers (local, avoids changing jellyfin_api.cpp)
// -------------------------------------------------------

static int xmb_json_str_range(const char *start, int len,
                               const char *key, char *out, int out_size) {
    char search[64];
    snprintf(search, sizeof(search), "\"%s\":\"", key);
    int slen = strlen(search);
    const char *p = start, *end = start + len;
    while (p + slen <= end) {
        if (memcmp(p, search, slen) == 0) {
            p += slen;
            int i = 0;
            while (p < end && *p != '"' && i < out_size-1) out[i++] = *p++;
            out[i] = '\0';
            return 1;
        }
        p++;
    }
    out[0] = '\0';
    return 0;
}

static int xmb_json_int_range(const char *start, int len,
                               const char *key, int def) {
    char search[64];
    snprintf(search, sizeof(search), "\"%s\":", key);
    int slen = strlen(search);
    const char *p = start, *end = start + len;
    while (p + slen <= end) {
        if (memcmp(p, search, slen) == 0) {
            p += slen;
            while (p < end && *p == ' ') p++;
            if (*p < '0' || *p > '9') return def;
            return atoi(p);
        }
        p++;
    }
    return def;
}

static long long xmb_json_ll_range(const char *start, int len,
                                    const char *key, long long def) {
    char search[64];
    snprintf(search, sizeof(search), "\"%s\":", key);
    int slen = strlen(search);
    const char *p = start, *end = start + len;
    while (p + slen <= end) {
        if (memcmp(p, search, slen) == 0) {
            p += slen;
            while (p < end && *p == ' ') p++;
            if (*p < '0' || *p > '9') return def;
            long long v = 0;
            while (p < end && *p >= '0' && *p <= '9') v = v * 10 + (*p++ - '0');
            return v;
        }
        p++;
    }
    return def;
}

static int xmb_json_first_arr_str(const char *start, int len,
                                   const char *key, char *out, int out_size) {
    char search[64];
    snprintf(search, sizeof(search), "\"%s\":[\"", key);
    int slen = strlen(search);
    const char *p = start, *end = start + len;
    while (p + slen <= end) {
        if (memcmp(p, search, slen) == 0) {
            p += slen;
            int i = 0;
            while (p < end && *p != '"' && i < out_size-1) out[i++] = *p++;
            out[i] = '\0';
            return 1;
        }
        p++;
    }
    out[0] = '\0';
    return 0;
}

static int parse_xmb_items(const char *json, XMBItem *arr, int max) {
    const char *p = strstr(json, "\"Items\":[");
    if (!p) return 0;
    p += 9;

    int count = 0;
    while (count < max && *p) {
        while (*p && *p != '{' && *p != ']') p++;
        if (!*p || *p == ']') break;

        const char *obj = p;
        int  depth = 0;
        bool in_str = false, esc = false;
        while (*p) {
            char c = *p;
            if (esc) { esc = false; }
            else if (in_str) { if (c == '\\') esc = true; else if (c == '"') in_str = false; }
            else { if (c == '"') in_str = true; else if (c == '{') depth++; else if (c == '}') { if (--depth == 0) { p++; break; } } }
            p++;
        }
        int olen = (int)(p - obj);

        XMBItem it; memset(&it, 0, sizeof(it));
        xmb_json_str_range(obj, olen, "Id",   it.id,   sizeof(it.id));
        xmb_json_str_range(obj, olen, "Name", it.name, sizeof(it.name));
        xmb_json_str_range(obj, olen, "Type", it.type, sizeof(it.type));
        if (!it.id[0]) continue;

        int year = xmb_json_int_range(obj, olen, "ProductionYear", 0);
        if (year > 0) snprintf(it.year_str, sizeof(it.year_str), "%d", year);

        long long ticks = xmb_json_ll_range(obj, olen, "RunTimeTicks", 0);
        if (ticks > 0) {
            int total_min = (int)(ticks / 600000000LL);
            int h = total_min / 60, m = total_min % 60;
            if (h > 0) snprintf(it.duration_str, sizeof(it.duration_str), "%dh %dm", h, m);
            else        snprintf(it.duration_str, sizeof(it.duration_str), "%dm", m);
        }

        xmb_json_first_arr_str(obj, olen, "Genres", it.genre, sizeof(it.genre));

        char container[16] = "";
        xmb_json_str_range(obj, olen, "Container", container, sizeof(container));
        if (strstr(container, "hevc") || strstr(container, "h265") ||
            strstr(container, "265"))
            strncpy(it.codec, "H.265", sizeof(it.codec)-1);
        else
            strncpy(it.codec, "H.264", sizeof(it.codec)-1);

        decode_unicode_escapes(it.name);
        decode_unicode_escapes(it.genre);
        arr[count++] = it;
    }
    return count;
}

// -------------------------------------------------------
// XMB API fetch helpers
// -------------------------------------------------------

static void xmb_detect_tabs(void) {
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

static void xmb_fetch_tab_items(int tab) {
    if (g_items_loaded[tab]) return;
    g_item_count[tab] = 0;

    const char *lib_id = g_tabs[tab].library_id;
    if (!lib_id[0]) { g_items_loaded[tab] = true; return; }

    char url[512];
    snprintf(url, sizeof(url),
        "%s/Users/%s/Items?ParentId=%s&Limit=%d"
        "&SortBy=SortName&SortOrder=Ascending"
        "&Fields=Genres,RunTimeTicks,ProductionYear,Container",
        g_server, g_userid, lib_id, XMB_ITEMS_MAX);

    int status = http_request(0, url, NULL, g_token, responseBuffer, RESPONSE_SIZE);
    if (status == 200)
        g_item_count[tab] = parse_xmb_items(responseBuffer, g_items[tab], XMB_ITEMS_MAX);
    g_items_loaded[tab] = true;
}

static void xmb_do_search(void) {
    g_search_results_count = 0;
    if (!g_search_buf[0]) return;

    char encoded[192];
    url_encode_query(g_search_buf, encoded, sizeof(encoded));

    char url[512];
    snprintf(url, sizeof(url),
        "%s/Users/%s/Items?searchTerm=%s&Recursive=true"
        "&IncludeItemTypes=Movie,Series,Episode&Limit=%d"
        "&SortBy=SortName&SortOrder=Ascending"
        "&Fields=Genres,RunTimeTicks,ProductionYear,Container",
        g_server, g_userid, encoded, XMB_ITEMS_MAX);

    char dbg[512];
    snprintf(dbg, sizeof(dbg), "search url: %s", url);
    plog(dbg);

    int status = http_request(0, url, NULL, g_token, responseBuffer, RESPONSE_SIZE);
    if (status == 200)
        g_search_results_count = parse_xmb_items(responseBuffer, g_search_results, XMB_ITEMS_MAX);

    snprintf(dbg, sizeof(dbg), "search status: %d count: %d",
             status, g_search_results_count);
    plog(dbg);
}

static int xmb_fetch_seasons(const char *series_id, XMBItem *arr, int max) {
    char url[512];
    snprintf(url, sizeof(url),
        "%s/Shows/%s/Seasons?userId=%s&Fields=ProductionYear,RunTimeTicks",
        g_server, series_id, g_userid);
    int status = http_request(0, url, NULL, g_token, responseBuffer, RESPONSE_SIZE);
    if (status != 200) return 0;
    return parse_xmb_items(responseBuffer, arr, max);
}

static int xmb_fetch_episodes(const char *series_id, const char *season_id,
                               XMBItem *arr, int max) {
    char url[512];
    snprintf(url, sizeof(url),
        "%s/Shows/%s/Episodes?seasonId=%s&userId=%s"
        "&Fields=ProductionYear,RunTimeTicks,Genres,Container",
        g_server, series_id, season_id, g_userid);
    int status = http_request(0, url, NULL, g_token, responseBuffer, RESPONSE_SIZE);
    if (status != 200) return 0;
    int count = parse_xmb_items(responseBuffer, arr, max);
    for (int i = 0; i < count; i++)
        strncpy(arr[i].type, "Episode", sizeof(arr[i].type)-1);
    return count;
}

static int xmb_fetch_collection_items(const char *collection_id, XMBItem *arr, int max) {
    char url[512];
    snprintf(url, sizeof(url),
        "%s/Users/%s/Items?ParentId=%s"
        "&Fields=Genres,RunTimeTicks,ProductionYear,Container"
        "&SortBy=SortName&SortOrder=Ascending",
        g_server, g_userid, collection_id);
    int status = http_request(0, url, NULL, g_token, responseBuffer, RESPONSE_SIZE);
    if (status != 200) return 0;
    return parse_xmb_items(responseBuffer, arr, max);
}

// -------------------------------------------------------
// OSK helpers
// -------------------------------------------------------

static int osk_row_len(int r) {
    if (r >= OSK_ROWS_N) {
        return 3;
    }
    const char **rows = g_osk_sym ? OSK_SYMBOLS : OSK_LETTERS;
    int base = strlen(rows[r]);
    if (!g_osk_sym && r == OSK_ROWS_N - 1) base++;
    if  (g_osk_sym && r == OSK_ROWS_N - 1) base++;
    return base;
}

static char osk_current_char(void) {
    if (g_osk_row >= OSK_ROWS_N) {
        return 0;
    }
    const char **rows = g_osk_sym ? OSK_SYMBOLS : OSK_LETTERS;
    const char *row   = rows[g_osk_row];
    int base_len = strlen(row);
    bool is_toggle = (g_osk_row == OSK_ROWS_N-1 && g_osk_col == base_len);
    if (is_toggle) return 0;
    if (g_osk_col < base_len) return row[g_osk_col];
    return 0;
}

// -------------------------------------------------------
// XMB input
// -------------------------------------------------------

static void xmb_switch_tab(int new_tab) {
    if (new_tab < 0 || new_tab >= XMB_TAB_COUNT) return;
    if (!g_tabs[new_tab].enabled) return;
    g_active_tab = new_tab;
    g_sel = 0;
    g_scroll_top = 0;
    g_tv_depth = 0; g_tv_sub_sel = 0; g_tv_sub_scroll = 0;
    g_col_depth = 0; g_col_sub_sel = 0; g_col_sub_scroll = 0;
    if (new_tab == XMB_TAB_SEARCH) {
        g_osk_row = 0; g_osk_col = 0; g_osk_sym = false;
    }
}

static int xmb_next_enabled(int start, int dir) {
    int t = start + dir;
    while (t >= 0 && t < XMB_TAB_COUNT) {
        if (g_tabs[t].enabled) return t;
        t += dir;
    }
    return start;
}

static bool xmb_handle_input_browse(void) {
    static bool s_movie_just_exited = false;
    int tab = g_active_tab;
    int vis = XMB_ITEMS_VIS;

    if (BTN_PRESSED(l1)) { xmb_switch_tab(xmb_next_enabled(g_active_tab, -1)); return false; }
    if (BTN_PRESSED(r1)) { xmb_switch_tab(xmb_next_enabled(g_active_tab, +1)); return false; }

    // TV sub-screen (depth > 0): seasons or episodes list
    if (tab == XMB_TAB_TV && g_tv_depth > 0) {
        if (BTN_PRESSED(circle)) {
            g_tv_depth--;
            g_tv_sub_sel = 0; g_tv_sub_scroll = 0;
            return false;
        }
        if (BTN_PRESSED(up)) {
            if (g_tv_sub_sel > 0) {
                g_tv_sub_sel--;
                if (g_tv_sub_sel < g_tv_sub_scroll) g_tv_sub_scroll = g_tv_sub_sel;
            }
        }
        if (BTN_PRESSED(down)) {
            if (g_tv_sub_sel < g_tv_sub_count - 1) {
                g_tv_sub_sel++;
                if (g_tv_sub_sel >= g_tv_sub_scroll + vis)
                    g_tv_sub_scroll = g_tv_sub_sel - vis + 1;
            }
        }
        if (BTN_PRESSED(cross) && g_tv_sub_count > 0 && g_tv_sub_sel < g_tv_sub_count) {
            const XMBItem *it = &g_tv_sub_items[g_tv_sub_sel];
            if (g_tv_depth == 1) {
                strncpy(g_tv_season_id,   it->id,   sizeof(g_tv_season_id)-1);
                strncpy(g_tv_season_name, it->name, sizeof(g_tv_season_name)-1);
                g_tv_sub_count = xmb_fetch_episodes(g_tv_series_id, g_tv_season_id,
                                                     g_tv_sub_items, XMB_ITEMS_MAX);
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

    // Collections sub-screen (depth > 0): movies inside a collection
    if (tab == XMB_TAB_COLLECTIONS && g_col_depth > 0) {
        if (BTN_PRESSED(circle)) {
            g_col_depth = 0;
            g_col_sub_sel = 0; g_col_sub_scroll = 0;
            return false;
        }
        if (BTN_PRESSED(up)) {
            if (g_col_sub_sel > 0) {
                g_col_sub_sel--;
                if (g_col_sub_sel < g_col_sub_scroll) g_col_sub_scroll = g_col_sub_sel;
            }
        }
        if (BTN_PRESSED(down)) {
            if (g_col_sub_sel < g_col_sub_count - 1) {
                g_col_sub_sel++;
                if (g_col_sub_sel >= g_col_sub_scroll + vis)
                    g_col_sub_scroll = g_col_sub_sel - vis + 1;
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

    // Normal browse (depth 0)
    if (BTN_PRESSED(circle)) return false;

    int count = g_item_count[tab];

    if (BTN_PRESSED(up)) {
        if (g_sel > 0) {
            g_sel--;
            if (g_sel < g_scroll_top) g_scroll_top = g_sel;
        }
    }
    if (BTN_PRESSED(down)) {
        if (g_sel < count - 1) {
            g_sel++;
            if (g_sel >= g_scroll_top + vis) g_scroll_top = g_sel - vis + 1;
        }
    }

    if (s_movie_just_exited) { s_movie_just_exited = false; return false; }

    if (BTN_PRESSED(cross) && count > 0 && g_sel < count) {
        const XMBItem *it = &g_items[tab][g_sel];
        if (tab == XMB_TAB_TV && strcmp(it->type, "Series") == 0) {
            strncpy(g_tv_series_id,   it->id,   sizeof(g_tv_series_id)-1);
            strncpy(g_tv_series_name, it->name, sizeof(g_tv_series_name)-1);
            g_tv_sub_count = xmb_fetch_seasons(g_tv_series_id, g_tv_sub_items, XMB_ITEMS_MAX);
            g_tv_depth = 1; g_tv_sub_sel = 0; g_tv_sub_scroll = 0;
        } else if (tab == XMB_TAB_COLLECTIONS) {
            strncpy(g_col_id,   it->id,   sizeof(g_col_id)-1);
            strncpy(g_col_name, it->name, sizeof(g_col_name)-1);
            g_col_sub_count = xmb_fetch_collection_items(g_col_id, g_col_sub_items, XMB_ITEMS_MAX);
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

    if (BTN_PRESSED(triangle) && count > 0 && g_sel < count) {
        const XMBItem *it = &g_items[tab][g_sel];
        // Drain RSX commands before the inner loop writes new wave geometry.
        rsxSync();
        flip();
        init_btns();
        while (running) {
            waitflip();
            sysUtilCheckCallback();
            padInfo pi; padData pd;
            ioPadGetInfo(&pi);
            for (int i = 0; i < MAX_PADS; i++) {
                if (!pi.status[i]) continue;
                ioPadGetData(i, &pd); update_buttons(&pd);
                if (BTN_PRESSED(circle) || BTN_PRESSED(triangle)) goto info_done;
            }
            clearScreen(XMB_BG);
            wave_draw();
            rsxSync();
            drawTTF(XMB_ITEM_PAD, XMB_TOPBAR_H + 10, it->name, 24, 0x00FFFFFF);
            drawTTF(XMB_ITEM_PAD, XMB_TOPBAR_H + 46, it->year_str, 14, 0x00FFFFFF);
            xmb_draw_meta(XMB_ITEM_PAD, XMB_TOPBAR_H + 68, it);
            drawTTF(XMB_ITEM_PAD, XMB_TOPBAR_H + 100, "Coming soon", 16, 0x00888888);
            drawTTF(XMB_ITEM_PAD, (u32)(display_height - XMB_BOTTOM_PAD + 16), "O BACK", 14, 0x00FFFFFF);
            flip();
        }
        info_done:
        init_btns();
    }
    return false;
}

static bool xmb_handle_input_search(void) {
    if (BTN_PRESSED(l1)) { xmb_switch_tab(xmb_next_enabled(g_active_tab, -1)); return false; }
    if (BTN_PRESSED(r1)) { xmb_switch_tab(xmb_next_enabled(g_active_tab, +1)); return false; }
    if (BTN_PRESSED(circle) && !g_search_buf[0]) {
        xmb_switch_tab(xmb_next_enabled(g_active_tab, +1));
        return false;
    }
    if (BTN_PRESSED(circle)) { g_search_buf[0] = '\0'; g_search_results_count = 0; return false; }

    int row_count = OSK_ROWS_N + 1;

    if (BTN_PRESSED(up)) {
        g_osk_row = (g_osk_row - 1 + row_count) % row_count;
        int ml = osk_row_len(g_osk_row);
        if (g_osk_col >= ml) g_osk_col = ml - 1;
    }
    if (BTN_PRESSED(down)) {
        g_osk_row = (g_osk_row + 1) % row_count;
        int ml = osk_row_len(g_osk_row);
        if (g_osk_col >= ml) g_osk_col = ml - 1;
    }
    if (BTN_PRESSED(left)) {
        int ml = osk_row_len(g_osk_row);
        g_osk_col = (g_osk_col - 1 + ml) % ml;
    }
    if (BTN_PRESSED(right)) {
        int ml = osk_row_len(g_osk_row);
        g_osk_col = (g_osk_col + 1) % ml;
    }

    if (BTN_PRESSED(cross)) {
        if (g_osk_row == OSK_ROWS_N) {
            if (g_osk_col == 0) {
                int len = strlen(g_search_buf);
                if (len < (int)sizeof(g_search_buf)-1) { g_search_buf[len]=' '; g_search_buf[len+1]='\0'; }
            } else if (g_osk_col == 1) {
                int len = strlen(g_search_buf);
                if (len > 0) g_search_buf[len-1] = '\0';
            } else {
                g_search_buf[0] = '\0';
                g_search_results_count = 0;
            }
        } else {
            const char **rows = g_osk_sym ? OSK_SYMBOLS : OSK_LETTERS;
            int base_len = strlen(rows[g_osk_row]);
            if (g_osk_row == OSK_ROWS_N - 1 && g_osk_col == base_len) {
                g_osk_sym = !g_osk_sym;
                g_osk_col = 0;
            } else {
                char ch = osk_current_char();
                if (ch) {
                    int len = strlen(g_search_buf);
                    if (len < (int)sizeof(g_search_buf)-1) {
                        g_search_buf[len] = ch;
                        g_search_buf[len+1] = '\0';
                    }
                }
            }
        }
    }

    if (BTN_PRESSED(triangle) || BTN_PRESSED(start)) {
        xmb_do_search();
    }

    if (BTN_PRESSED(cross) && g_osk_row == OSK_ROWS_N) {
        // handled above
    }

    return false;
}

// -------------------------------------------------------
// XMB main loop
// -------------------------------------------------------

void ui_run_xmb(void) {
    memset(g_items, 0, sizeof(g_items));
    memset(g_item_count, 0, sizeof(g_item_count));
    memset(g_items_loaded, 0, sizeof(g_items_loaded));
    memset(g_search_buf, 0, sizeof(g_search_buf));
    g_search_results_count = 0;
    g_active_tab = XMB_TAB_MOVIES;
    g_sel = 0; g_scroll_top = 0;
    g_osk_row = 0; g_osk_col = 0; g_osk_sym = false;
    wave_reset();

    xmb_detect_tabs();

    if (!g_tabs[XMB_TAB_MOVIES].enabled) {
        for (int t = 0; t < XMB_TAB_COUNT; t++) {
            if (g_tabs[t].enabled) { g_active_tab = t; break; }
        }
    }

    OSK_Y0 = XMB_CONTENT_Y + 58;

    init_btns();
    padInfo padinfo; padData paddata;

    while (running) {
        waitflip();
        sysUtilCheckCallback();
        clearScreen(XMB_BG);
        wave_draw();

        int tab = g_active_tab;
        if (tab != XMB_TAB_SEARCH && tab != XMB_TAB_MUSIC && tab != XMB_TAB_SETTINGS)
            if (!g_items_loaded[tab]) xmb_fetch_tab_items(tab);

        ioPadGetInfo(&padinfo);
        for (int i = 0; i < MAX_PADS; i++) {
            if (!padinfo.status[i]) continue;
            ioPadGetData(i, &paddata);
            update_buttons(&paddata);
        }
        bool should_exit = false;
        if (tab == XMB_TAB_SEARCH)
            should_exit = xmb_handle_input_search();
        else
            should_exit = xmb_handle_input_browse();
        if (should_exit) break;

        rsxSync();

        // Divider line
        u32 *div = color_buffer[curr_fb] + XMB_DIVIDER_Y * display_width;
        for (u32 x = 0; x < display_width; x++) div[x] = XMB_DIVIDER_CLR;

        if (tab == XMB_TAB_SEARCH) {
            xmb_cpu_draw_osk();
            xmb_cpu_draw_search_results();
        } else if (tab != XMB_TAB_MUSIC && tab != XMB_TAB_SETTINGS) {
            if (tab == XMB_TAB_TV && g_tv_depth > 0)
                xmb_cpu_draw_sub();
            else if (tab == XMB_TAB_COLLECTIONS && g_col_depth > 0)
                xmb_cpu_draw_col_sub();
            else
                xmb_cpu_draw_items(tab);
        }

        drawTTF(XMB_ITEM_PAD, 16, "JELLYFIN-PS3", 28, 0x007C3CEA, true);
        {
            char hints[32];
            snprintf(hints, sizeof(hints), "< L1   R1 >");
            int hx = (int)display_width - (int)(strlen(hints) * 8) - XMB_ITEM_PAD;
            drawTTF((u32)(hx > 0 ? hx : 0), 24, hints, 14, 0x00FFFFFF);
        }

        {
            const char *tab_name = g_tabs[g_active_tab].label;
            int hx = (int)display_width / 2 - ttf_text_width(tab_name, 28) / 2;
            if (hx < (int)XMB_ITEM_PAD) hx = (int)XMB_ITEM_PAD;
            drawTTF((u32)hx, (u32)(XMB_DIVIDER_Y + 14), tab_name, 28, 0x00FFFFFF);
        }

        if (tab == XMB_TAB_SEARCH) {
            xmb_rsx_draw_osk();
        } else if (tab == XMB_TAB_MUSIC || tab == XMB_TAB_SETTINGS) {
            drawTTF(XMB_ITEM_PAD, (u32)(XMB_CONTENT_Y + 40), "Coming soon", 22, 0x00FFFFFF);
        } else if (tab == XMB_TAB_TV && g_tv_depth > 0) {
            char crumb[256];
            if (g_tv_depth == 1)
                snprintf(crumb, sizeof(crumb), "%s > Seasons", g_tv_series_name);
            else
                snprintf(crumb, sizeof(crumb), "%s > %s > Episodes",
                         g_tv_series_name, g_tv_season_name);
            drawTTF(XMB_ITEM_PAD, (u32)(XMB_CONTENT_Y + 2), crumb, 14, 0x00888888);
            if (g_tv_sub_count == 0)
                drawTTF(XMB_ITEM_PAD, (u32)(XMB_CONTENT_Y + 46), "No items.", 16, 0x00FFFFFF);
            else
                xmb_draw_sub_list();
        } else if (tab == XMB_TAB_COLLECTIONS && g_col_depth > 0) {
            char crumb[256];
            snprintf(crumb, sizeof(crumb), "%s > Movies", g_col_name);
            drawTTF(XMB_ITEM_PAD, (u32)(XMB_CONTENT_Y + 2), crumb, 14, 0x00888888);
            if (g_col_sub_count == 0)
                drawTTF(XMB_ITEM_PAD, (u32)(XMB_CONTENT_Y + 46), "No items.", 16, 0x00FFFFFF);
            else
                xmb_draw_col_sub_list();
        } else {
            if (g_items_loaded[tab] && g_item_count[tab] == 0)
                drawTTF(XMB_ITEM_PAD, (u32)(XMB_CONTENT_Y + 20), "No items.", 16, 0x00FFFFFF);
            else
                xmb_draw_item_list(tab);
        }

        drawTTF(XMB_ITEM_PAD,
                (u32)(display_height - XMB_BOTTOM_PAD + 16),
                "X SELECT   O BACK   /\\ INFO", 21, 0x00FFFFFF);

        xmb_draw_tabs();

        flip();
        sysUtilCheckCallback();
    }
}

// -------------------------------------------------------
// Lifecycle
// -------------------------------------------------------

void ui_init(void) {
    rsxSetBlendFunc(context,
        GCM_SRC_ALPHA, GCM_ONE_MINUS_SRC_ALPHA,
        GCM_SRC_ALPHA, GCM_ONE_MINUS_SRC_ALPHA);
    rsxSetBlendEquation(context, GCM_FUNC_ADD, GCM_FUNC_ADD);
    rsxSetBlendEnable(context, GCM_TRUE);
    setRenderTarget(curr_fb);
    ttf_init();
    wave_init();
}

void ui_restore_rsx_state(void) {
    rsxSetBlendFunc(context,
        GCM_SRC_ALPHA, GCM_ONE_MINUS_SRC_ALPHA,
        GCM_SRC_ALPHA, GCM_ONE_MINUS_SRC_ALPHA);
    rsxSetBlendEquation(context, GCM_FUNC_ADD, GCM_FUNC_ADD);
    rsxSetBlendEnable(context, GCM_TRUE);
    rsxSetDepthTestEnable(context, GCM_FALSE);
    rsxSetDepthWriteEnable(context, GCM_FALSE);
    setRenderTarget(curr_fb);
}

void ui_cleanup(void) {
    visuals_cleanup();
}
