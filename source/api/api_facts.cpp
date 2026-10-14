// Item facts, fetched off the render thread -- see api_facts.h.
//
// One pending slot and a 12-entry cache.  The render thread writes the slot
// and reads the cache; the worker takes the slot, fetches into its OWN
// buffer (never the shared responseBuffer, for the reason jellyfin_stop_
// transcode gives in api_detail.cpp), parses, and publishes.  http_request()
// serialises itself, so a fetch here only ever waits behind another request.

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <ppu-types.h>
#include <sys/thread.h>
#include <sys/mutex.h>

#include "api_facts.h"
#include "jellyfin_api.h"
#include "timing.h"
#include "plog.h"

#define FACTS_N         12
#define FACTS_BUF       (160 * 1024)       // one item + its streams, with room
#define FACTS_RETRY_US  30000000ULL        // a failed item is tried again after 30 s

typedef struct {
    char               id[64];
    ItemFacts          f;
    bool               ok;
    unsigned long long at_us;              // when it landed (0 = empty entry)
    unsigned long long used_us;            // last read, for eviction
} FactsEntry;

static FactsEntry        s_cache[FACTS_N];
static char              s_pending[64];
static sys_mutex_t       s_mtx;
static sys_ppu_thread_t  s_tid;
static int               s_started = 0;    // 1 up, -1 could not start
static char              s_buf[FACTS_BUF];

bool facts_wanted(const char *type) {
    return type && (!strcmp(type, "Movie") || !strcmp(type, "Episode") ||
                    !strcmp(type, "Video") || !strcmp(type, "MusicVideo"));
}

// Under s_mtx.
static FactsEntry *find(const char *id) {
    for (int i = 0; i < FACTS_N; i++)
        if (s_cache[i].at_us && !strcmp(s_cache[i].id, id)) return &s_cache[i];
    return NULL;
}

static void publish(const char *id, const ItemFacts *f, bool ok) {
    sysMutexLock(s_mtx, 0);
    FactsEntry *e = find(id);
    if (!e) {
        e = &s_cache[0];
        for (int i = 0; i < FACTS_N; i++) {
            if (!s_cache[i].at_us) { e = &s_cache[i]; break; }
            if (s_cache[i].used_us < e->used_us) e = &s_cache[i];
        }
    }
    snprintf(e->id, sizeof e->id, "%s", id);
    e->f       = *f;
    e->ok      = ok;
    e->at_us   = timing_get_us();
    e->used_us = e->at_us;
    sysMutexUnlock(s_mtx);
}

static void facts_thread(void *arg) {
    (void)arg;
    for (;;) {
        char id[64] = "";
        sysMutexLock(s_mtx, 0);
        if (s_pending[0]) {
            memcpy(id, s_pending, sizeof id);
            s_pending[0] = '\0';
        }
        sysMutexUnlock(s_mtx);

        if (!id[0]) { usleep(40000); continue; }   // 25 Hz poll of one byte

        char url[512];
        snprintf(url, sizeof url,
                 "%s/Users/%s/Items?Ids=%s&Fields=MediaStreams,Overview,People,OfficialRating"
                 "&EnableImages=false&EnableUserData=false",
                 g_server, g_userid, id);
        s_buf[0] = '\0';
        const int st = http_request(HTTP_GET, url, NULL, g_token, s_buf, sizeof s_buf);
        ItemFacts f;
        if (st == 200 && s_buf[0]) {
            facts_parse(s_buf, &f);
            publish(id, &f, true);
        } else {
            memset(&f, 0, sizeof f);
            publish(id, &f, false);
            char msg[96];
            snprintf(msg, sizeof msg, "facts: http %d for %.40s", st, id);
            plog(msg);
        }
    }
}

// Started lazily, from the render thread -- the only thread that calls in.
static bool start(void) {
    if (s_started) return s_started == 1;
    sys_mutex_attr_t attr;
    sysMutexAttrInitialize(attr);
    if (sysMutexCreate(&s_mtx, &attr) != 0) {
        plog("facts: mutex failed -- screens draw without the tech strip");
        s_started = -1;
        return false;
    }
    // 64 KB: http_request's frame alone is 5.4 KB (api_playstate.cpp measured
    // it), and this thread formats and parses on top of that.
    if (sysThreadCreate(&s_tid, facts_thread, NULL, 1500, 0x10000, 0,
                        (char *)"jf_facts") != 0) {
        plog("facts: thread failed -- screens draw without the tech strip");
        s_started = -1;
        return false;
    }
    s_started = 1;
    plog("facts: worker up");
    return true;
}

void facts_request(const char *id) {
    if (!id || !id[0] || !start()) return;
    sysMutexLock(s_mtx, 0);
    const FactsEntry *e = find(id);
    const bool fresh = e && (e->ok || timing_get_us() - e->at_us < FACTS_RETRY_US);
    if (!fresh && strcmp(s_pending, id) != 0)
        snprintf(s_pending, sizeof s_pending, "%s", id);
    sysMutexUnlock(s_mtx);
}

bool facts_get(const char *id, ItemFacts *out, unsigned long long *age_us) {
    if (!id || !id[0] || s_started != 1) return false;
    bool got = false;
    sysMutexLock(s_mtx, 0);
    FactsEntry *e = find(id);
    if (e && e->ok) {
        const unsigned long long now = timing_get_us();
        *out = e->f;
        if (age_us) *age_us = now - e->at_us;
        e->used_us = now;
        got = true;
    }
    sysMutexUnlock(s_mtx);
    return got;
}
