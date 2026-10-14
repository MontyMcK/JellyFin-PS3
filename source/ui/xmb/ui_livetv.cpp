// The Live TV tab: a channel list with what is on now, and a guide grid.
//
// Not a library to browse: its rows come from /LiveTv (api_livetv.cpp), and
// the tab is a screen of its own the way Settings is.  Everything that is only
// arithmetic -- sorting, progress, what airs when, channel up/down -- is
// api/livetv.cpp, host-tested.  This file is the rest:
//
//   * a worker thread does every request, so the render thread never waits on
//     the network.  It runs one job at a time (the channel list, or the guide
//     for the rows and window on screen) into buffers of its own, and the render
//     thread copies a finished job in between frames' work.  A job asked for
//     again while one runs is simply asked for after it.
//   * the list refreshes every 60 s while the tab is open and again on return
//     from the player, so "now" stays current.
//   * the guide is hidden entirely when no channel has any programme: Right
//     then does nothing and the list shows names only.
//   * a server with more than PAGE_MIN channels is shown a category at a time
//     (L2 / R2 change it).  The categories are blocks of channel numbers: one
//     light request at the start fetches every channel's number, the pure
//     livetv_page_ranges() turns them into list ranges, and only the page on
//     screen is fetched in full.  The blocks are the ones the server's
//     playlist is numbered in (CATS below).
//   * Select asks for a name on the on-screen keyboard and lists every channel
//     that matches, from all categories; O goes back to the category.
//
// Layout is all UIS-scaled; both screens work at 720p and 1080p.

#include "i18n.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/thread.h>
#include <sysutil/sysutil.h>

#include "ui_internal.h"
#include "ui_render_internal.h"
#include "ui_card_gpu.h"
#include "ui_spine.h"
#include "ui_wave.h"
#include "ui_buffering.h"      // loading_run
#include "ui_sfx.h"
#include "api_livetv.h"
#include "api_userdata.h"
#include "livetv_prefs.h"
#include "jf_paths.h"
#include "jellyfin_api.h"
#include "thumbnail_cache.h"
#include "player.h"
#include "plog.h"
#include "timing.h"

namespace {

// ---------------------------------------------------------------------------
//  Constants
// ---------------------------------------------------------------------------

const uint64_t T_MIN  = 600000000ULL;               // ticks per minute
const uint64_t T_30M  = 30 * T_MIN;
const uint64_t T_3H   = 180 * T_MIN;

const int      MAX_CH      = LIVETV_MAX_CHANNELS;
const int      MAX_INDEX   = 6000;                  // channel numbers kept (24 KB)
const int      PAGE_MIN    = 200;                   // at most this many: one list, no pages
const int      MAX_PAGES   = 24;
const int      MAX_PG      = 400;                   // programmes kept: ~130 KB
const int      GUIDE_ROWS  = 16;                    // channels asked for at once
const uint64_t REFRESH_US  = 60ULL * 1000000ULL;
const uint64_t DEBOUNCE_US = 250000ULL;             // a moving view asks once it settles

// ---------------------------------------------------------------------------
//  State
// ---------------------------------------------------------------------------

JFChannel *s_ch = NULL;     int s_n = 0;            // what is shown
JFProgram *s_pg = NULL;     int s_pn = 0;
int        s_pg_first = 0, s_pg_rows = 0;           // which rows the guide data is for
uint64_t   s_pg_win0 = 0;                           // and which window
bool       s_loaded = false, s_failed = false;
bool       s_alloc_failed = false;
int        s_utc_offset = 0;                        // seconds: time zone + summer time

int        s_sel = 0, s_top = 0;                    // selected channel, first visible row
bool       s_guide = false;                         // the guide grid is up
uint64_t   s_gt = 0;                                // the guide's focus: a time
uint64_t   s_base_win0 = 0;                         // the earliest window the guide shows
uint64_t   s_win0 = 0;                              // the window being shown
u64        s_last_refresh_us = 0;                   // when the channel list last arrived
u64        s_view_changed_us = 0;                   // when the visible rows or window last moved
int        s_seen_first = -1, s_seen_win_top = -1;
uint64_t   s_seen_win0 = 0;
bool       s_in_details = false;

// ---- categories ----
// The lowest channel number of each, ascending.  Labels from 1000 up are
// generic; the ones below it are sports.  Names in capitals are not translated.
struct CatDef { int base; const char *label; bool translate; };
const CatDef CATS[] = {
    {    1, TRN("Networks"),        true  }, {  101, "NFL",                  false },
    {  151, "NBA",                  false }, {  201, "NHL",                  false },
    {  251, "MLB",                  false }, {  331, TRN("Motorsport"),      true  },
    {  351, TRN("Combat"),          true  }, {  371, TRN("Regional teams"),  true  },
    {  421, TRN("International"),   true  }, {  471, "ESPN+",                false },
    {  651, "PPV",                  false }, {  671, "WNBA",                 false },
    { 1000, TRN("News"),            true  }, { 2000, TRN("Entertainment"),   true  },
    { 3000, TRN("Movies"),          true  }, { 4000, TRN("Kids"),            true  },
    { 5000, TRN("Music"),           true  }, { 6000, TRN("Canadian"),        true  },
    { 7000, TRN("Latino"),          true  }, { 8000, "24/7",                 false },
    { 9000, TRN("Local (US)"),      true  }, {19000, TRN("Other"),           true  },
};
const int N_CATS = (int)(sizeof CATS / sizeof CATS[0]);
const int SPORTS_BELOW = 1000;

struct Page { int first, count, cat, key; };    // key: livetv_prefs.h (a category's base, or LP_FAVS / LP_RECENT)
Page       s_pages[MAX_PAGES];  int s_npages = 0;
// Every category the server has channels in (before the viewer hides any), in
// the order livetv_page_ranges gave them.
int        s_raw_first[MAX_PAGES], s_raw_count[MAX_PAGES], s_raw_cat[MAX_PAGES];
int        s_raw_n = 0;
LtPrefs    s_prefs;             bool s_prefs_loaded = false;
const char *PREFS_FILE = "jellyfin_livetv.txt";
int        s_page = 0;                              // the one shown
float     *s_nums = NULL;       int s_nnums = 0;    // every channel's number, list order
bool       s_index_ok = false;                      // s_nums / s_pages describe the server
bool       s_index_tried = false;                   // asked since the tab opened (or the list changed)
bool paging(void) { return s_index_ok && s_nnums > PAGE_MIN && s_npages > 1; }

// ---- search ----
bool s_search_on = false;
char s_term[48]  = "";
int  s_gen       = 0;       // bumped when the list being fetched changes (page, search)

// ---- the worker ----
enum { J_IDLE = 0, J_QUEUED, J_RUNNING, J_DONE };
enum { JOB_CHANNELS = 1, JOB_GUIDE = 2, JOB_INDEX = 4 };

struct Job {
    int      kind;
    int      cfirst, ccount, gen;                   // the channel window asked for, and which list (s_gen)
    int      mode;                                  // 0 a range, 1 My Channels, 2 Recently watched
    int      n_recent;
    char     recent[LP_MAX_RECENT][LP_ID_MAX];
    char     term[48];                              // a search, or "" for the page
    int      first, rows;                           // guide rows asked for
    int      n_ids;
    char     ids[GUIDE_ROWS][40];
    uint64_t w0, w1;
};

volatile int     s_jstate = J_IDLE;
Job              s_job;
JFChannel       *w_ch = NULL;    int w_n = 0;       bool w_ch_ok = false;
int              w_ctotal = 0;                      // the server's channel count, with that reply
float           *w_nums = NULL;  int w_nnums = 0;   bool w_idx_ok = false;
JFProgram       *w_pg = NULL;    int w_pn = 0;
sys_ppu_thread_t s_tid;
bool             s_have_tid = false;
volatile bool    s_run = false;

void worker_main(void *) {
    while (s_run) {
        if (s_jstate != J_QUEUED) { usleep(30000); continue; }
        __sync_synchronize();
        s_jstate = J_RUNNING;
        const Job j = s_job;
        w_ch_ok = false;
        w_idx_ok = false;
        w_pn = 0;
        if (j.kind & JOB_INDEX) {
            w_nnums = jf_fetch_channel_numbers(w_nums, MAX_INDEX, NULL);
            w_idx_ok = w_nnums >= 0;
        }
        if (j.kind & JOB_CHANNELS) {
            int total = 0, n;
            if (j.mode == 1) {
                n = jf_fetch_channels_range(w_ch, MAX_CH, 0, MAX_CH, &total, NULL, "&isFavorite=true");
            } else if (j.mode == 2) {
                if (j.n_recent <= 0) { n = 0; }
                else {
                    char extra[LP_MAX_RECENT * (LP_ID_MAX + 1) + 8] = "&Ids=";
                    for (int i = 0; i < j.n_recent; i++) {
                        if (i) strcat(extra, ",");
                        strcat(extra, j.recent[i]);
                    }
                    n = jf_fetch_channels_range(w_ch, MAX_CH, 0, MAX_CH, &total, NULL, extra);
                    // Newest watched first (the server answers in its own order).
                    for (int a = 1; a < n; a++) {
                        int ra = LP_MAX_RECENT;
                        for (int i = 0; i < j.n_recent; i++) if (!strcmp(j.recent[i], w_ch[a].id)) { ra = i; break; }
                        JFChannel t = w_ch[a];
                        int b = a - 1;
                        for (; b >= 0; b--) {
                            int rb = LP_MAX_RECENT;
                            for (int i = 0; i < j.n_recent; i++) if (!strcmp(j.recent[i], w_ch[b].id)) { rb = i; break; }
                            if (rb <= ra) break;
                            w_ch[b + 1] = w_ch[b];
                        }
                        w_ch[b + 1] = t;
                    }
                }
            } else {
                n = jf_fetch_channels_range(w_ch, MAX_CH, j.cfirst, j.ccount, &total,
                                            j.term[0] ? j.term : NULL);
            }
            if (n >= 0) { w_n = n; w_ctotal = total; w_ch_ok = true; }
        }
        if (j.kind & JOB_GUIDE) {
            const char *ids[GUIDE_ROWS];
            for (int i = 0; i < j.n_ids; i++) ids[i] = j.ids[i];
            w_pn = jf_fetch_programs(ids, j.n_ids, j.w0, j.w1, w_pg, MAX_PG);
        }
        __sync_synchronize();
        s_jstate = J_DONE;
    }
    sysThreadExit(0);
}

bool ensure_alloc(void) {
    if (s_ch && s_pg && w_ch && w_pg && s_nums && w_nums) return true;
    if (s_alloc_failed) return false;
    s_ch = (JFChannel *)malloc(sizeof(JFChannel) * MAX_CH);
    w_ch = (JFChannel *)malloc(sizeof(JFChannel) * MAX_CH);
    s_pg = (JFProgram *)malloc(sizeof(JFProgram) * MAX_PG);
    w_pg = (JFProgram *)malloc(sizeof(JFProgram) * MAX_PG);
    s_nums = (float *)malloc(sizeof(float) * MAX_INDEX);
    w_nums = (float *)malloc(sizeof(float) * MAX_INDEX);
    if (!s_ch || !w_ch || !s_pg || !w_pg || !s_nums || !w_nums) {
        free(s_ch); free(w_ch); free(s_pg); free(w_pg); free(s_nums); free(w_nums);
        s_ch = w_ch = NULL; s_pg = w_pg = NULL; s_nums = w_nums = NULL;
        s_alloc_failed = true;
        plog("livetv: no memory for the channel list");
        return false;
    }
    return true;
}

bool ensure_worker(void) {
    if (s_have_tid) return true;
    s_run = true;
    static char name[] = "jf_livetv";
    if (sysThreadCreate(&s_tid, worker_main, NULL, 1300, 128 * 1024, THREAD_JOINABLE, name) != 0) {
        s_run = false;
        plog("livetv: worker thread did not start");
        return false;
    }
    s_have_tid = true;
    return true;
}

// ---------------------------------------------------------------------------
//  Time
// ---------------------------------------------------------------------------

uint64_t floor_half_hour(uint64_t t) { return t - (t % T_30M); }

// HH:MM on the console's local clock.
void fmt_hm(uint64_t ticks, char *out, int cap) { livetv_format_hm(ticks, s_utc_offset, out, cap); }

void read_utc_offset(void) { s_utc_offset = jf_utc_offset_secs(); }

// ---------------------------------------------------------------------------
//  Geometry
// ---------------------------------------------------------------------------

int band_top(void)    { return XMB_CONTENT_Y + UIS_H(10); }
int band_bottom(void) { return (int)display_height - XMB_BOTTOM_PAD; }
int row_h(void)       { return UIS_H(62); }
int row_pitch(void)   { return row_h() + UIS_H(8); }
int rows_visible(void) {
    const int n = (band_bottom() - band_top() + UIS_H(8)) / row_pitch();
    return n < 1 ? 1 : n;
}
int list_x(void) { return ((int)display_width - XMB_LIST_W) / 2; }

// The guide spans the screen: a pinned logo column, then three hours.
int guide_x0(void)     { return XMB_ITEM_PAD; }
int guide_logo_w(void) { return UIS_W(150); }
int guide_cells_x(void){ return guide_x0() + guide_logo_w() + UIS_W(10); }
int guide_cells_w(void){ return (int)display_width - XMB_ITEM_PAD - guide_cells_x(); }
int guide_head_h(void) { return UIS_H(30); }
int guide_row_h(void)  { return UIS_H(54); }
int guide_pitch(void)  { return guide_row_h() + UIS_H(6); }
int guide_top(void)    { return band_top() + guide_head_h(); }
int guide_rows_visible(void) {
    const int n = (band_bottom() - guide_top() + UIS_H(6)) / guide_pitch();
    return n < 1 ? 1 : n;
}

int x_of_time(uint64_t t) {
    if (t <= s_win0) return guide_cells_x();
    if (t >= s_win0 + T_3H) return guide_cells_x() + guide_cells_w();
    return guide_cells_x() + (int)((double)(t - s_win0) / (double)T_3H * guide_cells_w());
}

// ---------------------------------------------------------------------------
//  Programmes for a channel (from the guide data), in start order
// ---------------------------------------------------------------------------

int progs_of(const char *channel_id, const JFProgram **out, int max) {
    int n = 0;
    for (int i = 0; i < s_pn && n < max; i++) {
        if (strcmp(s_pg[i].channel_id, channel_id) != 0) continue;
        if (s_pg[i].end_ticks <= s_pg[i].start_ticks) continue;
        int j = n++;
        while (j > 0 && out[j - 1]->start_ticks > s_pg[i].start_ticks) { out[j] = out[j - 1]; j--; }
        out[j] = &s_pg[i];
    }
    return n;
}

// What a list row shows: the programme on now and the one after it.  The
// guide's data wins (it has the next one); the channel's own CurrentProgram
// stands in until it lands.
struct NowNext {
    char     now[128];
    uint64_t now_start, now_end;
    char     next[128];
    uint64_t next_start;
};

void now_next(const JFChannel *c, uint64_t now, NowNext *o) {
    memset(o, 0, sizeof *o);
    const JFProgram *ps[24];
    const int n = progs_of(c->id, ps, 24);
    int cur = -1;
    for (int i = 0; i < n; i++)
        if (ps[i]->start_ticks <= now && now < ps[i]->end_ticks) cur = i;
    if (cur >= 0) {
        snprintf(o->now, sizeof o->now, "%s", ps[cur]->name);
        o->now_start = ps[cur]->start_ticks; o->now_end = ps[cur]->end_ticks;
        for (int i = cur + 1; i < n; i++)
            if (ps[i]->start_ticks >= ps[cur]->end_ticks) {
                snprintf(o->next, sizeof o->next, "%s", ps[i]->name);
                o->next_start = ps[i]->start_ticks;
                break;
            }
    } else if (c->now_title[0] && c->now_start_ticks <= now && now < c->now_end_ticks) {
        snprintf(o->now, sizeof o->now, "%s", c->now_title);
        o->now_start = c->now_start_ticks; o->now_end = c->now_end_ticks;
        for (int i = 0; i < n; i++)
            if (ps[i]->start_ticks >= c->now_end_ticks) {
                snprintf(o->next, sizeof o->next, "%s", ps[i]->name);
                o->next_start = ps[i]->start_ticks;
                break;
            }
    }
}

bool have_guide(void) {
    if (s_pn > 0) return true;
    for (int i = 0; i < s_n; i++) if (s_ch[i].now_title[0]) return true;
    return false;
}

// ---------------------------------------------------------------------------
//  Jobs
// ---------------------------------------------------------------------------

void submit(int kind) {
    if (s_jstate != J_IDLE || !s_run) return;
    memset(&s_job, 0, sizeof s_job);
    s_job.kind = kind;
    s_job.gen = s_gen;
    if (s_search_on) {
        snprintf(s_job.term, sizeof s_job.term, "%s", s_term);
        s_job.cfirst = 0;
        s_job.ccount = MAX_CH;
    } else if (paging() && s_pages[s_page].key < 0) {
        s_job.mode = s_pages[s_page].key == LP_FAVS ? 1 : 2;
        s_job.cfirst = 0;
        s_job.ccount = MAX_CH;
        s_job.n_recent = s_prefs.n_recent;
        for (int i = 0; i < s_prefs.n_recent; i++)
            snprintf(s_job.recent[i], sizeof s_job.recent[i], "%s", s_prefs.recent[i]);
    } else if (paging()) {
        s_job.cfirst = s_pages[s_page].first;
        s_job.ccount = s_pages[s_page].count < MAX_CH ? s_pages[s_page].count : MAX_CH;
    } else {
        s_job.cfirst = 0;
        s_job.ccount = MAX_CH;
    }
    if (kind & JOB_GUIDE) {
        int first = s_top - 2;
        if (first < 0) first = 0;
        int rows = s_n - first;
        if (rows > GUIDE_ROWS) rows = GUIDE_ROWS;
        if (rows < 0) rows = 0;
        s_job.first = first; s_job.rows = rows; s_job.n_ids = rows;
        for (int i = 0; i < rows; i++) snprintf(s_job.ids[i], sizeof s_job.ids[i], "%s", s_ch[first + i].id);
        s_job.w0 = s_win0;
        s_job.w1 = s_win0 + T_3H;
    }
    __sync_synchronize();
    s_jstate = J_QUEUED;
}

// ---------------------------------------------------------------------------
//  The viewer's arrangement (livetv_prefs.h)
// ---------------------------------------------------------------------------

void prefs_load(void) {
    if (s_prefs_loaded) return;
    s_prefs_loaded = true;
    lp_init(&s_prefs);
    FILE *f = fopen(jf_data_path(PREFS_FILE), "r");
    if (!f) return;
    char buf[2048];
    const size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = '\0';
    lp_parse(buf, &s_prefs);
}

void prefs_save(void) {
    char buf[2048];
    if (lp_format(&s_prefs, buf, sizeof buf) < 0) return;
    FILE *f = fopen(jf_data_path(PREFS_FILE), "w");
    if (!f) return;
    fputs(buf, f);
    fclose(f);
}

// The key of the page on screen while the tab is paged (and not searching), else 0.
int cur_key(void) {
    return (paging() && !s_search_on && s_page < s_npages) ? s_pages[s_page].key : 0;
}

// The pages on offer: what the server has, less what the viewer hid, behind
// My Channels / Recently watched.  `want` is the page to stay on (0 = the one
// the viewer left on last time).
void build_pages(int want) {
    int bases[MAX_PAGES], keys[MAX_PAGES];
    for (int i = 0; i < s_raw_n; i++) bases[i] = CATS[s_raw_cat[i]].base;
    const int n = lp_build_pages(&s_prefs, bases, s_raw_n, keys, MAX_PAGES);
    s_npages = 0;
    for (int k = 0; k < n; k++) {
        Page &pg = s_pages[s_npages++];
        pg.key = keys[k]; pg.first = 0; pg.count = 0; pg.cat = -1;
        for (int r = 0; r < s_raw_n; r++)
            if (keys[k] > 0 && bases[r] == keys[k]) {
                pg.first = s_raw_first[r]; pg.count = s_raw_count[r]; pg.cat = s_raw_cat[r];
            }
    }
    s_page = lp_pick_page(keys, n, want ? want : s_prefs.last);
}

// A channel was played.
void note_recent(const char *id) {
    if (!id || !id[0]) return;
    prefs_load();
    const int before = s_prefs.n_recent;
    lp_note_recent(&s_prefs, id);
    prefs_save();
    // The first channel watched brings the Recently watched page into the list.
    if (before == 0 && s_index_ok && s_npages > 0) build_pages(s_pages[s_page].key);
}

// A finished job, into the display buffers.  Render thread.
void collect(void) {
    if (s_jstate != J_DONE) return;
    __sync_synchronize();
    const Job j = s_job;
    if (j.kind & JOB_INDEX) {
        if (w_idx_ok) {
            const int key_was = (s_index_ok && s_page < s_npages) ? s_pages[s_page].key : 0;
            memcpy(s_nums, w_nums, sizeof(float) * (size_t)w_nnums);
            s_nnums = w_nnums;
            int bases[N_CATS];
            for (int i = 0; i < N_CATS; i++) bases[i] = CATS[i].base;
            s_raw_n = livetv_page_ranges(s_nums, s_nnums, bases, N_CATS,
                                         s_raw_first, s_raw_count, s_raw_cat, MAX_PAGES);
            prefs_load();
            build_pages(key_was);
            s_index_ok = true;
            char b[80];
            snprintf(b, sizeof b, "livetv: %d channel(s) in %d page(s)", s_nnums, s_npages);
            plog(b);
            const int key_now = (s_page < s_npages) ? s_pages[s_page].key : 0;
            if (!s_search_on && (key_now != key_was || !s_loaded)) {
                s_gen++; s_n = 0; s_loaded = false; s_sel = 0; s_top = 0;
            }
            s_last_refresh_us = 0;          // the page is known now: fetch it
        }
    }
    if ((j.kind & JOB_CHANNELS) && j.gen != s_gen) {
        // The page or the search changed while this one was on its way: drop it.
        s_last_refresh_us = 0;
    } else if (j.kind & JOB_CHANNELS) {
        if (w_ch_ok) {
            char keep[40] = "";
            if (s_sel >= 0 && s_sel < s_n) snprintf(keep, sizeof keep, "%s", s_ch[s_sel].id);
            memcpy(s_ch, w_ch, sizeof(JFChannel) * (size_t)w_n);
            s_n = w_n;
            s_loaded = true; s_failed = false;
            int at = -1;
            for (int i = 0; i < s_n; i++) if (keep[0] && !strcmp(s_ch[i].id, keep)) { at = i; break; }
            if (at >= 0) s_sel = at;
            if (s_sel >= s_n) s_sel = s_n > 0 ? s_n - 1 : 0;
            s_pg_rows = 0;                  // the guide's rows are by position: ask again
            // The server's list grew or shrank since the numbers were read.
            if (s_index_ok && !j.term[0] && j.mode == 0 && w_ctotal != s_nnums) s_index_tried = false;
        } else if (!s_loaded) {
            s_failed = true;
        }
        s_last_refresh_us = timing_get_us();
    }
    if ((j.kind & JOB_GUIDE) && j.gen == s_gen) {       // (not another list's rows)
        memcpy(s_pg, w_pg, sizeof(JFProgram) * (size_t)w_pn);
        s_pn = w_pn;
        s_pg_first = j.first; s_pg_rows = j.rows; s_pg_win0 = j.w0;
    }
    __sync_synchronize();
    s_jstate = J_IDLE;
}

// The guide data this view wants, and whether what is held already covers it.
bool guide_current(void) {
    if (s_pg_rows <= 0 || s_pg_win0 != s_win0) return false;
    const int vis = s_guide ? guide_rows_visible() : rows_visible();
    return s_top >= s_pg_first && s_top + vis <= s_pg_first + s_pg_rows;
}

// Once a frame, from a draw phase (render thread).
void tick(void) {
    static unsigned frame = ~0u;
    if (spine_frame_id() == frame) return;
    frame = spine_frame_id();

    collect();
    if (!s_run || !s_ch) return;
    const u64 now = timing_get_us();

    // The window follows the clock until the guide is being read.
    if (!s_guide) {
        const uint64_t base = floor_half_hour(jf_now_ticks());
        if (base != s_base_win0) { s_base_win0 = base; s_win0 = base; }
    }
    // Remember when the view last moved, so a long scroll asks once it stops.
    const int vis = s_guide ? guide_rows_visible() : rows_visible();
    if (s_seen_win_top != s_top || s_seen_first != vis || s_seen_win0 != s_win0) {
        s_seen_win_top = s_top; s_seen_first = vis; s_seen_win0 = s_win0;
        s_view_changed_us = now;
    }
    if (s_jstate != J_IDLE) return;

    if (!s_index_tried) {                   // before the list: it says which part to fetch
        s_index_tried = true;
        submit(JOB_INDEX);
        return;
    }
    if (s_last_refresh_us == 0 || now - s_last_refresh_us >= REFRESH_US) {
        if (s_last_refresh_us == 0) s_last_refresh_us = now;   // (so a failure does not retry every frame)
        submit(JOB_CHANNELS);
        return;
    }
    if (s_n > 0 && !guide_current() && now - s_view_changed_us >= DEBOUNCE_US)
        submit(JOB_GUIDE);
}

// ---------------------------------------------------------------------------
//  Selection
// ---------------------------------------------------------------------------

// The list on screen is about to be another one (a category, a search): empty
// it until the new one arrives, and let anything still on its way be dropped.
void list_changed(void) {
    s_gen++;
    s_n = 0; s_pn = 0;
    s_loaded = false; s_failed = false;
    s_sel = 0; s_top = 0;
    s_pg_rows = 0;
    s_last_refresh_us = 0;
}

// L2 / R2: another category.
void set_page(int p) {
    if (s_npages <= 1) return;
    if (p < 0) p = s_npages - 1;
    if (p >= s_npages) p = 0;
    if (p == s_page) return;
    s_page = p;
    s_prefs.last = s_pages[s_page].key;
    prefs_save();
    list_changed();
}

void search_begin(const char *term) {
    snprintf(s_term, sizeof s_term, "%s", term);
    s_search_on = true;
    list_changed();
}

void search_end(void) {
    s_search_on = false;
    s_term[0] = '\0';
    list_changed();
}

// Select: ask for a name, then list the channels that match it.
void search_ask(void) {
    char term[48];
    rsxSync();                              // finish this frame before the keyboard takes over
    flip();
    const int r = get_input(term, sizeof term, TR("Search channels"), false);
    init_btns();
    // Trailing spaces would only make the match narrower than it looks.
    int len = (int)strlen(term);
    while (len > 0 && term[len - 1] == ' ') term[--len] = '\0';
    if (r == 1 && len > 0) search_begin(term);
}

// "Sports | NFL", "News": the page's name.
void cat_label(int cat, char *out, int cap) {
    const CatDef &c = CATS[cat];
    const char *name = c.translate ? tr(c.label) : c.label;
    if (c.base < SPORTS_BELOW) snprintf(out, (size_t)cap, "%s | %s", TR("Sports"), name);
    else                       snprintf(out, (size_t)cap, "%s", name);
}

void key_label(int key, int cat, char *out, int cap) {
    if (key == LP_FAVS)        snprintf(out, (size_t)cap, "%s", TR("My Channels"));
    else if (key == LP_RECENT) snprintf(out, (size_t)cap, "%s", TR("Recently watched"));
    else                       cat_label(cat, out, cap);
}

void page_label(char *out, int cap) {
    key_label(s_pages[s_page].key, s_pages[s_page].cat, out, cap);
}

void keep_visible(void) {
    const int vis = s_guide ? guide_rows_visible() : rows_visible();
    if (s_sel < s_top) s_top = s_sel;
    if (s_sel >= s_top + vis) s_top = s_sel - vis + 1;
    if (s_top > s_n - vis) s_top = s_n - vis;
    if (s_top < 0) s_top = 0;
}

// ---------------------------------------------------------------------------
//  Start: which pages the tab offers
// ---------------------------------------------------------------------------

void customise(void) {
    if (!s_index_ok) return;
    struct Row { int key, cat, count; };
    Row rows[MAX_PAGES + 2];
    int nr = 0, bases[MAX_PAGES];
    rows[nr++] = { LP_FAVS, -1, 0 };
    rows[nr++] = { LP_RECENT, -1, 0 };
    for (int i = 0; i < s_raw_n && nr < MAX_PAGES + 2; i++) {
        bases[i] = CATS[s_raw_cat[i]].base;
        rows[nr++] = { bases[i], s_raw_cat[i], s_raw_count[i] };
    }
    const int was_key = s_page < s_npages ? s_pages[s_page].key : 0;
    bool changed = false;
    int sel = 0, top = 0;

    s_in_details = true;
    rsxSync();
    flip();
    init_btns();
    bool armed = false;
    while (running) {
        waitflip();
        sysUtilCheckCallback();
        poll_buttons();
        if (!armed) {
            if (!btn_cur.cross && !btn_cur.circle && !btn_cur.start) armed = true;
        } else {
            if (BTN_PRESSED(circle) || BTN_PRESSED(start)) { ui_sfx_play(SFX_CANCEL); break; }
            if (BTN_REPEAT(up)   && sel > 0)      { sel--; ui_sfx_play(SFX_CURSOR); }
            if (BTN_REPEAT(down) && sel < nr - 1) { sel++; ui_sfx_play(SFX_CURSOR); }
            if (BTN_PRESSED(cross)) {
                const int key = rows[sel].key;
                const bool hide = !lp_is_hidden(&s_prefs, key);
                if (hide && key > 0 && lp_visible_categories(&s_prefs, bases, s_raw_n) <= 1) {
                    ui_sfx_play(SFX_CANCEL);            // one category always stays
                } else {
                    lp_set_hidden(&s_prefs, key, hide);
                    changed = true;
                    ui_sfx_play(SFX_OPTION);
                }
            }
        }
        const int VIS = 8;
        if (sel < top) top = sel;
        if (sel >= top + VIS) top = sel - VIS + 1;

        clearScreen(XMB_BG);
        wave_draw();
        rsxSync();
        const int pw = UIS_W(640), rh = UIS_H(40), ph = UIS_H(104) + VIS * rh + UIS_H(14);
        const int px = ((int)display_width - pw) / 2, py = ((int)display_height - ph) / 2;
        drawRect((u32)px, (u32)py, (u32)pw, (u32)ph, XMB_PANEL);
        drawRect((u32)px, (u32)py, (u32)pw, 1, XMB_HAIRLINE);
        drawRect((u32)px, (u32)(py + ph - 1), (u32)pw, 1, XMB_HAIRLINE);
        drawRect((u32)px, (u32)py, 1, (u32)ph, XMB_HAIRLINE);
        drawRect((u32)(px + pw - 1), (u32)py, 1, (u32)ph, XMB_HAIRLINE);
        const int cx = px + UIS_W(30);
        drawTTF((u32)cx, (u32)(py + UIS_H(20)), TR("Customise Live TV"), UIS_TF(24), XMB_WHITE, true);
        drawTTF((u32)cx, (u32)(py + UIS_H(58)),
                TR("Ticked pages are shown. L2 / R2 skip the others."), UIS_TF(13), XMB_TEXT_DIM);
        for (int r = 0; r < VIS && top + r < nr; r++) {
            const int i = top + r, y = py + UIS_H(90) + r * rh, h = rh - UIS_H(4);
            const bool is_sel = (i == sel);
            const bool shown = !lp_is_hidden(&s_prefs, rows[i].key);
            if (is_sel) {
                drawRect((u32)(px + UIS_W(12)), (u32)y, (u32)(pw - UIS_W(24)), (u32)h, XMB_PANEL_HI);
                drawRect((u32)(px + UIS_W(8)), (u32)y, UIS_W(3), (u32)h, XMB_ACCENT);
            }
            const int bs = UIS_H(18), bx = px + UIS_W(30), by = y + (h - bs) / 2;
            drawRect((u32)bx, (u32)by, (u32)bs, (u32)bs, XMB_TEXT_FAINT);
            drawRect((u32)(bx + 2), (u32)(by + 2), (u32)(bs - 4), (u32)(bs - 4), XMB_PANEL);
            if (shown) drawRect((u32)(bx + 4), (u32)(by + 4), (u32)(bs - 8), (u32)(bs - 8), XMB_ACCENT);
            char lab[96];
            key_label(rows[i].key, rows[i].cat, lab, sizeof lab);
            drawTTF((u32)(bx + bs + UIS_W(16)), (u32)(y + (h - UIS_H(18)) / 2 - UIS_H(1)), lab, UIS_TF(18),
                    shown ? (is_sel ? XMB_WHITE : XMB_TEXT) : XMB_TEXT_FAINT);
            if (rows[i].key > 0) {
                char cnt[16];
                snprintf(cnt, sizeof cnt, "%d", rows[i].count);
                const int cw = ttf_text_width(cnt, UIS_TF(15));
                drawTTF((u32)(px + pw - UIS_W(34) - cw), (u32)(y + (h - UIS_H(15)) / 2 - UIS_H(1)), cnt,
                        UIS_TF(15), XMB_TEXT_FAINT);
            }
        }
        { static const Hint hh[] = {{'X', TRN("Show / hide")}, {'C', TRN("Done")}};
          draw_hints_bar(hh, 2); }
        flip();
    }
    init_btns();
    s_in_details = false;
    if (changed) {
        prefs_save();
        build_pages(was_key);
        if ((s_page < s_npages ? s_pages[s_page].key : 0) != was_key) list_changed();
    }
}

void play_channel(int idx) {
    if (idx < 0 || idx >= s_n) return;
    JFItem jf;
    memset(&jf, 0, sizeof jf);
    snprintf(jf.id,   sizeof jf.id,   "%s", s_ch[idx].id);
    snprintf(jf.name, sizeof jf.name, "%s", s_ch[idx].name);
    snprintf(jf.type, sizeof jf.type, "TvChannel");
    note_recent(jf.id);
    show_player(&jf, 0, NULL);
    s_last_refresh_us = 0;                  // what is on has moved on
    init_btns();
    // Channel up/down inside the player: the list follows to where it ended.
    if (player_live_last_channel() && strcmp(player_live_last_channel(), jf.id) != 0)
        note_recent(player_live_last_channel());
    const int at = xmb_livetv_index_of(player_live_last_channel());
    if (at >= 0) { s_sel = at; s_pg_rows = 0; keep_visible(); }
}

// ---------------------------------------------------------------------------
//  A programme's details
// ---------------------------------------------------------------------------

struct OverviewJob { char id[40]; char text[1024]; };
void overview_work(void *arg) {
    OverviewJob *j = (OverviewJob *)arg;
    jf_fetch_program_overview(j->id, j->text, sizeof j->text);
}

// Word-wrapped text; returns the lines drawn.
int wrap_text(int x, int y, const char *text, float px, u32 colour, int max_w, int pitch, int max_lines) {
    const char *p = text;
    int lines = 0;
    while (*p && lines < max_lines) {
        while (*p == ' ') p++;
        char line[160];
        int n = 0, last_space = -1;
        while (p[n] && p[n] != '\n' && n < (int)sizeof line - 1) {
            line[n] = p[n];
            line[n + 1] = '\0';
            if (p[n] == ' ') last_space = n;
            if (ttf_text_width(line, px) > max_w) {
                if (last_space > 0) n = last_space;
                else if (n > 0) n--;
                break;
            }
            n++;
        }
        if (n == 0) n = 1;
        memcpy(line, p, (size_t)n);
        line[n] = '\0';
        drawTTF((u32)x, (u32)y, line, px, colour);
        y += pitch;
        lines++;
        p += n;
        if (*p == '\n') p++;
    }
    return lines;
}

void show_details(const JFChannel *c, const JFProgram *p) {
    OverviewJob job;
    memset(&job, 0, sizeof job);
    snprintf(job.id, sizeof job.id, "%s", p->id);
    s_in_details = true;
    loading_run(overview_work, &job, TR("Loading"), false);

    rsxSync();
    flip();
    init_btns();
    bool armed = false;
    while (running) {
        waitflip();
        sysUtilCheckCallback();
        poll_buttons();
        if (!armed) {
            if (!btn_cur.cross && !btn_cur.circle) armed = true;
        } else if (BTN_PRESSED(cross) || BTN_PRESSED(circle)) {
            ui_sfx_play(BTN_PRESSED(cross) ? SFX_DECIDE : SFX_CANCEL);
            break;
        }
        clearScreen(XMB_BG);
        wave_draw();
        rsxSync();
        const int pw = UIS_W(640), ph = UIS_H(330);
        const int px = ((int)display_width - pw) / 2, py = ((int)display_height - ph) / 2;
        drawRect((u32)px, (u32)py, (u32)pw, (u32)ph, XMB_PANEL);
        drawRect((u32)px, (u32)py, (u32)pw, 1, XMB_HAIRLINE);
        drawRect((u32)px, (u32)(py + ph - 1), (u32)pw, 1, XMB_HAIRLINE);
        drawRect((u32)px, (u32)py, 1, (u32)ph, XMB_HAIRLINE);
        drawRect((u32)(px + pw - 1), (u32)py, 1, (u32)ph, XMB_HAIRLINE);
        const int cx = px + UIS_W(30), mw = pw - UIS_W(60);
        int y = py + UIS_H(26);
        char title[160];
        snprintf(title, sizeof title, "%s", p->name);
        int tl = (int)strlen(title);
        while (tl > 1 && ttf_text_width(title, UIS_TF(24), true) > mw) title[--tl] = '\0';
        drawTTF((u32)cx, (u32)y, title, UIS_TF(24), XMB_WHITE, true);
        y += UIS_H(40);
        char a[16], b[16], when[160];
        fmt_hm(p->start_ticks, a, sizeof a);
        fmt_hm(p->end_ticks, b, sizeof b);
        snprintf(when, sizeof when, "%s - %s  \xC2\xB7  %s", a, b, c->name);
        drawTTF((u32)cx, (u32)y, when, UIS_TF(15), XMB_ACCENT_ALT);
        y += UIS_H(28);
        if (p->episode_title[0]) {
            drawTTF((u32)cx, (u32)y, p->episode_title, UIS_TF(15), XMB_TEXT);
            y += UIS_H(28);
        }
        if (job.text[0])
            wrap_text(cx, y, job.text, UIS_TF(14), XMB_TEXT_DIM, mw, UIS_H(22), 6);
        else
            drawTTF((u32)cx, (u32)y, TR("No description."), UIS_TF(14), XMB_TEXT_FAINT);
        { static const Hint h[] = {{'X', TRN("OK")}};
          draw_hints_bar(h, 1); }
        flip();
    }
    init_btns();
    s_in_details = false;
}

// ---------------------------------------------------------------------------
//  The guide's focus
// ---------------------------------------------------------------------------

// The programme in `row` (a channel index) at the focus time, or NULL.
const JFProgram *focus_cell(int row) {
    if (row < 0 || row >= s_n) return NULL;
    const JFProgram *ps[24];
    const int n = progs_of(s_ch[row].id, ps, 24);
    const JFProgram *best = NULL;
    for (int i = 0; i < n; i++)
        if (ps[i]->start_ticks <= s_gt && s_gt < ps[i]->end_ticks) best = ps[i];
    return best;
}

void guide_move_time(int dir) {
    const JFProgram *cur = focus_cell(s_sel);
    const JFProgram *ps[24];
    const int n = progs_of(s_ch[s_sel].id, ps, 24);
    if (dir > 0) {
        // The next programme after the focused one (or after the focus time).
        const uint64_t after = cur ? cur->end_ticks : s_gt;
        for (int i = 0; i < n; i++)
            if (ps[i]->start_ticks >= after) { s_gt = ps[i]->start_ticks; goto moved; }
        s_gt = after + (cur ? 0 : T_30M);         // nothing known: step by time
    } else {
        const uint64_t before = cur ? cur->start_ticks : s_gt;
        for (int i = n - 1; i >= 0; i--)
            if (ps[i]->end_ticks <= before) { s_gt = ps[i]->start_ticks; goto moved; }
        s_gt = before > T_30M ? before - T_30M : before;
    }
moved:
    // Scroll the window so the focus stays inside it, never earlier than the
    // window that starts at the last half hour.
    if (s_gt >= s_win0 + T_3H) s_win0 = floor_half_hour(s_gt) - T_30M * 2;
    if (s_gt < s_win0)         s_win0 = floor_half_hour(s_gt);
    if (s_win0 < s_base_win0)  s_win0 = s_base_win0;
}

// Is the focus on the first thing the guide can show on this row?
bool at_first_column(void) {
    if (s_win0 > s_base_win0) return false;
    const JFProgram *ps[24];
    const int n = progs_of(s_ch[s_sel].id, ps, 24);
    const JFProgram *cur = focus_cell(s_sel);
    if (!cur) return s_gt <= s_win0 + T_30M;
    for (int i = 0; i < n; i++)
        if (ps[i]->end_ticks <= cur->start_ticks && ps[i]->end_ticks > s_win0) return false;
    return true;
}

void close_guide(void) {
    s_guide = false;
    keep_visible();
    s_view_changed_us = timing_get_us();
}

void open_guide(void) {
    const uint64_t now = jf_now_ticks();
    s_guide = true;
    s_base_win0 = floor_half_hour(now);
    s_win0 = s_base_win0;
    s_gt = now;
    keep_visible();
    s_view_changed_us = timing_get_us();
}

}   // namespace

// ---------------------------------------------------------------------------
//  The tab, as the XMB drives it
// ---------------------------------------------------------------------------

void xmb_livetv_on_enter(void) {
    if (!ensure_alloc()) return;
    ensure_worker();
    read_utc_offset();
    s_guide = false;
    s_top = 0;
    if (s_search_on) search_end();              // a search belongs to one visit
    if (!s_index_ok) s_index_tried = false;     // try the numbers again
    s_last_refresh_us = 0;          // refresh now: the list may be a minute old
    s_view_changed_us = timing_get_us();
    if (s_sel >= s_n) s_sel = 0;
    keep_visible();
}

void xmb_livetv_stop(void) {
    s_run = false;
    if (s_have_tid) { u64 r; sysThreadJoin(s_tid, &r); s_have_tid = false; }
}

int  xmb_livetv_count(void) { return s_n; }
bool xmb_livetv_get(int i, JFChannel *out) {
    if (i < 0 || i >= s_n || !out) return false;
    *out = s_ch[i];
    return true;
}
int xmb_livetv_index_of(const char *id) {
    if (!id) return -1;
    for (int i = 0; i < s_n; i++) if (!strcmp(s_ch[i].id, id)) return i;
    return -1;
}

bool xmb_livetv_at_top(void) { return s_sel <= 0; }
bool xmb_livetv_modal(void)  { return s_guide || s_in_details || s_search_on; }

bool xmb_input_livetv(void) {
    if (BTN_PRESSED(l1)) { s_guide = false; xmb_switch_tab(xmb_next_enabled(g_active_tab, -1)); return false; }
    if (BTN_PRESSED(r1)) { s_guide = false; xmb_switch_tab(xmb_next_enabled(g_active_tab, +1)); return false; }
    if (!s_guide && !s_in_details) {
        if (BTN_PRESSED(select)) { search_ask(); return false; }
        if (BTN_PRESSED(start) && paging() && !s_search_on) { customise(); return false; }
        if (s_search_on && BTN_PRESSED(circle)) { search_end(); ui_sfx_play(SFX_CANCEL); return false; }
        if (paging() && !s_search_on) {
            if (BTN_PRESSED(l2)) { set_page(s_page - 1); ui_sfx_play(SFX_CURSOR); return false; }
            if (BTN_PRESSED(r2)) { set_page(s_page + 1); ui_sfx_play(SFX_CURSOR); return false; }
        }
    }
    if (!s_ch || s_n <= 0) return false;

    if (!s_guide) {
        const int was = s_sel;
        if (BTN_REPEAT(up)   && s_sel > 0)       s_sel--;
        if (BTN_REPEAT(down) && s_sel < s_n - 1) s_sel++;
        if (s_sel != was) { keep_visible(); ui_sfx_play(SFX_CURSOR); }
        if (BTN_PRESSED(cross)) { play_channel(s_sel); return false; }
        if (BTN_PRESSED(triangle)) {
            // Optimistic: star it now, put it back if the server refuses, and
            // keep the selection on the same channel as the list reorders.
            const bool want = !s_ch[s_sel].favourite;
            char id[40];
            snprintf(id, sizeof id, "%s", s_ch[s_sel].id);
            if (jf_set_favourite(id, want)) {
                if (!want && cur_key() == LP_FAVS) {
                    // Un-starred on My Channels: it leaves the page.
                    memmove(&s_ch[s_sel], &s_ch[s_sel + 1], sizeof(JFChannel) * (size_t)(s_n - s_sel - 1));
                    s_n--;
                    if (s_sel >= s_n) s_sel = s_n > 0 ? s_n - 1 : 0;
                } else {
                    s_ch[s_sel].favourite = want;
                    livetv_sort_channels(s_ch, s_n);
                    const int at = xmb_livetv_index_of(id);
                    if (at >= 0) s_sel = at;
                }
                s_pg_rows = 0;
                keep_visible();
                ui_sfx_play(SFX_OPTION);
            }
        }
        if (BTN_PRESSED(right) && have_guide()) { open_guide(); ui_sfx_play(SFX_CURSOR); }
        return false;
    }

    // ---- the guide ----
    if (BTN_PRESSED(circle)) { close_guide(); ui_sfx_play(SFX_CANCEL); return false; }
    const int was = s_sel;
    if (BTN_REPEAT(up)   && s_sel > 0)       s_sel--;
    if (BTN_REPEAT(down) && s_sel < s_n - 1) s_sel++;
    if (s_sel != was) { keep_visible(); ui_sfx_play(SFX_CURSOR); }
    if (BTN_PRESSED(left) && at_first_column()) { close_guide(); ui_sfx_play(SFX_CURSOR); return false; }
    if (BTN_REPEAT(left) && !at_first_column())   guide_move_time(-1);
    if (BTN_REPEAT(right))                        guide_move_time(+1);
    if (BTN_PRESSED(cross)) {
        const uint64_t now = jf_now_ticks();
        const JFProgram *p = focus_cell(s_sel);
        if (p && p->start_ticks <= now && now < p->end_ticks) {
            play_channel(s_sel);
        } else if (p) {
            JFProgram copy = *p;                  // the data may refresh under the panel
            const JFChannel ch = s_ch[s_sel];
            show_details(&ch, &copy);
        } else {
            play_channel(s_sel);                  // no guide for this row: just watch it
        }
    }
    return false;
}

void xmb_livetv_hints(void) {
    Hint h[10]; int n = 0;
    if (!s_guide) {
        h[n].glyph = 'X'; h[n].label = TRN("Watch"); n++;
        h[n].glyph = 'T'; h[n].label = TRN("Favourite"); n++;
        if (have_guide()) { h[n].glyph = 'E'; h[n].label = TRN("Guide"); n++; }
        // L2 and R2 pair into one "Category" hint (an empty label pairs with the next).
        if (paging() && !s_search_on) {
            h[n].glyph = 'L'; h[n].label = ""; n++;
            h[n].glyph = 'R'; h[n].label = TRN("Category"); n++;
        }
        h[n].glyph = 'B'; h[n].label = TRN("Search"); n++;
        if (paging() && !s_search_on) { h[n].glyph = 'A'; h[n].label = TRN("Customise"); n++; }
        if (s_search_on) { h[n].glyph = 'C'; h[n].label = TRN("Back"); n++; }
    } else {
        h[n].glyph = 'X'; h[n].label = TRN("Watch / details"); n++;
        h[n].glyph = 'E'; h[n].label = TRN("Move"); n++;
        h[n].glyph = 'C'; h[n].label = TRN("List"); n++;
    }
    // Every bar but Search's ends with Square and the visualiser it switches to.
    h[n].glyph = 'S'; h[n].label = wave_vis_next_label(); n++;
    draw_hints_bar(h, n);
}

// ---------------------------------------------------------------------------
//  Drawing: GPU phase (guide cells), CPU phase (logos, bars), text
// ---------------------------------------------------------------------------

static const int ROW_MAX_PROGS = 24;

void xmb_livetv_gpu(void) {
    tick();
    if (!s_guide || !s_ch || s_n <= 0) return;
    const int rows = guide_rows_visible();
    const int radius = UIS_H(4);
    for (int r = 0; r < rows && s_top + r < s_n; r++) {
        const int y = guide_top() + r * guide_pitch();
        const JFProgram *ps[ROW_MAX_PROGS];
        const int n = progs_of(s_ch[s_top + r].id, ps, ROW_MAX_PROGS);
        const JFProgram *foc = (s_top + r == s_sel) ? focus_cell(s_sel) : NULL;
        for (int i = 0; i < n; i++) {
            if (ps[i]->end_ticks <= s_win0 || ps[i]->start_ticks >= s_win0 + T_3H) continue;
            const int x0 = x_of_time(ps[i]->start_ticks), x1 = x_of_time(ps[i]->end_ticks);
            if (x1 - x0 < UIS_W(8)) continue;
            wave_draw_rrect_outline_gpu(x0 + 1, y, x1 - x0 - 2, guide_row_h(), radius, 1,
                                        XMB_HAIRLINE, 255, XMB_PANEL, XMB_PANEL, 110);
        }
        if (foc) {
            const int x0 = x_of_time(foc->start_ticks), x1 = x_of_time(foc->end_ticks);
            if (x1 - x0 >= UIS_W(8)) spine_focus_ring_gpu(x0 + 1, y, x1 - x0 - 2, guide_row_h());
        } else if (s_top + r == s_sel) {
            spine_focus_ring_gpu(guide_cells_x(), y, guide_cells_w(), guide_row_h());   // an empty row
        }
    }
    // The "now" line, through every row.
    const uint64_t now = jf_now_ticks();
    if (now >= s_win0 && now < s_win0 + T_3H) {
        const int x = x_of_time(now);
        const int y0 = guide_top() - UIS_H(4);
        const int h = rows * guide_pitch();
        ui_rect_gpu_draw(x, y0, UIS_W(2), h, XMB_ACCENT, 255);
    }
}

// A logo, or a plain panel when the channel has none.
static void draw_logo(const JFChannel *c, int x, int y, int w, int h) {
    bool drawn = false;
    if (c->logo_tag[0]) {
        char key[48];
        snprintf(key, sizeof key, "logo:%s", c->id);
        drawn = xmb_cpu_blit_thumb(key, x, y, w, h);
    }
    if (!drawn) drawRect((u32)x, (u32)y, (u32)w, (u32)h, XMB_THUMB_DIM);
}

void xmb_cpu_draw_livetv(void) {
    tick();
    if (!s_ch || s_n <= 0) return;
    const uint64_t now = jf_now_ticks();

    if (!s_guide) {
        const int vis = rows_visible();
        const int lx = list_x();
        for (int r = 0; r < vis && s_top + r < s_n; r++) {
            const int i = s_top + r;
            const int y = band_top() + r * row_pitch();
            const bool sel = (i == s_sel);
            drawRect((u32)lx, (u32)y, (u32)XMB_LIST_W, (u32)row_h(), sel ? XMB_PANEL_HI : XMB_PANEL);
            if (sel) drawRect((u32)(lx - UIS_W(4)), (u32)y, UIS_W(3), (u32)row_h(), XMB_ACCENT);
            const int lw = UIS_W(96), lh = UIS_H(54);
            draw_logo(&s_ch[i], lx + UIS_W(70), y + (row_h() - lh) / 2, lw, lh);
            // The programme's progress, a thin bar under its title on the right.
            NowNext nn;
            now_next(&s_ch[i], now, &nn);
            const int pm = livetv_progress_permille(now, nn.now_start, nn.now_end);
            if (pm >= 0) {
                const int bw = UIS_W(250), bx = lx + XMB_LIST_W - UIS_W(16) - bw;
                const int by = y + UIS_H(31);
                drawRect((u32)bx, (u32)by, (u32)bw, UIS_H(3), XMB_HAIRLINE);
                drawRect((u32)bx, (u32)by, (u32)(bw * pm / 1000), UIS_H(3), XMB_ACCENT);
            }
        }
        return;
    }

    // The guide: the pinned logos and the time grid.
    const int rows = guide_rows_visible();
    for (int r = 0; r < rows && s_top + r < s_n; r++) {
        const int i = s_top + r, y = guide_top() + r * guide_pitch();
        const bool sel = (i == s_sel);
        drawRect((u32)guide_x0(), (u32)y, (u32)guide_logo_w(), (u32)guide_row_h(),
                 sel ? XMB_PANEL_HI : XMB_PANEL);
        if (sel) drawRect((u32)(guide_x0() - UIS_W(4)), (u32)y, UIS_W(3), (u32)guide_row_h(), XMB_ACCENT);
        const int lh = UIS_H(40), lw = UIS_W(70);
        draw_logo(&s_ch[i], guide_x0() + guide_logo_w() - lw - UIS_W(8), y + (guide_row_h() - lh) / 2, lw, lh);
        // Without the RSX card path the cells are plain panels.
        if (!ui_card_gpu_ready()) {
            const JFProgram *ps[ROW_MAX_PROGS];
            const int n = progs_of(s_ch[i].id, ps, ROW_MAX_PROGS);
            for (int k = 0; k < n; k++) {
                if (ps[k]->end_ticks <= s_win0 || ps[k]->start_ticks >= s_win0 + T_3H) continue;
                const int x0 = x_of_time(ps[k]->start_ticks), x1 = x_of_time(ps[k]->end_ticks);
                if (x1 - x0 < UIS_W(8)) continue;
                drawRect((u32)(x0 + 1), (u32)y, (u32)(x1 - x0 - 2), (u32)guide_row_h(),
                         (sel && focus_cell(i) == ps[k]) ? XMB_PANEL_HI : XMB_PANEL);
            }
        }
    }
    // Half-hour grid lines in the header.
    for (int k = 0; k <= 6; k++) {
        const int x = guide_cells_x() + (int)((long long)guide_cells_w() * k / 6);
        drawRect((u32)x, (u32)(band_top() + UIS_H(18)), 1, UIS_H(10), XMB_HAIRLINE);
    }
}

// Text clipped to max_w with an ellipsis.
//
// Trimming one character at a time re-measures the whole string at every step,
// and a list screen clips the same few dozen programme titles every frame: that
// was 9-14 ms of the text phase (frames of 21-26 ms) with the guide loaded.  A
// title's clipped form only changes when the title does, so keep the results.
struct ClipEntry { char text[160]; char out[160]; int px100, max_w; bool bold, used; };

static void clip_text(const char *text, float px, int max_w, bool bold, char *buf, size_t cap) {
    static ClipEntry cache[128];
    static int next = 0;
    const int px100 = (int)(px * 100.0f + 0.5f);
    for (int i = 0; i < 128; i++) {
        const ClipEntry &e = cache[i];
        if (e.used && e.px100 == px100 && e.max_w == max_w && e.bold == bold && !strcmp(e.text, text)) {
            snprintf(buf, cap, "%s", e.out);
            return;
        }
    }
    snprintf(buf, cap, "%s", text);
    int len = (int)strlen(buf);
    if (ttf_text_width(buf, px, bold) > max_w) {
        while (len > 3 && ttf_text_width(buf, px, bold) > max_w) {
            buf[--len] = '\0';
            if (len > 3) { buf[len - 1] = '.'; buf[len - 2] = '.'; buf[len - 3] = '.'; }
        }
    }
    ClipEntry &e = cache[next];
    next = (next + 1) % 128;
    snprintf(e.text, sizeof e.text, "%s", text);
    snprintf(e.out, sizeof e.out, "%s", buf);
    e.px100 = px100; e.max_w = max_w; e.bold = bold; e.used = true;
}

static void clip_draw(int x, int y, const char *text, float px, u32 colour, int max_w, bool bold) {
    char buf[160];
    clip_text(text, px, max_w, bold, buf, sizeof buf);
    drawTTF((u32)x, (u32)y, buf, px, colour, bold);
}

void xmb_draw_livetv(void) {
    tick();
    const int cx = (int)display_width / 2;
    if (!s_ch || s_alloc_failed) {
        const char *m = TR("Live TV is unavailable");
        drawTTF((u32)(cx - ttf_text_width(m, UIS_TF(18)) / 2), (u32)(band_top() + UIS_H(60)), m,
                UIS_TF(18), XMB_TEXT_DIM);
        return;
    }
    if (s_search_on && !s_guide) {
        char line[200];
        snprintf(line, sizeof line, TR("Search: %s  (%d found)  -  O to go back"), s_term, s_n);
        drawTTF((u32)list_x(), (u32)(band_top() - UIS_H(18)), line, UIS_TF(13), XMB_ACCENT_ALT);
    } else if (paging() && !s_guide) {
        char lab[96], line[160];
        page_label(lab, sizeof lab);
        snprintf(line, sizeof line, "L2 <   %s   (%d / %d)   > R2", lab, s_page + 1, s_npages);
        drawTTF((u32)list_x(), (u32)(band_top() - UIS_H(18)), line, UIS_TF(13), XMB_ACCENT_ALT);
    }
    if (s_n <= 0) {
        const char *m = s_failed ? TR("Couldn't load the channel list")
                      : !s_loaded ? TR("Loading channels...")
                      : s_search_on ? TR("No channels found")
                      : cur_key() == LP_FAVS ? TR("No favourites yet. Press the Favourite button on a channel.")
                      : cur_key() == LP_RECENT ? TR("Nothing watched yet")
                      : TR("No channels");
        drawTTF((u32)(cx - ttf_text_width(m, UIS_TF(18)) / 2), (u32)(band_top() + UIS_H(60)), m,
                UIS_TF(18), XMB_TEXT_DIM);
        return;
    }
    const uint64_t now = jf_now_ticks();

    if (!s_guide) {
        const int vis = rows_visible();
        const int lx = list_x();
        for (int r = 0; r < vis && s_top + r < s_n; r++) {
            const int i = s_top + r;
            const JFChannel *c = &s_ch[i];
            const int y = band_top() + r * row_pitch();
            const bool sel = (i == s_sel);
            const u32 clr = sel ? XMB_WHITE : XMB_TEXT_DIM;
            if (c->favourite)
                drawIcon((u32)(lx + UIS_W(10)), (u32)(y + (row_h() - UIS_H(14)) / 2 - UIS_H(1)),
                         ICON_STAR, UIS_TF(14.0f), XMB_ACCENT_ALT);
            // Channel number: right-aligned in its column, so the digits line up.
            if (c->number[0]) {
                const int nw = ttf_text_width(c->number, UIS_TF(17));
                drawTTF((u32)(lx + UIS_W(62) - nw), (u32)(y + (row_h() - UIS_H(17)) / 2 - UIS_H(1)),
                        c->number, UIS_TF(17), XMB_TEXT_FAINT);
            }
            // Name; the programme (title, then "Next:") on the right.
            const int name_x = lx + UIS_W(182);
            const int right_w = UIS_W(270);
            const int name_w = XMB_LIST_W - UIS_W(182) - right_w - UIS_W(24);
            clip_draw(name_x, y + (row_h() - UIS_H(18)) / 2 - UIS_H(1), c->name, UIS_TF(18), clr, name_w, sel);
            NowNext nn;
            now_next(c, now, &nn);
            const int rx = lx + XMB_LIST_W - UIS_W(16) - UIS_W(250);
            if (nn.now[0]) {
                clip_draw(rx, y + UIS_H(8), nn.now, UIS_TF(14), sel ? XMB_TEXT : XMB_TEXT_DIM,
                          UIS_W(250), false);
                if (nn.next[0]) {
                    char hm[16], line[160];
                    fmt_hm(nn.next_start, hm, sizeof hm);
                    snprintf(line, sizeof line, TR("Next: %s  %s"), hm, nn.next);
                    clip_draw(rx, y + UIS_H(40), line, UIS_TF(12), XMB_TEXT_FAINT, UIS_W(250), false);
                }
            }
        }
        if (s_n > vis) {
            char pos[24];
            snprintf(pos, sizeof pos, "%d / %d", s_sel + 1, s_n);
            const int pw = ttf_text_width(pos, UIS_TF(13));
            drawTTF((u32)(lx + XMB_LIST_W - pw), (u32)(band_top() - UIS_H(18)), pos, UIS_TF(13), XMB_TEXT_FAINT);
        }
        return;
    }

    // ---- the guide ----
    // The time header: a label at every half hour.
    for (int k = 0; k < 6; k++) {
        char hm[16];
        fmt_hm(s_win0 + (uint64_t)k * T_30M, hm, sizeof hm);
        const int x = guide_cells_x() + (int)((long long)guide_cells_w() * k / 6);
        drawTTF((u32)(x + UIS_W(4)), (u32)(band_top() + UIS_H(2)), hm, UIS_TF(13), XMB_TEXT_DIM);
    }
    const int rows = guide_rows_visible();
    for (int r = 0; r < rows && s_top + r < s_n; r++) {
        const int i = s_top + r, y = guide_top() + r * guide_pitch();
        const JFChannel *c = &s_ch[i];
        const bool sel = (i == s_sel);
        if (c->number[0]) drawTTF((u32)(guide_x0() + UIS_W(10)), (u32)(y + UIS_H(8)), c->number,
                                  UIS_TF(15), sel ? XMB_WHITE : XMB_TEXT_FAINT);
        clip_draw(guide_x0() + UIS_W(10), y + UIS_H(30), c->name, UIS_TF(12),
                  sel ? XMB_TEXT : XMB_TEXT_DIM, UIS_W(66), false);
        const JFProgram *ps[ROW_MAX_PROGS];
        const int n = progs_of(c->id, ps, ROW_MAX_PROGS);
        const JFProgram *foc = sel ? focus_cell(i) : NULL;
        for (int k = 0; k < n; k++) {
            if (ps[k]->end_ticks <= s_win0 || ps[k]->start_ticks >= s_win0 + T_3H) continue;
            const int x0 = x_of_time(ps[k]->start_ticks), x1 = x_of_time(ps[k]->end_ticks);
            if (x1 - x0 < UIS_W(40)) continue;
            const bool is_f = (foc == ps[k]);
            clip_draw(x0 + UIS_W(10), y + (guide_row_h() - UIS_H(14)) / 2 - UIS_H(1), ps[k]->name,
                      UIS_TF(14), is_f ? XMB_WHITE : XMB_TEXT, x1 - x0 - UIS_W(20), is_f);
        }
    }
}
