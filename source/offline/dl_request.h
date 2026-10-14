#pragma once
#include "dl_model.h"
#include "dl_manager.h"
#include "jellyfin_api.h"
#include "stream_request.h"

// -------------------------------------------------------------------------
//  Offline downloads -- from a Jellyfin item to a download request
// -------------------------------------------------------------------------
//  The item page already holds everything a download needs: the item, its
//  detail (overview, identity: series/season/episode/year/runtime), the
//  version list with each version's tracks, and the live settings.  This
//  turns those into a DlMeta + URL + extras WITHOUT any network call and
//  without deciding anything about the stream itself:
//
//    selection  = the version the user chose, with the player's own initial
//                 track rule (stream_select_initial: default audio, no subs)
//    decision   = stream_request_resolve(prefs, selection)   <- the player's
//    URL        = stream_url_build(decision, StartTimeTicks=0,
//                                  no PlaySessionId)          <- the player's
//
//  So the file on disk is exactly what Play would have streamed from the
//  start, with the same settings, at that moment.
//
//  No PlaybackInfo call: that would mint a playback session (and open a live
//  stream) just to download.  The version list comes from the item DTO the
//  info screen fetched (jellyfin_fetch_media_sources), which carries the
//  same MediaSources/MediaStreams the player's PlaybackInfo parses.
// -------------------------------------------------------------------------

typedef struct {
    const char          *server;     // g_server
    const char          *device_id;  // jf_device_id()
    const JFItem        *item;       // id, name, type
    const XMBItemDetail *detail;     // may be NULL (then no overview/identity)
    const JFMediaSource *source;     // the chosen version -- REQUIRED
    const StreamPrefs   *prefs;      // stream_prefs_current()
} DlRequestInput;

typedef struct {
    DlMeta        meta;
    char          url[DL_URL_MAX];
    char          poster_url[DL_ART_URL_MAX];
    char          backdrop_url[DL_ART_URL_MAX];
    DlExtras      extras;            // points into this struct
    StreamRequest decision;          // what was decided, for logging/tests
    // What the file is expected to need, as an upper bound, for the space
    // check (also in extras.estimate_bytes).  size_known is false when there
    // is neither a runtime nor a size to estimate from: the caller must then
    // refuse, since space cannot be checked.
    uint64_t      size_estimate;
    bool          size_known;
} DlRequest;

// The estimate itself, pure:
//   direct play (no video ceiling): the server's MediaSource.Size when it
//     reports one, else the largest rate a disc video stream carries over
//     the runtime;
//   a ceiling: (video ceiling + audio) bit/s over the runtime;
// both with 15% headroom for the transport stream.  False when neither a size
// nor a runtime is known.
bool dl_estimate_bytes(const StreamRequest *d, uint32_t runtime_secs,
                       uint64_t source_size_bytes, uint64_t *out);

// Artwork sizes: bounded server-side so each fits the worker's buffer, and
// big enough for a detail page (poster) or a full-width background.
#define DL_POSTER_MAX_W   400
#define DL_POSTER_MAX_H   600
#define DL_BACKDROP_MAX_W 1280

// False (out untouched) when the input cannot produce a request identical to
// playback: no item id, no source (its tracks decide the audio/HD-copy
// choice -- guessing would silently differ from Play), no server, or a URL
// that does not fit.
bool dl_request_build(const DlRequestInput *in, DlRequest *out);
