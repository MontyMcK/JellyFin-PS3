// The PS3 half of chapters.h: one request per film, on a worker, so the player
// never waits on the network.  Detached, with a generation number: a request
// for a film that has since ended finds the number moved on and throws its
// answer away.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sys/thread.h>

#include "chapters.h"
#include "jellyfin_api.h"
#include "http.h"
#include "plog.h"

namespace {

volatile int s_state = JF_CHAPTERS_IDLE;
volatile int s_gen   = 0;
JFChapters   s_list;

struct Req { int gen; char url[512]; char token[256]; };

const int BUF = 96 * 1024;      // an item's description with its chapters, nothing else asked for

void worker(void *arg) {
    Req *rq = (Req *)arg;
    char *buf = (char *)malloc(BUF);
    JFChapters *got = (JFChapters *)malloc(sizeof *got);
    int n = 0;
    if (buf && got) {
        buf[0] = '\0';
        const int st = http_request(HTTP_GET, rq->url, NULL, rq->token, buf, BUF);
        if (st == 200) n = jf_parse_chapters(buf, (int)strlen(buf), got);
        char b[80];
        snprintf(b, sizeof b, "chapters: http %d, %d chapter(s)", st, n);
        plog(b);
    }
    // One chapter is not a menu.
    if (rq->gen == s_gen) {
        if (n >= 2) {
            memcpy(&s_list, got, sizeof s_list);
            __sync_synchronize();
            s_state = JF_CHAPTERS_READY;
        } else {
            s_state = JF_CHAPTERS_NONE;
        }
    }
    free(buf);
    free(got);
    free(rq);
    sysThreadExit(0);
}

}   // namespace

void jf_chapters_reset(void) {
    s_gen++;
    s_state = JF_CHAPTERS_IDLE;
}

void jf_chapters_request(const char *item_id) {
    if (s_state != JF_CHAPTERS_IDLE || !item_id || !item_id[0]) return;
    Req *rq = (Req *)malloc(sizeof *rq);
    if (!rq) { s_state = JF_CHAPTERS_NONE; return; }
    rq->gen = s_gen;
    snprintf(rq->url, sizeof rq->url, "%s/Users/%s/Items/%s?Fields=Chapters",
             g_server, g_userid, item_id);
    snprintf(rq->token, sizeof rq->token, "%s", g_token);
    s_state = JF_CHAPTERS_LOADING;
    sys_ppu_thread_t tid;
    static char name[] = "jf_chapters";
    if (sysThreadCreate(&tid, worker, rq, 1600, 64 * 1024, 0, name) != 0) {
        free(rq);
        s_state = JF_CHAPTERS_NONE;
        plog("chapters: worker did not start");
    }
}

int jf_chapters_state(void) { return s_state; }

const JFChapters *jf_chapters(void) { return &s_list; }
