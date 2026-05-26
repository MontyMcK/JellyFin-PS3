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

#include <ppu-types.h>
#include <sys/thread.h>

#include "thumbnail_cache.h"
#include "bitmap.h"
#include "http.h"
#include "jellyfin_api.h"
#include "plog.h"
#include "ui_visuals.h"

#define THUMB_CACHE_SIZE  20
#define FETCH_BUF_SIZE    (48*1024)

typedef enum { SLOT_EMPTY = 0, SLOT_QUEUED, SLOT_READY } SlotState;

typedef struct {
    char      item_id[64];
    Bitmap    bmp;
    SlotState state;
} ThumbSlot;

static ThumbSlot        s_slots[THUMB_CACHE_SIZE];
static char             s_queue[THUMB_CACHE_SIZE][64];
static int              s_q_head     = 0;
static int              s_q_tail     = 0;
static volatile int     s_lock       = 0;
static sys_ppu_thread_t s_thread     = 0;
static volatile bool    s_running    = false;
static int              s_evict_next = 0;
static uint8_t          s_fetch_buf[FETCH_BUF_SIZE];

static void lock_acquire(void) { while (!__sync_bool_compare_and_swap(&s_lock, 0, 1)) ; }
static void lock_release(void) { __sync_bool_compare_and_swap(&s_lock, 1, 0); }

static bool q_empty(void) { return s_q_head == s_q_tail; }
static bool q_full(void)  { return ((s_q_tail + 1) % THUMB_CACHE_SIZE) == s_q_head; }

static void q_push(const char *id) {
    strncpy(s_queue[s_q_tail], id, 63);
    s_queue[s_q_tail][63] = '\0';
    s_q_tail = (s_q_tail + 1) % THUMB_CACHE_SIZE;
}

static bool q_pop(char *out) {
    if (q_empty()) return false;
    strncpy(out, s_queue[s_q_head], 63);
    out[63] = '\0';
    s_q_head = (s_q_head + 1) % THUMB_CACHE_SIZE;
    return true;
}

static int find_slot(const char *id) {
    for (int i = 0; i < THUMB_CACHE_SIZE; i++) {
        if (s_slots[i].state != SLOT_EMPTY &&
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
        lock_acquire();
        bool got = q_pop(item_id);
        lock_release();

        if (!got) { usleep(8000); continue; }

        lock_acquire();
        int si = find_slot(item_id);
        lock_release();

        if (si < 0 || s_slots[si].state != SLOT_QUEUED) continue;

        char url[512];
        snprintf(url, sizeof(url),
            "%s/Items/%s/Images/Primary?width=%d&height=%d&quality=90",
            g_server, item_id, (int)XMB_THUMB_W, (int)XMB_THUMB_H);

        int bytes = http_fetch_binary(url, g_token, s_fetch_buf, FETCH_BUF_SIZE);
        if (bytes <= 0) {
            char msg[128];
            snprintf(msg, sizeof(msg), "thumb: fetch failed %s (%d)", item_id, bytes);
            plog(msg);
            lock_acquire();
            if (find_slot(item_id) == si)
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
            if (find_slot(item_id) == si)
                s_slots[si].state = SLOT_EMPTY;
            lock_release();
            continue;
        }

        int total = (int)XMB_THUMB_W * (int)XMB_THUMB_H;
        for (int i = 0; i < total; i++) {
            u32 r = px[i*4+0];
            u32 g = px[i*4+1];
            u32 b = px[i*4+2];
            s_slots[si].bmp.pixels[i] = (r << 16) | (g << 8) | b;
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
    for (int i = 0; i < THUMB_CACHE_SIZE; i++)
        bitmapInit(&s_slots[i].bmp, XMB_THUMB_W, XMB_THUMB_H);
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
    for (int i = 0; i < THUMB_CACHE_SIZE; i++)
        bitmapDestroy(&s_slots[i].bmp);
}

void thumb_request(const char *item_id) {
    if (!item_id || !item_id[0]) return;
    lock_acquire();
    if (find_slot(item_id) >= 0) { lock_release(); return; }
    int si = claim_slot();
    if (si < 0) { lock_release(); return; }
    strncpy(s_slots[si].item_id, item_id, 63);
    s_slots[si].item_id[63] = '\0';
    s_slots[si].state = SLOT_QUEUED;
    if (!q_full()) q_push(item_id);
    lock_release();
}

const Bitmap *thumb_get(const char *item_id) {
    if (!item_id || !item_id[0]) return NULL;
    for (int i = 0; i < THUMB_CACHE_SIZE; i++) {
        if (s_slots[i].state == SLOT_READY &&
            strncmp(s_slots[i].item_id, item_id, 64) == 0)
            return &s_slots[i].bmp;
    }
    return NULL;
}
