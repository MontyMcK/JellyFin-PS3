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
u64 g_info_cooldown_until = 0;

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

// -------------------------------------------------------
// XMB JSON parsing helpers (local, avoids changing jellyfin_api.cpp)
// -------------------------------------------------------

int xmb_json_str_range(const char *start, int len,
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

int xmb_json_int_range(const char *start, int len,
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

long long xmb_json_ll_range(const char *start, int len,
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

int xmb_json_first_arr_str(const char *start, int len,
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

int parse_xmb_items(const char *json, XMBItem *arr, int max) {
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
// XMB main loop
// -------------------------------------------------------

// Defined in ui/ui_nav.cpp
void xmb_detect_tabs(void);
void xmb_fetch_tab_items(int tab);
bool xmb_handle_input_browse(void);

// Defined in ui/ui_search.cpp
bool xmb_handle_input_search(void);

void ui_run_xmb(void) {
    memset(g_items, 0, sizeof(g_items));
    memset(g_item_count, 0, sizeof(g_item_count));
    memset(g_items_loaded, 0, sizeof(g_items_loaded));
    memset(g_tab_start, 0, sizeof(g_tab_start));
    memset(g_tab_total, 0, sizeof(g_tab_total));
    memset(g_tab_name_filter, 0, sizeof(g_tab_name_filter));
    g_jumpbar_active = false;
    g_jumpbar_sel    = 1;
    memset(g_search_buf, 0, sizeof(g_search_buf));
    g_search_results_count = 0;
    g_active_tab = XMB_TAB_MOVIES;
    g_sel = 0; g_scroll_top = 0;
    g_tv_sub_start = 0; g_tv_sub_total = 0;
    g_col_sub_start = 0; g_col_sub_total = 0;
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
        draw_topbar_lr();

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
            if (tab == XMB_TAB_MUSIC) xmb_draw_jumpbar(tab);
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
            xmb_draw_jumpbar(tab);
        }

        // Contextual hints bar
        {
            bool in_tv_sub  = (tab == XMB_TAB_TV          && g_tv_depth  > 0);
            bool in_col_sub = (tab == XMB_TAB_COLLECTIONS && g_col_depth > 0);

            if (tab == XMB_TAB_SETTINGS) {
                /* no interactive content — omit hints */
            } else if (tab == XMB_TAB_SEARCH) {
                if (g_search_focus_results) {
                    static const Hint h[] = {{'D',"NAV"},{'X',"PLAY"},{'C',"BACK"}};
                    draw_hints_bar(h, 3);
                } else {
                    static const Hint h[] = {{'D',"NAV"},{'X',"TYPE"},{'C',"CLEAR"}};
                    draw_hints_bar(h, 3);
                }
            } else if (in_tv_sub || in_col_sub) {
                static const Hint h[] = {{'D',"NAV"},{'X',"SELECT"},{'C',"BACK"}};
                draw_hints_bar(h, 3);
            } else if (g_jumpbar_active) {
                static const Hint h[] = {{'D',"NAV"},{'X',"FILTER"},{'C',"CANCEL"}};
                draw_hints_bar(h, 3);
            } else {
                static const Hint h[] = {{'D',"NAV"},{'E',"JUMP"},{'X',"SELECT"},{'C',"BACK"},{'T',"INFO"}};
                draw_hints_bar(h, 5);
            }
        }

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
