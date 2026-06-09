// Legacy text-UI library browser and search screens, plus show_main_menu.

#include <stdio.h>
#include <string.h>

#include <io/pad.h>
#include <sysutil/sysutil.h>

#include "jellyfin_api.h"
#include "ui.h"
#include "player.h"

// Defined in jellyfin_api.cpp
int json_get_int(const char *json, const char *key, int def);

// Returns: >=0 selected, -1 back, -2 prev page, -3 next page
static int show_list(JFItem *arr, int count, const char *title,
                     bool has_prev, bool has_next) {
    int sel = 0, top = 0;
    init_btns();

    while (running) {
        sysUtilCheckCallback();
        poll_buttons();
        if (BTN_REPEAT(up))   { if (sel > 0)         { sel--; if (sel < top)          top = sel; } }
        if (BTN_REPEAT(down)) { if (sel < count - 1) { sel++; if (sel >= top+JF_PAGE) top = sel-JF_PAGE+1; } }
        if (BTN_PRESSED(cross))              return sel;
        if (BTN_PRESSED(circle))             return -1;
        if (BTN_PRESSED(l1) && has_prev)     return -2;
        if (BTN_PRESSED(r1) && has_next)     return -3;

        drawHeader();
        drawTextf(40, 65, "%.72s", title);
        if (count == 0) drawText(40, 87, "(no items)");

        int y = 87;
        for (int i = top; i < top+JF_PAGE && i < count; i++) {
            char line[80];
            const char *pfx = is_container(arr[i].type) ? ">" : "";
            if (i == sel) snprintf(line, sizeof(line), "[%s%.62s]", pfx, arr[i].name);
            else          snprintf(line, sizeof(line), " %s%.63s",  pfx, arr[i].name);
            drawText(40, y, line);
            y += LINE_HEIGHT;
        }

        char footer[128];
        snprintf(footer, sizeof(footer), "X:open  O:back  %d/%d%s%s",
                 count ? sel+1 : 0, count,
                 has_prev ? "  L1:prev" : "",
                 has_next ? "  R1:next" : "");
        drawText(40, 660, footer);
        flip();
    }
    return -1;
}

static void browse_level(const char *parent_id, const char *title, int depth) {
    if (depth > 6 || !running) return;

    int  start_index = 0;
    int  total_count = 0;
    bool need_fetch  = true;
    JFItem items[JF_MAX];
    int  count = 0;

    while (running) {
        if (need_fetch) {
            drawHeader();
            drawTextf(40, 100, "Loading %.60s...", title);
            flip();

            char url[512];
            if (!parent_id || !parent_id[0])
                snprintf(url, sizeof(url), "%s/Users/%s/Views", g_server, g_userid);
            else
                snprintf(url, sizeof(url),
                    "%s/Users/%s/Items?ParentId=%s&StartIndex=%d&Limit=%d"
                    "&SortBy=SortName&SortOrder=Ascending",
                    g_server, g_userid, parent_id, start_index, JF_MAX);

            int status = http_request(0, url, NULL, g_token, responseBuffer, RESPONSE_SIZE);
            count = 0;
            if (status == 200) {
                count       = parse_jf_items(responseBuffer, items, JF_MAX);
                total_count = json_get_int(responseBuffer, "TotalRecordCount", start_index + count);
            } else {
                drawHeader();
                drawTextf(40, 100, "Error loading items: %d", status);
                drawText(40, 130, "O: back");
                flip();
                init_btns();
                while (running) {
                    sysUtilCheckCallback();
                    poll_buttons();
                    if (BTN_PRESSED(circle)) return;
                }
                return;
            }
            need_fetch = false;
        }

        char page_title[128];
        if (total_count > JF_MAX)
            snprintf(page_title, sizeof(page_title), "%.46s [%d-%d/%d]",
                     title, start_index+1, start_index+count, total_count);
        else
            snprintf(page_title, sizeof(page_title), "%.72s", title);

        bool has_prev = (start_index > 0);
        bool has_next = (start_index + count < total_count);

        int sel = show_list(items, count, page_title, has_prev, has_next);

        if (sel == -1) return;
        if (sel == -2) { start_index -= JF_MAX; if (start_index < 0) start_index = 0; need_fetch = true; continue; }
        if (sel == -3) { start_index += JF_MAX;                                        need_fetch = true; continue; }

        if (is_container(items[sel].type))
            browse_level(items[sel].id, items[sel].name, depth + 1);
        else
            show_player(&items[sel]);
    }
}

void show_library_browser(void) {
    if (!g_userid[0]) {
        drawHeader();
        drawText(40, 100, "No user ID - please log out and log in again.");
        drawText(40, 130, "O: back");
        flip();
        init_btns();
        while (running) {
            sysUtilCheckCallback();
            poll_buttons();
            if (BTN_PRESSED(circle)) return;
        }
        return;
    }
    browse_level(NULL, "Libraries", 0);
}

void show_search(void) {
    char query[64] = "";
    if (get_input(query, sizeof(query), "Search movies/shows", false) != 1) return;
    if (!query[0]) return;

    drawHeader();
    drawTextf(40, 100, "Searching: %.60s", query);
    flip();

    char encoded[192];
    url_encode_query(query, encoded, sizeof(encoded));

    char url[512];
    snprintf(url, sizeof(url),
        "%s/Users/%s/Items?searchTerm=%s&Recursive=true"
        "&IncludeItemTypes=Movie,Series,Episode"
        "&Limit=%d&SortBy=SortName&SortOrder=Ascending",
        g_server, g_userid, encoded, JF_MAX);

    int status = http_request(0, url, NULL, g_token, responseBuffer, RESPONSE_SIZE);
    JFItem items[JF_MAX]; int count = 0;
    if (status == 200) {
        count = parse_jf_items(responseBuffer, items, JF_MAX);
    } else {
        drawHeader();
        drawTextf(40, 100, "Search failed: %d", status);
        drawText(40, 130, "O: back");
        flip();
        init_btns();
        while (running) {
            sysUtilCheckCallback();
            poll_buttons();
            if (BTN_PRESSED(circle)) return;
        }
        return;
    }

    while (running) {
        char result_title[96];
        snprintf(result_title, sizeof(result_title), "%.46s (%d results)", query, count);
        int sel = show_list(items, count, result_title, false, false);
        if (sel < 0) return;
        show_player(&items[sel]);
    }
}

void show_main_menu(void) {
    ui_run_xmb();
}
