// Offline downloads -- the service lifecycle, portable core.  See
// dl_service.h.

#include "dl_service.h"
#include "dl_manager.h"
#include "dl_platform.h"
#include "dl_store.h"

#include <stdio.h>
#include <string.h>

#define MAX_ROOTS 4

static const char   *s_roots[MAX_ROOTS];
static int           s_n_roots  = 0;
static volatile bool s_started  = false;   // worker thread exists
static volatile bool s_run      = false;   // worker should keep going
static volatile bool s_restored = false;   // written by the worker only
static bool          s_no_root  = false;   // worker-thread only
static char          s_root[DL_PATH_MAX] = "";

bool dl_svc_auth_header(char *out, int cap, const char *token,
                        const char *device_id) {
    if (cap > 0) out[0] = '\0';
    if (!token || !token[0]) return false;
    int n = snprintf(out, (size_t)cap,
        "X-Emby-Authorization: MediaBrowser Client=\"PS3\", Device=\"PS3\","
        " DeviceId=\"%s\", Version=\"0.1\", Token=\"%s\"",
        device_id ? device_id : "", token);
    if (n < 0 || n >= cap) { if (cap > 0) out[0] = '\0'; return false; }
    return true;
}

void dl_svc_set_session(const char *token, const char *device_id) {
    char line[512];
    dl_svc_auth_header(line, sizeof(line), token, device_id);
    dl_set_auth_header(line);    // "" holds the queue (dl_manager_step)
}

uint32_t dl_svc_tick(void) {
    if (!s_restored) {
        // Restore here, on the worker, so a slow HDD or a large queue never
        // holds up the UI.  Disk only: this works with the server down.
        for (int i = 0; i < s_n_roots; i++) {
            if (dl_manager_init(s_roots[i], NULL)) {
                snprintf(s_root, sizeof(s_root), "%s", s_roots[i]);
                char b[DL_PATH_MAX + 32];
                snprintf(b, sizeof(b), "dl: service using %s", s_root);
                dl_plat_log(b);
                break;
            }
            char b[DL_PATH_MAX + 32];
            snprintf(b, sizeof(b), "dl: root not writable: %s", s_roots[i]);
            dl_plat_log(b);
        }
        if (!s_root[0]) {
            s_no_root = true;
            dl_plat_log("dl: no writable root; downloads unavailable");
        }
        // Published last: a waiter (the offline startup path) must never
        // see "restored" while the store is still being read.
        __sync_synchronize();
        s_restored = true;
        return 0;
    }
    if (s_no_root) return 1000;
    if (dl_manager_step()) return 0;          // did an attempt; look again
    uint32_t wait = dl_next_wake_ms();
    return wait < 50 ? 50 : wait;
}

static void worker(void) {
    while (s_run && dl_plat_app_running()) {
        uint32_t wait = dl_svc_tick();
        if (wait) dl_plat_sleep_ms(wait);
    }
}

bool dl_svc_start(const char *const *roots, int n_roots) {
    if (s_started) return true;               // one worker, ever
    s_n_roots = n_roots < MAX_ROOTS ? n_roots : MAX_ROOTS;
    for (int i = 0; i < s_n_roots; i++) s_roots[i] = roots[i];
    s_restored = false;
    s_no_root  = false;
    s_root[0]  = '\0';
    s_run      = true;
    if (!dl_plat_thread_start(worker)) {
        s_run = false;
        dl_plat_log("dl: worker thread did not start");
        return false;
    }
    s_started = true;
    return true;
}

void dl_svc_stop(void) {
    if (!s_started) return;
    s_run = false;
    // The active transfer sees the suspension at its next read (about a
    // second) and parks the item back in the queue with its data.
    dl_set_suspended(true);
    dl_plat_thread_join();
    dl_set_suspended(false);
    dl_manager_shutdown();
    s_started = false;
    s_root[0] = '\0';
}

bool dl_svc_started(void) { return s_started; }
bool dl_svc_restored(void) { return s_started && s_restored; }
const char *dl_svc_root(void) { return s_root; }
