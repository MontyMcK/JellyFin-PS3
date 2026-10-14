#pragma once
#include <stdbool.h>
#include <stdint.h>

// -------------------------------------------------------------------------
//  Live TV: channels and programmes
// -------------------------------------------------------------------------
//  Pure parsing and arithmetic, with no PS3 headers, so tests/test_livetv.cpp
//  compiles the same file the console runs.  What fetches (api_livetv.cpp) and
//  what draws (the Live TV tab) sit on top.
//
//  Times are Jellyfin ticks: 100 ns units since 0001-01-01 UTC.  The console
//  clock is UTC (sysGetCurrentTime); the time zone only matters for display.

typedef struct {
    char id[40];
    char name[96];
    char number[12];
    char logo_tag[40];            // ImageTags.Primary, "" when there is no logo
    bool favourite;               // UserData.IsFavorite
    // The current programme (AddCurrentProgram=true); empty without a guide.
    char     now_title[128];
    uint64_t now_start_ticks, now_end_ticks;
} JFChannel;

typedef struct {
    char id[40];
    char channel_id[40];
    char name[128];
    char episode_title[96];
    uint64_t start_ticks, end_ticks;
    bool is_movie, is_series, is_news, is_sports, is_kids, is_live, is_premiere;
} JFProgram;

// Parse the Items of a /LiveTv/Channels or /LiveTv/Programs reply.  Return the
// number of items stored (at most max).  *total (optional) is the server's
// TotalRecordCount, or the count stored when it is absent.  A reply cut short
// by the response cap is parsed as far as it goes and reported through
// *truncated (optional).
int livetv_parse_channels(const char *json, int len, JFChannel *out, int max,
                          int *total, bool *truncated);
int livetv_parse_programs(const char *json, int len, JFProgram *out, int max,
                          int *total, bool *truncated);

// "2026-10-03T19:30:00.0000000Z" -> ticks.  The fraction may have any number
// of digits (or none), the zone may be Z, +hh:mm / -hh:mm, or absent (UTC).
// 0 when it is not a timestamp.
uint64_t jf_ticks_from_iso8601(const char *s);
// ticks -> "2026-10-03T19:30:00.0000000Z" (the form the server's date filters
// take).  Returns the length written, or 0.
int jf_iso8601_from_ticks(uint64_t ticks, char *out, int cap);
// Ticks for a Unix time in seconds (what sysGetCurrentTime returns).
uint64_t jf_ticks_from_unix(uint64_t unix_secs);
// "HH:MM" (24 h) of a tick time on a clock `utc_offset_secs` from UTC.
void livetv_format_hm(uint64_t ticks, int utc_offset_secs, char *out, int cap);

// ---- the list's rules -------------------------------------------------------

// Favourites first, then by channel number (numerically; channels without a
// number after those with one), then by name.  Stable.
void livetv_sort_channels(JFChannel *c, int n);

// Elapsed fraction of the current programme at `now`, in permille, clamped to
// [0, 1000].  -1 when there is no programme (no times, or end <= start).
int livetv_progress_permille(uint64_t now, uint64_t start, uint64_t end);

// A channel number's value; false when it is empty or not a number.
bool livetv_number_value(const char *s, double *v);

// Paging a long list by category.  `nums` are the channel numbers in list
// order (the server sorts by number); `bases` are the lowest number of each
// category, ascending.  Channels numbered below bases[0] join the first.  For
// every category that has channels, in order, fills first[] (index of its
// first channel), count[] and cat[] (its position in `bases`).  Returns the
// number of pages written, at most `max`.
int livetv_page_ranges(const float *nums, int n, const int *bases, int n_bases,
                       int *first, int *count, int *cat, int max);

// The channel after / before `cur` in list order, wrapping.  -1 for an empty
// list.  cur outside the list counts as before the first / after the last.
int livetv_step_channel(int cur, int n, int dir);

// The guide: the programme airing at `t` among progs[0..n) of one channel
// (start <= t < end), or -1.  The programmes may be unordered or overlap; an
// overlap resolves to the later start.  Zero-length programmes never air.
int livetv_programme_at(const JFProgram *progs, int n, uint64_t t);

// Debounce for channel changes: presses within `quiet_us` of the previous one
// collapse to the last.  Returns true when a pending change should open now.
bool livetv_debounce_ready(uint64_t last_press_us, uint64_t now_us, uint64_t quiet_us);
