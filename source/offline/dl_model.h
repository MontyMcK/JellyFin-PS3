#pragma once
#include <stdint.h>

// -------------------------------------------------------------------------
//  Offline downloads -- data model
// -------------------------------------------------------------------------
//  What one download IS, independent of where it is stored or how it is
//  fetched: its state, its bookkeeping record, its metadata, and the pure
//  rules that move it between states.  No I/O here, which is what lets every
//  rule be pinned by a host test (tests/test_offline.cpp).
//
//  Persisted records are plain `key=value` text, one file per item (see
//  dl_store.h for the layout).  Enums are written as NAMES, never digits:
//  the settings files in this app learned the hard way that a persisted digit
//  pins the enum's numbering forever.
// -------------------------------------------------------------------------

#define DL_MAX_ITEMS     64      // queue + library, hard cap

// HDD space downloads never touch: the console needs it to save games,
// install updates and keep its system caches.  A floor, not a default: the
// configured reserve can raise it and nothing can lower it.
#define DL_RESERVE_MIN   (10ull * 1024 * 1024 * 1024)
// The floor on a USB drive (a store whose root is an lfs path, "usb0:/..."):
// nothing of the console's lives there, so it only keeps the NTFS writer clear
// of a full volume, where a failed write would leave the drive marked for
// checking.  The same raise-only rules apply.
#define DL_RESERVE_MIN_USB (1ull * 1024 * 1024 * 1024)
#define DL_ID_MAX        64      // Jellyfin ids are 32 hex chars
#define DL_URL_MAX       1024
#define DL_TITLE_MAX     128
#define DL_ART_URL_MAX   384     // artwork request (no token)
// Artwork is best effort: this many failed fetches and it is given up on.
#define DL_ART_MAX_TRIES 3

// Consecutive failed attempts before a transient failure becomes FAILED.
// Reset whenever an attempt moves bytes, so a flaky link that keeps making
// progress never runs out of retries.
#define DL_MAX_ATTEMPTS  8
// A file that arrived whole but failed validation costs a whole film to try
// again, and progress does not reset this count: one retry, then the user.
#define DL_MEDIA_MAX_ATTEMPTS 2

typedef enum {
    DL_QUEUED = 0,     // waiting its turn (or waiting out a retry backoff)
    DL_DOWNLOADING,    // the worker is transferring it right now
    DL_PAUSED,         // user paused; partial data kept
    DL_COMPLETED,      // media file complete and in place
    DL_FAILED,         // gave up; partial data kept, user can retry
    DL_CANCELLED,      // user cancelled; partial data deleted
    DL_STATE_COUNT
} DlState;

typedef enum {
    DL_EV_START,         // worker picked it up
    DL_EV_PAUSE,         // user
    DL_EV_RESUME,        // user
    DL_EV_CANCEL,        // user
    DL_EV_COMPLETE,      // transfer finished and verified
    DL_EV_FAIL_RETRY,    // transient failure; back in the queue with backoff
    DL_EV_FAIL,          // permanent failure (or retries exhausted)
    DL_EV_RETRY,         // user asked to try again
    DL_EV_INTERRUPTED,   // transfer stopped without a verdict: app exit,
                         // playback started, restored after a crash
} DlEvent;

typedef enum {
    DL_ERR_NONE = 0,
    // transient -- retried automatically with backoff
    DL_ERR_UNREACHABLE,   // resolve/connect failed (server down, no network)
    DL_ERR_TIMEOUT,       // no headers / no body bytes within the deadline
    DL_ERR_NETWORK,       // connection reset or send failed
    DL_ERR_PARTIAL,       // closed before the declared length arrived
    DL_ERR_SERVER,        // HTTP 5xx
    DL_ERR_BAD_RESPONSE,  // malformed or inconsistent response
    // permanent -- needs the user (retry, re-login, free space)
    DL_ERR_HTTP,          // other HTTP 4xx
    DL_ERR_AUTH,          // HTTP 401/403
    DL_ERR_NOT_FOUND,     // HTTP 404/410
    DL_ERR_NO_SPACE,      // not enough room on the HDD
    DL_ERR_DISK,          // could not open/write the media file
    DL_ERR_UNSUPPORTED,   // a URL this client cannot fetch (https)
    DL_ERR_CORRUPT,       // on-disk state could not be restored
    // Stage 3
    DL_ERR_BAD_MEDIA,     // arrived whole but is not a complete TS (e.g. the
                          // server's transcode died part way): retried once
                          // (DL_MEDIA_MAX_ATTEMPTS), then failed
    // Why a PAUSED item is paused by the manager rather than the user.  The
    // item keeps its partial data and Resume continues it.
    DL_ERR_SPACE_UNKNOWN, // free HDD space cannot be read: nothing may write
    DL_ERR_COUNT
} DlError;

// The bookkeeping record for one download (state.txt).
typedef struct {
    char     id[DL_ID_MAX];        // Jellyfin item id; also the directory name
    char     title[DL_TITLE_MAX];  // display name, so the list never needs meta
    char     url[DL_URL_MAX];      // media request, WITHOUT any token
    DlState  state;
    DlError  error;                // why it last failed (NONE otherwise)
    int      http_status;          // last HTTP status seen, 0 = none
    uint32_t seq;                  // enqueue order -- the queue order
    uint32_t attempts;             // consecutive failed attempts
    uint64_t bytes_done;           // bytes on disk
    uint64_t bytes_total;          // 0 = not known yet
    uint64_t est_bytes;            // what the item is expected to need, for
                                   // space accounting; 0 = no estimate
    uint8_t  resumable;            // server has honoured a Range request
    // Stage 3.  All optional: a record without them loads as before.
    char     container[8];         // "ts" -> the MPEG-TS checks at completion
    uint32_t expect_secs;          // item runtime; 0 = unknown
    char     poster_url[DL_ART_URL_MAX];    // artwork still to fetch ("" =
    char     backdrop_url[DL_ART_URL_MAX];  //  fetched, none, or given up)
    uint32_t art_tries;            // failed artwork fetches so far
} DlRecord;

// Everything needed to show and play an item with the server gone
// (meta.txt).  Describes the DOWNLOADED file, not the server's source: a
// 4K HEVC source downloaded as a 1080p H.264 transcode is recorded as the
// latter, because that is what the player will be handed.
typedef struct {
    char     id[DL_ID_MAX];
    char     type[32];             // "Movie" / "Episode" (Jellyfin Type)
    char     title[DL_TITLE_MAX];
    char     series[DL_TITLE_MAX]; // episodes only
    char     series_id[DL_ID_MAX];
    int      season;               // -1 = n/a
    int      episode;              // -1 = n/a
    int      year;                 // 0 = unknown
    uint32_t runtime_secs;         // 0 = unknown
    char     overview[1024];
    // media information of the downloaded file
    char     container[16];        // "ts"
    char     video_codec[16];      // "h264"
    char     audio_codec[16];      // "ac3" / "mp3" / "dts" / "truehd"
    int      width, height;        // 0 = unknown
    int      audio_channels;       // 0 = unknown
    uint32_t video_bitrate;        // requested ceiling in bit/s; 0 = copy
    char     media_source_id[96];  // which version was downloaded
    int      audio_stream_index;   // -1 = server default
    char     quality[24];          // quality label at download time
    char     video_info[64];       // display strings, as the info page shows
    char     audio_info[128];
    // artwork, as file names inside the item directory ("" = none)
    char     poster[32];
    char     backdrop[32];
} DlMeta;

// ---- names -------------------------------------------------------------
const char *dl_state_name(DlState s);          // "queued" ...
bool        dl_state_from_name(const char *n, DlState *out);
const char *dl_error_name(DlError e);          // "unreachable" ...
bool        dl_error_from_name(const char *n, DlError *out);
// Short words for the UI ("Server unreachable", "Not enough HDD space").
const char *dl_error_text(DlError e);

// ---- rules --------------------------------------------------------------
// The state machine.  Returns true and sets *out for a legal transition;
// false (and *out untouched) for an illegal one, which callers treat as a
// no-op -- a stale button press must never corrupt a record.
bool dl_next_state(DlState s, DlEvent ev, DlState *out);

// True if the worker would ever transfer an item in this state.
bool dl_state_is_active(DlState s);

bool     dl_error_retryable(DlError e);
DlError  dl_error_for_http_status(int status);   // NONE for 2xx
// Delay before automatic retry number `attempts` (1-based): 2 s, 4 s, 8 s...
// capped at 5 minutes.
uint32_t dl_backoff_ms(uint32_t attempts);

// 0..1000, or -1 when the total is not known yet.
int dl_progress_permille(const DlRecord *r);

// Ids become directory names, so only [A-Za-z0-9_-], 1..63 chars.  Anything
// else ("../", "/", empty) is refused before it reaches a path.
bool dl_id_valid(const char *id);

void dl_record_init(DlRecord *r);
void dl_meta_init(DlMeta *m);

// ---- serialization --------------------------------------------------------
// format: returns bytes written (excluding NUL), or -1 if cap is too small.
// parse: returns false for anything malformed -- wrong/missing header,
// missing end marker (a torn write), unknown state name, bad numbers,
// invalid id, bytes_done > bytes_total.  Unknown keys are ignored so a newer
// build's files still load.
int  dl_record_format(const DlRecord *r, char *out, int cap);
bool dl_record_parse(const char *text, DlRecord *out);
int  dl_meta_format(const DlMeta *m, char *out, int cap);
bool dl_meta_parse(const char *text, DlMeta *out);
