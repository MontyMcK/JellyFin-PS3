// XMB search tab — OSK input and Jellyfin search.

#include <stdio.h>
#include <string.h>

#include <unistd.h>      // usleep -- the retry pause below
#include <sys/thread.h>

#include "ui_internal.h"
#include "jellyfin_api.h"
#include "player.h"
#include "plog.h"
#include "timing.h"

// Search used to run a BLOCKING http_request on the UI thread, on every single
// keystroke: type "matrix" and that is six round trips, each one freezing the
// OSK until the server answers.  On top of feeling broken, the one- and
// two-character queries in that sequence are the worst ones to send — Jellyfin
// matches them loosely, so results appeared, vanished and reappeared as the
// term grew, which is the "sometimes results, sometimes none" part.
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
//
// The debounce fixed the frequency but not the freeze, and the freeze then got
// WORSE: this server takes 2.5-8.4 s to emit the first byte of a search (see
// HANDOFF.md), so http.cpp now allows up to 24 s for it — which as a blocking
// call on the render thread is a 24-second dead console.  The request
// therefore runs on its own thread now, exactly like the thumbnail fetch and
// the update check, and the UI keeps running at 60 Hz with a "Searching..."
// line while it works.
//
// Two rules make the threading safe, and both matter:
//
//   1. The worker does NOT touch g_search_results.  It fills a private array,
//      and the render thread copies it across when it collects the result, so
//      the array the draw code walks is only ever written by the thread that
//      draws it.
//   2. The worker does NOT use the shared `responseBuffer`.  http_request()
//      holds a mutex while it fills the caller's buffer, but the CALLER parses
//      it after unlocking — so two callers sharing that global would happily
//      parse each other's response.  Search gets its own.
// The pause only has to cover the gap between releasing X on a key and
// starting to walk to the next one; any pad activity after that pushes the
// deadline back (below), so it no longer needs to span a whole key walk.
#define SEARCH_DEBOUNCE_US 600000   // idle pause before the query fires
// SEARCH_MIN_CHARS (shorter terms are noise, not a search) lives in
// ui_visuals.h so the renderer can name the number in its hint.

static u64  s_search_edit_us = 0;   // when the term last changed
static bool s_search_pending = false;
static u64  s_hold_since_us  = 0;   // pad has read "held" continuously since
static bool s_hold_logged    = false;

// What the results area should say.  The renderer used to print "No results"
// whenever the count was zero and the box was not empty -- which is also
// what a one-letter term (never queried) and a term still inside the
// debounce look like.  On the console that read as a dead search: type A,
// "No results", clear, type I N, "No results", leave -- all before a single
// query had gone out.  The state says which of those it actually is.
int g_search_state = SEARCH_IDLE;

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

// --- the search worker ----------------------------------------------------
//
// Private response buffer: see rule 2 in the note at the top of this file.
// Sized to match RESPONSE_SIZE because search is the LARGEST response the app
// asks for -- a broad term like "the" measured 255 KB against 63 KB for a
// whole library tab -- so it is the one request that must not be the one that
// gets truncated.
static char     s_search_resp[RESPONSE_SIZE];

static sys_ppu_thread_t s_search_thr     = 0;
static bool             s_search_running = false;   // render thread only
static volatile bool    s_search_done    = false;   // worker -> render thread
static char             s_search_term[sizeof(g_search_buf)];  // term in flight
static XMBItem          s_search_out[XMB_ITEMS_MAX];
static int              s_search_out_n   = 0;
static int              s_search_status  = 0;

bool xmb_search_in_flight(void) { return s_search_running; }

static void search_thread_fn(void *arg) {
    (void)arg;

    char encoded[192];
    url_encode_query(s_search_term, encoded, sizeof(encoded));

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

    // Retry a "no matches" answer, because on this server it is often a lie.
    //
    // THE PROBLEM IS SERVER-SIDE, and that is measured, not assumed: the exact
    // same URL, issued from a PC, ALSO comes back as a well-formed
    // `{"Items":[],"TotalRecordCount":0,"StartIndex":0}` -- 48 bytes, HTTP 200,
    // no timeout, no parse failure -- for some terms.  What the failures have
    // in common is how long they take:
    //
    //     DU  10.36s ->    48 B  count=0        DI   7.66s -> 112 KB count=76
    //     TI  10.38s ->    48 B  count=0        DIE 12.39s -> 114 KB count=60
    //     BAT 10.2 s ->    48 B  count=0        BA   3.0 s ->  79 KB count=40
    //
    // Everything that comes back empty sits at ~10.3 s, which looks like an
    // internal deadline in Jellyfin or in the debrid/AIOStreams backend behind
    // this library: when the query does not finish in time the server yields
    // zero rows instead of an error.  Nothing on this side can prevent that.
    //
    // What CAN be done is ask again, because the work the first attempt did is
    // cached: a repeat of the same term lands in 1.6-4 s and returns the full
    // set.  Measured on the console, 'BAT' did exactly this -- try 1 empty at
    // 10.2 s, try 2 with 41 results at 9.0 s.
    //
    // Three attempts, with the pause GROWING between them.  Two was not enough
    // for 'DI' (both attempts came back empty at ~10 s), and the reason to wait
    // longer rather than merely more often is that the backend appears to keep
    // working after it has answered -- the same term from a PC went 7.7 s cold
    // then 2.9 s warm.  Giving it a couple of seconds is what turns the next
    // attempt into the warm one.
    //
    // The cost is bounded and it is not a freeze: this is a worker thread, the
    // UI stays at 60 Hz with "Searching..." up, and a term that genuinely
    // matches nothing settles after three tries.
    static const u32 RETRY_PAUSE_US[] = { 500000, 2500000 };
    u64 t0 = timing_get_us();
    int status = 0, n = 0;

    for (int attempt = 0; attempt < 3; attempt++) {
        u64 ta = timing_get_us();
        status = http_request(0, url, NULL, g_token,
                              s_search_resp, sizeof(s_search_resp));
        n = (status == 200)
            ? parse_xmb_items(s_search_resp, s_search_out, XMB_ITEMS_MAX)
            : 0;

        snprintf(dbg, sizeof(dbg),
                 "search: term='%s' try=%d status=%d count=%d bytes=%d in %llums",
                 s_search_term, attempt + 1, status, n,
                 (int)strlen(s_search_resp),
                 (unsigned long long)((timing_get_us() - ta) / 1000));
        plog(dbg);

        if (status != 200) {
            char errbuf[320];
            snprintf(errbuf, sizeof(errbuf), "search_err: %.300s", s_search_resp);
            plog(errbuf);
            break;                       // a transport failure will not improve
        }
        if (n > 0) break;                // got what we asked for

        char respbuf[224];
        snprintf(respbuf, sizeof(respbuf),
                 "search_empty_resp: %.200s", s_search_resp);
        plog(respbuf);

        // Only worth retrying the case this exists for: the server said 200
        // and handed back an empty set.  Give it time to finish whatever it
        // was still doing when it answered, longer on each pass.
        if (attempt < (int)(sizeof(RETRY_PAUSE_US) / sizeof(RETRY_PAUSE_US[0])))
            usleep(RETRY_PAUSE_US[attempt]);
    }

    s_search_status = status;
    s_search_out_n  = n;

    snprintf(dbg, sizeof(dbg),
             "search: term='%s' FINAL status=%d count=%d in %llums",
             s_search_term, status, n,
             (unsigned long long)((timing_get_us() - t0) / 1000));
    plog(dbg);

    __sync_synchronize();   // results land before the done flag is visible
    s_search_done = true;
    sysThreadExit(0);
}

// Start a search for whatever is in g_search_buf.  No-op while one is already
// in flight -- the caller re-arms its debounce and tries again, so the term
// that actually goes out is the latest one the user settled on.
static void xmb_do_search(void) {
    if (s_search_running || !g_search_buf[0]) return;

    snprintf(s_search_term, sizeof(s_search_term), "%s", g_search_buf);
    s_search_done = false;

    s32 rc = sysThreadCreate(&s_search_thr, search_thread_fn, NULL,
                             1500, 65536, 0, "jf_search");
    if (rc != 0) {
        // Falling back to a blocking call here would reintroduce the freeze
        // this exists to remove, so don't: leave the previous results up and
        // say so.  Thread creation failing at all means the app is in trouble
        // for bigger reasons than search.
        char b[64];
        snprintf(b, sizeof(b), "search: sysThreadCreate FAILED 0x%08x", (u32)rc);
        plog(b);
        return;
    }
    s_search_running = true;
}

// Collect a finished search.  Called every frame from the input handler; does
// nothing until the worker signals.
static void xmb_search_collect(void) {
    if (!s_search_running || !s_search_done) return;

    u64 tret;
    sysThreadJoin(s_search_thr, &tret);
    s_search_thr     = 0;
    s_search_running = false;
    s_search_done    = false;

    // Publish only if the user has not moved on.  If they kept typing while
    // this was in flight the term no longer matches, and the debounce below
    // will fire a fresh query -- showing the stale hits in the meantime would
    // be worse than showing the previous ones.
    if (strcmp(s_search_term, g_search_buf) != 0) return;
    g_search_state = SEARCH_DONE;

    if (s_search_status == 200) {
        for (int i = 0; i < s_search_out_n; i++)
            g_search_results[i] = s_search_out[i];
        g_search_results_count = s_search_out_n;
    } else {
        g_search_results_count = 0;
    }

    // A new result set can be SHORTER than the one it replaces -- deleting a
    // character with square re-runs the query while the cursor may be sitting
    // in the results list -- so the selection and scroll have to be pulled
    // back inside the new list rather than left pointing past its end.
    if (g_search_results_count == 0) {
        g_search_focus_results = false;
        g_search_sel    = 0;
        g_search_scroll = 0;
    } else {
        if (g_search_sel >= g_search_results_count)
            g_search_sel = g_search_results_count - 1;
        if (g_search_scroll > g_search_sel) g_search_scroll = g_search_sel;
        if (g_search_scroll < 0)            g_search_scroll = 0;
    }
}

// Join the worker before the XMB tears down.  Safe to call unconditionally.
void xmb_search_shutdown(void) {
    if (!s_search_running) return;
    u64 tret;
    sysThreadJoin(s_search_thr, &tret);
    s_search_thr     = 0;
    s_search_running = false;
    s_search_done    = false;
}

bool xmb_handle_input_search(void) {
    // Pick up a finished query first, so a result that landed since the last
    // frame is on screen before this frame's input is considered.
    xmb_search_collect();

    char prev_buf[sizeof(g_search_buf)];
    strcpy(prev_buf, g_search_buf);

    if (BTN_PRESSED(l1)) { g_search_focus_results = false; xmb_switch_tab(xmb_next_enabled(g_active_tab, -1)); return false; }
    if (BTN_PRESSED(r1)) { g_search_focus_results = false; xmb_switch_tab(xmb_next_enabled(g_active_tab, +1)); return false; }
    if (BTN_PRESSED(circle) && !g_search_buf[0]) {
        xmb_switch_tab(xmb_next_enabled(g_active_tab, +1));
        return false;
    }
    if (BTN_PRESSED(circle)) { g_search_buf[0] = '\0'; g_search_results_count = 0; g_search_focus_results = false; s_search_pending = false; g_search_state = SEARCH_IDLE; return false; }

    // Square = backspace, from anywhere on this screen.
    //
    // The OSK has a Del key, but it sits on the bottom row: correcting one
    // letter means walking the cursor down to it, pressing X, and walking all
    // the way back to where you were typing.  A dedicated button is what makes
    // the keyboard usable, and square was the only face button doing nothing
    // here.  BTN_REPEAT rather than BTN_PRESSED so it can be held down.
    //
    // No early return: this falls through to the change detection below, which
    // is what re-arms the debounce and eventually sends the shortened term.
    if (BTN_REPEAT(square)) {
        int len = (int)strlen(g_search_buf);
        if (len > 0) {
            g_search_buf[len - 1] = '\0';
            // Deleting back to nothing means there is no query any more, so
            // drop the hits with it rather than leaving the previous term's
            // results sitting under an empty box.
            if (!g_search_buf[0]) {
                g_search_results_count = 0;
                g_search_focus_results = false;
            }
        }
    }

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
            const XMBItem *it = &g_search_results[g_search_sel];
            if (strcmp(it->type, "Series") == 0) {
                // Same rule as everywhere else: browse a show rather than
                // offer it a version overlay it has no versions for.
                if (xmb_open_series(it)) {
                    g_search_focus_results = false;
                    init_btns();
                }
            } else {
                xmb_show_item_info(it);
            }
            return false;
        }
        if (BTN_PRESSED(cross) && g_search_sel < g_search_results_count) {
            const XMBItem *it = &g_search_results[g_search_sel];
            // A SERIES is not playable — it is a folder.  X used to hand it
            // to the video player anyway, which is why picking a show from
            // search dropped straight into something instead of letting you
            // choose.  Open the Seasons -> Episodes browser the TV tab and
            // the Home rows already use.
            if (strcmp(it->type, "Series") == 0) {
                if (xmb_open_series(it)) {
                    g_search_focus_results = false;
                    init_btns();
                }
                return false;
            }
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
                s_search_pending = false;
                g_search_state = SEARCH_IDLE;
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
            g_search_state   = ((int)strlen(g_search_buf) >= SEARCH_MIN_CHARS)
                             ? SEARCH_PENDING : SEARCH_TOO_SHORT;
            {
                char dbg[96];
                snprintf(dbg, sizeof(dbg), "search: term='%s' queued", g_search_buf);
                plog(dbg);
            }
        } else {
            s_search_pending       = false;
            g_search_results_count = 0;
            g_search_focus_results = false;
            g_search_state         = SEARCH_IDLE;
        }
    }

    // Still working the keyboard — moving between keys, holding a direction,
    // deleting — so push the query back.  Held buttons count, which is what
    // keeps a long d-pad run from being read as a pause.
    if (s_search_pending &&
        (btn_cur.up || btn_cur.down || btn_cur.left || btn_cur.right ||
         btn_cur.cross || btn_cur.circle || btn_cur.square ||
         btn_cur.triangle || btn_cur.l1 || btn_cur.r1)) {
        s_search_edit_us = timing_get_us();
        // A query that has been "still typing" for many seconds is not being
        // typed; something is reading as held.  Say which, once, so a pulled
        // log shows whether the pad (or a second paired one -- pads are OR'd
        // together in poll_buttons) is pinning a button.
        if (s_hold_since_us == 0) s_hold_since_us = s_search_edit_us;
        if (!s_hold_logged && s_search_edit_us - s_hold_since_us > 5000000ULL) {
            s_hold_logged = true;
            char dbg[128];
            snprintf(dbg, sizeof(dbg),
                "search: query held back >5s by pad: u%d d%d l%d r%d x%d o%d sq%d tr%d l1%d r1%d",
                btn_cur.up, btn_cur.down, btn_cur.left, btn_cur.right,
                btn_cur.cross, btn_cur.circle, btn_cur.square,
                btn_cur.triangle, btn_cur.l1, btn_cur.r1);
            plog(dbg);
        }
    } else {
        s_hold_since_us = 0;        // pad quiet: the hold, if any, is over
    }

    if (s_search_pending &&
        timing_get_us() - s_search_edit_us >= SEARCH_DEBOUNCE_US) {
        if ((int)strlen(g_search_buf) >= SEARCH_MIN_CHARS) {
            // Leave it pending while a query is still in flight: xmb_do_search
            // refuses to start a second worker, and clearing the flag here
            // would silently drop the newer term.  Retrying next frame costs
            // nothing and sends whatever the user actually settled on.
            if (!s_search_running) {
                s_search_pending = false;
                s_hold_logged    = false;
                xmb_do_search();
                g_search_state = s_search_running ? SEARCH_QUERYING : SEARCH_DONE;
            }
        } else {
            s_search_pending = false;
            // Too short to search on: show nothing rather than whatever a
            // one-letter query happens to match -- and SAY so, rather than
            // "No results".
            g_search_results_count = 0;
            g_search_focus_results = false;
            g_search_state = SEARCH_TOO_SHORT;
        }
    }

    return false;
}
