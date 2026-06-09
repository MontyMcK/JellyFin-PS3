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
    btn_cur.l2       = pad->BTN_L2;
    btn_cur.r2       = pad->BTN_R2;
}

bool poll_buttons(void) {
    padInfo pi;
    ioPadGetInfo(&pi);
    padData merged; memset(&merged, 0, sizeof(merged));
    bool any = false;
    for (int i = 0; i < MAX_PADS; i++) {
        if (!pi.status[i]) continue;
        padData pd;
        ioPadGetData(i, &pd);
        if (!pd.len) continue;
        merged.BTN_UP       |= pd.BTN_UP;
        merged.BTN_DOWN     |= pd.BTN_DOWN;
        merged.BTN_LEFT     |= pd.BTN_LEFT;
        merged.BTN_RIGHT    |= pd.BTN_RIGHT;
        merged.BTN_CROSS    |= pd.BTN_CROSS;
        merged.BTN_CIRCLE   |= pd.BTN_CIRCLE;
        merged.BTN_SQUARE   |= pd.BTN_SQUARE;
        merged.BTN_TRIANGLE |= pd.BTN_TRIANGLE;
        merged.BTN_START    |= pd.BTN_START;
        merged.BTN_SELECT   |= pd.BTN_SELECT;
        merged.BTN_L1       |= pd.BTN_L1;
        merged.BTN_R1       |= pd.BTN_R1;
        merged.BTN_L2       |= pd.BTN_L2;
        merged.BTN_R2       |= pd.BTN_R2;
        any = true;
    }
    // If no pad delivered a fresh packet this frame, keep the last known button
    // state (the controller simply hasn't sent new data yet) — exactly like a game
    // does, so a held trigger reads as continuously held instead of flickering.
    // Still advance btn_prev so the BTN_PRESSED edge doesn't re-fire every idle
    // frame (which would spam taps/pause for any held button).
    if (!any) { btn_prev = btn_cur; return false; }
    merged.len = 1;
    update_buttons(&merged);
    return any;
}

void init_btns(void) {
    poll_buttons();
    btn_prev = btn_cur;
}

// Auto-repeat for held navigation buttons.  Returns true on the initial press,
// then every NAV_REPEAT_US after an initial NAV_DELAY_US for as long as the button
// is held — giving menus a steady, controllable scroll instead of one-per-tap.
bool btn_nav_repeat(bool held, int slot) {
    static u64  next_us[NAV_REPEAT_SLOTS] = { 0 };
    static bool active[NAV_REPEAT_SLOTS]  = { false };
    if (slot < 0 || slot >= NAV_REPEAT_SLOTS) return false;
    if (!held) { active[slot] = false; return false; }
    u64 now = timing_get_us();
    if (!active[slot]) {                       // first press
        active[slot]  = true;
        next_us[slot] = now + NAV_DELAY_US;
        return true;
    }
    if (now >= next_us[slot]) {                // repeat tick
        next_us[slot] = now + NAV_REPEAT_US;
        return true;
    }
    return false;
}

// -------------------------------------------------------
// On-screen keyboard (login / text entry)
//
// Matches the look of the XMB search bar OSK, but adds an on-screen SHIFT key
// (iOS/Android style) for upper/lower case plus a #+= key for symbols, so any
// username, password, or URL — including special characters — can be typed.
// -------------------------------------------------------

namespace {

enum OKind { OK_CHAR, OK_SHIFT, OK_SYM, OK_BACK, OK_SPACE, OK_ENTER };

struct OKey {
    OKind       kind;
    char        ch;     // OK_CHAR: the already-cased character to insert
    const char *label;  // non-char keys: button caption
    int         cols;   // width in OSK step units
};

struct ORow {
    OKey keys[12];
    int  n;
};

const int   OSK_MAX_ROWS = 6;       // letters use 5 rows, symbols use 6
const float OSK_LBL_PX   = 31.5f;   // 150% of the 21px search-bar OSK labels

OKey okey(OKind kind, char ch, const char *label, int cols) {
    OKey k; k.kind = kind; k.ch = ch; k.label = label; k.cols = cols;
    return k;
}

// Build the keyboard layout for the current mode (symbols vs letters) and case.
// Returns the number of rows used (including the bottom action row).
int osk_build(ORow rows[OSK_MAX_ROWS], bool sym, bool caps) {
    for (int i = 0; i < OSK_MAX_ROWS; i++) rows[i].n = 0;

    int nr;   // number of content rows (action row is appended after)
    if (!sym) {
        static const char *R0 = "1234567890";
        static const char *R1 = "qwertyuiop";
        static const char *R2 = "asdfghjkl";
        static const char *R3 = "zxcvbnm";
        for (const char *p = R0; *p; p++) rows[0].keys[rows[0].n++] = okey(OK_CHAR, *p, 0, 1);
        for (const char *p = R1; *p; p++) rows[1].keys[rows[1].n++] = okey(OK_CHAR, caps ? (char)toupper(*p) : *p, 0, 1);
        for (const char *p = R2; *p; p++) rows[2].keys[rows[2].n++] = okey(OK_CHAR, caps ? (char)toupper(*p) : *p, 0, 1);
        rows[3].keys[rows[3].n++] = okey(OK_SHIFT, 0, "CAPS", 2);
        for (const char *p = R3; *p; p++) rows[3].keys[rows[3].n++] = okey(OK_CHAR, caps ? (char)toupper(*p) : *p, 0, 1);
        rows[3].keys[rows[3].n++] = okey(OK_BACK, 0, "<", 2);
        nr = 4;
    } else {
        static const char *R0 = "1234567890";   // numbers row on the symbols page too
        static const char *R1 = "!@#$%^&*()";
        static const char *R2 = "-_=+[]{}|\\";
        static const char *R3 = ":;\"'`~<>?";
        static const char *R4 = ".,/?";
        for (const char *p = R0; *p; p++) rows[0].keys[rows[0].n++] = okey(OK_CHAR, *p, 0, 1);
        for (const char *p = R1; *p; p++) rows[1].keys[rows[1].n++] = okey(OK_CHAR, *p, 0, 1);
        for (const char *p = R2; *p; p++) rows[2].keys[rows[2].n++] = okey(OK_CHAR, *p, 0, 1);
        for (const char *p = R3; *p; p++) rows[3].keys[rows[3].n++] = okey(OK_CHAR, *p, 0, 1);
        for (const char *p = R4; *p; p++) rows[4].keys[rows[4].n++] = okey(OK_CHAR, *p, 0, 1);
        rows[4].keys[rows[4].n++] = okey(OK_BACK, 0, "<", 2);
        nr = 5;
    }
    rows[nr].keys[rows[nr].n++] = okey(OK_SYM,   0, sym ? "ABC" : "#+=", 2);
    rows[nr].keys[rows[nr].n++] = okey(OK_SPACE, 0, "SPACE", 5);
    rows[nr].keys[rows[nr].n++] = okey(OK_ENTER, 0, "ENTER", 3);
    return nr + 1;
}

int orow_units(const ORow *r) {
    int u = 0;
    for (int i = 0; i < r->n; i++) u += r->keys[i].cols;
    return u;
}

void osk_draw(const char *prompt, const char *input, bool is_password,
              const ORow rows[OSK_MAX_ROWS], int nrows, int sr, int sc, bool caps) {
    int W       = (int)display_width;
    int total_w = 10 * OSK_STEP_X - OSK_GAP;
    int barx    = (W - total_w) / 2;
    int y0      = XMB_CONTENT_Y + 58;

    waitflip();
    clearScreen(XMB_BG);
    wave_draw();
    rsxSync();

    // Divider under the title bar.
    u32 *div = color_buffer[curr_fb] + (u32)XMB_DIVIDER_Y * display_width;
    for (u32 x = 0; x < display_width; x++) div[x] = XMB_DIVIDER_CLR;

    // Input field background.
    drawRect((u32)barx, (u32)(XMB_CONTENT_Y + 8), (u32)total_w, 40, 0x001A1040);

    // Key cells (CPU rects).
    for (int r = 0; r < nrows; r++) {
        int rw = orow_units(&rows[r]) * OSK_STEP_X - OSK_GAP;
        int cx = (W - rw) / 2;
        int ry = y0 + r * OSK_STEP_Y;
        for (int c = 0; c < rows[r].n; c++) {
            const OKey *k = &rows[r].keys[c];
            int kw  = k->cols * OSK_STEP_X - OSK_GAP;
            u32 col = (r == sr && c == sc) ? XMB_KEY_SEL : XMB_KEY_NORMAL;
            if (k->kind == OK_SHIFT && caps) col = XMB_ACCENT;
            drawRect((u32)cx, (u32)ry, (u32)kw, OSK_KEY_H, col);
            cx += k->cols * OSK_STEP_X;
        }
    }

    // Title + prompt (RSX).
    drawTTF(XMB_ITEM_PAD, 16, "JELLYFIN-PS3", 28, XMB_ACCENT);
    {
        int pw = ttf_text_width(prompt, 24);
        int px = W / 2 - pw / 2;
        if (px < (int)XMB_ITEM_PAD) px = (int)XMB_ITEM_PAD;
        drawTTF((u32)px, (u32)(XMB_DIVIDER_Y + 14), prompt, 24, 0x00FFFFFF);
    }

    // Current text (masked for passwords) with a blinking cursor.
    {
        char shown[80];
        int  ilen = strlen(input);
        if (is_password) {
            int n = ilen > 60 ? 60 : ilen;
            memset(shown, '*', n);
            shown[n] = '\0';
        } else {
            const char *src = ilen > 60 ? input + ilen - 60 : input;
            snprintf(shown, sizeof(shown), "%s", src);
        }
        bool cur = ((timing_get_us() / 500000) & 1) == 0;
        char disp[96];
        snprintf(disp, sizeof(disp), "%s%s", shown, cur ? "_" : " ");
        const float typed_px = 27.0f;   // 150% of the original 18px
        int tw = ttf_text_width(disp, typed_px);
        int tx = W / 2 - tw / 2;
        if (tx < barx + 6) tx = barx + 6;
        drawTTF((u32)tx, (u32)(XMB_CONTENT_Y + 12), disp, typed_px, 0x00FFFFFF);
    }

    // Key labels (RSX).
    for (int r = 0; r < nrows; r++) {
        int rw = orow_units(&rows[r]) * OSK_STEP_X - OSK_GAP;
        int cx = (W - rw) / 2;
        int ry = y0 + r * OSK_STEP_Y + (OSK_KEY_H - (int)OSK_LBL_PX) / 2;
        for (int c = 0; c < rows[r].n; c++) {
            const OKey *k = &rows[r].keys[c];
            int  kw = k->cols * OSK_STEP_X - OSK_GAP;
            char chbuf[2] = { k->ch, '\0' };
            const char *lbl = (k->kind == OK_CHAR) ? chbuf : k->label;
            int lw = ttf_text_width(lbl, OSK_LBL_PX);
            int lx = cx + (kw - lw) / 2;
            drawTTF((u32)lx, (u32)ry, lbl, OSK_LBL_PX, 0x00FFFFFF);
            cx += k->cols * OSK_STEP_X;
        }
    }

    // Hints.
    static const Hint hints[] = {{'D',"MOVE"},{'X',"SELECT"},{'C',"CANCEL"}};
    draw_hints_bar(hints, 3);

    flip();
}

} // namespace

int get_input(char *out, int max_len, const char *prompt, bool is_password) {
    out[0] = '\0';

    int  sr = 1, sc = 0;            // start on the top letter row
    bool caps = false, sym = false;

    init_btns();

    while (running) {
        sysUtilCheckCallback();
        poll_buttons();

        ORow rows[OSK_MAX_ROWS];
        int  nrows = osk_build(rows, sym, caps);

        // Navigation.
        if (BTN_REPEAT(up)) {
            sr = (sr - 1 + nrows) % nrows;
            if (sc >= rows[sr].n) sc = rows[sr].n - 1;
        }
        if (BTN_REPEAT(down)) {
            sr = (sr + 1) % nrows;
            if (sc >= rows[sr].n) sc = rows[sr].n - 1;
        }
        if (BTN_REPEAT(left))  sc = (sc - 1 + rows[sr].n) % rows[sr].n;
        if (BTN_REPEAT(right)) sc = (sc + 1) % rows[sr].n;

        // Button shortcuts: Square = backspace, Start = confirm, Select/Circle = cancel.
        if (BTN_PRESSED(square)) { int l = strlen(out); if (l > 0) out[l-1] = '\0'; }
        if (BTN_PRESSED(start))  return 1;
        if (BTN_PRESSED(select) || BTN_PRESSED(circle)) return -1;

        // Activate the highlighted key.
        if (BTN_PRESSED(cross)) {
            const OKey *k = &rows[sr].keys[sc];
            switch (k->kind) {
            case OK_CHAR: {
                int l = strlen(out);
                if (l < max_len - 1) { out[l] = k->ch; out[l+1] = '\0'; }
                break;
            }
            case OK_SHIFT: caps = !caps; break;
            case OK_SYM:   sym = !sym; sc = 0; break;
            case OK_BACK:  { int l = strlen(out); if (l > 0) out[l-1] = '\0'; break; }
            case OK_SPACE: { int l = strlen(out); if (l < max_len - 1) { out[l] = ' '; out[l+1] = '\0'; } break; }
            case OK_ENTER: return 1;
            }
            // The layout (and row count) may have changed; re-clamp selection.
            nrows = osk_build(rows, sym, caps);
            if (sr >= nrows)      sr = nrows - 1;
            if (sc >= rows[sr].n) sc = rows[sr].n - 1;
            if (sc < 0)           sc = 0;
        }

        nrows = osk_build(rows, sym, caps);
        osk_draw(prompt, out, is_password, rows, nrows, sr, sc, caps);
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

// Settings tab state
int  g_settings_sel     = 0;
bool g_settings_confirm = false;

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
    g_settings_sel     = 0;
    g_settings_confirm = false;
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

    while (running) {
        waitflip();
        sysUtilCheckCallback();
        clearScreen(XMB_BG);
        wave_draw();

        int tab = g_active_tab;
        if (tab != XMB_TAB_SEARCH && tab != XMB_TAB_MUSIC && tab != XMB_TAB_SETTINGS)
            if (!g_items_loaded[tab]) xmb_fetch_tab_items(tab);

        poll_buttons();
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
        } else if (tab == XMB_TAB_SETTINGS) {
            xmb_cpu_draw_settings();
        } else if (tab != XMB_TAB_MUSIC) {
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
        } else if (tab == XMB_TAB_SETTINGS) {
            xmb_draw_settings();
        } else if (tab == XMB_TAB_MUSIC) {
            drawTTF(XMB_ITEM_PAD, (u32)(XMB_CONTENT_Y + 40), "Coming soon", 22, 0x00FFFFFF);
            xmb_draw_jumpbar(tab);
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
                if (g_settings_confirm) {
                    static const Hint h[] = {{'X',"CONFIRM"},{'C',"CANCEL"}};
                    draw_hints_bar(h, 2);
                } else {
                    static const Hint h[] = {{'D',"NAV"},{'X',"SELECT"}};
                    draw_hints_bar(h, 2);
                }
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
