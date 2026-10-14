// Triangle on a season: queue the whole season for offline (G1).
//
// The sheet (render thread) lists the season's episodes behind the LOADING
// screen, works out whether they fit above the HDD reserve, and asks.  The
// queuing itself then runs on a worker thread of its own, so browsing carries
// on; its progress is a line beside the breadcrumb.  Everything that decides
// anything (skip, stop on no space, cancel) is offline/dl_season.cpp, which
// the host tests drive; this file is what talks to the server.
//
// The worker reads its replies into a buffer of its own: responseBuffer
// belongs to whatever the UI thread is fetching.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sys/thread.h>

#include "ui_internal.h"
#include "ui_buffering.h"       // loading_run
#include "jellyfin_api.h"
#include "http.h"
#include "plog.h"
#include "timing.h"
#include "slog.h"
#include "dl_manager.h"
#include "dl_season.h"
#include "dl_service.h"
#include "dl_ui.h"
#include "stream_request.h"

// ---------------------------------------------------------------------------
//  What talks to the server
// ---------------------------------------------------------------------------

namespace {

struct Work {
    char          *buf;            // private reply buffer (RESPONSE_SIZE)
    XMBItem       *items;          // parse target for the episode list
    char          *obj;            // one episode's JSON, NUL-terminated
    JFMediaSources *versions;
    XMBItemDetail *detail;
};

const int OBJ_MAX = 24 * 1024;

bool work_alloc(Work *w) {
    memset(w, 0, sizeof *w);
    w->buf      = (char *)malloc(RESPONSE_SIZE);
    w->items    = (XMBItem *)malloc(sizeof(XMBItem) * DL_SEASON_MAX);
    w->obj      = (char *)malloc(OBJ_MAX);
    w->versions = (JFMediaSources *)malloc(sizeof(JFMediaSources));
    w->detail   = (XMBItemDetail *)malloc(sizeof(XMBItemDetail));
    if (w->buf && w->items && w->obj && w->versions && w->detail) return true;
    free(w->buf); free(w->items); free(w->obj); free(w->versions); free(w->detail);
    memset(w, 0, sizeof *w);
    return false;
}

void work_free(Work *w) {
    free(w->buf); free(w->items); free(w->obj); free(w->versions); free(w->detail);
    memset(w, 0, sizeof *w);
}

struct EpCtx { DlSeasonEp *out; Work *w; };

// One episode's object: the fields XMBItem has no room for.
void each_episode(int i, const char *o, int olen, void *vctx) {
    EpCtx *c = (EpCtx *)vctx;
    DlSeasonEp *e = &c->out[i];
    memset(e, 0, sizeof *e);
    int n = olen < OBJ_MAX - 1 ? olen : OBJ_MAX - 1;
    memcpy(c->w->obj, o, (size_t)n);
    c->w->obj[n] = '\0';
    jellyfin_parse_item_identity(c->w->obj, &e->identity);
    e->runtime_secs = e->identity.runtime_secs;
    xmb_json_str_range(o, olen, "Overview", e->overview, sizeof e->overview);
}

int season_list(void *ctx, const char *series_id, const char *season_id,
                DlSeasonEp *out, int max) {
    Work *w = (Work *)ctx;
    char url[640];
    snprintf(url, sizeof url,
             "%s/Shows/%s/Episodes?SeasonId=%s&UserId=%s"
             "&Fields=Overview,RunTimeTicks,ProductionYear",
             g_server, series_id, season_id, g_userid);
    const int st = http_request(HTTP_GET, url, NULL, g_token, w->buf, RESPONSE_SIZE);
    if (st != 200) {
        char b[80];
        snprintf(b, sizeof b, "season: episode list http %d", st);
        plog(b);
        return -1;
    }
    EpCtx c = { out, w };
    const int n = parse_xmb_items_each(w->buf, w->items, max < DL_SEASON_MAX ? max : DL_SEASON_MAX,
                                       each_episode, &c);
    for (int i = 0; i < n; i++) {
        snprintf(out[i].id,   sizeof out[i].id,   "%s", w->items[i].id);
        snprintf(out[i].name, sizeof out[i].name, "%s", w->items[i].name);
    }
    return n;
}

// Fetch what the episode's page would hold and queue it as that page would:
// the first version (what Play streams), through dl_download_item.
int season_queue(void *ctx, const DlSeasonEp *ep) {
    Work *w = (Work *)ctx;
    char url[640];
    snprintf(url, sizeof url, "%s/Users/%s/Items/%s?Fields=MediaSources,MediaStreams",
             g_server, g_userid, ep->id);
    const int st = http_request(HTTP_GET, url, NULL, g_token, w->buf, RESPONSE_SIZE);
    if (st != 200) return DL_E_INVALID;
    memset(w->versions, 0, sizeof *w->versions);
    if (jellyfin_parse_media_sources(w->buf, w->versions) <= 0) return DL_E_INVALID;

    memset(w->detail, 0, sizeof *w->detail);
    w->detail->identity = ep->identity;
    snprintf(w->detail->overview, sizeof w->detail->overview, "%s", ep->overview);
    snprintf(w->detail->series_id, sizeof w->detail->series_id, "%s", ep->identity.series_id);
    snprintf(w->detail->series_name, sizeof w->detail->series_name, "%s", ep->identity.series_name);
    w->detail->season_num = ep->identity.season;

    JFItem jf;
    memset(&jf, 0, sizeof jf);
    snprintf(jf.id,   sizeof jf.id,   "%s", ep->id);
    snprintf(jf.name, sizeof jf.name, "%s", ep->name);
    snprintf(jf.type, sizeof jf.type, "Episode");
    return dl_download_item(&jf, w->detail, &w->versions->source[0]);
}

const DlSeasonOps k_ops = { season_list, season_queue };

// ---- the worker thread ----

struct Job { char series[64]; char season[64]; int limit; };
Job              s_job;
sys_ppu_thread_t s_tid;
bool             s_have_tid = false;

void worker_main(void *) {
    Work w;
    if (work_alloc(&w)) {
        dl_season_run(&k_ops, &w, s_job.series, s_job.season, s_job.limit);
        work_free(&w);
    } else {
        plog("season: no memory for the worker");
    }
    sysThreadExit(0);
}

bool season_start(const char *series, const char *season, int limit) {
    if (dl_season_busy()) return false;
    if (s_have_tid) { u64 r; sysThreadJoin(s_tid, &r); s_have_tid = false; }
    snprintf(s_job.series, sizeof s_job.series, "%s", series);
    snprintf(s_job.season, sizeof s_job.season, "%s", season);
    s_job.limit = limit;
    static char name[] = "jf_season";
    if (sysThreadCreate(&s_tid, worker_main, NULL, 1300, 128 * 1024, THREAD_JOINABLE, name) != 0)
        return false;
    s_have_tid = true;
    return true;
}

// ---- the list for the sheet, behind the LOADING screen ----

struct ListJob { DlSeasonEp *eps; int n; };
void list_work(void *arg) {
    ListJob *j = (ListJob *)arg;
    Work w;
    j->n = -1;
    if (!work_alloc(&w)) return;
    j->n = season_list(&w, g_tv_series_id, g_tv_season_id, j->eps, DL_SEASON_MAX);
    work_free(&w);
}

}   // namespace

// ---------------------------------------------------------------------------
//  The sheet
// ---------------------------------------------------------------------------

void xmb_season_download_sheet(const XMBItem *season) {
    if (!season) return;
    // The season screen's own state names the series and the season.
    snprintf(g_tv_season_id,   sizeof g_tv_season_id,   "%s", season->id);
    snprintf(g_tv_season_name, sizeof g_tv_season_name, "%s", season->name);
    if (dl_season_busy()) {
        xmb_dl_notice(TR("Download season"), TR("A season is already being added to Downloads."));
        return;
    }

    DlSeasonEp *eps = (DlSeasonEp *)malloc(sizeof(DlSeasonEp) * DL_SEASON_MAX);
    uint64_t   *est = (uint64_t *)malloc(sizeof(uint64_t) * DL_SEASON_MAX);
    if (!eps || !est) { free(eps); free(est); return; }
    ListJob job = { eps, 0 };
    loading_run(list_work, &job, TR("Loading"), false);

    if (job.n < 0) {
        xmb_dl_notice(TR("Download season"), TR("Couldn't get this season's episodes from the server."));
    } else if (job.n == 0) {
        xmb_dl_notice(TR("Download season"), TR("This season has no episodes."));
    } else {
        StreamPrefs prefs;
        stream_prefs_current(&prefs);
        StreamSelection sel;
        memset(&sel, 0, sizeof sel);
        sel.item_id = season->id; sel.cur_audio = -1; sel.cur_sub = -1;
        StreamRequest rq;
        stream_request_resolve(&prefs, &sel, &rq);
        dl_season_estimates(&rq, eps, job.n, est);

        uint64_t need = 0;
        for (int i = 0; i < job.n; i++) need += est[i];
        const DlSpaceReport rep = dl_space_report(0);
        const int fit = dl_space_fit_prefix(&rep, est, job.n);

        char title[DL_TITLE_MAX + 16], line[200], act[80], a[24], b[24];
        snprintf(title, sizeof title, TR("Download %s"), season->name);
        dl_ui_format_bytes(need, a, sizeof a);
        dl_ui_format_bytes(rep.avail_bytes, b, sizeof b);
        if (rep.state == DL_SPACE_UNKNOWN) {
            xmb_dl_notice(title, TR("Can't check free HDD space: downloads paused."));
        } else if (fit >= job.n) {
            snprintf(line, sizeof line, TR("%d episodes, about %s. %s free above the reserve."),
                     job.n, a, b);
            snprintf(act, sizeof act, TR("Download %s (%d episodes)"), season->name, job.n);
            if (xmb_dl_confirm(title, line, TR("Cancel"), act))
                season_start(g_tv_series_id, season->id, 0);
        } else if (fit > 0) {
            char keep[24];
            dl_ui_format_bytes(rep.reserve_bytes, keep, sizeof keep);
            snprintf(line, sizeof line, TR("Needs about %s, %s available (%s is always kept free)."),
                     a, b, keep);
            snprintf(act, sizeof act, TR("Download the first %d that fit"), fit);
            if (xmb_dl_confirm(title, line, TR("Cancel"), act))
                season_start(g_tv_series_id, season->id, fit);
        } else {
            char keep[24];
            dl_ui_format_bytes(rep.reserve_bytes, keep, sizeof keep);
            snprintf(line, sizeof line,
                     TR("Not enough space: needs about %s, %s available (%s is always kept free)."),
                     a, b, keep);
            xmb_dl_notice(title, line);
        }
    }
    free(eps);
    free(est);
}

// ---------------------------------------------------------------------------
//  Progress, beside the breadcrumb
// ---------------------------------------------------------------------------

void xmb_season_toast_draw(void) {
    static DlSeasonState prev = DL_SEASON_IDLE;
    static u64 done_at = 0;
    DlSeasonStatus st;
    dl_season_status(&st);
    const u64 now = timing_get_us();
    if (prev == DL_SEASON_RUNNING && st.state != DL_SEASON_RUNNING) done_at = now;
    prev = st.state;

    char msg[96] = "";
    if (st.state == DL_SEASON_RUNNING) {
        snprintf(msg, sizeof msg, TR("Queuing %d of %d..."), st.done + 1 > st.total ? st.total : st.done + 1,
                 st.total);
    } else if (st.state != DL_SEASON_IDLE && now - done_at < 5000000ULL) {
        if (st.state == DL_SEASON_DONE) {
            if (st.added > 0)
                snprintf(msg, sizeof msg,
                         st.added == 1 ? TR("%d episode added to Downloads")
                                       : TR("%d episodes added to Downloads"), st.added);
            else if (st.skipped > 0)
                snprintf(msg, sizeof msg, TR("Already in Downloads"));
            else
                snprintf(msg, sizeof msg, TR("Nothing could be added"));
        } else if (st.state == DL_SEASON_NO_SPACE) {
            snprintf(msg, sizeof msg, TR("Stopped: not enough HDD space (%d added)"), st.added);
        } else if (st.state == DL_SEASON_CANCELLED) {
            snprintf(msg, sizeof msg, TR("Stopped (%d added)"), st.added);
        } else if (st.state == DL_SEASON_FAILED) {
            snprintf(msg, sizeof msg, TR("Couldn't get the season's episodes"));
        }
    }
    if (!msg[0]) return;
    const float px = UIS_TF(14);
    const int w = ttf_text_width(msg, px);
    drawTTF((u32)((int)display_width - XMB_ITEM_PAD - w), (u32)(XMB_CONTENT_Y + UIS_H(2)), msg, px,
            st.state == DL_SEASON_NO_SPACE || st.state == DL_SEASON_FAILED ? 0x00E8B64CUL
                                                                         : XMB_ACCENT_ALT);
}

// At exit: stop queuing and let the thread finish before the network goes.
void xmb_season_stop(void) {
    dl_season_cancel();
    if (s_have_tid) { u64 r; sysThreadJoin(s_tid, &r); s_have_tid = false; }
}
