#pragma once
// Jellyfin media segments: the intro / recap / credits markers behind a
// "Skip Intro" button.  GET /MediaSegments/{id} returns
//
//   {"Items":[{"Id":"..","ItemId":"..","Type":"Intro",
//              "StartTicks":2280000000,"EndTicks":2400000000}, ...],
//    "TotalRecordCount":1,"StartIndex":0}
//
// Ticks are 100 ns.  Segments come from a server-side provider (Intro Skipper,
// Gelato's IntroDB, ...).  Gelato stores them against the MEDIA SOURCE the
// provider saw -- one of an episode's many versions -- not the episode, and
// only once that version has been played; so ask by media source id first.

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SEG_UNKNOWN = 0,
    SEG_INTRO,
    SEG_RECAP,
    SEG_OUTRO,      // credits
    SEG_PREVIEW,
    SEG_COMMERCIAL,
} MediaSegmentType;

typedef struct {
    MediaSegmentType type;
    double start_secs, end_secs;
} MediaSegment;

#define MEDIA_SEGMENTS_MAX 8

// Parse a /MediaSegments response.  Returns how many segments were written to
// `out` (at most `max`), skipping malformed entries and ones that end before
// they start.  A body that is not the expected shape yields 0.
int media_segments_parse(const char *json, int len, MediaSegment *out, int max);

// "Skip Intro", "Skip Recap", "Skip Credits" ... or NULL for types not worth
// a skip button (unknown).
const char *media_segment_skip_label(MediaSegmentType t);

#ifdef __cplusplus
}
#endif
