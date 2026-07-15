#include "img_arena.h"
#include "plog.h"

#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <stdio.h>
#include <lv2/thread.h>

static u8              *s_base  = NULL;
static size_t           s_size  = 0;
static size_t           s_used  = 0;
static size_t           s_peak  = 0;
static sys_ppu_thread_t s_owner = 0;
static bool             s_armed = false;

bool img_arena_reserve(size_t bytes) {
    if (s_base) return true;
    s_base = (u8*)memalign(128, bytes);
    s_size = s_base ? bytes : 0;
    s_used = 0;
    s_peak = 0;
    char buf[64];
    snprintf(buf, sizeof(buf), "img_arena: %uKB scratch %s",
             (unsigned)(bytes / 1024), s_base ? "ok" : "FAILED");
    plog(buf);
    return s_base != NULL;
}

bool   img_arena_ready(void) { return s_base != NULL; }
size_t img_arena_size(void)  { return s_size; }
size_t img_arena_peak(void)  { return s_peak; }

static bool arena_owns(const void *p) {
    return s_base && (const u8*)p >= s_base && (const u8*)p < s_base + s_size;
}

// True only on the thread that called begin(), between begin() and end().
static bool arena_armed_here(void) {
    if (!s_armed) return false;
    sys_ppu_thread_t me = 0;
    if (sysThreadGetId(&me) != 0) return false;
    return me == s_owner;
}

void img_arena_begin(void) {
    sys_ppu_thread_t me = 0;
    if (sysThreadGetId(&me) != 0) return;
    s_owner = me;
    s_used  = 0;
    s_armed = (s_base != NULL);
}

void img_arena_end(void) {
    s_armed = false;
    s_owner = 0;
}

void *img_arena_malloc(size_t sz) {
    if (arena_armed_here()) {
        size_t need = (sz + 127) & ~(size_t)127;   // keep blocks cache-aligned
        if (need >= sz && s_used + need <= s_size) {
            void *p = s_base + s_used;
            s_used += need;
            if (s_used > s_peak) s_peak = s_used;
            return p;
        }
        // Doesn't fit the scratch (or overflowed the round-up): the real heap
        // is still the correct answer, just not a guaranteed one.
    }
    return malloc(sz);
}

void img_arena_free(void *p) {
    if (!p) return;
    if (arena_owns(p)) return;   // reclaimed wholesale by the next begin()
    free(p);
}

void *img_arena_realloc(void *p, size_t oldsz, size_t newsz) {
    if (!p) return img_arena_malloc(newsz);
    if (!arena_owns(p)) return realloc(p, newsz);
    // In-arena block: bump a fresh one and copy across.  This never shrinks
    // the arena, which is fine — begin() reclaims it all.  If the new size
    // doesn't fit, img_arena_malloc migrates the block out to the heap.
    void *n = img_arena_malloc(newsz);
    if (!n) return NULL;
    size_t copy = (oldsz < newsz) ? oldsz : newsz;
    if (copy) memcpy(n, p, copy);
    return n;
}
