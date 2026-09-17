#pragma once
#include <ppu-types.h>

// -------------------------------------------------------------------------
//  Video quality selection
// -------------------------------------------------------------------------
//  Picks the transcode the server is asked for, the way the Jellyfin web
//  player's quality dropdown does.  Chosen on the item info screen (Triangle)
//  next to the Version row, so both "which file" and "how big a stream" are
//  decided before playback starts — the player negotiates once and does not
//  switch mid-stream, matching how the audio/subtitle menus already work.
//
//  Why it matters beyond looks: the request size sets the network bandwidth,
//  the jitter buffer's per-frame cost (a 1080p slot is 3.13 MB, a 480p one
//  under 0.5 MB) and how much work the server does.  Dropping a step is the
//  first thing to try when playback stalls — especially in HD audio mode,
//  where the audio track is stream-copied at several Mbps on top of the
//  video.
//
//  VQ_AUTO reproduces the previous behaviour exactly: follow the 1080p
//  (Alpha) toggle, 1080p High/4.2/10 Mbps when it is on, else the shipped
//  720p baseline/3.1/4 Mbps path.  It is the default, so an install that
//  never touches this row behaves byte-for-byte as before.
//
//  Persisted as a single digit in the app data dir next to the other
//  settings files.

typedef enum {
    VQ_AUTO  = 0,   // follow the 1080p toggle (default, previous behaviour)
    VQ_1080P = 1,   // 1920x1080 High 4.2, 10 Mbps
    VQ_720P  = 2,   // 1280x720 baseline 3.1, 4 Mbps
    VQ_480P  = 3,   // 854x480 baseline 3.1, 1.5 Mbps
    VQ_360P  = 4,   // 640x360 baseline 3.1, 0.7 Mbps
    // DIRECT PLAY: ask the server to COPY the source video through untouched
    // instead of re-encoding it -- full disc bitrate, and no encoder on the
    // server that can fall behind real time.  Added LAST on purpose: the
    // setting persists as a digit, so appending keeps every already-saved
    // value meaning exactly what it meant.
    VQ_ORIGINAL = 5,
    // Middle ground between the 10 Mbps 1080p transcode and uncapped direct
    // play.  A ceiling is a ceiling ON THE COPY (see build_stream_url), so:
    //   source <= ceiling -> copied untouched, no encoder involved
    //   source >  ceiling -> transcoded down to it
    // Useful when a remux is more data than the link can carry but the
    // server can still encode at the lower rate.  Appended, like ORIGINAL,
    // so saved settings keep their meaning.
    VQ_1080P_20 = 6,   // 1920x1080 High 4.2, copy up to 20 Mbps
    VQ_1080P_30 = 7,   // 1920x1080 High 4.2, copy up to 30 Mbps
    VQ_COUNT = 8,
} vquality_t;

void       vquality_load(void);         // read the persisted value at startup
void       vquality_save(void);
vquality_t vquality_get(void);
void       vquality_set(vquality_t q);  // set + persist
void       vquality_next(int delta);    // cycle (left/right on the info row)

// Short label for the info screen: "Auto", "1080p", ..., "Original".
const char *vquality_label(vquality_t q);

// Everything the stream request needs, resolved against the 1080p toggle for
// VQ_AUTO.  max_w/max_h are the display's own limits, applied so a 720p
// request on a 576-line output does not ask for more than can be shown —
// the same clamp the player applied before this existed.  Any output may be
// NULL.
//
// *out_vbitrate == 0 means DIRECT PLAY: send no VideoBitrate at all and let
// the server copy the source stream.  A ceiling is not merely unnecessary
// there, it is fatal -- Jellyfin checks the requested bitrate before allowing
// a copy, so asking for 10 Mbps on a 30 Mbps remux silently demotes it to a
// transcode.  Exactly the trap the HD audio path already documents for
// AudioBitrate.
void vquality_params(vquality_t q, bool hd_toggle,
                     u32 max_w, u32 max_h,
                     u32 *out_w, u32 *out_h,
                     const char **out_profile, const char **out_level,
                     unsigned *out_vbitrate);
