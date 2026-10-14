// Stereo tap for the Canyon visualizer -- see music_sv.h.

#include <string.h>
#include <sys/mutex.h>

#include "music_sv.h"

#define SV_TAP_CAP 2048                 // stereo pairs (power of two, >= 512)
#define SV_TAKE    512

static float       s_ring[SV_TAP_CAP * 2];
static int         s_wr    = 0;
static u64         s_count = 0, s_last = 0;
static sys_mutex_t s_mtx;
static bool        s_mtx_ok = false;

void music_sv_reset(void) {
    if (!s_mtx_ok) {
        sys_mutex_attr_t a;
        sysMutexAttrInitialize(a);
        sysMutexCreate(&s_mtx, &a);
        s_mtx_ok = true;
    }
    sysMutexLock(s_mtx, 0);
    memset(s_ring, 0, sizeof(s_ring));
    s_wr = 0;
    s_count = s_last = 0;
    sysMutexUnlock(s_mtx);
}

void music_sv_push(const float *lr, int n_pairs) {
    if (!s_mtx_ok || n_pairs <= 0) return;
    sysMutexLock(s_mtx, 0);
    for (int i = 0; i < n_pairs; i++) {
        s_ring[s_wr * 2]     = lr[i * 2];
        s_ring[s_wr * 2 + 1] = lr[i * 2 + 1];
        s_wr = (s_wr + 1) & (SV_TAP_CAP - 1);
    }
    s_count += (u64)n_pairs;
    sysMutexUnlock(s_mtx);
}

bool music_sv_latest(float *lr) {
    if (!s_mtx_ok) return false;
    bool fresh = false;
    sysMutexLock(s_mtx, 0);
    if (s_count != s_last && s_count >= SV_TAKE) {
        s_last = s_count;
        int start = (s_wr - SV_TAKE) & (SV_TAP_CAP - 1);
        for (int i = 0; i < SV_TAKE; i++) {
            int k = (start + i) & (SV_TAP_CAP - 1);
            lr[i * 2]     = s_ring[k * 2];
            lr[i * 2 + 1] = s_ring[k * 2 + 1];
        }
        fresh = true;
    }
    sysMutexUnlock(s_mtx);
    return fresh;
}
