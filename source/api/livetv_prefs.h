#pragma once
// The viewer's own arrangement of the Live TV tab (host-tested, no PS3 headers).
//
// The tab shows a big channel list a category at a time (ui_livetv.cpp).  This
// is what makes it the viewer's: which categories are shown at all, which page
// it opens on, and the channels watched last.  Two pages belong to the viewer
// rather than to the server's numbering:
//
//   LP_FAVS    "My Channels": every starred channel, from all categories
//   LP_RECENT  "Recently watched": the last channels played, newest first
//
// A page is named by a key: a category's lowest channel number (> 0), or one of
// the two above (< 0).  The file is jellyfin_livetv.txt, one fact per line:
//     hide <key>      a page the viewer turned off
//     recent <id>     a channel watched (newest first)
//     last <key>      the page shown last

#include <stdbool.h>

#define LP_FAVS         (-1)
#define LP_RECENT       (-2)
#define LP_MAX_HIDDEN   32
#define LP_MAX_RECENT   12
#define LP_ID_MAX       40

typedef struct {
    int  hidden[LP_MAX_HIDDEN];  int n_hidden;
    char recent[LP_MAX_RECENT][LP_ID_MAX];  int n_recent;
    int  last;                   // 0 = none
} LtPrefs;

#ifdef __cplusplus
extern "C" {
#endif

void lp_init(LtPrefs *p);
// Reads the file's text; unknown or malformed lines are skipped.  Always
// leaves `p` valid.  Returns how many lines it used.
int  lp_parse(const char *text, LtPrefs *p);
// The file's text.  Returns its length, or -1 when `cap` is too small.
int  lp_format(const LtPrefs *p, char *out, int cap);

bool lp_is_hidden(const LtPrefs *p, int key);
void lp_set_hidden(LtPrefs *p, int key, bool hidden);
// A channel was played: it goes first, once, and only the newest LP_MAX_RECENT stay.
void lp_note_recent(LtPrefs *p, const char *id);
// Position of `id` in the recent list (0 = newest), or LP_MAX_RECENT when absent.
int  lp_recent_rank(const LtPrefs *p, const char *id);

// The pages to offer, in order, as keys: My Channels, Recently watched (only
// when something was watched), then the categories (`bases`, ascending), each
// unless hidden.  Never fewer than one category: when every one is hidden they
// all show.  Returns the count (at most `max`).
int  lp_build_pages(const LtPrefs *p, const int *bases, int n_bases, int *out, int max);
// How many categories lp_build_pages would offer.
int  lp_visible_categories(const LtPrefs *p, const int *bases, int n_bases);
// Which of `keys` to open on: `want` if offered, else the first category (a
// viewer with no stars should not land on an empty My Channels), else 0.
int  lp_pick_page(const int *keys, int n, int want);

#ifdef __cplusplus
}
#endif
