// Async log — ring buffer drained by a dedicated thread.
// Written to /dev_hdd0/tmp/player_log.txt.

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <ppu-types.h>
#include <sys/thread.h>

#include "plog.h"
#include "jf_paths.h"

#define PLOG_RING  256
#define PLOG_LEN   128

static char              s_plog_ring[PLOG_RING][PLOG_LEN];
static volatile int      s_plog_wr      = 0;
static volatile int      s_plog_rd      = 0;
static volatile int      s_plog_lock    = 0;
static volatile bool     s_plog_running = false;
static FILE             *s_plog_file    = NULL;
static sys_ppu_thread_t  s_plog_tid     = 0;

void plog(const char *msg) {
    if (!s_plog_running) return;
    while (!__sync_bool_compare_and_swap(&s_plog_lock, 0, 1))
        ;
    int next = (s_plog_wr + 1) % PLOG_RING;
    if (next != s_plog_rd) {
        strncpy(s_plog_ring[s_plog_wr], msg, PLOG_LEN - 1);
        s_plog_ring[s_plog_wr][PLOG_LEN - 1] = '\0';
        s_plog_wr = next;
    }
    __sync_bool_compare_and_swap(&s_plog_lock, 1, 0);
}

static void plog_drain(void) {
    char local[PLOG_LEN];
    while (true) {
        while (!__sync_bool_compare_and_swap(&s_plog_lock, 0, 1))
            ;
        bool empty = (s_plog_rd == s_plog_wr);
        if (!empty) {
            memcpy(local, s_plog_ring[s_plog_rd], PLOG_LEN);
            s_plog_rd = (s_plog_rd + 1) % PLOG_RING;
        }
        __sync_bool_compare_and_swap(&s_plog_lock, 1, 0);
        if (empty) break;
        if (s_plog_file) {
            time_t t = time(NULL);
            struct tm *tm = localtime(&t);
            fprintf(s_plog_file, "[%04d-%02d-%02d %02d:%02d:%02d] %s\n",
                    tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
                    tm->tm_hour, tm->tm_min, tm->tm_sec, local);
            fflush(s_plog_file);
        }
    }
}

static void plog_thread_fn(void *arg) {
    (void)arg;
    while (s_plog_running) {
        plog_drain();
        usleep(5000);
    }
    plog_drain();
    sysThreadExit(0);
}

void plog_start(void) {
    if (s_plog_running) return;
    s_plog_file    = fopen("/dev_hdd0/tmp/player_log.txt", "w");
    s_plog_running = true;
    sysThreadCreate(&s_plog_tid, plog_thread_fn, NULL,
                    1200, 32 * 1024, 0, "jf_plog");
}

void plog_stop(void) {
    s_plog_running = false;
    if (s_plog_tid) {
        u64 tret;
        sysThreadJoin(s_plog_tid, &tret);
        s_plog_tid = 0;
    }
    if (s_plog_file) {
        fclose(s_plog_file);
        s_plog_file = NULL;
    }
}

// -------------------------------------------------------
// User toggle — persisted so the choice survives restarts.
// -------------------------------------------------------

// Persistent (survives reboot): stored in the app data dir, NOT /dev_hdd0/tmp
// (which is wiped on boot).  See jf_paths.h.
#define PLOG_SETTINGS_NAME "jellyfin_settings.txt"

static bool s_plog_enabled = false;   // off by default

bool plog_enabled(void) { return s_plog_enabled; }

static void plog_save_setting(void) {
    FILE *f = fopen(jf_data_path(PLOG_SETTINGS_NAME), "w");
    if (f) {
        fprintf(f, "plog=%d\n", s_plog_enabled ? 1 : 0);
        fclose(f);
    }
}

void plog_load_setting(void) {
    s_plog_enabled = false;
    FILE *f = fopen(jf_data_path(PLOG_SETTINGS_NAME), "r");
    if (f) {
        char line[64];
        while (fgets(line, sizeof(line), f)) {
            int v;
            if (sscanf(line, "plog=%d", &v) == 1) s_plog_enabled = (v != 0);
        }
        fclose(f);
    }
    if (s_plog_enabled) plog_start();
}

void plog_set_enabled(bool on) {
    if (on == s_plog_enabled) return;
    s_plog_enabled = on;
    plog_save_setting();
    if (on) {
        plog_start();
        plog("plog: enabled via settings");
    } else {
        plog_stop();
    }
}
