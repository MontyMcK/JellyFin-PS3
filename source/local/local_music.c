// A folder of music files as an album: see local_music.h.

#include "i18n.h"
#include "local_music.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lfs_path.h"

void lm_track_fill(LmTrack *t, const char *path, const LaMeta *m) {
    memset(t, 0, sizeof *t);
    snprintf(t->path, sizeof t->path, "%s", path);
    snprintf(t->title, sizeof t->title, "%s", m->title);
    snprintf(t->artist, sizeof t->artist, "%s", m->artist);
    snprintf(t->album, sizeof t->album, "%s", m->album);
    t->track_no = m->track_no;
    t->disc_no = m->disc_no;
    t->duration_secs = m->duration_secs;
    t->kind = m->kind;
    t->pic_off = m->pic_off;
    t->pic_len = m->pic_len;
}

int lm_cmp(const LmTrack *a, const LmTrack *b) {
    const int da = a->disc_no > 0 ? a->disc_no : 1, db = b->disc_no > 0 ? b->disc_no : 1;
    if (da != db) return da < db ? -1 : 1;
    const bool ta = a->track_no > 0, tb = b->track_no > 0;
    if (ta != tb) return ta ? -1 : 1;
    if (ta && a->track_no != b->track_no) return a->track_no < b->track_no ? -1 : 1;
    return lfs_natural_cmp(lfs_path_leaf(a->path), lfs_path_leaf(b->path));
}

static int lm_cmp_q(const void *a, const void *b) { return lm_cmp((const LmTrack *)a, (const LmTrack *)b); }

void lm_sort(LmTrack *t, int n) {
    if (n > 1) qsort(t, (size_t)n, sizeof *t, lm_cmp_q);
}

// ---------------------------------------------------------------------------
//  Names
// ---------------------------------------------------------------------------

static bool is_digit(char c) { return c >= '0' && c <= '9'; }

// Copies the file's name (no folders, no extension), underscores as spaces, a leading track number removed.
static void stem_title(const char *path, char *out, int cap) {
    const char *leaf = lfs_path_leaf(path);
    char tmp[256];
    snprintf(tmp, sizeof tmp, "%s", leaf);
    char *dot = strrchr(tmp, '.');
    if (dot && dot != tmp) *dot = '\0';
    for (char *p = tmp; *p; p++) if (*p == '_') *p = ' ';
    // "03 - ", "03. ", "1-04 ", "07) ", "07 ": one or two digits, maybe "-" and one or two more, a separator, and
    // something after it.  A bare space is a separator only after a number with a leading zero ("10 Years" stays).
    const char *s = tmp;
    int i = 0;
    while (i < 2 && is_digit(s[i])) i++;
    if (i > 0) {
        int j = i;
        bool disc_track = false;                        // "1-04": a disc and a track
        if (s[j] == '-' && is_digit(s[j + 1])) {
            j++;
            for (int d = 0; d < 2 && is_digit(s[j]); d++) j++;
            disc_track = true;
        }
        const char *rest = s + j;
        bool punct = false;
        while (*rest == ' ' || *rest == '.' || *rest == ')' || *rest == '-') { if (*rest != ' ') punct = true; rest++; }
        if (rest > s + j && *rest && (punct || disc_track || s[0] == '0')) s = rest;
    }
    snprintf(out, (size_t)cap, "%s", s);
    for (size_t n = strlen(out); n > 0 && out[n - 1] == ' '; n--) out[n - 1] = '\0';
    if (!out[0]) snprintf(out, (size_t)cap, "%s", leaf);
}

void lm_display_title(const LmTrack *t, char *out, int cap) {
    if (cap <= 0) return;
    if (t->title[0]) snprintf(out, (size_t)cap, "%s", t->title);
    else stem_title(t->path, out, cap);
}

void lm_stem_title(const char *path_or_name, char *out, int cap) {
    if (cap <= 0) return;
    stem_title(path_or_name, out, cap);
}

bool lm_is_one_album(const LmTrack *t, int n) {
    const char *first = NULL;
    for (int i = 0; i < n; i++) {
        if (!t[i].album[0]) continue;
        if (!first) first = t[i].album;
        else if (strcmp(first, t[i].album) != 0) return false;
    }
    return true;
}

void lm_album_name(const LmTrack *t, int n, const char *folder, char *out, int cap) {
    if (cap <= 0) return;
    out[0] = '\0';
    int best = -1, best_count = 0;
    for (int i = 0; i < n; i++) {
        if (!t[i].album[0]) continue;
        int c = 0;
        for (int j = 0; j < n; j++) if (!strcmp(t[i].album, t[j].album)) c++;
        if (c > best_count) { best_count = c; best = i; }
    }
    if (best >= 0) { snprintf(out, (size_t)cap, "%s", t[best].album); return; }
    if (folder) {
        snprintf(out, (size_t)cap, "%s", folder);
        for (char *p = out; *p; p++) if (*p == '_') *p = ' ';
    }
}

void lm_album_artist(const LmTrack *t, int n, char *out, int cap) {
    if (cap <= 0) return;
    out[0] = '\0';
    int best = -1, best_count = 0, tagged = 0;
    for (int i = 0; i < n; i++) if (t[i].artist[0]) tagged++;
    for (int i = 0; i < n; i++) {
        if (!t[i].artist[0]) continue;
        int c = 0;
        for (int j = 0; j < n; j++) if (!strcmp(t[i].artist, t[j].artist)) c++;
        if (c > best_count) { best_count = c; best = i; }
    }
    if (best < 0) return;
    if (best_count * 10 >= tagged * 7) snprintf(out, (size_t)cap, "%s", t[best].artist);
    else snprintf(out, (size_t)cap, TR("Various Artists"));
}

int lm_pick_art(const LmTrack *t, int n) {
    for (int i = 0; i < n; i++) if (t[i].pic_len > 0) return i;
    return -1;
}

// ---------------------------------------------------------------------------
//  The cover registry
// ---------------------------------------------------------------------------

#define ART_SLOTS 8

static struct { LmArt art; unsigned id; } s_art[ART_SLOTS];
static unsigned s_next_id = 1;
static volatile int s_lock = 0;

static void lock(void) { while (__sync_lock_test_and_set(&s_lock, 1)) { } }
static void unlock(void) { __sync_lock_release(&s_lock); }

void lm_art_register(const char *path, uint64_t off, uint32_t len, char *key, int cap) {
    lock();
    // the oldest slot (or an empty one) is replaced
    int slot = 0;
    for (int i = 0; i < ART_SLOTS; i++) {
        if (s_art[i].id == 0) { slot = i; break; }
        if (s_art[i].id < s_art[slot].id) slot = i;
    }
    snprintf(s_art[slot].art.path, sizeof s_art[slot].art.path, "%s", path);
    s_art[slot].art.off = off;
    s_art[slot].art.len = len;
    s_art[slot].id = s_next_id++;
    if (s_next_id == 0) s_next_id = 1;
    const unsigned id = s_art[slot].id;
    unlock();
    if (cap > 0) snprintf(key, (size_t)cap, "lm:%u", id);
}

bool lm_art_is_key(const char *key) { return key && !strncmp(key, "lm:", 3); }

bool lm_art_find(const char *key, LmArt *out) {
    if (!lm_art_is_key(key)) return false;
    char *end = NULL;
    const unsigned long v = strtoul(key + 3, &end, 10);
    if (!end || *end || v == 0) return false;
    bool found = false;
    lock();
    for (int i = 0; i < ART_SLOTS; i++) {
        if (s_art[i].id == (unsigned)v) { *out = s_art[i].art; found = true; break; }
    }
    unlock();
    return found;
}
