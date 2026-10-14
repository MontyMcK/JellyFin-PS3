// Item "facts": the technical strip the redesigned Home and detail screens
// print about a file -- container, video, audio and subtitles, in the terms
// this client cares about (is the audio lossless?).
//
// FETCHED OFF THE RENDER THREAD.  The Home queue asks for the focused item's
// facts every time the focus settles, and a blocking request there would
// freeze the glide the queue exists to show.  So requests go to a worker
// (one slot; the newest wins) and land in a small cache the screens read
// each frame.  Until an item's facts arrive the screen simply draws without
// them, then fades them in.
//
// The worker lives for the process, like the playstate reporter's
// (api_playstate.cpp explains why tearing it down is the worse option).
#pragma once
#include <ppu-types.h>

typedef struct {
    char container[12];    // "MKV"
    char video[40];        // "1080P H.264", "4K HEVC HDR10"
    char audio[40];        // default track: "DTS-HD MA 5.1", "TRUEHD ATMOS 7.1"
    char audio_more[48];   // the next track, "+ AC-3 5.1" ("" when only one)
    char subs[32];         // default or first: "PGS ENG"; "NONE" when there are none
    int  n_subs;
    bool lossless;         // the default audio track is lossless
    u32  runtime_secs;

    // For the Triangle quick-peek (xmb/ui_peek.cpp): what the item is about,
    // from the same one request.  Series are asked about too (they have no
    // streams, so only these fields fill).
    char overview[520];    // synopsis, decoded, cut at a word boundary
    char rating[12];       // OfficialRating, "PG-13"
    char cast[4][40];      // the first four actors, billing order
    char cast_id[4][40];   // their person ids (portrait thumbnails)
    int  n_cast;
} ItemFacts;

// Queue item_id for fetching (render thread only).  Cheap when it is cached
// or already the pending request; safe to call every frame.
void facts_request(const char *item_id);

// Copy out item_id's facts if they have arrived.  *age_us (optional) is how
// long ago they did, for fading them in.  False while pending, or when the
// fetch failed.
bool facts_get(const char *item_id, ItemFacts *out, unsigned long long *age_us);

// Worth asking about?  Only video items have streams to describe.
bool facts_wanted(const char *item_type);

// Parse one item's JSON (a BaseItemDto with MediaStreams) into *out.
// Exposed for the host tests.
void facts_parse(const char *json, ItemFacts *out);
