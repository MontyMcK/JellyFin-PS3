// See segments.h.
//
// The worker is a detached one-shot thread.  It owns its argument and
// response buffer, and publishes only if its generation is still current, so
// a fetch that outlives its playback (a slow server, a 20 s retry wait) can
// never write into the next title's segments and nothing ever has to join it.
// Entries are written before the count, with a barrier between, and are
// written at most once per generation, so the player thread reads them
// without a lock.

#include "segments.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/thread.h>

#include "http.h"
#include "jellyfin_api.h"
#include "plog.h"
#include "timing.h"

#define SEG_BODY_BYTES   16384
#define SEG_RETRY_US     20000000ULL
#define SEG_SKIP_QUIET_US 3000000ULL

static MediaSegment  s_segs[MEDIA_SEGMENTS_MAX];
static volatile int  s_count;
static volatile u32  s_gen;
static u64           s_quiet_until_us;

typedef struct {
    u32  gen;
    char item_id[64];
    char source_id[64];
} SegJob;

static int fetch(const char *id, MediaSegment *out) {
    char url[512];
    snprintf(url, sizeof(url), "%s/MediaSegments/%s", g_server, id);
    char *body = (char *)calloc(1, SEG_BODY_BYTES);
    if (!body) return 0;
    const int st = http_request(HTTP_GET, url, NULL, g_token, body, SEG_BODY_BYTES - 1);
    int n = 0;
    if (st == 200)
        n = media_segments_parse(body, (int)strlen(body), out, MEDIA_SEGMENTS_MAX);
    free(body);
    char line[160];
    snprintf(line, sizeof(line), "segments: %s -> http %d, %d segment(s)", id, st, n);
    plog(line);
    return n;
}

static int fetch_job(const SegJob *job, MediaSegment *out) {
    int n = 0;
    if (job->source_id[0]) n = fetch(job->source_id, out);
    if (!n && strcmp(job->source_id, job->item_id) != 0) n = fetch(job->item_id, out);
    return n;
}

static void publish(const SegJob *job, const MediaSegment *segs, int n) {
    if (job->gen != s_gen || s_count) return;
    memcpy(s_segs, segs, sizeof(MediaSegment) * (size_t)n);
    __sync_synchronize();
    s_count = n;
    for (int i = 0; i < n; i++) {
        char line[96];
        snprintf(line, sizeof(line), "segments: type=%d %.1f-%.1f s",
                 (int)segs[i].type, segs[i].start_secs, segs[i].end_secs);
        plog(line);
    }
}

static void seg_worker(void *arg) {
    SegJob *job = (SegJob *)arg;
    MediaSegment segs[MEDIA_SEGMENTS_MAX];
    int n = fetch_job(job, segs);
    if (!n) {
        // Gelato's IntroDB looks a version up when it is first played, so
        // the answer may simply not exist yet.  One retry, abandoned the
        // moment this playback ends.
        const u64 until = timing_get_us() + SEG_RETRY_US;
        while (job->gen == s_gen && timing_get_us() < until) usleep(250000);
        if (job->gen == s_gen) n = fetch_job(job, segs);
    }
    if (n) publish(job, segs, n);
    free(job);
    sysThreadExit(0);
}

void segments_start(const char *item_id, const char *source_id) {
    segments_stop();
    if (!item_id || !item_id[0]) return;
    SegJob *job = (SegJob *)calloc(1, sizeof(SegJob));
    if (!job) return;
    job->gen = s_gen;
    snprintf(job->item_id, sizeof(job->item_id), "%s", item_id);
    snprintf(job->source_id, sizeof(job->source_id), "%s", source_id ? source_id : "");
    sys_ppu_thread_t tid;
    if (sysThreadCreate(&tid, seg_worker, job, 1600, 64 * 1024, 0, (char *)"jf_segments") != 0) {
        plog("segments: worker failed to start");
        free(job);
    }
}

void segments_stop(void) {
    s_gen = s_gen + 1;
    s_count = 0;
    s_quiet_until_us = 0;
}

const MediaSegment *segments_at(double pos) {
    const int n = s_count;
    if (!n || timing_get_us() < s_quiet_until_us) return NULL;
    __sync_synchronize();
    for (int i = 0; i < n; i++) {
        const MediaSegment *s = &s_segs[i];
        if (media_segment_skip_label(s->type) && pos >= s->start_secs && pos < s->end_secs - 1.0)
            return s;
    }
    return NULL;
}

double segments_outro_start(void) {
    const int n = s_count;
    __sync_synchronize();
    for (int i = 0; i < n; i++)
        if (s_segs[i].type == SEG_OUTRO) return s_segs[i].start_secs;
    return -1.0;
}

void segments_skipped(void) {
    s_quiet_until_us = timing_get_us() + SEG_SKIP_QUIET_US;
}
