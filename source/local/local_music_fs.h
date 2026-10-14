#pragma once
#include "lfs.h"
#include "local_music.h"

// -------------------------------------------------------------------------
//  Music on a drive, read through lfs
// -------------------------------------------------------------------------
//  The part of local_music.h that touches files: the tags of a folder's music files, the cover picture
//  that goes with them, and the thumbnail cache's provider for it.  Blocking (reads from a drive): run
//  it off the render thread (loading_run).
// -------------------------------------------------------------------------

// The tags of the music files among `entries` (the listing of folder `dir`), the ones that can be decoded
// kept as tracks in album order.  At most `max` of them.  *skipped (may be NULL) gets how many music files
// were left out because they could not be read or decoded.  Returns the number of tracks.
int lm_scan_folder(const char *dir, const lfs_entry *entries, int n, LmTrack *out, int max, int *skipped);

// The album's cover, registered with the cover registry: the picture inside the first track that has one,
// else a picture file in the folder (cover / folder / front / albumart / album, .jpg .jpeg .png).  Writes
// the key the thumbnail cache is asked for; false (and an empty key) when there is none.
bool lm_register_cover(const char *dir, const LmTrack *t, int n, char *key, int cap);

// Installed with thumb_set_local_bytes(): the bytes of a registered cover (see thumbnail_cache.h).
int lm_art_bytes(const char *key, unsigned char *buf, int cap);
