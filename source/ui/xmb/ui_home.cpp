// Home shelf screen — a vertically-scrolling stack of horizontally-scrolling
// rows (Continue Watching, Next Up, Recently Added Movies/Shows, and a Music
// "coming soon" stub), mirroring the Jellyfin web home.  Each row carries its
// own card aspect ratio.  Card images / selection are drawn in the CPU phase
// (xmb_draw_card, shared with the grid); titles / chevrons in the text phase.

#include <stdio.h>
#include <string.h>

#include "ui_internal.h"
#include "ui_render_internal.h"
#include "jellyfin_api.h"

// -------------------------------------------------------
// Model
// -------------------------------------------------------

#define HOME_ROW_MAX 25

typedef enum { HROW_LANDSCAPE, HROW_PORTRAIT, HROW_STUB } HomeRowKind;

typedef struct {
    const char *title;
    HomeRowKind kind;
    XMBItem     items[HOME_ROW_MAX];
    int         count;
    int         scroll;   // leftmost visible card index
    bool        loaded;
} HomeRow;

enum { HR_CONTINUE, HR_NEXTUP, HR_MOVIES, HR_SHOWS, HR_MUSIC, HOME_ROWS_N };

static HomeRow s_rows[HOME_ROWS_N];
static int     s_focus_row = 0;
static int     s_focus_col = 0;
static int     s_vscroll   = 0;   // pixels the stack is scrolled up
static bool    s_inited    = false;

// -------------------------------------------------------
// Layout metrics (resolution-independent, computed each call)
// -------------------------------------------------------

#define HOME_HEADER_H 30          // row-title band above the cards
#define HOME_LABEL_H  26          // card-title band beneath the cards
#define HOME_ROW_GAP  14          // gap between rows
#define HOME_SIDE_PAD XMB_ITEM_PAD
#define HOME_CARD_GAP XMB_CARD_GAP_X

static int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
static int view_top(void) { return XMB_CONTENT_Y; }
static int view_bot(void) { return (int)display_height - XMB_BOTTOM_PAD; }

static int row_card_h(HomeRowKind k) {
    int vh = view_bot() - view_top();
    // Tuned so ~5 landscape stills / ~8 portrait posters fit per row at 1080p,
    // and ~3 rows are visible before vertical scrolling.
    return (k == HROW_PORTRAIT) ? clampi(vh * 44 / 100, 160, 280)
                                : clampi(vh * 24 / 100, 110, 175);
}
static int row_card_w(HomeRowKind k) {
    int h = row_card_h(k);
    return (k == HROW_PORTRAIT) ? h * 2 / 3 : h * 16 / 9;
}
static int row_band_h(HomeRowKind k) {
    return HOME_HEADER_H + row_card_h(k) + HOME_LABEL_H + HOME_ROW_GAP;
}
// Absolute top of row r (as if s_vscroll == 0).
static int row_abs_top(int r) {
    int y = view_top();
    for (int i = 0; i < r; i++) y += row_band_h(s_rows[i].kind);
    return y;
}
static int row_visible_cols(HomeRowKind k) {
    int per   = row_card_w(k) + HOME_CARD_GAP;
    int avail = (int)display_width - 2 * HOME_SIDE_PAD + HOME_CARD_GAP;
    int n     = avail / per;
    return n < 1 ? 1 : n;
}
// Pixel width spanned by a full row of `vis` cards.
static int row_strip_w(HomeRowKind k) {
    int vis = row_visible_cols(k);
    return vis * row_card_w(k) + (vis - 1) * HOME_CARD_GAP;
}
// Left edge of the card strip, centered on screen (like the library grid)
// rather than pinned to the side pad — otherwise the cards never fill the
// width and the whole row reads as shoved to the left.  Shared by the title,
// cards, labels and chevrons so they all move together.
static int row_origin_x(HomeRowKind k) {
    int x0 = ((int)display_width - row_strip_w(k)) / 2;
    return x0 < HOME_SIDE_PAD ? HOME_SIDE_PAD : x0;
}

// -------------------------------------------------------
// Init + fetch
// -------------------------------------------------------

static void home_init_once(void) {
    if (s_inited) return;
    s_inited = true;
    memset(s_rows, 0, sizeof(s_rows));
    s_rows[HR_CONTINUE].title = "Continue Watching";        s_rows[HR_CONTINUE].kind = HROW_LANDSCAPE;
    s_rows[HR_NEXTUP].title   = "Next Up";                  s_rows[HR_NEXTUP].kind   = HROW_LANDSCAPE;
    s_rows[HR_MOVIES].title   = "Recently Added in Movies"; s_rows[HR_MOVIES].kind   = HROW_PORTRAIT;
    s_rows[HR_SHOWS].title    = "Recently Added in Shows";  s_rows[HR_SHOWS].kind    = HROW_PORTRAIT;
    s_rows[HR_MUSIC].title    = "Music";                    s_rows[HR_MUSIC].kind    = HROW_STUB;
}

static void home_fetch_row(int r) {
    HomeRow *row = &s_rows[r];
    char url[512];
    const char *fields = "Genres,RunTimeTicks,ProductionYear,Container";

    if (r == HR_CONTINUE) {
        snprintf(url, sizeof(url),
            "%s/Users/%s/Items/Resume?Limit=%d&Recursive=true&MediaTypes=Video&Fields=%s",
            g_server, g_userid, HOME_ROW_MAX, fields);
    } else if (r == HR_NEXTUP) {
        snprintf(url, sizeof(url),
            "%s/Shows/NextUp?userId=%s&Limit=%d&Fields=%s",
            g_server, g_userid, HOME_ROW_MAX, fields);
    } else if (r == HR_MOVIES || r == HR_SHOWS) {
        int   tab  = (r == HR_MOVIES) ? XMB_TAB_MOVIES : XMB_TAB_TV;
        const char *lib  = g_tabs[tab].library_id;
        const char *type = (r == HR_MOVIES) ? "Movie" : "Series";
        if (!lib[0]) { row->count = 0; row->loaded = true; return; }
        snprintf(url, sizeof(url),
            "%s/Users/%s/Items?ParentId=%s&IncludeItemTypes=%s&Recursive=true"
            "&SortBy=DateCreated&SortOrder=Descending&Limit=%d&Fields=%s",
            g_server, g_userid, lib, type, HOME_ROW_MAX, fields);
    } else {
        row->loaded = true;   // stub
        return;
    }

    int status = http_request(0, url, NULL, g_token, responseBuffer, RESPONSE_SIZE);
    row->count = (status == 200)
               ? parse_xmb_items(responseBuffer, row->items, HOME_ROW_MAX) : 0;
    row->loaded = true;
    if (row->scroll > row->count) row->scroll = 0;
}

// Fetch at most one pending row per frame so the UI fills in progressively
// instead of stalling on several blocking HTTP calls up front.
static void home_step_load(void) {
    for (int r = 0; r < HOME_ROWS_N; r++) {
        if (s_rows[r].kind == HROW_STUB) continue;
        if (!s_rows[r].loaded) { home_fetch_row(r); return; }
    }
}

void xmb_home_on_enter(void) {
    home_init_once();
    s_focus_row = 0; s_focus_col = 0; s_vscroll = 0;
    // Continue Watching + Next Up change after every playback — refetch them.
    s_rows[HR_CONTINUE].loaded = false; s_rows[HR_CONTINUE].scroll = 0;
    s_rows[HR_NEXTUP].loaded   = false; s_rows[HR_NEXTUP].scroll   = 0;
}

// -------------------------------------------------------
// Focus / scroll bookkeeping
// -------------------------------------------------------

static void ensure_row_visible(void) {
    int top  = row_abs_top(s_focus_row);
    int band = row_band_h(s_rows[s_focus_row].kind);
    if (top - s_vscroll < view_top())              s_vscroll = top - view_top();
    if (top + band - s_vscroll > view_bot())       s_vscroll = top + band - view_bot();
    if (s_vscroll < 0) s_vscroll = 0;
}
static void ensure_col_visible(void) {
    HomeRow *row = &s_rows[s_focus_row];
    if (row->kind == HROW_STUB) { row->scroll = 0; return; }
    int vis = row_visible_cols(row->kind);
    if (s_focus_col < row->scroll)             row->scroll = s_focus_col;
    if (s_focus_col > row->scroll + vis - 1)   row->scroll = s_focus_col - vis + 1;
    if (row->scroll < 0) row->scroll = 0;
}
static void clamp_col(void) {
    HomeRow *row = &s_rows[s_focus_row];
    int maxc = (row->kind == HROW_STUB || row->count == 0) ? 0 : row->count - 1;
    if (s_focus_col > maxc) s_focus_col = maxc;
    if (s_focus_col < 0)    s_focus_col = 0;
    ensure_col_visible();
}

// -------------------------------------------------------
// Rendering
// -------------------------------------------------------

static void home_clip_text(int x, int y, const char *s, float px, u32 color,
                           int max_w, bool bold = false) {
    if (ttf_text_width(s, px, bold) <= max_w) { drawTTF((u32)x, (u32)y, s, px, color, bold); return; }
    char buf[160]; snprintf(buf, sizeof(buf), "%s", s);
    int len = (int)strlen(buf);
    while (len > 1) {
        buf[--len] = '\0';
        char t[164]; snprintf(t, sizeof(t), "%s..", buf);
        if (ttf_text_width(t, px, bold) <= max_w) { drawTTF((u32)x, (u32)y, t, px, color, bold); return; }
    }
}

static void home_sel_frame(int cx, int cy, int w, int h) {
    const int T = 2, G = 2, O = G + T;
    drawRect((u32)(cx - O), (u32)(cy - O),     (u32)(w + 2*O), T, XMB_KEY_SEL);
    drawRect((u32)(cx - O), (u32)(cy + h + G), (u32)(w + 2*O), T, XMB_KEY_SEL);
    drawRect((u32)(cx - O), (u32)(cy - G),     T, (u32)(h + 2*G), XMB_KEY_SEL);
    drawRect((u32)(cx + w + G), (u32)(cy - G), T, (u32)(h + 2*G), XMB_KEY_SEL);
}

// True when row r's card band touches the viewport (cheap vertical cull).
static bool row_on_screen(int r, int *out_vy, int *out_card_y) {
    int vy     = row_abs_top(r) - s_vscroll;
    int card_y = vy + HOME_HEADER_H;
    if (out_vy) *out_vy = vy;
    if (out_card_y) *out_card_y = card_y;
    int ch = row_card_h(s_rows[r].kind);
    return !(card_y + ch < view_top() || vy > view_bot());
}

void xmb_home_cpu_phase(void) {
    home_init_once();
    home_step_load();

    // Rows scroll smoothly by pixels, so a row leaving the top/bottom is drawn
    // partly outside the content band.  Scissor cards (and their text below) to
    // the band so they never bleed over the tab bar or the hints bar.
    g_cpu_clip_top = view_top();
    g_cpu_clip_bot = view_bot();

    for (int r = 0; r < HOME_ROWS_N; r++) {
        HomeRow *row = &s_rows[r];
        int card_y;
        if (!row_on_screen(r, NULL, &card_y)) continue;
        int cw = row_card_w(row->kind), ch = row_card_h(row->kind);
        int x0 = row_origin_x(row->kind);

        if (row->kind == HROW_STUB) {
            drawRect((u32)x0, (u32)card_y, (u32)cw, (u32)ch, XMB_THUMB_DIM);
            if (r == s_focus_row) home_sel_frame(x0, card_y, cw, ch);
            continue;
        }

        int vis = row_visible_cols(row->kind);
        for (int c = row->scroll; c < row->scroll + vis && c < row->count; c++) {
            int cx = x0 + (c - row->scroll) * (cw + HOME_CARD_GAP);
            bool sel = (r == s_focus_row && c == s_focus_col);
            xmb_draw_card(row->items[c].id, cx, card_y, cw, ch,
                          row->items[c].progress_pct, sel);
        }
    }

    g_cpu_clip_top = 0; g_cpu_clip_bot = 0;
}

void xmb_home_text_phase(void) {
    home_init_once();

    g_cpu_clip_top = view_top();
    g_cpu_clip_bot = view_bot();

    for (int r = 0; r < HOME_ROWS_N; r++) {
        HomeRow *row = &s_rows[r];
        int vy, card_y;
        if (!row_on_screen(r, &vy, &card_y)) continue;
        int cw = row_card_w(row->kind), ch = row_card_h(row->kind);
        int x0 = row_origin_x(row->kind);
        bool row_focused = (r == s_focus_row);

        // Row header.
        drawTTF((u32)x0, (u32)(vy + 4), row->title, 18,
                row_focused ? XMB_TEXT : XMB_TEXT_DIM, true);

        if (row->kind == HROW_STUB) {
            const char *msg = "Coming soon";
            int tw = ttf_text_width(msg, 16);
            drawTTF((u32)(x0 + (cw - tw) / 2),
                    (u32)(card_y + ch / 2 - 8), msg, 16, XMB_TEXT_FAINT);
            continue;
        }

        if (row->count == 0) {
            // drawTTF is Latin-1, so plain "..." (no UTF-8 ellipsis).
            const char *msg = row->loaded ? "Nothing here yet" : "Loading...";
            drawTTF((u32)x0, (u32)(card_y + ch / 2 - 8), msg, 15, XMB_TEXT_FAINT);
            continue;
        }

        int vis = row_visible_cols(row->kind);
        for (int c = row->scroll; c < row->scroll + vis && c < row->count; c++) {
            int cx = x0 + (c - row->scroll) * (cw + HOME_CARD_GAP);
            int ty = card_y + ch + 5;
            bool sel = (row_focused && c == s_focus_col);
            home_clip_text(cx, ty, row->items[c].name, 15,
                           sel ? XMB_WHITE : XMB_TEXT_DIM, cw, sel);
            if (sel && row->items[c].year_str[0])
                drawTTF((u32)cx, (u32)(ty + 19), row->items[c].year_str, 13, XMB_TEXT_FAINT);
        }

        // Off-screen content chevrons, just outside the centered strip.
        int cev_y = card_y + ch / 2 - 11;
        if (row->scroll > 0)
            drawIcon((u32)(x0 - 26), (u32)cev_y, 0xE5CB, 22, XMB_ICON_IDLE);
        if (row->scroll + vis < row->count)
            drawIcon((u32)(x0 + row_strip_w(row->kind) + 4), (u32)cev_y,
                     0xE5CC, 22, XMB_ICON_IDLE);
    }

    g_cpu_clip_top = 0; g_cpu_clip_bot = 0;
}

// -------------------------------------------------------
// Input
// -------------------------------------------------------

static void home_activate(void) {
    HomeRow *row = &s_rows[s_focus_row];
    if (row->kind == HROW_STUB || row->count == 0 || s_focus_col >= row->count) return;
    XMBItem *it = &row->items[s_focus_col];

    // A show opens the existing TV Series -> Seasons -> Episodes sub-screen.
    if (s_focus_row == HR_SHOWS && strcmp(it->type, "Series") == 0) {
        if (!g_tabs[XMB_TAB_TV].enabled) return;
        g_active_tab = XMB_TAB_TV;
        strncpy(g_tv_series_id,   it->id,   sizeof(g_tv_series_id)-1);
        strncpy(g_tv_series_name, it->name, sizeof(g_tv_series_name)-1);
        g_tv_sub_start = 0; g_tv_sub_total = 0;
        g_tv_sub_count = xmb_fetch_seasons(g_tv_series_id, g_tv_sub_items,
                                           XMB_ITEMS_MAX, 0, &g_tv_sub_total);
        g_tv_depth = 1; g_tv_sub_sel = 0; g_tv_sub_scroll = 0;
        init_btns();
        return;
    }

    // Continue Watching resumes; everything else plays from the start.
    u32 resume = (s_focus_row == HR_CONTINUE) ? it->resume_secs : 0;
    xmb_play_item(it, resume);
    s_rows[HR_CONTINUE].loaded = false; s_rows[HR_CONTINUE].scroll = 0;
    s_rows[HR_NEXTUP].loaded   = false; s_rows[HR_NEXTUP].scroll   = 0;
    init_btns();
}

bool xmb_handle_input_home(void) {
    home_init_once();

    if (BTN_PRESSED(l1)) { xmb_switch_tab(xmb_next_enabled(g_active_tab, -1)); return false; }
    if (BTN_PRESSED(r1)) { xmb_switch_tab(xmb_next_enabled(g_active_tab, +1)); return false; }

    if (BTN_REPEAT(up)   && s_focus_row > 0)              { s_focus_row--; clamp_col(); ensure_row_visible(); }
    if (BTN_REPEAT(down) && s_focus_row < HOME_ROWS_N - 1) { s_focus_row++; clamp_col(); ensure_row_visible(); }

    HomeRow *row = &s_rows[s_focus_row];
    if (BTN_REPEAT(right) && row->kind != HROW_STUB && s_focus_col + 1 < row->count) { s_focus_col++; ensure_col_visible(); }
    if (BTN_REPEAT(left)  && s_focus_col > 0)                                        { s_focus_col--; ensure_col_visible(); }

    if (BTN_PRESSED(cross)) home_activate();
    if (BTN_PRESSED(triangle) && row->kind != HROW_STUB && s_focus_col < row->count)
        xmb_show_item_info(&row->items[s_focus_col]);

    return false;
}
