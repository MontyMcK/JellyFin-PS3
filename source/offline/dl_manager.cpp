// Offline downloads -- the download manager.  See dl_manager.h.

#include "dl_manager.h"
#include "dl_http.h"
#include "dl_platform.h"
#include "dl_store.h"
#include "dl_ts.h"

#include <stdio.h>
#include <string.h>

// One transfer buffer for the process, used two ways at once: sockets are
// read straight into its free tail, and the payload is written to the HDD
// only when it is nearly full (or at a checkpoint / the end).
//
// That write batching is the lesson from pkgi-ps3, the PS3's fastest package
// downloader: it never writes per network read -- its bytes go through
// newlib's buffered fwrite -- and it keeps its per-read bookkeeping near zero
// (free space is queried once per 512 writes, progress redrawn every 500 ms).
// On this console every lv2 fs write is a syscall into an encrypted HDD, so
// issuing one per 1.4-64 KB network read costs far more than one per ~450 KB.
//
// 512 KB total.  Static, like stream.cpp's 256 KB socket buffer and
// http.cpp's 388 KB body buffer, so it is carved out before the heap is and
// can never be the allocation that fails when the player needs memory.
#define DL_XFER_BUF (512 * 1024)
// Never ask netRecv for less than this: the tail is flushed first.  256 KB is
// what stream.cpp asks for per read (after Movian); this keeps every read
// at 64 KB+ while leaving most of the buffer to batch the writes.
#define DL_RECV_MIN (64 * 1024)

// Free space above this is not a reading but a fault: no console drive is
// anywhere near it.  Treated as unknown, like an error.
#define DL_FREE_SANE_MAX (8ull * 1024 * 1024 * 1024 * 1024)

enum { CTL_NONE = 0, CTL_PAUSE, CTL_CANCEL, CTL_REMOVE };

typedef struct {
    bool     used;
    bool     active;         // owned by the worker; rec is the worker's to write
    bool     removing;       // remove requested while active: hidden already
    volatile int ctl;        // posted to the worker for the active item
    uint64_t retry_at_ms;    // not persisted: backoff restarts after a reboot
    DlRecord rec;
} Slot;

static Slot     s_slots[DL_MAX_ITEMS];
static bool     s_ready = false;
static DlConfig s_cfg;
static char     s_auth[512] = "";
static volatile bool s_suspended = false;
static volatile bool s_auth_hold = false;    // a 401 until the next login
static volatile bool s_space_hold = false;   // the HDD is at its reserve, or unreadable
static DlSpaceReport s_last_report;          // behind the last enqueue refusal
// Playback, from dl_playback_begin/end: a heavy stream blocks downloads
// outright; a light one lets them run, paced to stream_share_bps.
static volatile bool s_play_block = false;
static volatile bool s_play_light = false;
static uint32_t s_next_seq = 1;
static uint8_t  s_buf[DL_XFER_BUF];

#define LOCK()   dl_plat_lock()
#define UNLOCK() dl_plat_unlock()

static void logf_id(const char *id, const char *what) {
    char b[128];
    snprintf(b, sizeof(b), "dl: %.8s %s", id, what);
    dl_plat_log(b);
}

void dl_config_defaults(DlConfig *c) {
    // The console needs free HDD to save games, install updates and keep its
    // caches; a full drive breaks things well outside this app.
    c->reserve_bytes     = DL_RESERVE_MIN;
    // Same header deadline as the player's stream_open(): a transcode that
    // extracts a subtitle or re-encodes 4K can take over a minute to start.
    c->head_timeout_ms   = 120000;
    c->idle_timeout_ms   = 30000;
    c->checkpoint_ms     = 2000;
    c->checkpoint_bytes  = 8ull * 1024 * 1024;
    c->space_check_bytes = 64ull * 1024 * 1024;
    // Progress is shown, not logged: four updates a second is smooth, and
    // anything faster is lock traffic the UI cannot display anyway.
    c->progress_ms       = 250;
    // Beside a light (480p-or-below) stream, downloads may use 12 Mbps.  The
    // console pulls ~25 Mbps over HTTP in total (vquality.h has the three
    // measurements); the heaviest light stream asks for ~2.2 Mbps, and the
    // rest is headroom the player's read-ahead ring needs to refill after a
    // server hiccup -- that refill, not the average, is what stutters.
    c->stream_share_bps  = 12000000;
}

bool dl_manager_ready(void) { return s_ready; }

void dl_set_auth_header(const char *line) {
    LOCK();
    snprintf(s_auth, sizeof(s_auth), "%s", line ? line : "");
    s_auth_hold = false;          // a new session: worth trying again
    UNLOCK();
}

bool dl_auth_held(void) { return s_auth_hold || !s_auth[0]; }

void dl_set_suspended(bool suspended) { s_suspended = suspended; }

void dl_playback_begin(const char *stream_url) {
    const bool light = dl_stream_is_light(stream_url);
    const bool was_block = s_play_block, was_light = s_play_light;
    s_play_light = light;
    s_play_block = !light;
    if (was_block != s_play_block || was_light != s_play_light)
        dl_plat_log(light ? "dl: light stream playing -- downloads paced"
                          : "dl: stream playing -- downloads stopped");
}

void dl_playback_begin_local(bool light) {
    const bool was_block = s_play_block, was_light = s_play_light;
    s_play_light = light;
    s_play_block = !light;
    if (was_block != s_play_block || was_light != s_play_light)
        dl_plat_log(light ? "dl: light local playback -- downloads paced"
                          : "dl: local playback -- downloads stopped");
}

void dl_playback_end(void) {
    if (s_play_block || s_play_light) dl_plat_log("dl: playback ended");
    s_play_block = false;
    s_play_light = false;
}

bool dl_playback_blocking(void) { return s_play_block; }

// -------------------------------------------------------------------------
// Slot helpers (callers hold the lock)
// -------------------------------------------------------------------------

static Slot *find_slot(const char *id) {
    for (int i = 0; i < DL_MAX_ITEMS; i++)
        if (s_slots[i].used && !s_slots[i].removing &&
            strcmp(s_slots[i].rec.id, id) == 0)
            return &s_slots[i];
    return NULL;
}

static Slot *free_slot(void) {
    for (int i = 0; i < DL_MAX_ITEMS; i++)
        if (!s_slots[i].used) return &s_slots[i];
    return NULL;
}

// Applies ev if legal.  False (record untouched) otherwise.
static bool apply(DlRecord *r, DlEvent ev) {
    DlState n;
    if (!dl_next_state(r->state, ev, &n)) return false;
    r->state = n;
    return true;
}

// What the HDD can take above the reserve.  Anything that cannot be read, or
// reads as impossible, is UNKNOWN -- and unknown means no writes.
static void space_eval(uint64_t need, uint64_t committed, DlSpaceReport *out) {
    memset(out, 0, sizeof(*out));
    out->reserve_bytes    = s_cfg.reserve_bytes;
    out->committed_bytes  = committed;
    out->need_bytes       = need + committed;
    const uint64_t freeb  = dl_plat_free_bytes(dl_store_root());
    if (freeb == DL_FREE_UNKNOWN || freeb > DL_FREE_SANE_MAX) {
        out->state = DL_SPACE_UNKNOWN;
        return;
    }
    out->free_bytes  = freeb;
    out->avail_bytes = freeb > out->reserve_bytes ? freeb - out->reserve_bytes : 0;
    out->state = out->need_bytes <= out->avail_bytes ? DL_SPACE_OK : DL_SPACE_LOW;
}

static void space_log(const DlSpaceReport *r, const char *verdict) {
    char b[128];
    snprintf(b, sizeof(b), "dl: space free=%lluMB reserve=%lluMB need=%lluMB -> %s",
             (unsigned long long)(r->free_bytes >> 20),
             (unsigned long long)(r->reserve_bytes >> 20),
             (unsigned long long)(r->need_bytes >> 20), verdict);
    dl_plat_log(b);
}

// What an item still has to fetch, as far as anyone knows: the larger of its
// estimate and the length the server declared, less what is on disk.
static uint64_t item_remaining(const DlRecord *r) {
    const uint64_t total = r->est_bytes > r->bytes_total ? r->est_bytes : r->bytes_total;
    return total > r->bytes_done ? total - r->bytes_done : 0;
}

// Still to fetch for everything queued, active or paused, except `skip`.
// The caller holds the lock.
static uint64_t committed_locked(const Slot *skip) {
    uint64_t sum = 0;
    for (int i = 0; i < DL_MAX_ITEMS; i++) {
        const Slot *s = &s_slots[i];
        if (!s->used || s->removing || s == skip) continue;
        if (s->rec.state == DL_QUEUED || s->rec.state == DL_DOWNLOADING ||
            s->rec.state == DL_PAUSED)
            sum += item_remaining(&s->rec);
    }
    return sum;
}

// How much free room a transfer must see before it writes on: the next
// space_check_bytes it will write, plus what is still buffered and not yet
// written, so no write ever lands inside the reserve.
static uint64_t write_margin(void) { return s_cfg.space_check_bytes + DL_XFER_BUF; }

uint64_t dl_reserve_bytes(void) { return s_cfg.reserve_bytes; }
bool dl_space_held(void) { return s_space_hold; }

DlSpaceReport dl_space_report(uint64_t new_bytes) {
    DlSpaceReport r;
    memset(&r, 0, sizeof(r));
    if (!s_ready) { r.state = DL_SPACE_UNKNOWN; return r; }
    LOCK();
    space_eval(new_bytes, committed_locked(NULL), &r);
    UNLOCK();
    return r;
}

DlSpaceReport dl_last_space_report(void) { return s_last_report; }

int dl_space_fit_prefix(const DlSpaceReport *rep, const uint64_t *est, int n) {
    if (!rep || rep->state == DL_SPACE_UNKNOWN) return 0;
    uint64_t used = rep->committed_bytes;
    int k = 0;
    while (k < n && used + est[k] <= rep->avail_bytes) { used += est[k]; k++; }
    return k;
}

// -------------------------------------------------------------------------
// Restore
// -------------------------------------------------------------------------

static void restore_one(const char *id) {
    Slot *s = free_slot();
    if (!s) { logf_id(id, "restore: table full, skipped"); return; }

    char media[DL_PATH_MAX], part[DL_PATH_MAX];
    dl_store_item_file(media, sizeof(media), id, DL_FILE_MEDIA);
    dl_store_item_file(part,  sizeof(part),  id, DL_FILE_PART);
    const int64_t media_sz = dl_plat_file_size(media);
    const int64_t part_sz  = dl_plat_file_size(part);

    static DlMeta m;   // init runs single-threaded
    const bool have_meta = dl_store_load_meta(id, &m);

    DlRecord r;
    if (dl_store_load_record(id, &r)) {
        if (r.state == DL_DOWNLOADING) apply(&r, DL_EV_INTERRUPTED);
        if (r.state != DL_COMPLETED && media_sz > 0 &&
            (r.bytes_total == 0 || (uint64_t)media_sz == r.bytes_total)) {
            // Stopped between renaming the finished file and saving the
            // record that says so.  The file is the truth.
            r.state = DL_COMPLETED;
            r.error = DL_ERR_NONE;
        }
        if (r.state == DL_COMPLETED) {
            if (media_sz <= 0 ||
                (r.bytes_total > 0 && (uint64_t)media_sz != r.bytes_total)) {
                r.state = DL_FAILED;
                r.error = DL_ERR_CORRUPT;
                r.bytes_done = part_sz > 0 ? (uint64_t)part_sz : 0;
                logf_id(id, "restore: media missing or wrong size");
            } else {
                r.bytes_done = r.bytes_total = (uint64_t)media_sz;
            }
        } else {
            // What is on disk beats what the record last checkpointed: the
            // record can lag the file by up to one checkpoint.
            r.bytes_done = part_sz > 0 ? (uint64_t)part_sz : 0;
            if (r.bytes_total > 0 && r.bytes_done > r.bytes_total) {
                dl_plat_truncate(part);
                r.bytes_done = 0;
            }
        }
    } else {
        // No readable record.  Rebuild one from the files so the item is
        // either playable again or at least visible (and removable).
        dl_record_init(&r);
        snprintf(r.id, sizeof(r.id), "%s", id);
        snprintf(r.title, sizeof(r.title), "%s", have_meta ? m.title : id);
        if (have_meta && media_sz > 0) {
            r.state = DL_COMPLETED;
            r.bytes_done = r.bytes_total = (uint64_t)media_sz;
            logf_id(id, "restore: record rebuilt from media");
        } else {
            r.state = DL_FAILED;
            r.error = DL_ERR_CORRUPT;
            r.bytes_done = part_sz > 0 ? (uint64_t)part_sz : 0;
            logf_id(id, "restore: record damaged");
        }
    }
    memset(s, 0, sizeof(*s));
    s->used = true;
    s->rec  = r;
    if (r.seq >= s_next_seq) s_next_seq = r.seq + 1;
}

bool dl_manager_init(const char *root, const DlConfig *cfg) {
    s_ready = false;
    // The internal disk's floor is DL_RESERVE_MIN; a USB drive's (an lfs path,
    // not "/...") is DL_RESERVE_MIN_USB.
    const uint64_t floor = (root && root[0] == '/') ? DL_RESERVE_MIN : DL_RESERVE_MIN_USB;
    if (cfg) s_cfg = *cfg;
    else { dl_config_defaults(&s_cfg); s_cfg.reserve_bytes = floor; }
    // The reserve may be raised, never lowered: below the floor it is the
    // floor, and the optional setting (whole GB) can only add to it.
    {
        const int gb = dl_plat_reserve_gb();
        const uint64_t want = gb > 0 ? (uint64_t)gb * 1024ull * 1024 * 1024 : 0;
        if (want > s_cfg.reserve_bytes) s_cfg.reserve_bytes = want;
        if (s_cfg.reserve_bytes < floor) s_cfg.reserve_bytes = floor;
    }
    s_space_hold = false;
    if (!dl_store_init(root)) return false;
    {
        DlSpaceReport rep;
        space_eval(0, 0, &rep);
        char b[160];
        if (rep.state == DL_SPACE_UNKNOWN)
            snprintf(b, sizeof(b), "dl: root %s free=unknown reserve=%lluMB (downloads paused)",
                     root, (unsigned long long)(rep.reserve_bytes >> 20));
        else
            snprintf(b, sizeof(b), "dl: root %s free=%lluMB reserve=%lluMB",
                     root, (unsigned long long)(rep.free_bytes >> 20),
                     (unsigned long long)(rep.reserve_bytes >> 20));
        dl_plat_log(b);
    }

    LOCK();
    memset(s_slots, 0, sizeof(s_slots));
    s_next_seq = 1;
    static char ids[DL_MAX_ITEMS][DL_ID_MAX];
    int n = dl_store_list_ids(ids, DL_MAX_ITEMS);
    for (int i = 0; i < n; i++) restore_one(ids[i]);
    // Rebuilt records have no queue position yet: put them after the rest,
    // then persist whatever restore changed.
    for (int i = 0; i < DL_MAX_ITEMS; i++) {
        Slot *s = &s_slots[i];
        if (!s->used) continue;
        if (s->rec.seq == 0) s->rec.seq = s_next_seq++;
        dl_store_save_record(&s->rec);
    }
    s_ready = true;
    UNLOCK();
    {
        char b[64];
        snprintf(b, sizeof(b), "dl: restored %d item(s) from %s", n, root);
        dl_plat_log(b);
    }
    return true;
}

void dl_manager_shutdown(void) {
    LOCK();
    s_ready = false;
    UNLOCK();
}

// -------------------------------------------------------------------------
// Control API
// -------------------------------------------------------------------------

DlResult dl_enqueue(const DlMeta *meta, const char *url, uint64_t size_hint,
                    const DlExtras *extras) {
    if (!s_ready) return DL_E_NOT_READY;
    if (!meta || !url || !dl_id_valid(meta->id) || !meta->title[0])
        return DL_E_INVALID;
    DlUrl u;
    if (strlen(url) >= DL_URL_MAX || !dl_url_parse(url, &u)) return DL_E_INVALID;
    if (extras) {
        const char *arts[2] = { extras->poster_url, extras->backdrop_url };
        for (int i = 0; i < 2; i++)
            if (arts[i] && arts[i][0] &&
                (strlen(arts[i]) >= DL_ART_URL_MAX || !dl_url_parse(arts[i], &u)))
                return DL_E_INVALID;
        if (extras->container && strlen(extras->container) >= sizeof(((DlRecord *)0)->container))
            return DL_E_INVALID;
    }

    DlResult res = DL_OK;
    LOCK();
    Slot *s = find_slot(meta->id);
    bool requeue = false;
    if (s) {
        if (s->rec.state != DL_FAILED && s->rec.state != DL_CANCELLED) {
            UNLOCK();
            return DL_E_EXISTS;
        }
        requeue = true;
    } else if (!(s = free_slot())) {
        UNLOCK();
        return DL_E_FULL;
    }
    if (requeue && s->rec.error == DL_ERR_CORRUPT) {
        // Its record was lost, so nothing vouches for the partial's bytes
        // (which file, which quality, which offset): start clean.
        dl_store_remove_partial(meta->id);
        s->rec.bytes_done = 0;
        s->rec.bytes_total = 0;
    }
    uint64_t have = requeue ? s->rec.bytes_done : 0;
    const uint64_t estimate = size_hint ? size_hint
                            : (extras ? extras->estimate_bytes : 0);
    uint64_t need = estimate > have ? estimate - have : 0;
    if (need == 0) need = s_cfg.space_check_bytes;   // size unknown: at least a chunk
    {
        DlSpaceReport rep;
        space_eval(need, committed_locked(s), &rep);
        s_last_report = rep;
        space_log(&rep, rep.state == DL_SPACE_OK ? "ok"
                      : rep.state == DL_SPACE_LOW ? "refused" : "unknown");
        if (rep.state == DL_SPACE_UNKNOWN) { UNLOCK(); return DL_E_SPACE_UNKNOWN; }
        if (rep.state == DL_SPACE_LOW)     { UNLOCK(); return DL_E_NO_SPACE; }
    }

    if (!dl_store_make_item_dir(meta->id) || !dl_store_save_meta(meta)) {
        if (!requeue) dl_store_remove_item(meta->id);
        UNLOCK();
        return DL_E_IO;
    }
    DlRecord r;
    if (requeue) {
        r = s->rec;
        apply(&r, DL_EV_RETRY);
    } else {
        dl_record_init(&r);
        snprintf(r.id, sizeof(r.id), "%s", meta->id);
        r.seq = s_next_seq++;
    }
    snprintf(r.title, sizeof(r.title), "%s", meta->title);
    snprintf(r.url, sizeof(r.url), "%s", url);
    r.state = DL_QUEUED;
    r.error = DL_ERR_NONE;
    r.http_status = 0;
    r.attempts = 0;
    if (!requeue) r.bytes_total = size_hint;   // refined by the response
    r.est_bytes = estimate;
    snprintf(r.container, sizeof(r.container), "%s",
             extras && extras->container ? extras->container : "");
    r.expect_secs = extras ? extras->expect_secs : 0;
    snprintf(r.poster_url, sizeof(r.poster_url), "%s",
             extras && extras->poster_url ? extras->poster_url : "");
    snprintf(r.backdrop_url, sizeof(r.backdrop_url), "%s",
             extras && extras->backdrop_url ? extras->backdrop_url : "");
    r.art_tries = 0;
    if (!dl_store_save_record(&r)) {
        if (!requeue) dl_store_remove_item(meta->id);
        res = DL_E_IO;
    } else {
        if (!requeue) memset(s, 0, sizeof(*s));
        s->used = true;
        s->rec = r;
        s->retry_at_ms = 0;
        logf_id(r.id, requeue ? "re-queued" : "queued");
    }
    UNLOCK();
    return res;
}

// Shared body of pause/resume/cancel/retry.  On an idle item it is a plain
// transition; on the active one, ctl_if_active is posted to the worker.
static DlResult transition(const char *id, DlEvent ev, int ctl_if_active) {
    if (!s_ready) return DL_E_NOT_READY;
    LOCK();
    Slot *s = find_slot(id);
    if (!s) { UNLOCK(); return DL_E_NOT_FOUND; }
    if (s->active) {
        DlResult r = DL_E_STATE;
        if (ctl_if_active) {
            // Posted requests only ever escalate (pause < cancel < remove):
            // a pause pressed after a cancel must not rescue the download.
            if (ctl_if_active > s->ctl) s->ctl = ctl_if_active;
            r = DL_OK;
        } else if (ev == DL_EV_RESUME && s->ctl == CTL_PAUSE) {
            s->ctl = CTL_NONE;   // resumed before the pause landed
            r = DL_OK;
        }
        UNLOCK();
        return r;
    }
    DlRecord r = s->rec;
    if (!apply(&r, ev)) { UNLOCK(); return DL_E_STATE; }
    if (ev == DL_EV_RETRY && !r.url[0]) {
        // A record rebuilt from damaged files has no request to repeat;
        // it has to be downloaded again from the item page.
        UNLOCK();
        return DL_E_STATE;
    }
    if (ev == DL_EV_RESUME || ev == DL_EV_RETRY) {
        r.error = DL_ERR_NONE;
        r.attempts = 0;
        s->retry_at_ms = 0;
    }
    if (ev == DL_EV_CANCEL) {
        dl_store_remove_partial(id);
        r.bytes_done = 0;
        r.error = DL_ERR_NONE;
    }
    s->rec = r;
    dl_store_save_record(&r);
    UNLOCK();
    return DL_OK;
}

DlResult dl_pause(const char *id)  { return transition(id, DL_EV_PAUSE,  CTL_PAUSE); }
DlResult dl_resume(const char *id) { return transition(id, DL_EV_RESUME, CTL_NONE); }
DlResult dl_cancel(const char *id) { return transition(id, DL_EV_CANCEL, CTL_CANCEL); }
DlResult dl_retry(const char *id)  { return transition(id, DL_EV_RETRY,  CTL_NONE); }

DlResult dl_remove(const char *id) {
    if (!s_ready) return DL_E_NOT_READY;
    LOCK();
    Slot *s = find_slot(id);
    if (!s) { UNLOCK(); return DL_E_NOT_FOUND; }
    if (s->active) {
        // The worker deletes it when the transfer stops; hide it now.
        s->ctl = CTL_REMOVE;
        s->removing = true;
        UNLOCK();
        return DL_OK;
    }
    bool gone = dl_store_remove_item(id);
    s->used = false;
    UNLOCK();
    logf_id(id, gone ? "removed" : "removed (directory left behind)");
    return DL_OK;
}

// -------------------------------------------------------------------------
// Queries
// -------------------------------------------------------------------------

static void fill_status(const Slot *s, uint64_t now, DlStatus *out) {
    out->rec = s->rec;
    out->active = s->active;
    out->retry_in_ms = (s->rec.state == DL_QUEUED && s->retry_at_ms > now)
                           ? (uint32_t)(s->retry_at_ms - now) : 0;
}

int dl_list(DlStatus *out, int max, bool completed_only) {
    if (!s_ready) return 0;
    const uint64_t now = dl_plat_now_ms();
    LOCK();
    // Selection by increasing seq; 64 items makes the quadratic walk free.
    int n = 0;
    uint32_t last = 0;
    while (n < max) {
        const Slot *best = NULL;
        for (int i = 0; i < DL_MAX_ITEMS; i++) {
            const Slot *s = &s_slots[i];
            if (!s->used || s->removing || s->rec.seq <= last) continue;
            if (completed_only && s->rec.state != DL_COMPLETED) continue;
            if (!best || s->rec.seq < best->rec.seq) best = s;
        }
        if (!best) break;
        fill_status(best, now, &out[n++]);
        last = best->rec.seq;
    }
    UNLOCK();
    return n;
}

bool dl_find(const char *id, DlStatus *out) {
    if (!s_ready) return false;
    const uint64_t now = dl_plat_now_ms();
    LOCK();
    Slot *s = find_slot(id);
    if (s) fill_status(s, now, out);
    UNLOCK();
    return s != NULL;
}

static int ids_in_order(char (*ids)[DL_ID_MAX], int max, bool completed_only);

int dl_ids(char (*ids)[DL_ID_MAX], int max) { return ids_in_order(ids, max, false); }
int dl_completed_ids(char (*ids)[DL_ID_MAX], int max) { return ids_in_order(ids, max, true); }

void dl_counts(int *active, int *completed, int *failed) {
    int a = 0, c = 0, f = 0;
    if (s_ready) {
        LOCK();
        for (int i = 0; i < DL_MAX_ITEMS; i++) {
            const Slot *s = &s_slots[i];
            if (!s->used || s->removing) continue;
            switch (s->rec.state) {
            case DL_QUEUED: case DL_DOWNLOADING: case DL_PAUSED: a++; break;
            case DL_COMPLETED: c++; break;
            case DL_FAILED: f++; break;
            default: break;
            }
        }
        UNLOCK();
    }
    if (active) *active = a;
    if (completed) *completed = c;
    if (failed) *failed = f;
}

static int ids_in_order(char (*ids)[DL_ID_MAX], int max, bool completed_only) {
    if (!s_ready) return 0;
    LOCK();
    int n = 0;
    uint32_t last = 0;
    while (n < max) {
        const Slot *best = NULL;
        for (int i = 0; i < DL_MAX_ITEMS; i++) {
            const Slot *s = &s_slots[i];
            if (!s->used || s->removing || s->rec.seq <= last ||
                (completed_only && s->rec.state != DL_COMPLETED))
                continue;
            if (!best || s->rec.seq < best->rec.seq) best = s;
        }
        if (!best) break;
        snprintf(ids[n++], DL_ID_MAX, "%s", best->rec.id);
        last = best->rec.seq;
    }
    UNLOCK();
    return n;
}

bool dl_load_meta(const char *id, DlMeta *out) {
    if (!s_ready) return false;
    LOCK();
    bool ok = find_slot(id) && dl_store_load_meta(id, out);
    UNLOCK();
    return ok;
}

bool dl_media_path(const char *id, char *out, int cap) {
    if (!s_ready) return false;
    LOCK();
    Slot *s = find_slot(id);
    bool ok = s && s->rec.state == DL_COMPLETED &&
              dl_store_item_file(out, cap, id, DL_FILE_MEDIA);
    UNLOCK();
    return ok;
}

uint32_t dl_next_wake_ms(void) {
    uint32_t wake = 1000;
    if (!s_ready) return wake;
    const uint64_t now = dl_plat_now_ms();
    LOCK();
    for (int i = 0; i < DL_MAX_ITEMS; i++) {
        const Slot *s = &s_slots[i];
        if (!s->used || s->removing || s->rec.state != DL_QUEUED) continue;
        if (s->retry_at_ms <= now) { wake = 0; break; }
        uint64_t d = s->retry_at_ms - now;
        if (d < wake) wake = (uint32_t)d;
    }
    UNLOCK();
    return wake;
}

// -------------------------------------------------------------------------
// The transfer
// -------------------------------------------------------------------------

typedef enum { OUT_DONE, OUT_CTL, OUT_SUSPENDED, OUT_FAIL, OUT_SPACE } OutKind;

typedef struct {
    OutKind  kind;
    DlError  err;
    int      http_status;
    bool     progressed;   // bytes moved this attempt
} Outcome;

static Outcome fail(DlError e, int status = 0) {
    Outcome o = { OUT_FAIL, e, status, false };
    return o;
}

// May a transfer write on?  `remaining` is what it still has to fetch (0 =
// unknown).  Decided against the reserve plus a write margin, logged either
// way.  On refusal the item pauses; the queue is held until space returns.
static bool space_gate(uint64_t remaining, DlError *why) {
    uint64_t margin = write_margin();
    if (remaining && remaining < margin) margin = remaining;
    DlSpaceReport rep;
    space_eval(margin, 0, &rep);
    space_log(&rep, rep.state == DL_SPACE_OK ? "ok"
                  : rep.state == DL_SPACE_LOW ? "paused" : "unknown");
    if (rep.state == DL_SPACE_OK) return true;
    s_space_hold = true;
    *why = rep.state == DL_SPACE_LOW ? DL_ERR_NO_SPACE : DL_ERR_SPACE_UNKNOWN;
    return false;
}

static Outcome space_pause(DlError why, int status) {
    Outcome o = { OUT_SPACE, why, status, false };
    return o;
}

// Posted control, or a suspension, that should stop the attempt now.
static bool stop_requested(const Slot *s, Outcome *o) {
    if (s->ctl != CTL_NONE) { o->kind = OUT_CTL; return true; }
    if (s_suspended || s_play_block) { o->kind = OUT_SUSPENDED; return true; }
    return false;
}

// Payload waiting in s_buf[0..*fill) goes to disk.
static DlTsScan s_ts;            // the scan of the current attempt's bytes

static DlError flush_buf(int fh, int *fill, uint64_t *on_disk) {
    if (*fill == 0) return DL_ERR_NONE;
    dl_ts_feed(&s_ts, s_buf, *fill);   // exactly the bytes that go to disk
    if (dl_plat_file_write(fh, s_buf, *fill) != *fill) {
        DlSpaceReport rep;
        space_eval(1, 0, &rep);
        space_log(&rep, rep.state == DL_SPACE_OK ? "write failed" : "paused");
        return rep.state == DL_SPACE_LOW     ? DL_ERR_NO_SPACE
             : rep.state == DL_SPACE_UNKNOWN ? DL_ERR_SPACE_UNKNOWN
                                             : DL_ERR_DISK;
    }
    *on_disk += (uint64_t)*fill;
    *fill = 0;
    return DL_ERR_NONE;
}

// Publish progress to the table (always) and to disk (when asked).
static void publish(Slot *s, const DlRecord *r, bool persist) {
    LOCK();
    s->rec.bytes_done  = r->bytes_done;
    s->rec.bytes_total = r->bytes_total;
    s->rec.resumable   = r->resumable;
    s->rec.http_status = r->http_status;
    if (persist) dl_store_save_record(&s->rec);
    UNLOCK();
}

// -------------------------------------------------------------------------
// Artwork: small whole-body GETs, best effort
// -------------------------------------------------------------------------

// GET a small resource whole into s_buf (the worker's buffer; the media
// transfer has not started yet).  Returns its length, or -1 with *status
// set to the HTTP status (0 = no response).  Bounded by the same deadlines
// as the media transfer, and abandoned as soon as a stop is posted.
static int fetch_small(Slot *s, const char *url, int *status, bool *is_image) {
    *status = 0;
    *is_image = false;
    DlUrl u;
    if (!dl_url_parse(url, &u)) return -1;
    char auth[sizeof(s_auth)];
    LOCK();
    snprintf(auth, sizeof(auth), "%s", s_auth);
    UNLOCK();
    char req[DL_ART_URL_MAX + 1024];
    int rlen = dl_http_build_get(req, sizeof(req), &u, 0, auth[0] ? auth : NULL);
    if (rlen < 0) return -1;
    int h = dl_plat_connect(u.host, u.port);
    if (h < 0) return -1;
    if (dl_plat_send(h, req, rlen) != rlen) { dl_plat_close(h); return -1; }

    Outcome o;
    static DlHeadReader hr;   // worker thread only
    dl_head_reader_init(&hr);
    int fill = 0;
    uint64_t t0 = dl_plat_now_ms(), last_rx = t0;
    while (!hr.done) {
        if (stop_requested(s, &o)) { dl_plat_close(h); return -1; }
        int n = dl_plat_recv(h, s_buf, DL_XFER_BUF);
        if (n > 0) {
            int used = dl_head_reader_feed(&hr, s_buf, n);
            if (used < 0) { dl_plat_close(h); return -1; }
            fill = n - used;
            if (fill > 0) memmove(s_buf, s_buf + used, (size_t)fill);
            continue;
        }
        if (n == DL_RECV_TIMEOUT && dl_plat_now_ms() - t0 < s_cfg.head_timeout_ms)
            continue;
        dl_plat_close(h);
        return -1;
    }
    DlHttpHead hd;
    if (!dl_http_parse_head(hr.buf, hr.n, &hd)) { dl_plat_close(h); return -1; }
    *status = hd.status;
    *is_image = hd.content_type[0] == '\0' ||
                strncmp(hd.content_type, "image/", 6) == 0;
    if (hd.status != 200) { dl_plat_close(h); return -1; }

    DlChunked ch;
    dl_chunked_init(&ch);
    int got = 0;   // decoded payload at s_buf[0..got)
    int n = fill;  // undecoded bytes at s_buf[got..got+n)
    for (;;) {
        if (n > 0) {
            last_rx = dl_plat_now_ms();
            if (hd.chunked) {
                n = dl_chunked_decode(&ch, s_buf + got, n, s_buf + got);
                if (n < 0) { dl_plat_close(h); return -1; }
            }
            got += n;
            n = 0;
        }
        if ((hd.chunked && ch.done) ||
            (hd.content_length >= 0 && got >= hd.content_length)) break;
        if (got >= DL_XFER_BUF) { dl_plat_close(h); return -1; }   // too big
        if (stop_requested(s, &o)) { dl_plat_close(h); return -1; }
        n = dl_plat_recv(h, s_buf + got, DL_XFER_BUF - got);
        if (n > 0) continue;
        if (n == DL_RECV_TIMEOUT) {
            n = 0;
            if (dl_plat_now_ms() - last_rx < s_cfg.idle_timeout_ms) continue;
            dl_plat_close(h);
            return -1;
        }
        dl_plat_close(h);
        if (n == DL_RECV_CLOSED && !hd.chunked && hd.content_length < 0) break;
        return -1;       // cut short
    }
    dl_plat_close(h);
    if (hd.content_length >= 0 && got > hd.content_length) got = (int)hd.content_length;
    return got;
}

static bool looks_like_image(const uint8_t *p, int n) {
    if (n >= 3 && p[0] == 0xFF && p[1] == 0xD8 && p[2] == 0xFF) return true;   // JPEG
    if (n >= 8 && memcmp(p, "\x89PNG\r\n\x1a\n", 8) == 0) return true;      // PNG
    return false;
}

// Fetch whatever artwork is still wanted.  Never fails the download: a
// missing poster is a cosmetic loss, a missing film is not.  Each image is
// fetched once -- an existing file is never re-requested -- and a failed
// fetch is retried on later attempts only up to DL_ART_MAX_TRIES in total.
static void fetch_artwork(Slot *s, DlRecord *r) {
    struct { char *url; const char *leaf; bool poster; } art[2] = {
        { r->poster_url,   DL_FILE_POSTER,   true  },
        { r->backdrop_url, DL_FILE_BACKDROP, false },
    };
    bool changed = false;
    for (int i = 0; i < 2; i++) {
        if (!art[i].url[0]) continue;
        char path[DL_PATH_MAX];
        dl_store_item_file(path, sizeof(path), r->id, art[i].leaf);
        bool stored = dl_plat_file_size(path) > 0;
        if (!stored) {
            int status = 0;
            bool is_image = false;
            int len = fetch_small(s, art[i].url, &status, &is_image);
            if (len > 0 && is_image && looks_like_image(s_buf, len) &&
                dl_store_save_blob(r->id, art[i].leaf, s_buf, len)) {
                stored = true;
                char b[64];
                snprintf(b, sizeof(b), "%s saved, %d bytes", art[i].leaf, len);
                logf_id(r->id, b);
            } else if (status == 404 || status == 410) {
                art[i].url[0] = '\0';          // the server has none: done
                changed = true;
                continue;
            } else {
                if (s->ctl != CTL_NONE || s_suspended || s_play_block) return;
                r->art_tries++;
                changed = true;
                logf_id(r->id, "artwork fetch failed (non-fatal)");
            }
        }
        if (stored) {
            // Record it in the metadata only once the file is really there.
            LOCK();
            static DlMeta m;   // lock held
            if (dl_store_load_meta(r->id, &m)) {
                snprintf(art[i].poster ? m.poster : m.backdrop, sizeof(m.poster),
                         "%s", art[i].leaf);
                dl_store_save_meta(&m);
            }
            UNLOCK();
            art[i].url[0] = '\0';
            changed = true;
        }
    }
    if (r->art_tries >= DL_ART_MAX_TRIES && (r->poster_url[0] || r->backdrop_url[0])) {
        logf_id(r->id, "artwork given up");
        r->poster_url[0] = r->backdrop_url[0] = '\0';
        changed = true;
    }
    if (changed) {
        LOCK();
        snprintf(s->rec.poster_url, sizeof(s->rec.poster_url), "%s", r->poster_url);
        snprintf(s->rec.backdrop_url, sizeof(s->rec.backdrop_url), "%s", r->backdrop_url);
        s->rec.art_tries = r->art_tries;
        dl_store_save_record(&s->rec);
        UNLOCK();
    }
}

static Outcome attempt(Slot *s, DlRecord *r) {
    char part[DL_PATH_MAX], media[DL_PATH_MAX];
    dl_store_item_file(part,  sizeof(part),  r->id, DL_FILE_PART);
    dl_store_item_file(media, sizeof(media), r->id, DL_FILE_MEDIA);

    // The partial on disk is the resume point, whatever the record says.
    int64_t psz = dl_plat_file_size(part);
    uint64_t have = psz > 0 ? (uint64_t)psz : 0;
    if (r->bytes_total > 0 && have > r->bytes_total) {
        dl_plat_truncate(part);
        have = 0;
    }
    const uint64_t have_at_start = have;
    r->bytes_done = have;
    // The TS scan is only meaningful for a file it saw from byte 0; set when
    // the body starts writing at offset 0 (a fresh or restarted transfer).
    dl_ts_init(&s_ts);
    bool ts_from_zero = false;
    const bool is_ts = strcmp(r->container, "ts") == 0;

    {
        DlError why;
        const uint64_t total = r->bytes_total > r->est_bytes ? r->bytes_total : r->est_bytes;
        if (!space_gate(total > have ? total - have : 0, &why))
            return space_pause(why, 0);
    }

    DlUrl u;
    if (!dl_url_parse(r->url, &u)) return fail(DL_ERR_UNSUPPORTED);

    // Artwork first, while the item is being set up: small, best effort,
    // and the Downloads list can show the poster while the film arrives.
    if (r->poster_url[0] || r->backdrop_url[0]) fetch_artwork(s, r);

    char auth[sizeof(s_auth)];
    LOCK();
    snprintf(auth, sizeof(auth), "%s", s_auth);
    UNLOCK();

    char req[DL_URL_MAX + 1024];
    int rlen = dl_http_build_get(req, sizeof(req), &u, have,
                                 auth[0] ? auth : NULL);
    if (rlen < 0) return fail(DL_ERR_UNSUPPORTED);

    int h = dl_plat_connect(u.host, u.port);
    if (h < 0) return fail(DL_ERR_UNREACHABLE);
    if (dl_plat_send(h, req, rlen) != rlen) {
        dl_plat_close(h);
        return fail(DL_ERR_NETWORK);
    }

    // ---- response head ----
    Outcome o = { OUT_FAIL, DL_ERR_NONE, 0, false };
    static DlHeadReader hr;   // 8 KB; only the worker thread gets here
    dl_head_reader_init(&hr);
    int body_off = 0, body_n = 0;
    uint64_t t0 = dl_plat_now_ms();
    while (!hr.done) {
        if (stop_requested(s, &o)) { dl_plat_close(h); return o; }
        int n = dl_plat_recv(h, s_buf, DL_XFER_BUF);
        if (n > 0) {
            int used = dl_head_reader_feed(&hr, s_buf, n);
            if (used < 0) { dl_plat_close(h); return fail(DL_ERR_BAD_RESPONSE); }
            body_off = used;
            body_n = n - used;
            continue;
        }
        if (n == DL_RECV_TIMEOUT) {
            if (dl_plat_now_ms() - t0 >= s_cfg.head_timeout_ms) {
                dl_plat_close(h);
                return fail(DL_ERR_TIMEOUT);
            }
            continue;
        }
        dl_plat_close(h);   // closed or reset before a complete head
        return fail(n == DL_RECV_CLOSED && hr.n == 0 ? DL_ERR_NETWORK
                                                     : DL_ERR_BAD_RESPONSE);
    }
    DlHttpHead hd;
    if (!dl_http_parse_head(hr.buf, hr.n, &hd)) {
        dl_plat_close(h);
        return fail(DL_ERR_BAD_RESPONSE);
    }
    r->http_status = hd.status;

    // ---- what the server did with the range ----
    bool restart = false;          // discard the partial and write from 0
    if (hd.status == 416) {
        dl_plat_close(h);
        if (hd.range_unsatisfied && hd.range_total >= 0 && have > 0 &&
            have == (uint64_t)hd.range_total) {
            r->bytes_total = have;   // nothing left to fetch
            goto verify;
        }
        dl_plat_truncate(part);
        r->bytes_done = 0;
        return fail(DL_ERR_BAD_RESPONSE, 416);
    }
    if (hd.status != 200 && hd.status != 206) {
        dl_plat_close(h);
        return fail(dl_error_for_http_status(hd.status), hd.status);
    }
    if (dl_http_content_type_is_error_page(hd.content_type)) {
        dl_plat_close(h);
        return fail(DL_ERR_BAD_RESPONSE, hd.status);
    }
    if (hd.status == 206) {
        const bool consistent =
            hd.has_range && !hd.range_unsatisfied && hd.range_start == have &&
            (r->bytes_total == 0 || hd.range_total < 0 ||
             (uint64_t)hd.range_total == r->bytes_total);
        if (!consistent) {
            // Not the bytes we asked for, or the file changed size under us:
            // appending would splice two different files together.
            dl_plat_close(h);
            dl_plat_truncate(part);
            r->bytes_done = 0;
            return fail(DL_ERR_BAD_RESPONSE, 206);
        }
        r->resumable = 1;
        if (hd.range_total >= 0) r->bytes_total = (uint64_t)hd.range_total;
        else if (hd.content_length >= 0)
            r->bytes_total = have + (uint64_t)hd.content_length;
    } else {
        // 200: a full body from byte 0, whatever we asked for.
        restart = have > 0;
        r->resumable = hd.accept_ranges ? 1 : 0;
        r->bytes_total = hd.content_length >= 0 ? (uint64_t)hd.content_length : 0;
    }
    if (restart) {
        logf_id(r->id, "server ignored Range; starting over");
        if (!dl_plat_truncate(part)) { dl_plat_close(h); return fail(DL_ERR_DISK); }
        have = 0;
        r->bytes_done = 0;
    }
    {
        DlError why;
        if (!space_gate(r->bytes_total > have ? r->bytes_total - have : 0, &why)) {
            dl_plat_close(h);
            return space_pause(why, hd.status);
        }
    }

    {
        // ---- body ----
        int fh = dl_plat_file_open_append(part);
        if (fh < 0) { dl_plat_close(h); return fail(DL_ERR_DISK, hd.status); }

        DlChunked ch;
        dl_chunked_init(&ch);
        const int64_t cl = hd.content_length;   // -1 when chunked/unknown
        uint64_t body_got = 0;                  // payload of this response
        uint64_t on_disk  = have;               // have, minus what is in s_buf
        int      fill     = 0;                  // payload waiting in s_buf
        uint64_t last_ckpt_bytes = have, last_space_bytes = have;
        const uint64_t t_start = dl_plat_now_ms();
        uint64_t last_ckpt_ms = t_start, last_rx_ms = t_start, last_pub_ms = t_start;
        ts_from_zero = (have == 0);
        uint32_t pace_bps = 0;                  // rate the pacing is anchored to
        uint64_t pace_t0 = 0, pace_b0 = 0;
        bool finished = false, stopped = false;
        DlError err = DL_ERR_NONE;
        publish(s, r, true);

        // Body bytes that arrived with the head are the first data.
        int n = body_n;
        if (n > 0) memmove(s_buf, s_buf + body_off, (size_t)n);
        for (;;) {
            if (n > 0) {
                uint8_t *data = s_buf + fill;
                last_rx_ms = dl_plat_now_ms();
                if (hd.chunked) {
                    n = dl_chunked_decode(&ch, data, n, data);   // in place
                    if (n < 0) { err = DL_ERR_BAD_RESPONSE; break; }
                } else if (cl >= 0 && body_got + (uint64_t)n > (uint64_t)cl) {
                    n = (int)((uint64_t)cl - body_got);   // ignore trailing junk
                }
                if (r->bytes_total > 0 && have + (uint64_t)n > r->bytes_total) {
                    err = DL_ERR_BAD_RESPONSE;             // more than declared
                    break;
                }
                fill     += n;
                have     += (uint64_t)n;
                body_got += (uint64_t)n;
                r->bytes_done = have;
            }
            const uint64_t now = dl_plat_now_ms();
            const bool done_now = (hd.chunked && ch.done) ||
                                  (cl >= 0 && body_got >= (uint64_t)cl);
            const bool ckpt = have - last_ckpt_bytes >= s_cfg.checkpoint_bytes ||
                              now - last_ckpt_ms >= s_cfg.checkpoint_ms;
            // Write when the tail can no longer take a full read, at every
            // checkpoint (the record must never claim bytes that are only in
            // RAM), and at the end.
            if (fill > 0 && (DL_XFER_BUF - fill < DL_RECV_MIN || ckpt || done_now)) {
                err = flush_buf(fh, &fill, &on_disk);
                if (err != DL_ERR_NONE) break;
            }
            if (ckpt) {
                dl_plat_file_sync(fh);
                last_ckpt_bytes = have;
                last_ckpt_ms = now;
            }
            if (ckpt || now - last_pub_ms >= s_cfg.progress_ms) {
                publish(s, r, ckpt);
                last_pub_ms = now;
            }
            if (have - last_space_bytes >= s_cfg.space_check_bytes) {
                last_space_bytes = have;
                DlError why;
                if (!space_gate(r->bytes_total > have ? r->bytes_total - have : 0, &why)) {
                    err = why;
                    break;
                }
            }
            if (done_now) { finished = true; break; }
            if (stop_requested(s, &o)) { stopped = true; break; }

            // Beside a light stream, hold the average to its share.  Sleeping
            // here lets the socket buffer fill, and TCP flow control slows the
            // server for us -- no data is dropped.
            const uint32_t rate = s_play_light ? s_cfg.stream_share_bps : 0;
            if (rate != pace_bps) { pace_bps = rate; pace_t0 = now; pace_b0 = have; }
            if (pace_bps) {
                for (;;) {
                    const uint64_t t = dl_plat_now_ms();
                    const uint64_t allowed = pace_b0 + (t - pace_t0) * pace_bps / 8000;
                    if (have <= allowed) break;
                    uint64_t wait = (have - allowed) * 8000 / pace_bps + 1;
                    dl_plat_sleep_ms((unsigned)(wait > 100 ? 100 : wait));
                    if (stop_requested(s, &o)) { stopped = true; break; }
                    if ((s_play_light ? s_cfg.stream_share_bps : 0) != pace_bps) break;
                }
                if (stopped) break;
                last_rx_ms = dl_plat_now_ms();   // waiting on ourselves is not idle
            }

            n = dl_plat_recv(h, s_buf + fill, DL_XFER_BUF - fill);
            if (n > 0) continue;
            if (n == DL_RECV_TIMEOUT) {
                n = 0;
                if (dl_plat_now_ms() - last_rx_ms >= s_cfg.idle_timeout_ms) {
                    err = DL_ERR_TIMEOUT;
                    break;
                }
                continue;
            }
            if (n == DL_RECV_CLOSED) {
                // Only a body with no declared length ends on close.
                if (!hd.chunked && cl < 0) finished = true;
                else err = DL_ERR_PARTIAL;
            } else {
                err = DL_ERR_NETWORK;
            }
            n = 0;
            break;
        }
        // Keep whatever arrived, whatever ended the attempt: the partial on
        // disk is what the next attempt resumes from.  (Not after a failed
        // write -- the disk is what failed.)
        if (fill > 0 && err != DL_ERR_DISK && err != DL_ERR_NO_SPACE &&
            err != DL_ERR_SPACE_UNKNOWN) {
            DlError werr = flush_buf(fh, &fill, &on_disk);
            if (werr != DL_ERR_NONE && !stopped) { err = werr; finished = false; }
        }
        dl_plat_file_sync(fh);
        dl_plat_file_close(fh);
        dl_plat_close(h);
        have = on_disk;
        r->bytes_done = have;
        o.progressed = have > have_at_start;
        o.http_status = hd.status;

        if (stopped) return o;
        if (!finished) {
            o.kind = (err == DL_ERR_NO_SPACE || err == DL_ERR_SPACE_UNKNOWN)
                         ? OUT_SPACE : OUT_FAIL;
            o.err = err;
            return o;
        }
        if (r->bytes_total == 0) r->bytes_total = have;
    }

verify:
    if (have == 0 || have != r->bytes_total) {
        Outcome f = fail(have == 0 ? DL_ERR_BAD_RESPONSE : DL_ERR_PARTIAL,
                         r->http_status);
        f.progressed = have > have_at_start;
        return f;
    }
    if (is_ts) {
        // Length says all the promised bytes came.  Is it the film?  Size must
        // be whole packets; and when the scan saw the whole file, every packet
        // must be in sync and the video must cover (nearly) the runtime.
        bool short_media = false;
        const bool ok = ts_from_zero
            ? dl_ts_plausible(&s_ts, have, r->expect_secs, &short_media)
            : (have % DL_TS_PACKET) == 0;
        if (!ok) {
            char b[96];
            snprintf(b, sizeof(b), "not a complete TS: %s (span %llds of %us, %llu sync errors)",
                     short_media ? "ended early" : "malformed",
                     (long long)dl_ts_span_secs(&s_ts), (unsigned)r->expect_secs,
                     (unsigned long long)s_ts.sync_errors);
            logf_id(r->id, b);
            // Never resumable: whatever the next attempt gets must start over.
            dl_plat_truncate(part);
            r->bytes_done = 0;
            Outcome f = fail(DL_ERR_BAD_MEDIA, r->http_status);
            return f;
        }
    }
    dl_plat_remove(media);
    if (!dl_plat_rename(part, media)) return fail(DL_ERR_DISK, r->http_status);
    r->bytes_done = have;
    Outcome done = { OUT_DONE, DL_ERR_NONE, r->http_status, have > have_at_start };
    return done;
}

static void finish(Slot *s, DlRecord *r, const Outcome *o) {
    const int ctl = s->ctl;
    s->active = false;
    s->ctl = CTL_NONE;

    if (ctl == CTL_REMOVE) {
        dl_store_remove_item(r->id);
        logf_id(r->id, "removed");
        s->used = false;
        s->removing = false;
        return;
    }
    char msg[96];
    if (o->kind == OUT_DONE) {
        // Completion is a fact; a pause or cancel racing it does not undo it.
        apply(r, DL_EV_COMPLETE);
        r->error = DL_ERR_NONE;
        r->attempts = 0;
        snprintf(msg, sizeof(msg), "completed, %llu bytes",
                 (unsigned long long)r->bytes_done);
    } else if (o->kind == OUT_CTL && ctl == CTL_CANCEL) {
        apply(r, DL_EV_CANCEL);
        dl_store_remove_partial(r->id);
        r->bytes_done = 0;
        r->error = DL_ERR_NONE;
        snprintf(msg, sizeof(msg), "cancelled");
    } else if (o->kind == OUT_CTL) {   // CTL_PAUSE
        apply(r, DL_EV_PAUSE);
        snprintf(msg, sizeof(msg), "paused at %llu",
                 (unsigned long long)r->bytes_done);
    } else if (o->kind == OUT_SPACE) {
        // Not a failure: the partial is kept and Resume carries on once the
        // user has freed space.  The reason rides in the record's error.
        apply(r, DL_EV_PAUSE);
        r->error = o->err;
        if (o->http_status) r->http_status = o->http_status;
        s->retry_at_ms = 0;
        snprintf(msg, sizeof(msg), "paused at %llu: %s",
                 (unsigned long long)r->bytes_done, dl_error_name(o->err));
    } else if (o->kind == OUT_SUSPENDED) {
        apply(r, DL_EV_INTERRUPTED);
        s->retry_at_ms = 0;
        snprintf(msg, sizeof(msg), "suspended at %llu",
                 (unsigned long long)r->bytes_done);
    } else {
        r->error = o->err;
        if (o->http_status) r->http_status = o->http_status;
        const bool auth = o->err == DL_ERR_AUTH;
        const uint32_t limit = o->err == DL_ERR_BAD_MEDIA ? DL_MEDIA_MAX_ATTEMPTS
                                                          : DL_MAX_ATTEMPTS;
        if (dl_error_retryable(o->err)) {
            // Progress resets the count: only attempts that move nothing
            // add up to giving up.  Not for bad media: a whole film that
            // failed validation is exactly the attempt not to repeat freely.
            r->attempts = (o->progressed && o->err != DL_ERR_BAD_MEDIA)
                              ? 1 : r->attempts + 1;
        }
        if (auth) {
            // The session died, not the download: park it and hold the whole
            // queue until someone signs in again (dl_set_auth_header).
            apply(r, DL_EV_FAIL_RETRY);
            s->retry_at_ms = 0;
            s_auth_hold = true;
            snprintf(msg, sizeof(msg), "sign-in expired (http 401): queue held");
        } else if (dl_error_retryable(o->err) && r->attempts < limit) {
            apply(r, DL_EV_FAIL_RETRY);
            uint32_t wait = dl_backoff_ms(r->attempts);
            s->retry_at_ms = dl_plat_now_ms() + wait;
            snprintf(msg, sizeof(msg), "%s (http %d), retry %u in %us",
                     dl_error_name(o->err), r->http_status,
                     (unsigned)r->attempts, (unsigned)(wait / 1000));
        } else {
            apply(r, DL_EV_FAIL);
            snprintf(msg, sizeof(msg), "failed: %s (http %d)",
                     dl_error_name(o->err), r->http_status);
        }
    }
    s->rec = *r;
    dl_store_save_record(r);
    logf_id(r->id, msg);
}

bool dl_manager_step(void) {
    if (!s_ready || s_suspended || s_play_block || s_auth_hold || !s_auth[0])
        return false;
    // Space is checked again before anything starts while the queue is held;
    // the hold lifts by itself once the HDD reads above the reserve again.
    if (s_space_hold) {
        DlSpaceReport rep;
        space_eval(write_margin(), 0, &rep);
        if (rep.state != DL_SPACE_OK) return false;
        s_space_hold = false;
        dl_plat_log("dl: space available again -- queue released");
    }
    const uint64_t now = dl_plat_now_ms();
    LOCK();
    Slot *pick = NULL;
    for (int i = 0; i < DL_MAX_ITEMS; i++) {
        Slot *s = &s_slots[i];
        if (!s->used || s->removing || s->rec.state != DL_QUEUED) continue;
        if (s->retry_at_ms > now) continue;
        if (!pick || s->rec.seq < pick->rec.seq) pick = s;
    }
    if (!pick) { UNLOCK(); return false; }
    apply(&pick->rec, DL_EV_START);
    pick->active = true;
    pick->ctl = CTL_NONE;
    dl_store_save_record(&pick->rec);
    DlRecord r = pick->rec;
    UNLOCK();

    Outcome o = attempt(pick, &r);

    LOCK();
    finish(pick, &r, &o);
    UNLOCK();
    return true;
}
