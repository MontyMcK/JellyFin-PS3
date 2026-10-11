#pragma once
// A film's chapters, from the item the server describes (host-tested parser,
// plus the one session-wide list the player's menu reads).
//
// Jellyfin lists them as  "Chapters":[{"StartPositionTicks":0,"Name":"Chapter 1",
// "ImageTag":"..."}, ...]  on the item when asked for Fields=Chapters; a tick is
// 100 ns.  A film the server has no chapters for has an empty list, and so
// does anything streamed from a debrid plugin.

#define JF_MAX_CHAPTERS 48

struct JFChapter {
    double start_secs;
    char   name[48];      // as the server has it (may be empty)
};

struct JFChapters {
    int       n;
    JFChapter ch[JF_MAX_CHAPTERS];
};

// Parses the chapters out of one item's JSON.  Returns how many were kept: the
// ones with a start time, in order, each later than the one before (a chapter
// that does not advance is dropped), at most JF_MAX_CHAPTERS.
int  jf_parse_chapters(const char *json, int len, JFChapters *out);

// The chapter playing at `pos_secs` (the last one that has started), or -1
// before the first.
int  jf_chapter_index_at(const JFChapters *c, double pos_secs);

// "3  The Heist  1:02:03" for the menu.  `unnamed_fmt` ("Chapter %d") fills in
// a chapter with no name.  A long name is cut on a character boundary.
void jf_chapter_label(const JFChapter *c, int index, const char *unnamed_fmt,
                      char *out, int cap);

// ---- the list for the film being played (api/chapters_fetch.cpp, PS3) ----
enum { JF_CHAPTERS_IDLE = 0, JF_CHAPTERS_LOADING, JF_CHAPTERS_READY, JF_CHAPTERS_NONE };

// A new film: forget the last one's, and let any request still on its way be dropped.
void              jf_chapters_reset(void);
// Ask the server for `item_id`'s chapters on a worker; the player polls the state.
void              jf_chapters_request(const char *item_id);
int               jf_chapters_state(void);
// Valid while the state is JF_CHAPTERS_READY.
const JFChapters *jf_chapters(void);
