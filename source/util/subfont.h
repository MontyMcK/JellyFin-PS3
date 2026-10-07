#pragma once

// -------------------------------------------------------------------------
//  Subtitle typeface
// -------------------------------------------------------------------------
//  The faces people associate with subtitles -- Arial, Helvetica, Netflix
//  Sans, Tiresias Screenfont -- are all proprietary and cannot ship in a
//  GPLv3 package.  These three are the open faces closest to them, and all
//  are bold: every broadcaster and streaming service sets subtitles semibold
//  or heavier, and weight matters more for legibility across a room than
//  which humanist sans it is.
//
//    Open Sans          already bundled for the UI. Closest to Helvetica or
//                       Arial in colour and width. The default.
//    Noto Sans          Open Sans's sibling, a shade more open and even in
//                       rhythm. SIL OFL 1.1.
//    Roboto Condensed   narrower, so a long line of dialogue fits on one row
//                       instead of wrapping to two. Apache 2.0.
//
//  The two added faces are subset to Latin, Latin-1 Supplement, Latin
//  Extended-A and the punctuation SubRip carries, which is why they cost
//  41 KB together rather than 1.1 MB.
//
//  Persisted as a digit beside the other settings files.

typedef enum {
    SUBFONT_OPENSANS = 0,
    SUBFONT_NOTO     = 1,
    SUBFONT_ROBOCOND = 2,
    SUBFONT_COUNT    = 3,
} subfont_t;

void        subfont_load(void);
void        subfont_cycle(void);
subfont_t   subfont_get(void);
const char *subfont_label(void);

// The UI_FACE_* id for the current choice, for drawTTF_face().
int         subfont_face(void);
