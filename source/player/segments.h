#pragma once
// Skip Intro / Recap / Credits: fetch the playing title's media segments off
// the player thread, and answer "is there something to skip right now?".
#include "media_segments.h"

// Start fetching for a new playback.  source_id is the chosen version (may be
// NULL/empty).  Asks by source id, then item id, and -- because Gelato's
// IntroDB fills segments in on a version's FIRST play -- once more after
// 20 s if both came back empty.  Returns immediately; a worker does the HTTP.
void segments_start(const char *item_id, const char *source_id);

// End of playback: forget the segments and ignore any fetch still in flight.
void segments_stop(void);

// The skippable segment covering pos_secs, or NULL.  Stops offering it in the
// last second, and for a few seconds after segments_skipped() so a skip still
// being carried out is not offered twice.
const MediaSegment *segments_at(double pos_secs);
void segments_skipped(void);

// Start of the title's credits (an Outro segment), seconds; < 0 when the
// server has none (yet).
double segments_outro_start(void);
