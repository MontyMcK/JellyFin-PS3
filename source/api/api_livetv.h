#pragma once
#include <stdint.h>
#include "livetv.h"

// Live TV, console side: the server's channel list and guide, and closing a
// live stream.  Blocking calls with a private reply buffer each, so a worker
// thread can run them while the UI thread uses responseBuffer.  See livetv.h
// for the parsing they rest on.

// One page of the Live TV tab: the most channels held at once.  A longer list
// is shown a category at a time (ui_livetv.cpp).
#define LIVETV_MAX_CHANNELS 700

// The channel list, sorted (favourites, then number, then name), with each
// channel's current programme.  Fetched in pages so a reply never reaches the
// response cap.  Returns the count, or -1 when the server could not be asked.
int jf_fetch_channels(JFChannel *out, int max);

// The same, for `count` channels from list position `start` (the server's
// order: by number).  *total (optional) is how many channels the server has.
//
// `search` (optional) lists only the channels whose name matches it, from
// every category, in number order.  The server's /LiveTv/Channels ignores a
// search term, so this asks /Users/<id>/Items for channels; those replies carry
// no current programme (the guide request fills the rows in).
//
// `items_extra` (optional) is more query for that same /Users/<id>/Items route,
// such as "&isFavorite=true" (every starred channel, from all categories) or
// "&Ids=<id>,<id>" (those channels).
int jf_fetch_channels_range(JFChannel *out, int max, int start, int count, int *total,
                            const char *search = NULL, const char *items_extra = NULL);

// Every channel's number, in the server's order and nothing else (small
// replies), for working out where the categories start.  Up to `max` numbers
// stored; a channel without a number stores -1.  Returns the count, or -1 when
// the server could not be asked.  *total (optional) is the server's count.
int jf_fetch_channel_numbers(float *out, int max, int *total);

// Programmes airing between `from_ticks` (their end must be after it) and
// `to_ticks` (their start must be before it) for up to `n` channels, in start
// order per request.  Asks in pages of a few channels (the URL has a length
// limit).  Returns the count stored.
int jf_fetch_programs(const char *const *channel_ids, int n,
                      uint64_t from_ticks, uint64_t to_ticks,
                      JFProgram *out, int max);

// One programme's details: its overview.  False when it cannot be fetched.
bool jf_fetch_program_overview(const char *program_id, char *out, int cap);

// POST /LiveStreams/Close: the server stops the live source.  True on 2xx.
bool jf_livestream_close(const char *live_stream_id);

// The console clock as ticks (UTC).
uint64_t jf_now_ticks(void);
// Seconds the console's local clock is ahead of UTC: time zone plus summer time.
int jf_utc_offset_secs(void);
