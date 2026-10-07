#pragma once
#ifndef JF_SUBTITLES_TEST
#include <ppu-types.h>
#endif

// -------------------------------------------------------------------------
//  On-device subtitle rendering
// -------------------------------------------------------------------------
//  Why this exists: the device profile declared every subtitle format as
//  SubtitleMethod=Encode, so switching subtitles on asked the server to BURN
//  them into the video. That forces a full video transcode, which throws away
//  the stream-copy path -- and the copy path is the only one that carries the
//  source's own lossless TrueHD / DTS-HD MA track. So subtitles and lossless
//  audio were mutually exclusive, and nothing said so: you just quietly lost
//  the feature this fork exists for.
//
//  Text subtitles do not need the server's encoder. Jellyfin will hand any
//  text format over as SubRip on request, and this app already has a TTF
//  renderer and an accurate playback clock. Fetching the file once and
//  drawing it here keeps the video stream copied, untouched and lossless.
//
//  SCOPE: text formats (SubRip, and ASS/SSA/VTT converted to SubRip
//  server-side, which drops styling but keeps the words and the timing) via
//  subs_load()/subs_text_at(), AND now PGS bitmap subtitles via
//  subs_load_pgs()/subs_pgs_at() -- see subtitles_pgs.h for the RLE decoder
//  and its own scope limits. VOBSUB is still burn-in only: it is a
//  different (much older, DVD-era) run-length format subtitles_pgs.c does
//  not decode.
//
//  Memory: cues are a fixed table sized for a long film with dense dialogue.
//  A 3-hour feature runs to roughly 2000 cues; 4096 x 208 bytes is ~850 KB,
//  allocated once and reused, so a subtitle change costs no allocation.
//
//  PGS memory is a separate, smaller commitment: the whole .sup elementary
//  stream is fetched once and kept resident (see PGS_SUP_MAX in
//  subtitles.cpp -- sized as a guess pending real file sizes from an actual
//  disc, flag this if it turns out wrong on hardware), plus one decoded-
//  bitmap scratch buffer that grows on demand up to subtitles_pgs.h's
//  PGS_MAX_W x PGS_MAX_H. Both are freed only on subs_clear(), same
//  keep-the-table-between-tracks philosophy as the text cues above.

#include "subtitles_pgs.h"

#ifdef __cplusplus
extern "C" {
#endif

// Fetch and parse one subtitle stream.  Returns the number of cues loaded, or
// -1 on failure (which the caller should treat as "leave subtitles off"
// rather than as a reason to fail playback).  Replaces whatever was loaded.
int  subs_load(const char *item_id, const char *media_source_id, int stream_index);

// PGS (bitmap) counterpart to subs_load() -- fetches the raw .sup stream
// once and indexes its display sets (cheap; see pgs_build_index). Bitmaps
// are decoded lazily, one at a time, as playback reaches them. Same
// "-1 means leave subtitles off" contract, and also replaces whatever was
// loaded (text or PGS).
int  subs_load_pgs(const char *item_id, const char *media_source_id,
                   int stream_index);

// Forget the current track (whichever kind is loaded).  Safe to call when
// nothing is loaded.
void subs_clear(void);

bool subs_active(void);

// True once subs_load_pgs() has succeeded and subs_clear() hasn't run
// since -- tells the caller which of subs_text_at()/subs_pgs_at() to use.
bool subs_is_pgs(void);

// Text to show at this playback position, or NULL when no cue is active.
// Lines are separated by '\n'; the caller draws them.  Cheap enough to call
// once per frame: it remembers where it was and walks forward, so a normal
// playback pass is O(1) per call and a seek costs one binary search.
const char *subs_text_at(u64 pts_ms);

// Bitmap to show at this playback position, or NULL when no cue is active
// (including an explicit "hide" epoch) or subs_is_pgs() is false. Safe to
// call once per frame like subs_text_at(): the underlying bitmap is decoded
// only when the active display set actually changes, not every call.
const PgsBitmap *subs_pgs_at(u64 pts_ms);

// After a seek the cursor must not be trusted to walk forward.
void subs_reset_cursor(void);

#ifdef __cplusplus
}
#endif
