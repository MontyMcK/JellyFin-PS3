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
#include "glog.h"
#include "timing.h"
#include "ui_visuals.h"

// ---- Diagnostics (GUIlog.txt, only when logging is enabled) ----------------
// Per-second counters, summarised + reset by the heartbeat in thumb_cache_tick.
// Racy across the fetch thread by design — approximate volumes are all we need.
static u32 d_req = 0, d_claim_ok = 0, d_claim_fail = 0, d_size_drop = 0,
           d_fail_drop = 0, d_fetch_ok = 0, d_fetch_fail = 0, d_decode_fail = 0,
           d_steal_drop = 0, d_get_hit = 0, d_get_miss = 0;
static bool d_fetch_gated = false;   // fetch thread's last-known gate state

// Sized for the Home shelf, which shows several rows of cards at once (more
// distinct (id,size) thumbs on screen than a single grid page) plus light
// prefetch.  Slot pixel budget is the largest grid card, which is >= any Home
// card, so more slots only costs slot headers + that fixed bitmap each.
#define THUMB_CACHE_SIZE  48
#define FETCH_BUF_SIZE    (256*1024)

typedef enum { SLOT_EMPTY = 0, SLOT_QUEUED, SLOT_READY } SlotState;

typedef struct {
    char      item_id[64];
    Bitmap    bmp;            // width/height = the size this slot was requested at
    SlotState state;
    u32       last_touch;     // s_frame when something on screen last wanted it
} ThumbSlot;

static ThumbSlot        s_slots[THUMB_CACHE_SIZE];
static u32              s_frame      = 0;   // advanced by thumb_cache_tick()
static u32              s_fetch_hold = 0;   // fetches paused until this frame
static volatile int     s_lock       = 0;
static sys_ppu_thread_t s_thread     = 0;
static volatile bool    s_running    = false;
static size_t           s_max_px     = 0;            // pixel capacity per slot
static uint8_t          s_fetch_buf[FETCH_BUF_SIZE];

static void lock_acquire(void) { while (!__sync_bool_compare_and_swap(&s_lock, 0, 1)) ; }
static void lock_release(void) { __sync_bool_compare_and_swap(&s_lock, 1, 0); }

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

// Returns a slot to (re)use: empty first, else the least-recently-touched
// slot — READY preferred, but stale QUEUED slots are fair game too (the old
// ring-queue design left QUEUED slots unreclaimable, and a burst of requests
// could wedge the whole cache with nothing fetchable ever again).  Stealing
// a QUEUED slot mid-fetch is safe: the fetch thread re-checks the slot's id
// before publishing, so a stale fetch just gets dropped.
static int claim_slot(void) {
    int best = -1; u32 best_age = 0;
    for (int i = 0; i < THUMB_CACHE_SIZE; i++) {
        if (s_slots[i].state == SLOT_EMPTY) return i;
        u32 age = s_frame - s_slots[i].last_touch;
        // Prefer READY over QUEUED at similar age: bias QUEUED down slightly
        // so we do not cancel in-flight fetches while idle art is available.
        if (s_slots[i].state == SLOT_QUEUED) age = (age > 30) ? age - 30 : 0;
        if (age > best_age || best < 0) { best_age = age; best = i; }
    }
    // Never evict something touched this frame — it is on screen right now.
    if (best >= 0 && s_frame - s_slots[best].last_touch == 0) return -1;
    if (best >= 0) {
        s_slots[best].state = SLOT_EMPTY;
        s_slots[best].item_id[0] = '\0';
    }
    return best;
}

static void fetch_thread_fn(void *arg) {
    (void)arg;
    char item_id[64];

    while (s_running) {
        // Serve the most-recently-touched QUEUED slot: that is what is on
        // screen right now.  Anything queued long ago was scrolled away and
        // will age out (thumb_cache_tick) without wasting a fetch on it.
        // While the tab-switch cooldown is armed, start nothing: rapid
        // L1/R1 flipping keeps pushing the hold forward, so the CPU never
        // burns JPEG decodes for tabs the user is skipping through.
        int si = -1; u32 best_touch = 0;
        lock_acquire();
        if ((s32)(s_fetch_hold - s_frame) > 0) {
            u32 hold = s_fetch_hold, frame = s_frame;
            bool was = d_fetch_gated; d_fetch_gated = true;
            lock_release();
            if (!was)
                glogf("fetch GATED (cooldown) hold=%u frame=%u remain=%d",
                      hold, frame, (int)(s32)(hold - frame));
            usleep(8000);
            continue;
        }
        if (d_fetch_gated) {
            d_fetch_gated = false;
            glogf("fetch RESUMED frame=%u", s_frame);
        }
        for (int i = 0; i < THUMB_CACHE_SIZE; i++) {
            if (s_slots[i].state != SLOT_QUEUED) continue;
            if (si < 0 || (s32)(s_slots[i].last_touch - best_touch) > 0) {
                si = i; best_touch = s_slots[i].last_touch;
            }
        }
        bool got = (si >= 0);
        int tw = 0, th = 0;
        if (got) {
            strncpy(item_id, s_slots[si].item_id, 63);
            item_id[63] = '\0';
            tw = (int)s_slots[si].bmp.width;
            th = (int)s_slots[si].bmp.height;
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

        glogf("fetch START %s %dx%d", item_id, tw, th);
        int bytes = http_fetch_binary(url, g_token, s_fetch_buf, FETCH_BUF_SIZE);
        if (bytes <= 0) {
            char msg[128];
            snprintf(msg, sizeof(msg), "thumb: fetch failed %s (%d)", item_id, bytes);
            plog(msg);
            d_fetch_fail++;
            glogf("fetch FAIL %s bytes=%d url=%.120s", item_id, bytes, url);
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
            d_decode_fail++;
            // stbi's own reason ("outofmem" vs a format complaint) plus a
            // direct heap probe: can we even get ~512KB right now (roughly a
            // 230x345 decode's transient)?  Together these say in one shot
            // whether this is a heap ceiling or an undecodable image.
            const char *reason = stbi_failure_reason();
            void *probe = malloc(512 * 1024);
            glogf("decode FAIL %s bytes=%d reason=%s probe512=%s",
                  item_id, bytes, reason ? reason : "(null)",
                  probe ? "ok" : "FAIL");
            if (probe) free(probe);
            lock_acquire();
            fail_note(item_id);
            if (strncmp(s_slots[si].item_id, item_id, 64) == 0)
                s_slots[si].state = SLOT_EMPTY;
            lock_release();
            continue;
        }

        // Slot may have been stolen for a different item while we fetched —
        // drop the result instead of scribbling stale art into it.
        lock_acquire();
        bool still_ours = (strncmp(s_slots[si].item_id, item_id, 64) == 0 &&
                           s_slots[si].state == SLOT_QUEUED);
        lock_release();
        if (!still_ours) {
            stbi_image_free(px);
            d_steal_drop++;
            glogf("fetch DROP (slot reused mid-fetch) %s", item_id);
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
        bool published = (strncmp(s_slots[si].item_id, item_id, 64) == 0);
        if (published) s_slots[si].state = SLOT_READY;
        lock_release();

        d_fetch_ok++;
        char msg[128];
        snprintf(msg, sizeof(msg), "thumb: ready %s (%dx%d)", item_id, w, h);
        plog(msg);
        glogf("fetch READY %s src=%dx%d slot=%d%s", item_id, w, h, si,
              published ? "" : " (but slot reused, dropped)");
    }

    sysThreadExit(0);
}

void thumb_cache_init(void) {
    // Reset slot state but keep the cached pixel buffers (a plain memset
    // would drop the pointers and leak the bitmaps every player session).
    for (int i = 0; i < THUMB_CACHE_SIZE; i++) {
        u32 *px = s_slots[i].bmp.pixels;
        memset(&s_slots[i], 0, sizeof(s_slots[i]));
        s_slots[i].bmp.pixels = px;
    }
    s_frame = 0;
    // Reset the fetch cooldown too.  s_fetch_hold is a FUTURE frame stamp
    // (set to s_frame + COOLDOWN by thumb_cache_retarget on tab switches).
    // A session resets s_frame to 0 here, but if the last tab switch before
    // playback left s_fetch_hold at a large value, the fetch thread would
    // see (s_fetch_hold - s_frame) > 0 and pause every fetch until s_frame
    // climbed back up — one frame per vsync, i.e. blank thumbnails for
    // however long the user had been browsing before they hit play.
    s_fetch_hold = 0;
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
    // The buffers are CACHED across shutdown/init (shutdown only stops the
    // fetch thread) so they keep one permanent home in the heap alongside
    // the player's boot-time reservations instead of re-racing for space.
    int failed = 0;
    for (int i = 0; i < THUMB_CACHE_SIZE; i++) {
        Bitmap *b = &s_slots[i].bmp;
        b->width  = 0;
        b->height = 0;
        if (!b->pixels) b->pixels = (u32*)memalign(16, s_max_px * 4);
        if (!b->pixels) failed++;
        b->offset = 0;
    }
    if (failed) {
        char buf[64];
        snprintf(buf, sizeof(buf), "thumb_cache: %d of %d slots FAILED", failed, THUMB_CACHE_SIZE);
        plog(buf);
    }
    s_running = true;
    int trc = sysThreadCreate(&s_thread, fetch_thread_fn, NULL, 1500, 65536, 0, "thumb_fetch");
    d_fetch_gated = false;
    glogf("INIT max_px=%u portrait=%dx%d landscape=%dx%d slotsFailed=%d "
          "thrRC=%d frame=0 hold=0",
          (unsigned)s_max_px, gp.card_w, gp.card_h, gl.card_w, gl.card_h,
          failed, trc);
}

void thumb_cache_shutdown(void) {
    glogf("SHUTDOWN (frame=%u hold=%u)", s_frame, s_fetch_hold);
    s_running = false;
    if (s_thread) {
        u64 tret;
        sysThreadJoin(s_thread, &tret);
        s_thread = 0;
    }
    // Slot pixel buffers stay cached — see thumb_cache_init.
}

void thumb_request(const char *item_id, int w, int h) {
    if (!item_id || !item_id[0] || w <= 0 || h <= 0) return;
    d_req++;
    if ((size_t)w * (size_t)h > s_max_px) {
        d_size_drop++;
        return;
    }
    lock_acquire();
    if (fail_listed(item_id)) { d_fail_drop++; lock_release(); return; }
    int have = find_slot(item_id, (u32)w, (u32)h);
    if (have >= 0) { s_slots[have].last_touch = s_frame; lock_release(); return; }
    int si = claim_slot();
    if (si < 0) { d_claim_fail++; lock_release(); return; }
    strncpy(s_slots[si].item_id, item_id, 63);
    s_slots[si].item_id[63] = '\0';
    s_slots[si].bmp.width  = (u32)w;
    s_slots[si].bmp.height = (u32)h;
    s_slots[si].state      = SLOT_QUEUED;
    s_slots[si].last_touch = s_frame;
    d_claim_ok++;
    lock_release();
    glogf("QUEUE %s %dx%d slot=%d", item_id, w, h, si);
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
            strncmp(s_slots[i].item_id, item_id, 64) == 0) {
            s_slots[i].last_touch = s_frame;
            d_get_hit++;
            return &s_slots[i].bmp;
        }
    }
    d_get_miss++;
    return NULL;
}

// Advance the cache clock and unload anything nothing has drawn or
// requested recently.  Called once per UI frame from the XMB loop; screens
// that do not tick (music Now Playing) simply freeze the clock, which
// keeps their art resident.  ~3 s at 60 fps: long enough to survive a
// quick scroll round-trip, short enough that a tab switch frees the whole
// previous screen within seconds.
#define THUMB_TTL_FRAMES 180

// Tab switch: age every slot to the TTL edge so the previous tab's art is
// unloaded on the next tick — except anything the new tab draws this same
// frame, which re-touches its slots and survives.  Also arms a short fetch
// cooldown so flipping through tabs quickly costs nothing.
#define THUMB_SWITCH_COOLDOWN 20   // frames (~0.3 s) before fetches resume
#define THUMB_TTL_FRAMES      180  // ~3 s at 60 fps

void thumb_cache_retarget(void) {
    lock_acquire();
    for (int i = 0; i < THUMB_CACHE_SIZE; i++) {
        if (s_slots[i].state == SLOT_EMPTY) continue;
        s_slots[i].last_touch = s_frame - THUMB_TTL_FRAMES;
    }
    s_fetch_hold = s_frame + THUMB_SWITCH_COOLDOWN;
    lock_release();
    glogf("RETARGET frame=%u -> hold=%u", s_frame, s_fetch_hold);
}

void thumb_cache_tick(void) {
    lock_acquire();
    s_frame++;
    int n_empty = 0, n_queued = 0, n_ready = 0;
    for (int i = 0; i < THUMB_CACHE_SIZE; i++) {
        if (s_slots[i].state == SLOT_EMPTY) continue;
        if (s_frame - s_slots[i].last_touch > THUMB_TTL_FRAMES) {
            s_slots[i].state = SLOT_EMPTY;
            s_slots[i].item_id[0] = '\0';
        }
    }
    for (int i = 0; i < THUMB_CACHE_SIZE; i++) {
        if      (s_slots[i].state == SLOT_EMPTY)  n_empty++;
        else if (s_slots[i].state == SLOT_QUEUED) n_queued++;
        else                                      n_ready++;
    }
    u32 frame = s_frame, hold = s_fetch_hold;
    bool run = s_running;
    lock_release();

    // Once-per-second heartbeat: the whole cache state on one line, plus the
    // per-second event counters, then reset them.  This is the line that
    // should reveal why posters are blank — read the QUEUE/fetch/gate flow
    // and the slot histogram (E/Q/R) against what's on screen.
    if (frame % 60 == 0) {
        bool gated = (s32)(hold - frame) > 0;
        glogf("HB f=%u thr=%d gate=%d(hold=%u) slots E/Q/R=%d/%d/%d | "
              "req=%u qOK=%u qFULL=%u szDrop=%u failList=%u | "
              "fOK=%u fFail=%u dFail=%u drop=%u | get H/M=%u/%u",
              frame, run ? 1 : 0, gated ? 1 : 0, hold,
              n_empty, n_queued, n_ready,
              d_req, d_claim_ok, d_claim_fail, d_size_drop, d_fail_drop,
              d_fetch_ok, d_fetch_fail, d_decode_fail, d_steal_drop,
              d_get_hit, d_get_miss);
        d_req = d_claim_ok = d_claim_fail = d_size_drop = d_fail_drop = 0;
        d_fetch_ok = d_fetch_fail = d_decode_fail = d_steal_drop = 0;
        d_get_hit = d_get_miss = 0;
    }
}
