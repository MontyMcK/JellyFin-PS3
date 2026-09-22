#include "video.h"
#include "video_internal.h"
#include "plog.h"
#include "hd1080.h"

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
// s_jbuf_mtx guards the ring indices, the free list and the reorder hold.
//
// A slot is a frame buffer; a POSITION is where that frame sits in display
// order.  The two used to be the same index, which is only right when the
// decoder hands pictures back in display order -- and the Cell H.264 decoder
// does not: it emits them in DECODE order (Movian's ps3_vdec.c: "Pictures are
// delivered out of order from the decoder so we need to deal with that").
// For a stream with B-frames that means P4 B2 b1 b3 P8 ..., and playing that
// in arrival order shows motion going forward-back-forward while the PTS
// lurch by several frames -- which is what a stream-copied Blu-ray looked
// like on the console.  So the producer writes into any free slot, pushes it
// into a small hold sorted by the decoder's picture-order key, and a picture
// is released to the display ring only once a LATER-ordered one has arrived
// (Movian's flush_to rule).  Streams without B-frames pass through with one
// picture of latency.

sys_mutex_t s_jbuf_mtx;

static u8  *s_jbuf_data[JBUF_MAX_SLOTS] = {};
static u64  s_jbuf_pts[JBUF_MAX_SLOTS]  = {};   // per SLOT
static s64  s_jbuf_dur[JBUF_MAX_SLOTS]  = {};   // remaining display duration per slot (us)
static u32  s_jbuf_seq[JBUF_MAX_SLOTS]  = {};   // display sequence, stamped on release
static u32  s_seq_counter          = 0;
static u32  s_jbuf_fw = 0, s_jbuf_fh = 0;

// Display ring: slot indices in display order.
static int  s_ring[JBUF_MAX_SLOTS]      = {};
static int  s_jb_wr = 0, s_jb_rd = 0;           // ring positions
static volatile int s_jb_n = 0;                 // displayable frames (in the ring)

// Free slots, and the one currently handed to the decoder to fill.
static int  s_free[JBUF_MAX_SLOTS]      = {};
static int  s_free_n  = 0;
static int  s_wr_slot = -1;
static volatile int s_jb_used = 0;              // cap - free: ring + hold + write slot

// Reorder hold: decoded, not yet released, sorted by order ascending.
// Capped so the display ring always keeps a share of the slots: bframes=3
// (x264 default, and what Blu-ray uses) peaks at 4 held, and the 1080p path
// only has 8 slots in total.  When it is full the lowest is released early,
// which for a deeper pyramid means one picture a touch out of order rather
// than a stall.
#define JBUF_HOLD_MAX 6
static struct { int slot; s64 order; } s_hold[JBUF_HOLD_MAX];
static int  s_hold_n    = 0;
static s64  s_max_order = -1;

// Slot buffers are CACHED once allocated: jbuf_reserve() grabs them at boot
// (before the UI fragments the heap) and jbuf_free() keeps them.  The big
// three (VDEC arena, these slots, HUD overlay) total ~162MB against a heap
// with only a few MB of slack, so allocating them per-session made every
// session a dice roll on whatever the UI had done to the heap layout.
static u32 s_jbuf_slot_bytes = 0;     // capacity of each cached slot
static int s_jb_cap = JBUF_SD_SLOTS;  // active ring capacity (set by jbuf_reserve)

// Active ring capacity: fewer slots for 1080p so the big frames fit.  The ring
// modulus/full-checks below all use this, so slots >= s_jb_cap are never
// touched even though the static arrays are always JBUF_MAX_SLOTS long.
//
// Keyed off the SLOT SIZE, not the 1080p toggle.  Since the info screen gained
// a quality row, the frame size no longer follows that toggle: asking for
// 1080p with the toggle off used to leave the capacity at the 24-slot SD
// figure while each slot held a 3.13 MB 1080p frame — a 75 MB jitter buffer,
// which is more than the console has left at that point.
// 1080p is 3.13 MB a slot, 720p 1.32 MB; anything above 2 MB is the big path.
static int jbuf_cap_for_bytes(u32 slot_bytes) {
    return (slot_bytes > 2u * 1024u * 1024u) ? JBUF_1080_SLOTS : JBUF_SD_SLOTS;
}

int jbuf_cap(void) { return s_jb_cap; }

// Prefill target must not exceed the capacity or the prefill loop can never
// reach it (it would spin the whole guard budget forever at 1080p).
int jbuf_prefill_target(void) {
    int cap = jbuf_cap();
    return JBUF_PREFILL < cap ? JBUF_PREFILL : cap;
}

// Planar YUV420P, 1.5 bpp — the universal frame format.  The decoder writes
// tight planar YUV (picsize == fw*fh*3/2); round the height up to a macroblock
// (16) for headroom.  This sizes the jitter-buffer slot that holds the whole
// planar frame (Y|Cb|Cr); the RSX plane textures are sized separately.
u32 vid_frame_bytes(u32 fw, u32 fh) {
    u32 fh_pad = (fh + 15u) & ~15u;
    return fw * fh_pad * 3u / 2u;
}

// Allocate the slot buffers if not already cached (or cached too small).
// Returns false with NOTHING left allocated on failure (the old code leaked
// the partial slots, so each failed attempt made the next one worse).
//
// Only the first jbuf_cap() slots are allocated — the 1080p path caches fewer,
// larger slots.  When the capacity later grows back (1080p -> 720p) the extra
// slots are (re)allocated here, and when it shrinks the surplus is freed, so
// the producer never dereferences a NULL slot below the active capacity.
bool jbuf_reserve(u32 fw, u32 fh) {
    u32 need = vid_frame_bytes(fw, fh);
    // From the size about to be reserved, not from any toggle — see
    // jbuf_cap_for_bytes.
    int cap  = jbuf_cap_for_bytes(need);
    s_jb_cap = cap;

    // Drop any slots beyond the active capacity (saves RAM at 1080p).
    for (int i = cap; i < JBUF_MAX_SLOTS; i++) {
        if (s_jbuf_data[i]) { free(s_jbuf_data[i]); s_jbuf_data[i] = NULL; }
    }
    // If the cached slots are too small for this frame size, they must all be
    // re-grabbed at the new size.
    if (s_jbuf_slot_bytes < need) {
        for (int i = 0; i < JBUF_MAX_SLOTS; i++) {
            if (s_jbuf_data[i]) { free(s_jbuf_data[i]); s_jbuf_data[i] = NULL; }
        }
        s_jbuf_slot_bytes = 0;
    }
    // Ensure every active slot is allocated (>= need bytes).
    for (int i = 0; i < cap; i++) {
        if (s_jbuf_data[i]) continue;
        s_jbuf_data[i] = (u8*)memalign(128, need);
        if (!s_jbuf_data[i]) {
            char buf[64];
            snprintf(buf, sizeof(buf), "jbuf_reserve: slot %d of %d FAILED (%u KB)",
                     i, cap, need / 1024);
            plog(buf);
            for (int j = 0; j < i; j++) { free(s_jbuf_data[j]); s_jbuf_data[j] = NULL; }
            s_jbuf_slot_bytes = 0;
            return false;
        }
    }
    s_jbuf_slot_bytes = need;
    return true;
}

// Every slot back on the free list, nothing held, nothing displayable.
// Caller holds s_jbuf_mtx (or is single-threaded at that point).
static void jbuf_reset_state(void) {
    s_jb_wr = s_jb_rd = s_jb_n = 0;
    s_free_n = 0;
    for (int i = s_jb_cap - 1; i >= 0; i--) s_free[s_free_n++] = i;
    s_wr_slot    = -1;
    s_hold_n     = 0;
    s_max_order  = -1;
    s_jb_used    = 0;
}

static int jbuf_hold_max(void) {
    int h = s_jb_cap / 2;
    return h < JBUF_HOLD_MAX ? h : JBUF_HOLD_MAX;
}

bool jbuf_alloc(u32 fw, u32 fh) {
    if (!jbuf_reserve(fw, fh)) return false;
    s_jbuf_fw = fw; s_jbuf_fh = fh;
    jbuf_reset_state();
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
    jbuf_reset_state();
    sysMutexDestroy(s_jbuf_mtx);
}

void jbuf_clear(void) {
    sysMutexLock(s_jbuf_mtx, 0);
    jbuf_reset_state();
    sysMutexUnlock(s_jbuf_mtx);
}

// Front / next slot of the display ring, or -1.
static inline int front_slot(void) { return (s_jb_n > 0) ? s_ring[s_jb_rd] : -1; }
static inline int next_slot(void)  { return (s_jb_n > 1) ? s_ring[(s_jb_rd + 1) % s_jb_cap] : -1; }

// Retire the front position and give its slot back.  Caller holds the mutex.
static void ring_pop_locked(void) {
    int slot = s_ring[s_jb_rd];
    s_jb_rd = (s_jb_rd + 1) % s_jb_cap;
    s_jb_n--;
    s_free[s_free_n++] = slot;
    s_jb_used = s_jb_cap - s_free_n;
}

const u8 *jbuf_peek(void)     { int f = front_slot(); return f >= 0 ? s_jbuf_data[f] : NULL; }
void      jbuf_pop(void) {
    sysMutexLock(s_jbuf_mtx, 0);
    if (s_jb_n > 0) ring_pop_locked();
    sysMutexUnlock(s_jbuf_mtx);
}
u32       jbuf_fw(void)       { return s_jbuf_fw; }
u32       jbuf_fh(void)       { return s_jbuf_fh; }
int       jbuf_count(void)    { return s_jb_n; }
int       jbuf_used(void)     { return s_jb_used; }
int       jbuf_rd(void)       { return s_jb_rd; }
u64       jbuf_peek_pts_us(void) { int f = front_slot(); return f >= 0 ? s_jbuf_pts[f] : 0; }
u32       jbuf_peek_seq(void) { int f = front_slot(); return f >= 0 ? s_jbuf_seq[f] : 0; }
const u8 *jbuf_slot_ptr(int i) { return (i >= 0 && i < JBUF_MAX_SLOTS) ? s_jbuf_data[i] : NULL; }

s64       jbuf_peek_dur(void)      { int f = front_slot(); return f >= 0 ? s_jbuf_dur[f] : 0; }
s64       jbuf_peek_next_dur(void) { int n = next_slot();  return n >= 0 ? s_jbuf_dur[n] : 0; }
const u8 *jbuf_peek_next(void)     { int n = next_slot();  return n >= 0 ? s_jbuf_data[n] : NULL; }

void jbuf_consume_dur(s64 us) {
    int f = front_slot();
    if (f >= 0) s_jbuf_dur[f] -= us;
}

void jbuf_advance(void) {
    sysMutexLock(s_jbuf_mtx, 0);
    if (s_jb_n > 0 && s_jbuf_dur[s_ring[s_jb_rd]] <= 0) ring_pop_locked();
    sysMutexUnlock(s_jbuf_mtx);
}

// ---- Producer side (decode thread only; see video_internal.h) ----

bool jbuf_full(void) {
    sysMutexLock(s_jbuf_mtx, 0);
    // A write slot already handed out but not yet pushed still counts as room:
    // a failed vdecGetPicture leaves it allocated and the next pull reuses it.
    bool full = (s_free_n == 0 && s_wr_slot < 0);
    sysMutexUnlock(s_jbuf_mtx);
    return full;
}

u8 *jbuf_write_ptr(void) {
    if (s_wr_slot < 0) {
        sysMutexLock(s_jbuf_mtx, 0);
        if (s_free_n > 0) {
            s_wr_slot = s_free[--s_free_n];
            s_jb_used = s_jb_cap - s_free_n;
        }
        sysMutexUnlock(s_jbuf_mtx);
        if (s_wr_slot < 0) return NULL;     // caller checked jbuf_full(); belt and braces
    }
    return s_jbuf_data[s_wr_slot];
}
int  jbuf_write_idx(void) { return s_wr_slot; }

void jbuf_set_dims(u32 fw, u32 fh) { s_jbuf_fw = fw; s_jbuf_fh = fh; }

// Append a slot to the display ring.  Caller holds the mutex.  The ring has
// one position per slot, so it can never overflow.
static void ring_release_locked(int slot) {
    s_jbuf_seq[slot]  = ++s_seq_counter;
    s_ring[s_jb_wr]   = slot;
    s_jb_wr = (s_jb_wr + 1) % s_jb_cap;
    s_jb_n++;
}

// Release held pictures from the front of the (sorted) hold while their order
// is <= up_to.  Caller holds the mutex.
static void hold_release_locked(s64 up_to) {
    int n = 0;
    while (n < s_hold_n && s_hold[n].order <= up_to) n++;
    for (int i = 0; i < n; i++) ring_release_locked(s_hold[i].slot);
    if (n > 0) {
        for (int i = n; i < s_hold_n; i++) s_hold[i - n] = s_hold[i];
        s_hold_n -= n;
    }
}

void jbuf_push(u64 pts_us, s64 dur_us, s64 order) {
    if (s_wr_slot < 0) return;              // nothing was written
    int slot   = s_wr_slot;
    s_wr_slot  = -1;
    s_jbuf_pts[slot] = pts_us;
    s_jbuf_dur[slot] = dur_us;

    sysMutexLock(s_jbuf_mtx, 0);
    if (order == JBUF_ORDER_NONE) {
        // No ordering key (no codec info on the picture): arrival order, as
        // before.  Anything still held goes out first so it cannot overtake.
        hold_release_locked(s_max_order);
        ring_release_locked(slot);
        sysMutexUnlock(s_jbuf_mtx);
        return;
    }

    // Movian's rule: a picture with a higher order than any seen so far means
    // everything at or below the PREVIOUS maximum is complete in display
    // order and can go out.  Pictures at or below the current maximum wait.
    if (s_hold_n >= jbuf_hold_max()) {
        // Hold full: the lowest goes out now.  With bframes<=3 this is the
        // picture the next P would have released anyway.
        ring_release_locked(s_hold[0].slot);
        for (int i = 1; i < s_hold_n; i++) s_hold[i - 1] = s_hold[i];
        s_hold_n--;
    }
    int at = s_hold_n;
    while (at > 0 && s_hold[at - 1].order > order) { s_hold[at] = s_hold[at - 1]; at--; }
    s_hold[at].slot  = slot;
    s_hold[at].order = order;
    s_hold_n++;

    if (s_max_order < 0) {
        s_max_order = order;
    } else if (order > s_max_order) {
        s64 flush_to = s_max_order;
        s_max_order  = order;
        hold_release_locked(flush_to);
    }
    sysMutexUnlock(s_jbuf_mtx);
}
