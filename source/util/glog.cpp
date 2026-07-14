// Diagnostic GUI / thumbnail-cache log.  Synchronous fprintf+fflush guarded
// by a spinlock — the call sites are low frequency (per-fetch events and a
// once-per-second heartbeat), so a background ring/thread isn't worth it.
// Everything is a no-op unless the user has logging enabled (plog_enabled()).

#include <stdio.h>
#include <stdarg.h>
#include <time.h>

#include "glog.h"
#include "plog.h"

static volatile int  s_glock      = 0;
static FILE         *s_gfile      = NULL;
static bool          s_gopen_done = false;   // one open attempt per boot

void glog(const char *msg) {
    if (!plog_enabled()) return;
    while (!__sync_bool_compare_and_swap(&s_glock, 0, 1))
        ;
    if (!s_gopen_done) {
        s_gfile      = fopen("/dev_hdd0/tmp/GUIlog.txt", "w");
        s_gopen_done = true;
    }
    if (s_gfile) {
        time_t t = time(NULL);
        struct tm *tm = localtime(&t);
        fprintf(s_gfile, "[%02d:%02d:%02d] %s\n",
                tm->tm_hour, tm->tm_min, tm->tm_sec, msg);
        fflush(s_gfile);
    }
    __sync_bool_compare_and_swap(&s_glock, 1, 0);
}

void glogf(const char *fmt, ...) {
    if (!plog_enabled()) return;   // skip the vsnprintf entirely when off
    char buf[192];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    glog(buf);
}
