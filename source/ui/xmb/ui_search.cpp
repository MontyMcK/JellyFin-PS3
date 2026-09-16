// XMB search tab — OSK input and Jellyfin search.

#include <stdio.h>
#include <string.h>

#include "ui_internal.h"
#include "jellyfin_api.h"
#include "player.h"
#include "plog.h"
#include "timing.h"

// Search is a BLOCKING http_request on the UI thread, and it used to run on
// every single keystroke: type "matrix" and that is six round trips, each one
// freezing the OSK until the server answers.  On top of feeling broken, the
// one-and-two-character queries in that sequence are the worst ones to send —
// Jellyfin matches them loosely, so results appeared, vanished and reappeared
// as the term grew, which is the "sometimes results, sometimes none" part.
//
// So: wait until typing PAUSES, then send one query, and never send a term
// too short to mean anything.  The pause is checked in the per-frame input
// handler, which is already called every frame.
// The wait is measured from the last PAD ACTIVITY, not the last letter.
// Timing it from the letter is why it still fired mid-word: picking the next
// key on an on-screen keyboard takes several d-pad presses, and none of those
// pushed the deadline back, so the query went out — and froze the UI — while
// the user was still walking to the next letter.  Any button activity now
// counts as "still typing".
#define SEARCH_DEBOUNCE_US 900000   // idle pause before the query fires
#define SEARCH_MIN_CHARS   2        // shorter terms are noise, not a search

static u64  s_search_edit_us = 0;   // when the term last changed
static bool s_search_pending = false;

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
                // Scroll against the rows that ACTUALLY fit.  This was a
                // hardcoded 6 while the renderer measured the screen and drew
                // as few as one, so the selection walked off the visible area
                // without ever scrolling.
                int vis = xmb_search_vis_rows();
                if (vis < 1) vis = 1;
                if (g_search_sel >= g_search_scroll + vis)
                    g_search_scroll = g_search_sel - vis + 1;
            }
        }
        // Triangle opens the info screen for a search hit, the same as it does
        // in the library grid — so a title found by search can be played from
        // a chosen version and quality instead of only the server's default.
        if (BTN_PRESSED(triangle) && g_search_sel < g_search_results_count &&
            timing_get_us() >= g_info_cooldown_until) {
            xmb_show_item_info(&g_search_results[g_search_sel]);
            return false;
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
            // Queue it; the query goes out once typing stops (see the note at
            // the top of this file).
            s_search_pending = true;
            s_search_edit_us = timing_get_us();
        } else {
            s_search_pending       = false;
            g_search_results_count = 0;
            g_search_focus_results = false;
        }
    }

    // Still working the keyboard — moving between keys, holding a direction,
    // deleting — so push the query back.  Held buttons count, which is what
    // keeps a long d-pad run from being read as a pause.
    if (s_search_pending &&
        (btn_cur.up || btn_cur.down || btn_cur.left || btn_cur.right ||
         btn_cur.cross || btn_cur.circle || btn_cur.square ||
         btn_cur.triangle || btn_cur.l1 || btn_cur.r1))
        s_search_edit_us = timing_get_us();

    if (s_search_pending &&
        timing_get_us() - s_search_edit_us >= SEARCH_DEBOUNCE_US) {
        s_search_pending = false;
        if ((int)strlen(g_search_buf) >= SEARCH_MIN_CHARS) {
            xmb_do_search();
        } else {
            // Too short to search on: show nothing rather than whatever a
            // one-letter query happens to match.
            g_search_results_count = 0;
            g_search_focus_results = false;
        }
    }

    return false;
}
