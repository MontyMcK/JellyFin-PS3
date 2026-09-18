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
//  SCOPE: text formats only (SubRip, and ASS/SSA/VTT converted to SubRip
//  server-side, which drops styling but keeps the words and the timing).
//  PGS and VOBSUB are bitmaps -- they cannot convert to text, so they keep
//  the burn-in path and still cost a transcode. Rendering those means an RLE
//  decoder and a blitted overlay, which is a separate piece of work.
//
//  Memory: cues are a fixed table sized for a long film with dense dialogue.
//  A 3-hour feature runs to roughly 2000 cues; 4096 x 208 bytes is ~850 KB,
//  allocated once and reused, so a subtitle change costs no allocation.

#ifdef __cplusplus
extern "C" {
#endif

// Fetch and parse one subtitle stream.  Returns the number of cues loaded, or
// -1 on failure (which the caller should treat as "leave subtitles off"
// rather than as a reason to fail playback).  Replaces whatever was loaded.
int  subs_load(const char *item_id, const char *media_source_id, int stream_index);

// Forget the current track.  Safe to call when nothing is loaded.
void subs_clear(void);

bool subs_active(void);

// Text to show at this playback position, or NULL when no cue is active.
// Lines are separated by '\n'; the caller draws them.  Cheap enough to call
// once per frame: it remembers where it was and walks forward, so a normal
// playback pass is O(1) per call and a seek costs one binary search.
const char *subs_text_at(u64 pts_ms);

// After a seek the cursor must not be trusted to walk forward.
void subs_reset_cursor(void);

#ifdef __cplusplus
}
#endif
