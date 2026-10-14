#pragma once
#include <stdint.h>
#include "dl_model.h"
#include "dl_store.h"

// -------------------------------------------------------------------------
//  Offline downloads -- the offline library (Stage 4)
// -------------------------------------------------------------------------
//  What can be played with the server gone, answered from the records the
//  download manager already keeps -- no second index.  An item is in the
//  library only if ALL of these hold, checked every time it is asked for
//  (the file can change under a running app: FTP, a full disk, a crash):
//
//    * its record is COMPLETED           (not queued/downloading/paused/
//                                          failed/cancelled)
//    * media.ts exists at exactly the size the record verified
//    * a TS download is still whole packets
//
//  Metadata comes from meta.txt; if that is missing, damaged or belongs to
//  another item ("stale"), the entry still plays, with what the record knows
//  (id, title) and meta_ok = false.  Artwork paths are set only when the
//  image file is really there.
//
//  Screens are Stage 5.  This is the model they (and offline playback) use.
// -------------------------------------------------------------------------

typedef struct {
    DlMeta   meta;                        // meta.txt, or a minimal stand-in
    bool     meta_ok;                     // meta.txt loaded for this item
    char     media_path[DL_PATH_MAX];
    char     poster_path[DL_PATH_MAX];    // "" when there is no image
    char     backdrop_path[DL_PATH_MAX];
    uint64_t bytes;
    uint32_t seq;                         // download order
} DlLibraryEntry;

// Ids of playable items in download order (at most max).  Cheap: no
// metadata is read.  0 before the service has restored the store.
int  dl_library_ids(char (*ids)[DL_ID_MAX], int max);
// One entry, fully verified.  False if the item is not playable right now.
bool dl_library_get(const char *id, DlLibraryEntry *out);
bool dl_library_has(const char *id);       // online item -> local copy?

// How the existing player should be set up for a local file.
typedef struct {
    uint32_t req_w, req_h;     // jitter-buffer frame ceiling: the file's own
                               // (it was downloaded at some quality, whatever
                               // the quality setting says now)
    uint32_t runtime_secs;     // from meta; the file's own index refines it
    bool     light;            // downloads may keep running beside it (paced)
} DlLocalPlan;

void dl_library_plan(const DlLibraryEntry *e, DlLocalPlan *out);

// The Stage 2 light rule, for a downloaded file instead of a stream URL:
// the same thresholds (DL_LIGHT_*).  Unknown metadata counts as heavy.
bool dl_meta_is_light(const DlMeta *m, bool meta_ok);
