#pragma once
#include <stdbool.h>
#include <stdint.h>

// -------------------------------------------------------------------------
//  What the Media browser calls a file: names, sizes, durations
// -------------------------------------------------------------------------
//  Pure (no PS3 headers) so tests/test_local_names.cpp compiles the same file
//  the console runs.  A drive's files are named the way a release group named
//  them ("The.Movie.Name.2019.1080p.BluRay.x264-GRP.mkv"); the browser shows
//  "The Movie Name (2019)" and keeps the file name for the details page.
// -------------------------------------------------------------------------

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char title[128];     // "The Movie Name"
    int  year;           // 0 = none found
} LocalTitle;

// File name -> title and year.  The extension goes; dots and underscores become spaces; the title
// ends at the year (a 4-digit 1900..2099 that is not the first word) or at the first release tag
// ("1080p", "BluRay", "x264", "DTS", ...), whichever comes first; brackets around the year go.  A
// name that is nothing but tags keeps its cleaned words.  Never returns an empty title for a
// non-empty name.
void local_clean_name(const char *file_name, LocalTitle *out);

// "The Movie Name (2019)", or the title alone.
void local_title_line(const LocalTitle *t, char *out, int cap);

// "4.2 GB", "812 MB", "37 KB", "0 B": one decimal above 1 GB, whole numbers below.
void local_format_size(uint64_t bytes, char *out, int cap);

// "1 h 52 min", "47 min", "under a minute", "2 h".  0 gives "".
void local_format_duration(uint32_t secs, char *out, int cap);

// "H.264 High 1920x1080 23.976 fps" style helpers live in local_probe; the resume line is here:
// "Resume from 1:12:30", "Resume from 4:05".
void local_format_resume(uint32_t secs, char *out, int cap);

#ifdef __cplusplus
}
#endif
