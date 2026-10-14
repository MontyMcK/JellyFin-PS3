// Warm the server's playback path in the background -- see vremember.h.
//
// On this server PlaybackInfo is where the Gelato plugin resolves a title's
// streams: 6-8 s the first time, ~0.1 s after.  Asking once while the user
// reads the details page means Play finds it warm.  The reply is thrown away;
// the request creates no transcode (PlaybackInfo only negotiates).
//
// COSTS, knowingly: http_request() serialises every caller on one mutex, so
// while a cold warm-up runs, other requests (the details page's facts, the
// cast headshots) wait behind it.  Play then waits for it too -- but only for
// the time it would have spent anyway.  Runs after the page has loaded, one
// title at a time, and never twice for the same title in a session.

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/thread.h>

#include "jellyfin_api.h"
#include "http.h"
#include "plog.h"
#include "timing.h"
#include "vremember.h"

#define WARM_BUF   (192 * 1024)
#define WARM_SEEN  32

static volatile bool s_busy = false;
static char          s_id[64];
static char          s_seen[WARM_SEEN][40];
static int           s_seen_n = 0;

static void warm_thread(void *arg)
{
    (void)arg;
    char *buf = (char *)malloc(WARM_BUF);
    if (buf) {
        char url[512];
        snprintf(url, sizeof url, "%s/Items/%s/PlaybackInfo?UserId=%s&StartTimeTicks=0",
                 g_server, s_id, g_userid);
        static const char body[] = "{\"DeviceProfile\":{\"Name\":\"PS3\"}}";
        const u64 t0 = timing_get_us();
        const int st = http_request(1, url, body, g_token, buf, WARM_BUF);
        char b[128];
        snprintf(b, sizeof b, "warm: PlaybackInfo %.32s http=%d in %llu ms", s_id, st,
                 (unsigned long long)((timing_get_us() - t0) / 1000));
        plog(b);
        free(buf);
    }
    __sync_synchronize();
    s_busy = false;
    sysThreadExit(0);
}

void jellyfin_warm_playback(const char *item_id)
{
    if (!item_id || !item_id[0] || strlen(item_id) >= sizeof s_id) return;
    if (s_busy) return;
    for (int i = 0; i < s_seen_n && i < WARM_SEEN; i++)
        if (strcmp(s_seen[i], item_id) == 0) return;
    snprintf(s_seen[s_seen_n % WARM_SEEN], sizeof s_seen[0], "%.39s", item_id);
    s_seen_n++;
    snprintf(s_id, sizeof s_id, "%s", item_id);
    s_busy = true;
    __sync_synchronize();
    sys_ppu_thread_t tid;
    static char name[] = "jf_warm";
    if (sysThreadCreate(&tid, warm_thread, NULL, 1600, 32 * 1024, 0, name) != 0)
        s_busy = false;
}
