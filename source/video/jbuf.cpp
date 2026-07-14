#include "video.h"
#include "video_internal.h"
#include "plog.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <sys/mutex.h>

// -------------------------------------------------------
// Jitter buffer
// -------------------------------------------------------
// Single producer (decode thread, via the video_internal.h producer API),
// single consumer (display thread, via the public jbuf_* API).
// s_jbuf_mtx guards s_jb_n and the read/write indices.

sys_mutex_t s_jbuf_mtx;

static u8  *s_jbuf_data[JBUF_SIZE] = {};
static u64  s_jbuf_pts[JBUF_SIZE]  = {};
static s64  s_jbuf_dur[JBUF_SIZE]  = {};  // remaining display duration per slot (us)
static u32  s_jbuf_seq[JBUF_SIZE]  = {};
static u32  s_seq_counter          = 0;
static u32  s_jbuf_fw = 0, s_jbuf_fh = 0;
static int  s_jb_wr = 0, s_jb_rd = 0;
static volatile int s_jb_n = 0;

// Slot buffers are CACHED once allocated: jbuf_reserve() grabs them at boot
// (before the UI fragments the heap) and jbuf_free() keeps them.  The big
// three (VDEC arena, these slots, HUD overlay) total ~162MB against a heap
// with only a few MB of slack, so allocating them per-session made every
// session a dice roll on whatever the UI had done to the heap layout.
static u32 s_jbuf_slot_bytes = 0;     // capacity of each cached slot

// Allocate the slot buffers if not already cached (or cached too small).
// Returns false with NOTHING left allocated on failure (the old code leaked
// the partial slots, so each failed attempt made the next one worse).
bool jbuf_reserve(u32 fw, u32 fh) {
    u32 need = fw * fh * 4;
    if (s_jbuf_data[0] && s_jbuf_slot_bytes >= need) return true;
    for (int i = 0; i < JBUF_SIZE; i++) {
        if (s_jbuf_data[i]) { free(s_jbuf_data[i]); s_jbuf_data[i] = NULL; }
    }
    s_jbuf_slot_bytes = 0;
    for (int i = 0; i < JBUF_SIZE; i++) {
        s_jbuf_data[i] = (u8*)memalign(128, need);
        if (!s_jbuf_data[i]) {
            char buf[64];
            snprintf(buf, sizeof(buf), "jbuf_reserve: slot %d of %d FAILED (%u KB)",
                     i, JBUF_SIZE, need / 1024);
            plog(buf);
            for (int j = 0; j < i; j++) { free(s_jbuf_data[j]); s_jbuf_data[j] = NULL; }
            return false;
        }
    }
    s_jbuf_slot_bytes = need;
    return true;
}

bool jbuf_alloc(u32 fw, u32 fh) {
    if (!jbuf_reserve(fw, fh)) return false;
    s_jbuf_fw = fw; s_jbuf_fh = fh;
    s_jb_wr = s_jb_rd = s_jb_n = 0;
    memset(s_jbuf_pts, 0, sizeof(s_jbuf_pts));
    memset(s_jbuf_dur, 0, sizeof(s_jbuf_dur));
    sys_mutex_attr_t attr;
    memset(&attr, 0, sizeof(attr));
    attr.attr_protocol  = SYS_LWMUTEX_ATTR_PROTOCOL;
    attr.attr_recursive = SYS_MUTEX_ATTR_RECURSIVE;
    sysMutexCreate(&s_jbuf_mtx, &attr);
    return true;
}

void jbuf_free(void) {
    // Slot buffers stay cached (see jbuf_reserve) — only session state dies.
    s_jb_wr = s_jb_rd = s_jb_n = 0;
    sysMutexDestroy(s_jbuf_mtx);
}

void jbuf_clear(void) {
    sysMutexLock(s_jbuf_mtx, 0);
    s_jb_rd = s_jb_wr = s_jb_n = 0;
    sysMutexUnlock(s_jbuf_mtx);
}

const u8 *jbuf_peek(void)     { return (s_jb_n > 0) ? s_jbuf_data[s_jb_rd] : NULL; }
void      jbuf_pop(void)      { s_jb_rd = (s_jb_rd + 1) % JBUF_SIZE; s_jb_n--; }
u32       jbuf_fw(void)       { return s_jbuf_fw; }
u32       jbuf_fh(void)       { return s_jbuf_fh; }
int       jbuf_count(void)    { return s_jb_n; }
int       jbuf_rd(void)       { return s_jb_rd; }
u64       jbuf_peek_pts_us(void) { return (s_jb_n > 0) ? s_jbuf_pts[s_jb_rd] : 0; }
u32       jbuf_peek_seq(void) { return (s_jb_n > 0) ? s_jbuf_seq[s_jb_rd] : 0; }
const u8 *jbuf_slot_ptr(int i) { return (i >= 0 && i < JBUF_SIZE) ? s_jbuf_data[i] : NULL; }

s64       jbuf_peek_dur(void)      { return (s_jb_n > 0) ? s_jbuf_dur[s_jb_rd] : 0; }
s64       jbuf_peek_next_dur(void) { return (s_jb_n > 1) ? s_jbuf_dur[(s_jb_rd + 1) % JBUF_SIZE] : 0; }
const u8 *jbuf_peek_next(void)     { return (s_jb_n > 1) ? s_jbuf_data[(s_jb_rd + 1) % JBUF_SIZE] : NULL; }

void jbuf_consume_dur(s64 us) {
    if (s_jb_n > 0) s_jbuf_dur[s_jb_rd] -= us;
}

void jbuf_advance(void) {
    sysMutexLock(s_jbuf_mtx, 0);
    if (s_jb_n > 0 && s_jbuf_dur[s_jb_rd] <= 0) {
        s_jb_rd = (s_jb_rd + 1) % JBUF_SIZE;
        s_jb_n--;
    }
    sysMutexUnlock(s_jbuf_mtx);
}

// ---- Producer side (decode thread only; see video_internal.h) ----

bool jbuf_full(void) {
    sysMutexLock(s_jbuf_mtx, 0);
    bool full = (s_jb_n >= JBUF_SIZE);
    sysMutexUnlock(s_jbuf_mtx);
    return full;
}

u8  *jbuf_write_ptr(void) { return s_jbuf_data[s_jb_wr]; }
int  jbuf_write_idx(void) { return s_jb_wr; }

void jbuf_set_dims(u32 fw, u32 fh) { s_jbuf_fw = fw; s_jbuf_fh = fh; }

void jbuf_push(u64 pts_us, s64 dur_us) {
    s_jbuf_pts[s_jb_wr] = pts_us;
    s_jbuf_dur[s_jb_wr] = dur_us;
    s_jbuf_seq[s_jb_wr] = ++s_seq_counter;
    sysMutexLock(s_jbuf_mtx, 0);
    s_jb_wr = (s_jb_wr + 1) % JBUF_SIZE;
    s_jb_n++;
    sysMutexUnlock(s_jbuf_mtx);
}
