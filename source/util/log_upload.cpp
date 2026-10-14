// Client log upload -- see log_upload.h.

#include "log_upload.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/thread.h>
#include "jellyfin_api.h"
#include "http.h"
#include "plog.h"
#include "update_check.h"     // APP_VERSION
#include "rsxutil.h"          // display_width/height

// Jellyfin rejects client logs over its configured limit (1 MB by default).
// The end of the file is what matters -- the stall is the last thing that
// happened -- so send the newest 600 KB, of which at most a third is the
// previous run.
#define LOGUP_MAX   (600 * 1024)
#define LOGUP_PREV  (200 * 1024)

static volatile int s_state  = LOGUP_IDLE;
static volatile int s_status = 0;
static sys_ppu_thread_t s_tid;
static bool s_have_tid = false;

// Appends up to cap trailing bytes of a file to dst; returns the count.
static int read_tail(const char *path, char *dst, int cap) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    long off = sz > cap ? sz - cap : 0;
    fseek(f, off, SEEK_SET);
    int n = (int)fread(dst, 1, (size_t)cap, f);
    fclose(f);
    return n;
}

static void upload_thread(void *arg) {
    (void)arg;
    char *buf = (char*)malloc(LOGUP_MAX + LOGUP_PREV + 2048);
    char *rsp = (char*)malloc(1024);
    if (!buf || !rsp) {
        s_status = -1; s_state = LOGUP_FAILED;
    } else {
        int n = snprintf(buf, 1024,
            "=== JellyFin-PS3 %s client log ===\n"
            "display=%ux%u\n\n", APP_VERSION,
            (unsigned)display_width, (unsigned)display_height);
        // The previous run first: a crash or power-off ends it, and that is
        // the run worth reading when this one starts clean.
        int pn = read_tail("/dev_hdd0/tmp/player_log.prev.txt", buf + n, LOGUP_PREV);
        n += pn;
        if (pn > 0) n += snprintf(buf + n, 64, "\n=== this run ===\n");
        int cur = read_tail("/dev_hdd0/tmp/player_log.txt", buf + n, LOGUP_MAX);
        n += cur;
        if (cur <= 0 && pn <= 0) {
            s_state = LOGUP_NOLOG;
        } else {
            buf[n] = 0;
            plog("log_upload: sending");
            char url[320];
            snprintf(url, sizeof url, "%s/ClientLog/Document", g_server);
            int st = http_post_text(url, buf, g_token, rsp, 1024);
            s_status = st;
            s_state = (st == 200 || st == 204) ? LOGUP_SENT : LOGUP_FAILED;
            char lb[64];
            snprintf(lb, sizeof lb, "log_upload: http=%d", st);
            plog(lb);
        }
    }
    free(buf); free(rsp);
    sysThreadExit(0);
}

bool log_upload_start(void) {
    if (s_state == LOGUP_SENDING) return false;
    if (s_have_tid) { u64 r; sysThreadJoin(s_tid, &r); s_have_tid = false; }
    s_state = LOGUP_SENDING;
    s_status = 0;
    if (sysThreadCreate(&s_tid, upload_thread, NULL, 1500, 64 * 1024,
                        THREAD_JOINABLE, (char*)"jf_logup") != 0) {
        s_state = LOGUP_FAILED;
        return false;
    }
    s_have_tid = true;
    return true;
}

int log_upload_state(void)  { return s_state; }
int log_upload_status(void) { return s_status; }
