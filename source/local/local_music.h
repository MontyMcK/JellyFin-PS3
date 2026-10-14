#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "local_tags.h"

// -------------------------------------------------------------------------
//  A folder of music files as an album
// -------------------------------------------------------------------------
//  What the browser needs to turn the audio files of a folder into a play queue: each file's tags kept
//  in a track record, the order an album is played in, the names to show, the album's title and artist,
//  and the cover art by way of a small registry the thumbnail cache asks (the cover is bytes inside a
//  file, or a picture file in the folder).
//
//  Pure (no PS3 headers): tests/test_local_music.cpp compiles this file.  Reading the files is
//  local_music_fs.cpp's job.
// -------------------------------------------------------------------------

#ifdef __cplusplus
extern "C" {
#endif

#define LM_MAX_TRACKS 100          // the music engine's queue length

typedef struct {
    char     path[256];            // an lfs path
    char     title[128];           // tags; empty when the file has none
    char     artist[96];
    char     album[128];
    int      track_no;             // 0 = not tagged
    int      disc_no;              // 0 = not tagged
    uint32_t duration_secs;        // 0 = unknown
    LaKind   kind;
    uint64_t pic_off;              // the cover inside the file
    uint32_t pic_len;              // 0 = none
} LmTrack;

void lm_track_fill(LmTrack *t, const char *path, const LaMeta *m);

// Disc, then track number (an untagged one after the tagged), then the file name in natural order.
int  lm_cmp(const LmTrack *a, const LmTrack *b);
void lm_sort(LmTrack *t, int n);

// The name to show for a track: its title tag, else the file's name with the extension, a leading track
// number ("03 - ", "1-04. ", "07_") and underscores taken out.
void lm_display_title(const LmTrack *t, char *out, int cap);
// The same from a file name or an lfs path alone (what a row shows before its tags are read).
void lm_stem_title(const char *path_or_name, char *out, int cap);

// Whether the tracks are one album: at most one distinct album tag among those that have one.  A folder that is not
// (a pile of songs) is played in its listing order, not by track number.
bool lm_is_one_album(const LmTrack *t, int n);

// The album's name: the album tag most of the tracks carry, else the folder's name (underscores as spaces).
void lm_album_name(const LmTrack *t, int n, const char *folder, char *out, int cap);

// The artist the album is credited to: the one that at least 70 % of the tracks carry; "Various Artists"
// when several share the tracks; empty when none is tagged.
void lm_album_artist(const LmTrack *t, int n, char *out, int cap);

// The track whose embedded cover to use: the first that has one, or -1.
int  lm_pick_art(const LmTrack *t, int n);

// ---- the cover art registry ----
// The thumbnail cache asks for a picture by a key; the key stands for a file on a drive and, when the
// picture is inside it, where.  A few are kept (the newest replace the oldest); safe from any thread.
typedef struct {
    char     path[256];
    uint64_t off;
    uint32_t len;                  // 0 = the whole file
} LmArt;

// Registers a picture and writes its key ("lm:<n>") to key.
void lm_art_register(const char *path, uint64_t off, uint32_t len, char *key, int cap);
bool lm_art_find(const char *key, LmArt *out);
// Whether a key is one of this registry's (by its prefix), found or not.
bool lm_art_is_key(const char *key);

#ifdef __cplusplus
}
#endif
