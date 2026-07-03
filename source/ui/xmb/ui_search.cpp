// XMB search tab — OSK input and Jellyfin search.

#include <stdio.h>
#include <string.h>

#include "ui_internal.h"
#include "jellyfin_api.h"
#include "player.h"
#include "plog.h"

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
bool g_search_focus_results = false;
int  g_search_sel           = 0;
int  g_search_scroll        = 0;
char g_search_buf[64];
int  g_search_results_count = 0;
XMBItem g_search_results[XMB_ITEMS_MAX];

int OSK_Y0 = 0;

static int osk_row_len(int r) {
    if (r >= OSK_ROWS_N)
        return 3;
    const char **rows = g_osk_sym ? OSK_SYMBOLS : OSK_LETTERS;
    int base = strlen(rows[r]);
    if (!g_osk_sym && r == OSK_ROWS_N - 1) base++;
    if  (g_osk_sym && r == OSK_ROWS_N - 1) base++;
    return base;
}

static char osk_current_char(void) {
    if (g_osk_row >= OSK_ROWS_N)
        return 0;
    const char **rows = g_osk_sym ? OSK_SYMBOLS : OSK_LETTERS;
    const char *row   = rows[g_osk_row];
    int base_len = strlen(row);
    bool is_toggle = (g_osk_row == OSK_ROWS_N-1 && g_osk_col == base_len);
    if (is_toggle) return 0;
    if (g_osk_col < base_len) return row[g_osk_col];
    return 0;
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

    snprintf(dbg, sizeof(dbg), "search status: %d count: %d", status, g_search_results_count);
    plog(dbg);

    {
        char dbg2[400];
        snprintf(dbg2, sizeof(dbg2),
            "search: term='%s' status=%d count=%d",
            g_search_buf, status, g_search_results_count);
        plog(dbg2);

        if (status != 200) {
            char errbuf[320];
            snprintf(errbuf, sizeof(errbuf), "search_err: %.300s", responseBuffer);
            plog(errbuf);
        } else if (g_search_results_count == 0) {
            char respbuf[224];
            snprintf(respbuf, sizeof(respbuf), "search_empty_resp: %.200s", responseBuffer);
            plog(respbuf);
        }
    }
}

bool xmb_handle_input_search(void) {
    char prev_buf[sizeof(g_search_buf)];
    strcpy(prev_buf, g_search_buf);

    if (BTN_PRESSED(l1)) { g_search_focus_results = false; xmb_switch_tab(xmb_next_enabled(g_active_tab, -1)); return false; }
    if (BTN_PRESSED(r1)) { g_search_focus_results = false; xmb_switch_tab(xmb_next_enabled(g_active_tab, +1)); return false; }
    if (BTN_PRESSED(circle) && !g_search_buf[0]) {
        xmb_switch_tab(xmb_next_enabled(g_active_tab, +1));
        return false;
    }
    if (BTN_PRESSED(circle)) { g_search_buf[0] = '\0'; g_search_results_count = 0; g_search_focus_results = false; return false; }

    int row_count = OSK_ROWS_N + 1;

    if (!g_search_focus_results) {
        if (BTN_REPEAT(up)) {
            if (g_osk_row == 0) {
                g_osk_row = row_count - 1;
            } else {
                g_osk_row--;
            }
            int ml = osk_row_len(g_osk_row);
            if (g_osk_col >= ml) g_osk_col = ml - 1;
        }
        if (BTN_REPEAT(down)) {
            if (g_osk_row == row_count - 1) {
                if (g_search_results_count > 0) {
                    g_search_focus_results = true;
                    g_search_sel    = 0;
                    g_search_scroll = 0;
                } else {
                    g_osk_row = 0;
                }
            } else {
                g_osk_row++;
                int ml = osk_row_len(g_osk_row);
                if (g_osk_col >= ml) g_osk_col = ml - 1;
            }
        }
        if (BTN_REPEAT(left)) {
            int ml = osk_row_len(g_osk_row);
            g_osk_col = (g_osk_col - 1 + ml) % ml;
        }
        if (BTN_REPEAT(right)) {
            int ml = osk_row_len(g_osk_row);
            g_osk_col = (g_osk_col + 1) % ml;
        }
    } else {
        if (BTN_REPEAT(up)) {
            if (g_search_sel == 0) {
                g_search_focus_results = false;
                g_osk_row = row_count - 1;
                int ml = osk_row_len(g_osk_row);
                if (g_osk_col >= ml) g_osk_col = ml - 1;
            } else {
                g_search_sel--;
                if (g_search_sel < g_search_scroll) g_search_scroll = g_search_sel;
            }
        }
        if (BTN_REPEAT(down)) {
            if (g_search_sel < g_search_results_count - 1) {
                g_search_sel++;
                if (g_search_sel >= g_search_scroll + 6)
                    g_search_scroll = g_search_sel - 5;
            }
        }
        if (BTN_PRESSED(cross) && g_search_sel < g_search_results_count) {
            const XMBItem *it = &g_search_results[g_search_sel];
            if (strcmp(it->type, "Episode") == 0)
                xmb_play_episode_with_next(it, 0);
            else
                xmb_play_item(it, 0);
            return false;
        }
    }

    if (!g_search_focus_results && BTN_PRESSED(cross)) {
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
                g_search_focus_results = false;
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

    if (strcmp(prev_buf, g_search_buf) != 0) {
        if (g_search_buf[0]) {
            xmb_do_search();
        } else {
            g_search_results_count = 0;
            g_search_focus_results = false;
        }
    }

    return false;
}
