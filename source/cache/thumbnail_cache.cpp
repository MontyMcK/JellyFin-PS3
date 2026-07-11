#define STB_IMAGE_IMPLEMENTATION
#ifdef __GNUC__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wtype-limits"
#endif
#include "stb_image.h"
#ifdef __GNUC__
#pragma GCC diagnostic pop
#endif

#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <malloc.h>

#include <ppu-types.h>
#include <sys/thread.h>

#include "thumbnail_cache.h"
#include "bitmap.h"
#include "http.h"
#include "jellyfin_api.h"
#include "plog.h"
#include "timing.h"
#include "ui_visuals.h"

// Sized for the Home shelf, which shows several rows of cards at once (more
// distinct (id,size) thumbs on screen than a single grid page) plus light
// prefetch.  Slot pixel budget is the largest grid card, which is >= any Home
// card, so more slots only costs slot headers + that fixed bitmap each.
#define THUMB_CACHE_SIZE  64
#define FETCH_BUF_SIZE    (256*1024)

typedef enum { SLOT_EMPTY = 0, SLOT_QUEUED, SLOT_READY } SlotState;

typedef struct {
    char      item_id[64];
    Bitmap    bmp;            // width/height = the size this slot was requested at
    SlotState state;
} ThumbSlot;

static ThumbSlot        s_slots[THUMB_CACHE_SIZE];
static int              s_queue[THUMB_CACHE_SIZE];   // slot indices
static int              s_q_head     = 0;
static int              s_q_tail     = 0;
static volatile int     s_lock       = 0;
static sys_ppu_thread_t s_thread     = 0;
static volatile bool    s_running    = false;
static int              s_evict_next = 0;
static size_t           s_max_px     = 0;            // pixel capacity per slot
static uint8_t          s_fetch_buf[FETCH_BUF_SIZE];

static void lock_acquire(void) { while (!__sync_bool_compare_and_swap(&s_lock, 0, 1)) ; }
static void lock_release(void) { __sync_bool_compare_and_swap(&s_lock, 1, 0); }

static bool q_empty(void) { return s_q_head == s_q_tail; }
static bool q_full(void)  { return ((s_q_tail + 1) % THUMB_CACHE_SIZE) == s_q_head; }

static void q_push(int si) {
    s_queue[s_q_tail] = si;
    s_q_tail = (s_q_tail + 1) % THUMB_CACHE_SIZE;
}

static bool q_pop(int *si) {
    if (q_empty()) return false;
    *si = s_queue[s_q_head];
    s_q_head = (s_q_head + 1) % THUMB_CACHE_SIZE;
    return true;
}

// Failed fetches back off for 10 s — without this the UI re-queues a
// failing thumb every single frame, hammering the server and the log.
// Accessed under s_lock (noted on the fetch thread, checked on request).
#define FAIL_BACKOFF_US 10000000ULL
#define FAIL_LIST_N     16
static struct { char id[64]; u64 until_us; } s_fail[FAIL_LIST_N];
static int s_fail_next = 0;

static bool fail_listed(const char *id) {
    u64 now = timing_get_us();
    for (int i = 0; i < FAIL_LIST_N; i++) {
        if (s_fail[i].id[0] && now < s_fail[i].until_us &&
            strncmp(s_fail[i].id, id, 64) == 0)
            return true;
    }
    return false;
}

static void fail_note(const char *id) {
    strncpy(s_fail[s_fail_next].id, id, 63);
    s_fail[s_fail_next].id[63] = '\0';
    s_fail[s_fail_next].until_us = timing_get_us() + FAIL_BACKOFF_US;
    s_fail_next = (s_fail_next + 1) % FAIL_LIST_N;
}

// An item can be cached at several sizes (e.g. portrait in Movies and
// landscape in Continue Watching), so a slot matches on id AND size.
static int find_slot(const char *id, u32 w, u32 h) {
    for (int i = 0; i < THUMB_CACHE_SIZE; i++) {
        if (s_slots[i].state != SLOT_EMPTY &&
            s_slots[i].bmp.width == w && s_slots[i].bmp.height == h &&
            strncmp(s_slots[i].item_id, id, 64) == 0)
            return i;
    }
    return -1;
}

// Returns an empty slot index, evicting the oldest SLOT_READY entry if needed.
static int claim_slot(void) {
    for (int i = 0; i < THUMB_CACHE_SIZE; i++) {
        if (s_slots[i].state == SLOT_EMPTY) return i;
    }
    for (int tries = 0; tries < THUMB_CACHE_SIZE; tries++) {
        int i = (s_evict_next + tries) % THUMB_CACHE_SIZE;
        if (s_slots[i].state == SLOT_READY) {
            s_evict_next = (i + 1) % THUMB_CACHE_SIZE;
            s_slots[i].state = SLOT_EMPTY;
            s_slots[i].item_id[0] = '\0';
            return i;
        }
    }
    return -1;
}

static void fetch_thread_fn(void *arg) {
    (void)arg;
    char item_id[64];

    while (s_running) {
        int si = -1;
        lock_acquire();
        bool got = q_pop(&si);
        int tw = 0, th = 0;
        if (got && s_slots[si].state == SLOT_QUEUED) {
            strncpy(item_id, s_slots[si].item_id, 63);
            item_id[63] = '\0';
            tw = (int)s_slots[si].bmp.width;
            th = (int)s_slots[si].bmp.height;
        } else {
            got = false;
        }
        lock_release();

        if (!got) { usleep(8000); continue; }

        // fillWidth/fillHeight = scale + centre-crop to exactly the card
        // size (portrait posters or landscape stills as requested).
        // format=Jpeg keeps PNG originals from arriving huge and slow.
        char url[512];
        snprintf(url, sizeof(url),
            "%s/Items/%s/Images/Primary?fillWidth=%d&fillHeight=%d"
            "&quality=75&format=Jpeg",
            g_server, item_id, tw, th);

        int bytes = http_fetch_binary(url, g_token, s_fetch_buf, FETCH_BUF_SIZE);
        if (bytes <= 0) {
            char msg[128];
            snprintf(msg, sizeof(msg), "thumb: fetch failed %s (%d)", item_id, bytes);
            plog(msg);
            lock_acquire();
            fail_note(item_id);
            if (strncmp(s_slots[si].item_id, item_id, 64) == 0)
                s_slots[si].state = SLOT_EMPTY;
            lock_release();
            continue;
        }

        int w, h, ch;
        unsigned char *px = stbi_load_from_memory(
            (const stbi_uc*)s_fetch_buf, bytes, &w, &h, &ch, 4);
        if (!px) {
            char msg[128];
            snprintf(msg, sizeof(msg), "thumb: decode failed %s", item_id);
            plog(msg);
            lock_acquire();
            fail_note(item_id);
            if (strncmp(s_slots[si].item_id, item_id, 64) == 0)
                s_slots[si].state = SLOT_EMPTY;
            lock_release();
            continue;
        }

        // Nearest-neighbour scale to exactly the card size — the server may
        // return different dimensions than requested, and a straight clamped
        // copy would letterbox instead of filling the card.  Every output
        // pixel is written, so no pre-clear is needed.
        Bitmap *bmp = &s_slots[si].bmp;
        for (u32 y = 0; y < bmp->height; y++) {
            u32 *dst = bmp->pixels + y * bmp->width;
            const unsigned char *src =
                px + (size_t)((u64)y * h / bmp->height) * w * 4;
            for (u32 x = 0; x < bmp->width; x++) {
                const unsigned char *s =
                    src + (size_t)((u64)x * w / bmp->width) * 4;
                dst[x] = ((u32)s[0] << 16) | ((u32)s[1] << 8) | s[2];
            }
        }
        stbi_image_free(px);

        lock_acquire();
        if (strncmp(s_slots[si].item_id, item_id, 64) == 0)
            s_slots[si].state = SLOT_READY;
        lock_release();

        char msg[128];
        snprintf(msg, sizeof(msg), "thumb: ready %s (%dx%d)", item_id, w, h);
        plog(msg);
    }

    sysThreadExit(0);
}

void thumb_cache_init(void) {
    memset(s_slots, 0, sizeof(s_slots));
    s_q_head = s_q_tail = s_evict_next = 0;
    // Every slot is sized for the biggest card of either orientation; the
    // actual bitmap dimensions are set per request.
    GridGeom gp, gl;
    xmb_grid_geom(XMB_TAB_MOVIES, &gp);   // portrait posters
    xmb_grid_geom(XMB_TAB_RESUME, &gl);   // landscape stills
    size_t pp = (size_t)gp.card_w * gp.card_h;
    size_t pl = (size_t)gl.card_w * gl.card_h;
    s_max_px  = (pp > pl) ? pp : pl;
    // Pixels live in MAIN memory (not RSX local): the UI blits cards with
    // the CPU every frame, and CPU reads of RSX-local memory are far too
    // slow (and the GPU transfer engine proved freeze-prone for this).
    for (int i = 0; i < THUMB_CACHE_SIZE; i++) {
        Bitmap *b = &s_slots[i].bmp;
        b->width  = 0;
        b->height = 0;
        b->pixels = (u32*)memalign(16, s_max_px * 4);
        b->offset = 0;
    }
    s_running = true;
    sysThreadCreate(&s_thread, fetch_thread_fn, NULL, 1500, 65536, 0, "thumb_fetch");
}

void thumb_cache_shutdown(void) {
    s_running = false;
    if (s_thread) {
        u64 tret;
        sysThreadJoin(s_thread, &tret);
        s_thread = 0;
    }
    for (int i = 0; i < THUMB_CACHE_SIZE; i++) {
        if (s_slots[i].bmp.pixels) { free(s_slots[i].bmp.pixels); s_slots[i].bmp.pixels = NULL; }
    }
}

void thumb_request(const char *item_id, int w, int h) {
    if (!item_id || !item_id[0] || w <= 0 || h <= 0) return;
    if ((size_t)w * (size_t)h > s_max_px) return;
    lock_acquire();
    if (fail_listed(item_id)) { lock_release(); return; }
    if (find_slot(item_id, (u32)w, (u32)h) >= 0) { lock_release(); return; }
    // Don't claim a slot we can't queue — it would stay QUEUED forever.
    if (q_full()) { lock_release(); return; }
    int si = claim_slot();
    if (si < 0) { lock_release(); return; }
    strncpy(s_slots[si].item_id, item_id, 63);
    s_slots[si].item_id[63] = '\0';
    s_slots[si].bmp.width  = (u32)w;
    s_slots[si].bmp.height = (u32)h;
    s_slots[si].state = SLOT_QUEUED;
    q_push(si);
    lock_release();
}

int thumb_max_square(void) {
    int e = 1;
    while ((size_t)(e + 1) * (size_t)(e + 1) <= s_max_px) e++;
    return e;
}

const Bitmap *thumb_get(const char *item_id, int w, int h) {
    if (!item_id || !item_id[0]) return NULL;
    for (int i = 0; i < THUMB_CACHE_SIZE; i++) {
        if (s_slots[i].state == SLOT_READY &&
            s_slots[i].bmp.width == (u32)w && s_slots[i].bmp.height == (u32)h &&
            strncmp(s_slots[i].item_id, item_id, 64) == 0)
            return &s_slots[i].bmp;
    }
    return NULL;
}
