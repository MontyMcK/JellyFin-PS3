#pragma once
#include <stddef.h>   // NULL
#include <stdint.h>
#include "dl_model.h"

// -------------------------------------------------------------------------
//  Offline downloads -- the download manager
// -------------------------------------------------------------------------
//  One service, not a screen: the item page's DOWNLOAD FOR OFFLINE, the
//  Downloads list and the Offline library all talk to this, and none of them
//  owns a transfer.  It keeps the queue, runs one transfer at a time on the
//  caller's worker thread (dl_ps3.cpp on the console, the test itself on the
//  host), and persists every state change so a power cut loses nothing but
//  the bytes since the last checkpoint.
//
//  Threading: every public call is safe from any thread.  dl_manager_step()
//  must only ever be called from ONE thread (the worker); the transfer itself
//  runs there without holding the lock, and control calls on the active item
//  (pause/cancel/remove) are posted to it and take effect within about a
//  second -- the socket's receive timeout.
//
//  Transfers: a plain HTTP GET streamed to <item>/media.ts.part through one
//  fixed 512 KB buffer -- never the file in RAM -- written in large batches
//  rather than per network read (the pkgi-ps3 lesson; see dl_manager.cpp).  When bytes are already on
//  disk the request carries "Range: bytes=N-":
//    206 from N       -> appended; this is a real resume
//    200              -> the server ignored the range (a live transcode
//                        does); the partial is discarded and it starts over
//    206 not from N   -> inconsistent; discarded, retried from 0
//    416 */N with N bytes already here -> it was already complete
//  The file only becomes media.ts once the length is verified.
// -------------------------------------------------------------------------

typedef enum {
    DL_OK = 0,
    DL_E_NOT_READY,   // dl_manager_init() has not succeeded
    DL_E_INVALID,     // bad id / url / arguments
    DL_E_EXISTS,      // already downloaded or already in the queue
    DL_E_FULL,        // DL_MAX_ITEMS reached
    DL_E_NO_SPACE,    // the HDD cannot take it
    DL_E_IO,          // could not write the item's files
    DL_E_NOT_FOUND,   // no such item
    DL_E_STATE,       // not possible in the item's current state
    DL_E_SPACE_UNKNOWN, // free HDD space cannot be read: nothing may start
    DL_E_SIZE_UNKNOWN,  // neither a size nor a runtime: space cannot be checked
    DL_E_NO_DRIVE,      // the download location is a USB drive that is not there / not writable
} DlResult;

typedef struct {
    DlRecord rec;
    uint32_t retry_in_ms;   // > 0 while waiting out a backoff
    bool     active;        // the worker is transferring it right now
} DlStatus;

// Tunables.  The defaults are for the console; the host tests shrink them.
typedef struct {
    uint64_t reserve_bytes;      // HDD space never used by downloads; raised
                                 // to DL_RESERVE_MIN when lower, at init
    uint32_t head_timeout_ms;    // wait for response headers
    uint32_t idle_timeout_ms;    // body silence that counts as a timeout
    uint32_t checkpoint_ms;      // persist progress at least this often
    uint64_t checkpoint_bytes;   // ...or at least every this many bytes
    uint64_t space_check_bytes;  // re-check free space this often
    uint32_t progress_ms;        // in-memory progress published this often
    uint32_t stream_share_bps;   // download cap beside a light stream, bit/s
} DlConfig;

void dl_config_defaults(DlConfig *c);

// Opens (creating if needed) the store at root and restores every item:
// an item that was DOWNLOADING when the app stopped goes back to QUEUED with
// its partial intact; damaged records are recovered from what is on disk or
// surfaced as FAILED/corrupt -- never silently dropped, since they still
// hold HDD space.  Needs no network.  cfg may be NULL.
bool dl_manager_init(const char *root, const DlConfig *cfg);
void dl_manager_shutdown(void);
bool dl_manager_ready(void);

// The auth header sent with every transfer, as a complete header line
// without CRLF.  Kept in memory only: records never contain a token.
// Nothing transfers without one.  A 401 puts the queue on hold (the item goes
// back to QUEUED, not FAILED: its session died, not the download) until the
// next call here -- i.e. until the user signs in again.
void dl_set_auth_header(const char *line);
bool dl_auth_held(void);   // "sign in to continue downloads", for the UI

// Queue an item.  meta is saved beside the media and must carry at least id
// and title; url is the media request (http://, no token).  size_hint is the
// expected size in bytes if known (0 if not) and is checked against free
// space now, so an item that cannot fit is refused up front rather than
// failing half way.  A FAILED or CANCELLED item is re-queued with the new
// url/meta, keeping any partial data.
// Stage 3 extras, supplied by dl_request_build(): what completion must
// validate and the artwork to fetch alongside.  NULL = none (plain file).
typedef struct {
    const char *container;      // "ts": validate as MPEG-TS at completion
    uint32_t    expect_secs;    // runtime the TS must roughly cover; 0 = skip
    const char *poster_url;     // fetched once, best effort; NULL/"" = none
    const char *backdrop_url;
    uint64_t    estimate_bytes; // upper estimate of what the file will need;
                                // used for space accounting when size_hint
                                // (an exact size) is 0
} DlExtras;

DlResult dl_enqueue(const DlMeta *meta, const char *url, uint64_t size_hint,
                    const DlExtras *extras = NULL);

// -------------------------------------------------------------------------
//  HDD space
// -------------------------------------------------------------------------
//  At least reserve_bytes (never under DL_RESERVE_MIN) of the HDD stays free.
//  The rules, all fail-closed:
//    * free space that cannot be read, or reads as implausible, is UNKNOWN,
//      and nothing starts or continues while it is;
//    * an item is accepted only if its estimate plus what every queued,
//      active and paused item still has to fetch fits above the reserve;
//    * a transfer re-checks before every space_check_bytes it writes and on
//      every failed write, and stops short of the reserve rather than at it:
//      the item goes to PAUSED (error = DL_ERR_NO_SPACE / DL_ERR_SPACE_UNKNOWN)
//      with its partial kept, and no other item starts until space returns.
typedef enum { DL_SPACE_OK = 0, DL_SPACE_LOW, DL_SPACE_UNKNOWN } DlSpaceState;

typedef struct {
    DlSpaceState state;
    uint64_t free_bytes;       // 0 when unknown
    uint64_t reserve_bytes;
    uint64_t committed_bytes;  // still to fetch for items already queued
    uint64_t need_bytes;       // the new item(s) plus committed_bytes
    uint64_t avail_bytes;      // free less the reserve (0 below it)
} DlSpaceReport;

// Would new_bytes more fit right now?  A pure query: nothing is reserved.
DlSpaceReport dl_space_report(uint64_t new_bytes);
// The report behind the last dl_enqueue() refusal, for the message.
DlSpaceReport dl_last_space_report(void);
uint64_t dl_reserve_bytes(void);
// How many of est[0..n) fit, in order, above the reserve (0..n).  Pure given
// the report's avail/committed figures.
int dl_space_fit_prefix(const DlSpaceReport *rep, const uint64_t *est, int n);
// True while the manager is holding the whole queue because of space.
bool dl_space_held(void);
DlResult dl_pause(const char *id);
DlResult dl_resume(const char *id);
DlResult dl_cancel(const char *id);    // deletes partial data, keeps the entry
DlResult dl_retry(const char *id);
DlResult dl_remove(const char *id);    // deletes everything for the item

// Snapshots, in queue order.  completed_only gives the Offline library.
int  dl_list(DlStatus *out, int max, bool completed_only);
bool dl_find(const char *id, DlStatus *out);
// Ids of every item in queue order (the Downloads list reads its visible
// rows with dl_find, instead of snapshotting all ~2 KB statuses).
int  dl_ids(char (*ids)[DL_ID_MAX], int max);
// Cheap tallies for a label, no copies: queued/downloading/paused count as
// active; completed as records (dl_library verifies the files).
void dl_counts(int *active, int *completed, int *failed);
// Ids of COMPLETED items in queue order -- just the ids (the offline
// library, dl_library.h, verifies each one).
int  dl_completed_ids(char (*ids)[DL_ID_MAX], int max);
bool dl_load_meta(const char *id, DlMeta *out);
// Path of the finished media file; false unless the item is COMPLETED.
bool dl_media_path(const char *id, char *out, int cap);

// ---- worker side ----------------------------------------------------------
// While suspended nothing starts, and an active transfer stops at its next
// read and goes back to QUEUED with its data (not PAUSED: the user did not
// ask).  Used when the service stops; playback uses dl_playback_begin().
void dl_set_suspended(bool suspended);
// Playback.  The player calls dl_playback_begin() with the URL of every
// stream it opens (initial open, seeks, track changes) and dl_playback_end()
// when it stops.  Bandwidth goes to the stream:
//   heavy stream (anything above 480p, direct play, HD audio copy):
//     downloads STOP -- the active one goes back to QUEUED with its data and
//     nothing starts until playback ends;
//   light stream (dl_stream_is_light, dl_http.h: 480p/360p, no HD copy):
//     downloads continue, paced to DlConfig.stream_share_bps.
// Independent of dl_set_suspended: ending playback never lifts a suspension.
void dl_playback_begin(const char *stream_url);
void dl_playback_end(void);
// The same gate for a LOCAL file (offline playback, Stage 4): no URL to
// classify, so the caller passes the file's own weight (dl_meta_is_light).
void dl_playback_begin_local(bool light);
bool dl_playback_blocking(void);   // "held while streaming", for the UI

// Runs one transfer attempt on the next due item.  Blocks for the length of
// the attempt.  Returns false when nothing was due.
bool dl_manager_step(void);
// How long the worker may sleep before something becomes due (capped at
// 1000 ms so control changes are noticed).
uint32_t dl_next_wake_ms(void);
