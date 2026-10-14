// Offline downloads -- queue a whole season.  See dl_season.h.

#include "dl_season.h"
#include "dl_platform.h"
#include "dl_request.h"      // dl_estimate_bytes

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static volatile DlSeasonState s_state = DL_SEASON_IDLE;
static volatile int s_total = 0, s_added = 0, s_skipped = 0, s_failed = 0;
static volatile int s_cancel = 0;

void dl_season_status(DlSeasonStatus *out) {
    out->state   = s_state;
    out->total   = s_total;
    out->added   = s_added;
    out->skipped = s_skipped;
    out->failed  = s_failed;
    out->done    = s_added + s_skipped + s_failed;
}

void dl_season_cancel(void) { s_cancel = 1; }
bool dl_season_busy(void)   { return s_state == DL_SEASON_RUNNING; }

// Already in the queue or the library: anything but a dead end.
static bool already_have(const char *id) {
    DlStatus st;
    if (!dl_find(id, &st)) return false;
    return st.rec.state != DL_FAILED && st.rec.state != DL_CANCELLED;
}

void dl_season_run(const DlSeasonOps *ops, void *ctx, const char *series_id,
                   const char *season_id, int limit) {
    s_cancel = 0;
    s_total = s_added = s_skipped = s_failed = 0;
    s_state = DL_SEASON_RUNNING;

    // ~150 KB, so on the heap for the length of the run rather than resident.
    DlSeasonEp *eps = (DlSeasonEp *)malloc(sizeof(DlSeasonEp) * DL_SEASON_MAX);
    if (!eps) { s_state = DL_SEASON_FAILED; return; }
    const int n = ops->list(ctx, series_id, season_id, eps, DL_SEASON_MAX);
    if (n < 0) { free(eps); s_state = DL_SEASON_FAILED; return; }
    int count = n;
    if (limit > 0 && limit < count) count = limit;
    s_total = count;

    DlSeasonState end = DL_SEASON_DONE;
    for (int i = 0; i < count; i++) {
        // A film being streamed has the network; the next episode waits.
        while (dl_playback_blocking() && !s_cancel) dl_plat_sleep_ms(500);
        if (s_cancel) { end = DL_SEASON_CANCELLED; break; }
        if (already_have(eps[i].id)) { s_skipped = s_skipped + 1; continue; }
        const int r = ops->queue(ctx, &eps[i]);
        if (r == DL_OK) {
            s_added = s_added + 1;
        } else if (r == DL_E_NO_SPACE || r == DL_E_SPACE_UNKNOWN) {
            end = DL_SEASON_NO_SPACE;
            break;
        } else {
            s_failed = s_failed + 1;
        }
    }
    char b[96];
    snprintf(b, sizeof b, "dl: season run %d of %d added=%d skipped=%d failed=%d state=%d",
             s_added + s_skipped + s_failed, s_total, (int)s_added, (int)s_skipped,
             (int)s_failed, (int)end);
    dl_plat_log(b);
    free(eps);
    s_state = end;
}

void dl_season_estimates(const StreamRequest *rq, const DlSeasonEp *eps, int n,
                         uint64_t *est) {
    for (int i = 0; i < n; i++) {
        const uint32_t secs = eps[i].runtime_secs ? eps[i].runtime_secs : 3600;
        uint64_t e = 0;
        if (!dl_estimate_bytes(rq, secs, 0, &e)) e = 0;
        est[i] = e;
    }
}
