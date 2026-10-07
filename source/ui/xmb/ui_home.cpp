// Home shelf screen — a vertically-scrolling stack of horizontally-scrolling
// rows (Continue Watching, Next Up, Recently Added Movies/Shows/Music),
// mirroring the Jellyfin web home.  Each row carries its
// own card aspect ratio.  Card images / selection are drawn in the CPU phase
// (xmb_draw_card, shared with the grid); titles / chevrons in the text phase.

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/thread.h>

#include "ui_internal.h"
#include "ui_render_internal.h"
#include "ui_card_gpu.h"
#include "ui_spine.h"       // frame clock + gliding focus ring
#include "jellyfin_api.h"
#include "music_screen.h"
#include "plog.h"

// -------------------------------------------------------
// Model
// -------------------------------------------------------

#define HOME_ROW_MAX 25

typedef enum { HROW_LANDSCAPE, HROW_PORTRAIT, HROW_SQUARE, HROW_STUB } HomeRowKind;

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

// What XMBItem has no room for, per row item, read from the same JSON
// (parse_xmb_items_each).  The spine's queue shows an episode by its series
// -- poster, name, "S1 E4" -- and a film's rating in the meta row.
typedef struct {
    char series_id[40];
    char series_name[64];
    char rating[12];      // OfficialRating, "PG-13"
    u8   season, episode;
} HomeExtra;
static HomeExtra s_extra[HOME_ROWS_N][HOME_ROW_MAX];

// g_play_gen when Continue Watching / Next Up were last fetched.
static unsigned s_seen_gen = 0;

static int     s_focus_row = 0;
static int     s_focus_col = 0;
static int     s_vscroll   = 0;   // pixels the stack is scrolled up
static bool    s_inited    = false;

// Scroll easing (the spine's approach; render/ui_spine.cpp).  s_vscroll and
// each row's `scroll` stay the LOGICAL positions -- input and the visibility
// bookkeeping never see these.  The draw walks read the eased copies, so a
// shelf change or a row stepping sideways glides instead of jumping.  With
// the spine gate off they snap every frame, which is the old drawing exactly.
static spine_approach s_vs_m;
static spine_approach s_hs_m[HOME_ROWS_N];
static unsigned       s_m_frame = 0;
static bool           s_m_fresh = true;

// -------------------------------------------------------
// Layout metrics (resolution-independent, computed each call)
// -------------------------------------------------------

#define HOME_HEADER_H UIS_H(30)          // row-title band above the cards
#define HOME_LABEL_H  UIS_H(26)          // card-title band beneath the cards
#define HOME_ROW_GAP  UIS_H(14)          // gap between rows
#define HOME_SIDE_PAD XMB_ITEM_PAD
#define HOME_CARD_GAP XMB_CARD_GAP_X

static int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
static int view_top(void) { return XMB_CONTENT_Y; }
static int view_bot(void) { return (int)display_height - XMB_BOTTOM_PAD; }

static int row_card_h(HomeRowKind k) {
    // Sized from the REST top, not view_top(): while the spine glides the
    // content in, view_top() moves, and a card height that moved with it
    // would re-request every thumbnail at a new size on every frame.
    int vh = view_bot() - XMB_CONTENT_Y_REST;
    // Tuned so ~5 landscape stills / ~8 portrait posters fit per row at 1080p,
    // and ~3 rows are visible before vertical scrolling.
    return (k == HROW_PORTRAIT) ? clampi(vh * 44 / 100, 160, 280)
                                : clampi(vh * 24 / 100, 110, 175);
}
static int row_card_w(HomeRowKind k) {
    int h = row_card_h(k);
    if (k == HROW_SQUARE) return h;   // album art is square
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

// Step the eased scroll values once per frame (all three phases call this;
// only the first in a frame does anything).
static void home_motion_tick(void) {
    const unsigned id = spine_frame_id();
    if (id == s_m_frame) return;
    const bool snap = s_m_fresh || !g_spine_on || s_m_frame + 1 < id;
    const unsigned long long dt = spine_frame_dt_us();
    if (snap) spine_approach_init(&s_vs_m, (float)s_vscroll, SPINE_CATEGORY_MS);
    spine_approach_set(&s_vs_m, (float)s_vscroll);
    if (!snap) spine_approach_step(&s_vs_m, dt);
    for (int r = 0; r < HOME_ROWS_N; r++) {
        if (snap) spine_approach_init(&s_hs_m[r], (float)s_rows[r].scroll,
                                      SPINE_CATEGORY_MS);
        spine_approach_set(&s_hs_m[r], (float)s_rows[r].scroll);
        if (!snap) spine_approach_step(&s_hs_m[r], dt);
    }
    s_m_frame = id;
    s_m_fresh = false;
}

static int   home_vs(void)  { return (int)(s_vs_m.value + 0.5f); }
static float home_hs(int r) { return s_hs_m[r].value; }

// The columns to draw for a row whose eased offset is hs: one extra card while
// it is between positions, so the card sliding in is there before it arrives.
// At rest this is exactly [scroll, scroll + vis).
static void home_cols(float hs, int vis, int count, int *c0, int *c1) {
    int first = (int)hs;                        // hs >= 0, so this is floor
    int last  = first + vis + ((float)first != hs ? 1 : 0);
    if (last > count) last = count;
    *c0 = first; *c1 = last;
}
static int home_card_x(int x0, int c, float hs, int pitch) {
    const float off = ((float)c - hs) * (float)pitch;
    return x0 + (int)(off + (off >= 0.0f ? 0.5f : -0.5f));
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
    s_rows[HR_MUSIC].title    = "Recently Added in Music"; s_rows[HR_MUSIC].kind    = HROW_SQUARE;
}

static void home_each_extra(int i, const char *o, int n, void *ctx) {
    HomeExtra *x = &((HomeExtra *)ctx)[i];
    xmb_json_str_range(o, n, "SeriesId",       x->series_id,   sizeof x->series_id);
    xmb_json_str_range(o, n, "SeriesName",     x->series_name, sizeof x->series_name);
    xmb_json_str_range(o, n, "OfficialRating", x->rating,      sizeof x->rating);
    decode_unicode_escapes(x->series_name);
    const int se = xmb_json_int_range(o, n, "ParentIndexNumber", 0);
    const int ep = xmb_json_int_range(o, n, "IndexNumber", 0);
    x->season  = (u8)(se > 0 && se < 256 ? se : 0);
    x->episode = (u8)(ep > 0 && ep < 256 ? ep : 0);
}

// Continue Watching and Next Up change with every playback: refetch them,
// and put their focus back on the front, where what was just watched lands.
static void home_stage_dynamic_reset(void);
static void home_mark_dynamic_stale(void) {
    s_rows[HR_CONTINUE].loaded = false; s_rows[HR_CONTINUE].scroll = 0;
    s_rows[HR_NEXTUP].loaded   = false; s_rows[HR_NEXTUP].scroll   = 0;
    s_seen_gen = g_play_gen;
    home_stage_dynamic_reset();
}

// The request for row r.  False when the row has nothing to ask for (a stub,
// or no library of that kind): it is then marked loaded and empty here.
static bool home_row_url(int r, char *url, size_t cap) {
    HomeRow *row = &s_rows[r];
    const char *fields = "Genres,RunTimeTicks,ProductionYear,Container,Tags";   // Tags: see is_gelato_stream()

    if (r == HR_CONTINUE) {
        snprintf(url, cap,
            "%s/Users/%s/Items/Resume?Limit=%d&Recursive=true&MediaTypes=Video&Fields=%s",
            g_server, g_userid, HOME_ROW_MAX, fields);
    } else if (r == HR_NEXTUP) {
        snprintf(url, cap,
            "%s/Shows/NextUp?userId=%s&Limit=%d&Fields=%s",
            g_server, g_userid, HOME_ROW_MAX, fields);
    } else if (r == HR_MUSIC) {
        // Home samples the FIRST music library; the others are still reachable
        // as their own tabs.
        int mt = xmb_tab_of_kind(TABKIND_MUSIC);
        if (mt < 0) { row->count = 0; row->loaded = true; return false; }
        const char *lib = g_tabs[mt].library_id;
        if (!lib[0]) { row->count = 0; row->loaded = true; return false; }
        snprintf(url, cap,
            "%s/Users/%s/Items?ParentId=%s&IncludeItemTypes=MusicAlbum"
            "&Recursive=true&SortBy=DateCreated&SortOrder=Descending"
            "&Limit=%d&Fields=%s",
            g_server, g_userid, lib, HOME_ROW_MAX, fields);
    } else if (r == HR_MOVIES || r == HR_SHOWS) {
        int tab = xmb_tab_of_kind((r == HR_MOVIES) ? TABKIND_MOVIES : TABKIND_TV);
        if (tab < 0) { row->count = 0; row->loaded = true; return false; }
        const char *lib  = g_tabs[tab].library_id;
        const char *type = (r == HR_MOVIES) ? "Movie" : "Series";
        if (!lib[0]) { row->count = 0; row->loaded = true; return false; }
        snprintf(url, cap,
            "%s/Users/%s/Items?ParentId=%s&IncludeItemTypes=%s&Recursive=true"
            "&SortBy=DateCreated&SortOrder=Descending&Limit=%d&Fields=%s",
            g_server, g_userid, lib, type, HOME_ROW_MAX, fields);
    } else {
        row->loaded = true;   // stub
        return false;
    }
    return true;
}

// Synchronous: the boot worker's prefetch (nothing else is drawing then).
static void home_fetch_row(int r) {
    HomeRow *row = &s_rows[r];
    char url[512];
    if (!home_row_url(r, url, sizeof url)) return;
    int status = http_request(0, url, NULL, g_token, responseBuffer, RESPONSE_SIZE);
    memset(s_extra[r], 0, sizeof s_extra[r]);
    row->count = (status == 200)
               ? parse_xmb_items_each(responseBuffer, row->items, HOME_ROW_MAX,
                                      home_each_extra, s_extra[r]) : 0;
    row->loaded = true;
    if (row->scroll > row->count) row->scroll = 0;
}

// --- rows off the render thread -------------------------------------------
//
// Measured 2026-09-24: back from a film, Home's first frame took 556 ms, and
// 1,136 ms the second time (`xmb: frame=... other=1114320`).  That was
// home_step_load() -- a blocking HTTP request on the RENDER thread, one row a
// frame, for Continue Watching and Next Up (every playback makes them stale).
// Worse, http_request() holds one mutex for every caller, so that frame also
// waited behind whatever the thumbnail thread was fetching at the time.
//
// So the rows are fetched by a worker of their own, one at a time, into its
// own buffer, and the render thread only copies a finished row in (a few KB).
// Until the new row lands the old one stays on screen.  A fetch that started
// before a playback bumped g_play_gen is thrown away for the dynamic rows and
// asked for again, so a stale Continue Watching can never be published.
#define HOME_WORK_BUF (256 * 1024)     // a 25-item row is ~40-80 KB of JSON
static char            *s_hw_buf = NULL;
static XMBItem          s_hw_items[HOME_ROW_MAX];
static HomeExtra        s_hw_extra[HOME_ROW_MAX];
static char             s_hw_url[512];
static int              s_hw_count = 0;
static int              s_hw_status = 0;
static int              s_hw_row = -1;          // the row in flight
static unsigned         s_hw_gen = 0;           // g_play_gen when it was asked for
static volatile int     s_hw_state = 0;         // 0 idle, 1 queued, 2 done
static volatile bool    s_hw_run = false;
static int              s_hw_ok = -1;           // -1 not tried, 0 failed, 1 up
static sys_ppu_thread_t s_hw_tid;

static void home_worker_fn(void *arg) {
    (void)arg;
    while (s_hw_run) {
        if (s_hw_state != 1) { usleep(4000); continue; }
        __sync_synchronize();
        memset(s_hw_extra, 0, sizeof s_hw_extra);
        const int status = http_request(0, s_hw_url, NULL, g_token, s_hw_buf, HOME_WORK_BUF);
        s_hw_status = status;
        s_hw_count  = (status == 200)
                    ? parse_xmb_items_each(s_hw_buf, s_hw_items, HOME_ROW_MAX,
                                           home_each_extra, s_hw_extra) : 0;
        __sync_synchronize();
        s_hw_state = 2;
    }
    sysThreadExit(0);
}

static bool home_worker_up(void) {
    if (s_hw_ok >= 0) return s_hw_ok == 1;
    s_hw_ok = 0;
    s_hw_buf = (char *)malloc(HOME_WORK_BUF);
    if (!s_hw_buf) { plog("home: row worker buffer FAILED -- rows load inline"); return false; }
    s_hw_run = true;
    static char name[] = "jf_homerow";
    if (sysThreadCreate(&s_hw_tid, home_worker_fn, NULL, 1300, 64 * 1024,
                        THREAD_JOINABLE, name) != 0) {
        s_hw_run = false;
        free(s_hw_buf); s_hw_buf = NULL;
        plog("home: row worker FAILED -- rows load inline");
        return false;
    }
    s_hw_ok = 1;
    return true;
}

// Render thread: publish a finished row.
static void home_worker_collect(void) {
    if (s_hw_state != 2) return;
    __sync_synchronize();
    const int r = s_hw_row;
    const bool dynamic = r == HR_CONTINUE || r == HR_NEXTUP;
    if (r >= 0 && r < HOME_ROWS_N && !(dynamic && s_hw_gen != g_play_gen)) {
        HomeRow *row = &s_rows[r];
        const int n = s_hw_count < 0 ? 0 : (s_hw_count > HOME_ROW_MAX ? HOME_ROW_MAX : s_hw_count);
        // A failed request keeps what the row had rather than emptying it.
        if (s_hw_status == 200 || row->count == 0) {
            memcpy(row->items, s_hw_items, (size_t)n * sizeof(XMBItem));
            memcpy(s_extra[r], s_hw_extra, sizeof s_extra[r]);
            row->count = n;
        }
        row->loaded = true;
        if (row->scroll > row->count) row->scroll = 0;
    }
    s_hw_row = -1;
    __sync_synchronize();
    s_hw_state = 0;
}

// Once per frame (render thread): publish what has landed, then ask for the
// next unloaded row.  Never blocks.
static void home_step_load(void) {
    if (!home_worker_up()) {
        // No worker: the old way, one blocking row a frame.
        for (int r = 0; r < HOME_ROWS_N; r++) {
            if (s_rows[r].kind == HROW_STUB) continue;
            if (!s_rows[r].loaded) { home_fetch_row(r); return; }
        }
        return;
    }
    home_worker_collect();
    if (s_hw_state != 0) return;              // one in flight
    for (int r = 0; r < HOME_ROWS_N; r++) {
        if (s_rows[r].kind == HROW_STUB || s_rows[r].loaded) continue;
        if (!home_row_url(r, s_hw_url, sizeof s_hw_url)) continue;
        s_hw_row = r;
        s_hw_gen = g_play_gen;
        __sync_synchronize();
        s_hw_state = 1;
        return;
    }
}

// Every row now, not one a frame -- for the boot worker (xmb_prepare).  The
// per-frame loader below then finds nothing left to do.
void xmb_home_prefetch(void) {
    home_init_once();
    for (int r = 0; r < HOME_ROWS_N; r++)
        if (s_rows[r].kind != HROW_STUB && !s_rows[r].loaded)
            home_fetch_row(r);
}

void xmb_home_on_enter(void) {
    home_init_once();
    if (g_spine_on) {
        // The XMB remembers where it was in a category.  Coming back to Home
        // keeps the category and each row's position, and refetches the
        // dynamic rows only when something has played since -- walking the
        // spine past Home used to cost two blocking fetches every time.
        if (g_play_gen != s_seen_gen) home_mark_dynamic_stale();
        return;
    }
    s_focus_row = 0; s_focus_col = 0; s_vscroll = 0;
    s_m_fresh = true;     // a new visit starts still, not sliding from the last
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
    drawRect((u32)(cx - O), (u32)(cy - O),     (u32)(w + 2*O), T, XMB_FOCUS_RING);
    drawRect((u32)(cx - O), (u32)(cy + h + G), (u32)(w + 2*O), T, XMB_FOCUS_RING);
    drawRect((u32)(cx - O), (u32)(cy - G),     T, (u32)(h + 2*G), XMB_FOCUS_RING);
    drawRect((u32)(cx + w + G), (u32)(cy - G), T, (u32)(h + 2*G), XMB_FOCUS_RING);
}

// True when row r's card band touches the viewport (cheap vertical cull).
static bool row_on_screen(int r, int *out_vy, int *out_card_y) {
    int vy     = row_abs_top(r) - home_vs();
    int card_y = vy + HOME_HEADER_H;
    if (out_vy) *out_vy = vy;
    if (out_card_y) *out_card_y = card_y;
    int ch = row_card_h(s_rows[r].kind);
    return !(card_y + ch < view_top() || vy > view_bot());
}

// Thumbnail requests, most wanted first.  The cache has 32 slots and a claim
// may evict anything not yet asked for THIS frame, so when Home wants more
// than fit, request order is priority.  It used to be top row to bottom, and
// ensure_row_visible() leaves the focused row at the BOTTOM of the view after
// a step down: the row being looked at starved, some of its posters never
// loading until the row above scrolled away (tester, 2026-09-27).  Now: the
// focused row, the other rows on screen nearest first, then the focused row's
// preload past its edges.  Once per frame, before the first draw touches.
static unsigned s_req_frame = ~0u;

static void home_request_cols(const HomeRow *row, int c0, int c1) {
    const int cw = row_card_w(row->kind), ch = row_card_h(row->kind);
    for (int c = c0 < 0 ? 0 : c0; c < c1 && c < row->count; c++) {
        const ThumbImg img = (row->kind == HROW_LANDSCAPE && row->items[c].has_thumb)
                           ? THUMB_IMG_THUMB : THUMB_IMG_PRIMARY;
        thumb_request(row->items[c].id, cw, ch, img);
    }
}

static void home_request_thumbs(void) {
    const unsigned id = spine_frame_id();
    if (id == s_req_frame) return;
    s_req_frame = id;
    for (int d = 0; d < HOME_ROWS_N; d++) {
        for (int s = -1; s <= 1; s += 2) {
            if (d == 0 && s > 0) continue;
            const int r = s_focus_row + s * d;
            if (r < 0 || r >= HOME_ROWS_N) continue;
            const HomeRow *row = &s_rows[r];
            if (row->kind == HROW_STUB || row->count <= 0) continue;
            if (!row_on_screen(r, NULL, NULL)) continue;
            int c0, c1;
            home_cols(home_hs(r), row_visible_cols(row->kind), row->count, &c0, &c1);
            home_request_cols(row, c0, c1);
        }
    }
    const HomeRow *row = &s_rows[s_focus_row];
    if (row->kind != HROW_STUB && row->count > 0) {
        const int vis = row_visible_cols(row->kind);
        home_request_cols(row, row->scroll - 1, row->scroll + vis + 3);
    }
}

// GPU phase (BEFORE rsxSync): card images only, from their VRAM mirrors.
// Mirrors xmb_home_cpu_phase's walk exactly -- same rows, same visibility
// test, same ThumbImg choice -- so a card either gets drawn here and skipped
// there, or missed here and blitted there.  Divergence between the two walks
// would show up as a card drawn twice or not at all.
void xmb_home_gpu_phase(void) {
    if (!ui_card_gpu_ready()) return;
    home_init_once();
    home_motion_tick();
    home_request_thumbs();

    ui_card_gpu_clip(view_top(), view_bot());

    for (int r = 0; r < HOME_ROWS_N; r++) {
        HomeRow *row = &s_rows[r];
        int card_y;
        if (!row_on_screen(r, NULL, &card_y)) continue;
        if (row->kind == HROW_STUB) continue;      // a CPU rect, not an image
        int cw = row_card_w(row->kind), ch = row_card_h(row->kind);
        int x0 = row_origin_x(row->kind);

        int vis = row_visible_cols(row->kind);
        const float hs = home_hs(r);
        int c0, c1;
        home_cols(hs, vis, row->count, &c0, &c1);
        for (int c = c0; c < c1; c++) {
            int cx = home_card_x(x0, c, hs, cw + HOME_CARD_GAP);
            // The GPU can draw a card partly off either edge, so the one
            // sliding out is only dropped once it is wholly gone.
            if (cx + cw <= 0 || cx >= (int)display_width) continue;
            ThumbImg img = (row->kind == HROW_LANDSCAPE && row->items[c].has_thumb)
                         ? THUMB_IMG_THUMB : THUMB_IMG_PRIMARY;
            xmb_card_gpu_one(row->items[c].id, cx, card_y, cw, ch, img);
            if (r == s_focus_row && c == s_focus_col)   // glides (spine gate)
                spine_focus_ring_gpu(cx, card_y, cw, ch);
        }
    }

    ui_card_gpu_clip(0, 0);
}

void xmb_home_cpu_phase(void) {
    home_init_once();
    home_step_load();
    home_motion_tick();
    home_request_thumbs();   // no-op if the GPU phase already did

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
        const float hs = home_hs(r);
        int c0, c1;
        home_cols(hs, vis, row->count, &c0, &c1);
        for (int c = c0; c < c1; c++) {
            int cx = home_card_x(x0, c, hs, cw + HOME_CARD_GAP);
            // CPU rects and text take unsigned x, so a card that is partly
            // off screen is left to the GPU image alone.  At rest every card
            // is on screen, so this never fires.
            if (cx < 0 || cx + cw > (int)display_width) continue;
            bool sel = (r == s_focus_row && c == s_focus_col);
            // Landscape rows want wide art.  An item's Primary is a portrait
            // poster for movies (which looked badly cropped in these 16:9
            // cards) but an already-16:9 still for episodes — so prefer the
            // Thumb only where the server actually published one.
            ThumbImg img = (row->kind == HROW_LANDSCAPE && row->items[c].has_thumb)
                         ? THUMB_IMG_THUMB : THUMB_IMG_PRIMARY;
            xmb_draw_card(row->items[c].id, cx, card_y, cw, ch,
                          row->items[c].progress_pct, sel,
                          r == HR_MUSIC ? row->items[c].name : NULL, img);
        }
    }

    g_cpu_clip_top = 0; g_cpu_clip_bot = 0;
}

void xmb_home_text_phase(void) {
    home_init_once();
    home_motion_tick();

    g_cpu_clip_top = view_top();
    g_cpu_clip_bot = view_bot();

    for (int r = 0; r < HOME_ROWS_N; r++) {
        HomeRow *row = &s_rows[r];
        int vy, card_y;
        if (!row_on_screen(r, &vy, &card_y)) continue;
        int cw = row_card_w(row->kind), ch = row_card_h(row->kind);
        int x0 = row_origin_x(row->kind);
        bool row_focused = (r == s_focus_row);

        // Row header, with a dim item count when the row overflows the screen
        // (replaces the old horizontal scrollbar as the "there's more" cue).
        // v1.0: row titles are eyebrows -- Microgramma, uppercase, 0.18em.
        int hw = xmb_draw_eyebrow(x0, vy + UIS_H(4), row->title,
                                  row_focused ? XMB_TEXT : XMB_TEXT_FAINT);
        if (row->kind != HROW_STUB && row->count > row_visible_cols(row->kind)) {
            char cnt[8];
            snprintf(cnt, sizeof(cnt), "%d", row->count);
            drawTTF((u32)(x0 + hw + UIS_W(10)),
                    (u32)(vy + UIS_H(2)), cnt, UIS_TF(11), XMB_TEXT_FAINT);
        }

        if (row->kind == HROW_STUB) {
            const char *msg = "Coming soon";
            int tw = ttf_text_width(msg, UIS_TF(16));
            drawTTF((u32)(x0 + (cw - tw) / 2),
                    (u32)(card_y + ch / 2 - UIS_H(8)), msg, UIS_TF(16), XMB_TEXT_FAINT);
            continue;
        }

        if (row->count == 0) {
            // drawTTF is Latin-1, so plain "..." (no UTF-8 ellipsis).
            const char *msg = row->loaded ? "Nothing here yet" : "Loading...";
            drawTTF((u32)x0, (u32)(card_y + ch / 2 - UIS_H(8)), msg, UIS_TF(15), XMB_TEXT_FAINT);
            continue;
        }

        int vis = row_visible_cols(row->kind);
        const float hs = home_hs(r);
        int c0, c1;
        home_cols(hs, vis, row->count, &c0, &c1);
        for (int c = c0; c < c1; c++) {
            int cx = home_card_x(x0, c, hs, cw + HOME_CARD_GAP);
            // CPU rects and text take unsigned x, so a card that is partly
            // off screen is left to the GPU image alone.  At rest every card
            // is on screen, so this never fires.
            if (cx < 0 || cx + cw > (int)display_width) continue;
            int ty = card_y + ch + UIS_H(5);
            bool sel = (row_focused && c == s_focus_col);
            home_clip_text(cx, ty, row->items[c].name, UIS_TF(15),
                           sel ? XMB_WHITE : XMB_TEXT_DIM, cw, sel);
            if (sel && row->items[c].year_str[0])
                drawTTF((u32)cx, (u32)(ty + UIS_H(19)), row->items[c].year_str, UIS_TF(13), XMB_TEXT_FAINT);
        }

    }

    // No scrollbars: horizontal overflow is signalled by the count in the row
    // header, vertical by the next row peeking in from the bottom edge.

    g_cpu_clip_top = 0; g_cpu_clip_bot = 0;
}

// -------------------------------------------------------
// The spine's Home: one category at a time (ui_home_stage.inc)
// -------------------------------------------------------

#include "ui_home_stage.inc"

static void home_stage_dynamic_reset(void) {
    s_row_col[HR_CONTINUE] = 0;
    s_row_col[HR_NEXTUP]   = 0;
    s_q_fresh = true;               // snap: nothing should slide back to 0
}

void xmb_queue_src(int *w, int *h) {
    *w = *h = 0;
    if (!g_spine_on) return;
    // The focused poster's own size, capped at 120,000 px (~15 MB for the
    // cache's 32 slots): at 720p that is the full 200x300, at 1080p it
    // fetches 283x424 for a 300x450 box -- a 6% stretch nobody will see.
    float fw = (float)UIS_W(200), fh = (float)UIS_H(300);
    const float cap = 120000.0f;
    if (fw * fh > cap) {
        const float k = sqrtf(cap / (fw * fh));
        fw *= k; fh *= k;
    }
    *w = (int)fw;
    *h = (int)fh;
}

// -------------------------------------------------------
// Input
// -------------------------------------------------------

static void home_activate(void) {
    HomeRow *row = &s_rows[s_focus_row];
    if (row->kind == HROW_STUB || row->count == 0 || s_focus_col >= row->count) return;
    XMBItem *it = &row->items[s_focus_col];

    // A show opens the existing TV Series -> Seasons -> Episodes sub-screen.
    // Keyed off the ITEM TYPE, not the row: a Series turns up in Continue
    // Watching and Recently Added too, and there it used to fall through to
    // the video player, which cannot play a folder.
    if (strcmp(it->type, "Series") == 0) {
        if (xmb_open_series(it)) init_btns();
        return;
    }

    // Albums open the music album screen (queue + Now Playing).
    if (strcmp(it->type, "MusicAlbum") == 0) {
        music_screen_open_album(it, "Home");
        init_btns();
        return;
    }

    // Continue Watching resumes; everything else plays from the start.
    // Episodes get the NEXT prompt / auto-advance no matter which row
    // they came from.
    u32 resume = (s_focus_row == HR_CONTINUE) ? it->resume_secs : 0;
    if (strcmp(it->type, "Episode") == 0)
        xmb_play_episode_with_next(it, resume);
    else
        xmb_play_item(it, resume);
    s_rows[HR_CONTINUE].loaded = false; s_rows[HR_CONTINUE].scroll = 0;
    s_rows[HR_NEXTUP].loaded   = false; s_rows[HR_NEXTUP].scroll   = 0;
    init_btns();
}

bool xmb_handle_input_home(void) {
    home_init_once();
    if (g_spine_on) return home_queue_input();

    if (BTN_PRESSED(l1)) { xmb_switch_tab(xmb_next_enabled(g_active_tab, -1)); return false; }
    if (BTN_PRESSED(r1)) { xmb_switch_tab(xmb_next_enabled(g_active_tab, +1)); return false; }

    if (BTN_REPEAT(up)   && s_focus_row > 0)              { s_focus_row--; clamp_col(); ensure_row_visible(); }
    if (BTN_REPEAT(down) && s_focus_row < HOME_ROWS_N - 1) { s_focus_row++; clamp_col(); ensure_row_visible(); }

    HomeRow *row = &s_rows[s_focus_row];
    if (BTN_REPEAT(right) && row->kind != HROW_STUB && s_focus_col + 1 < row->count) { s_focus_col++; ensure_col_visible(); }
    if (BTN_REPEAT(left)  && s_focus_col > 0)                                        { s_focus_col--; ensure_col_visible(); }

    if (BTN_PRESSED(cross)) home_activate();
    if (BTN_PRESSED(triangle) && row->kind != HROW_STUB && s_focus_col < row->count) {
        XMBItem *it = &row->items[s_focus_col];
        // Triangle on a show browses its seasons rather than opening the
        // version overlay, which for a Series could only ever say "Play".
        // Pick the episode first; Triangle THERE gives the version picker.
        if (strcmp(it->type, "Series") == 0) { if (xmb_open_series(it)) init_btns(); }
        else                                 xmb_show_item_info(it);
    }

    return false;
}
