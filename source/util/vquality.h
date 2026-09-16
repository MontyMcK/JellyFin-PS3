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
    VQ_COUNT = 5,
} vquality_t;

void       vquality_load(void);         // read the persisted value at startup
void       vquality_save(void);
vquality_t vquality_get(void);
void       vquality_set(vquality_t q);  // set + persist
void       vquality_next(int delta);    // cycle (left/right on the info row)

// Short label for the info screen: "Auto", "1080p", "720p", "480p", "360p".
const char *vquality_label(vquality_t q);

// Everything the stream request needs, resolved against the 1080p toggle for
// VQ_AUTO.  max_w/max_h are the display's own limits, applied so a 720p
// request on a 576-line output does not ask for more than can be shown —
// the same clamp the player applied before this existed.  Any output may be
// NULL.
void vquality_params(vquality_t q, bool hd_toggle,
                     u32 max_w, u32 max_h,
                     u32 *out_w, u32 *out_h,
                     const char **out_profile, const char **out_level,
                     unsigned *out_vbitrate);
