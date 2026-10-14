#pragma once
#include <stdint.h>
#include "dl_manager.h"
#include "jellyfin_api.h"      // JFItemIdentity
#include "stream_request.h"

// -------------------------------------------------------------------------
//  Offline downloads -- queue a whole season
// -------------------------------------------------------------------------
//  Triangle on a season (the TV seasons screen) queues its episodes through
//  the same path as the item page's Download button, one at a time, in order.
//  The orchestration here is portable and host-tested; what talks to the
//  server (listing the episodes, fetching one episode's versions) is behind
//  DlSeasonOps, which the console supplies from a worker thread
//  (ui/xmb/ui_season.cpp) and the tests supply from a script.
//
//  Rules:
//    * an episode already in the queue or library (any state but failed or
//      cancelled) is skipped, so running it twice adds nothing;
//    * running out of HDD space (or not being able to read it) stops the
//      whole run: the next episode would be refused too, and the ones queued
//      so far stay;
//    * any other failure skips that episode and carries on;
//    * cancel is checked between episodes.
// -------------------------------------------------------------------------

#define DL_SEASON_MAX 100          // episodes in one season

typedef struct {
    char           id[DL_ID_MAX];
    char           name[DL_TITLE_MAX];
    uint32_t       runtime_secs;   // 0 = not reported
    JFItemIdentity identity;       // series, season, episode, year
    char           overview[1024];
} DlSeasonEp;

typedef struct {
    // The season's episodes in order.  Returns the count (at most max), or
    // -1 when the server could not be asked.
    int (*list)(void *ctx, const char *series_id, const char *season_id,
                DlSeasonEp *out, int max);
    // Fetch what one episode needs and queue it.  Returns a DlResult.
    int (*queue)(void *ctx, const DlSeasonEp *ep);
} DlSeasonOps;

typedef enum {
    DL_SEASON_IDLE = 0,
    DL_SEASON_RUNNING,
    DL_SEASON_DONE,           // every episode that could be queued was
    DL_SEASON_CANCELLED,
    DL_SEASON_NO_SPACE,       // stopped: the HDD is at its reserve (or unreadable)
    DL_SEASON_FAILED,         // the episode list could not be fetched
} DlSeasonState;

typedef struct {
    DlSeasonState state;
    int total;                // episodes in the run
    int added;                // queued this run
    int skipped;              // already queued or downloaded
    int failed;               // refused for another reason
    int done;                 // added + skipped + failed: where the run is
} DlSeasonStatus;

// Run one season to the end, on the calling thread.  `limit` > 0 queues only
// the first `limit` episodes (the ones that fit).  Resets the status.
void dl_season_run(const DlSeasonOps *ops, void *ctx, const char *series_id,
                   const char *season_id, int limit);
void dl_season_status(DlSeasonStatus *out);     // safe from any thread
void dl_season_cancel(void);                    // takes effect between episodes
bool dl_season_busy(void);

// What each episode is expected to need, as an upper bound, for the sheet's
// "needs X, Y free": the request's ceiling over the episode's runtime (an hour
// when none is reported).
void dl_season_estimates(const StreamRequest *rq, const DlSeasonEp *eps, int n,
                         uint64_t *est);
