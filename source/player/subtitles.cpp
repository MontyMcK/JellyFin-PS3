// On-device subtitle rendering — see subtitles.h for why.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "subtitles.h"
#include <stdlib.h>
// The host test (tests/test_subtitles.cpp) compiles THIS file with stubs for
// the console-only dependencies, so the parser under test is the one the PS3
// actually runs rather than a copy that can drift away from it.
#ifndef JF_SUBTITLES_TEST
#include "http.h"
#include "jellyfin_api.h"
#include "plog.h"
#endif

// The cue tables are written by the decode thread when the file is a local one (cues arrive as it reads the
// file, subs_local_*) and read by the render thread: one mutex, taken by everything that touches them.  The host
// test (JF_SUBTITLES_TEST) is single-threaded.
#ifdef JF_SUBTITLES_TEST
#define SUB_LOCK()   ((void)0)
#define SUB_UNLOCK() ((void)0)
static void sub_lock_init(void) {}
#else
#include <sys/mutex.h>
static sys_mutex_t s_sub_mtx;
static bool        s_sub_mtx_ok = false;
// Created on first use, by subs_clear() at the start of a playback, before any second thread has cues to write.
static void sub_lock_init(void) {
    if (s_sub_mtx_ok) return;
    sys_mutex_attr_t attr;
    sysMutexAttrInitialize(attr);
    if (sysMutexCreate(&s_sub_mtx, &attr) == 0) s_sub_mtx_ok = true;
}
#define SUB_LOCK()   do { if (s_sub_mtx_ok) sysMutexLock(s_sub_mtx, 0); } while (0)
#define SUB_UNLOCK() do { if (s_sub_mtx_ok) sysMutexUnlock(s_sub_mtx); } while (0)
#endif

#define SUB_MAX_CUES   4096
#define SUB_TEXT_LEN   200

typedef struct {
    u32  start_ms;
    u32  end_ms;
    char text[SUB_TEXT_LEN];
} SubCue;

static SubCue *s_cues   = NULL;
static int     s_n      = 0;
static int     s_cursor = 0;

// Which kind of track (if any) is currently loaded. Both tables/buffers
// below are kept allocated across subs_clear() regardless of mode -- see
// the existing text-cue comment for why (switching tracks mid-film should
// not have to find that memory again on an already-tight heap).
typedef enum { SUB_NONE = 0, SUB_TEXT, SUB_PGS } SubMode;
static SubMode s_mode = SUB_NONE;

// ---- PGS state (see subtitles.h's memory note) ----------------------------
#define PGS_SUP_MAX (4 * 1024 * 1024)
static uint8_t  *s_pgs_raw     = NULL;   // the whole .sup body, kept resident
static int        s_pgs_raw_len = 0;
static PgsIndex   s_pgs_index;
static uint32_t   s_pgs_scanned = 0;     // local files: how far into s_pgs_raw the index reaches

static uint32_t  *s_pgs_px      = NULL;  // decoded-bitmap scratch, grows on demand
static int         s_pgs_px_cap = 0;     // pixels
static PgsBitmap   s_pgs_cur;
static bool        s_pgs_cur_ok    = false;
static int         s_pgs_cur_epoch = -1; // which epoch s_pgs_cur holds, -1 = none

bool subs_active(void) { return s_mode != SUB_NONE; }

bool subs_is_pgs(void) { return s_mode == SUB_PGS; }

void subs_reset_cursor(void) { SUB_LOCK(); s_cursor = 0; SUB_UNLOCK(); }

static void clear_locked(void)
{
    s_mode = SUB_NONE;
    s_n = 0;
    s_cursor = 0;
    // The tables themselves are kept: switching tracks mid-film should not
    // have to find that memory again on a heap that VDEC and the ring have
    // already carved up, and holding it costs nothing the player was going
    // to use.
    s_pgs_index.n   = 0;
    s_pgs_raw_len   = 0;
    s_pgs_scanned   = 0;
    s_pgs_cur_epoch = -1;
    s_pgs_cur_ok    = false;
}

void subs_clear(void)
{
    sub_lock_init();
    SUB_LOCK();
    clear_locked();
    SUB_UNLOCK();
}

// "00:01:23,456" -> milliseconds.  Accepts '.' as well as ',' because WebVTT
// converted to SubRip sometimes keeps the dot.
static bool parse_ts(const char *p, u32 *out)
{
    unsigned h, m, s, ms;
    if (sscanf(p, "%u:%u:%u,%u", &h, &m, &s, &ms) != 4 &&
        sscanf(p, "%u:%u:%u.%u", &h, &m, &s, &ms) != 4)
        return false;
    *out = ((h * 60u + m) * 60u + s) * 1000u + ms;
    return true;
}

// Strip the inline tags SubRip carries (<i>, <b>, {\an8} and friends).  We
// have one font and one position, so a tag we cannot honour is better removed
// than drawn literally as "<i>".
static void strip_tags(char *s)
{
    char *w = s;
    for (char *r = s; *r; r++) {
        if (*r == '<' || *r == '{') {
            const char close = (*r == '<') ? '>' : '}';
            const char *q = r;
            while (*q && *q != close) q++;
            if (*q == close) { r = (char *)q; continue; }   // skip the tag
        }
        *w++ = *r;
    }
    *w = '\0';
}

// One SubRip block: an optional index line, a timing line, then text until a
// blank line.  Returns the pointer just past the block, or NULL at the end.
static const char *parse_block(const char *p, SubCue *cue)
{
    while (*p == '\r' || *p == '\n') p++;
    if (!*p) return NULL;

    // Index line, if present — SubRip usually has one, some writers omit it.
    const char *arrow = strstr(p, "-->");
    if (!arrow) return NULL;
    const char *line_start = arrow;
    while (line_start > p && line_start[-1] != '\n') line_start--;

    if (!parse_ts(line_start, &cue->start_ms)) return NULL;
    const char *to = arrow + 3;
    while (*to == ' ') to++;
    if (!parse_ts(to, &cue->end_ms)) return NULL;

    const char *t = strchr(arrow, '\n');
    if (!t) return NULL;
    t++;

    int n = 0;
    cue->text[0] = '\0';
    while (*t) {
        // A blank line ends the block.
        if (*t == '\n' || (*t == '\r' && t[1] == '\n')) break;
        const char *eol = strchr(t, '\n');
        int len = eol ? (int)(eol - t) : (int)strlen(t);
        while (len > 0 && (t[len - 1] == '\r')) len--;
        if (n && n < SUB_TEXT_LEN - 1) cue->text[n++] = '\n';
        if (len > SUB_TEXT_LEN - 1 - n) len = SUB_TEXT_LEN - 1 - n;
        if (len > 0) { memcpy(cue->text + n, t, len); n += len; }
        cue->text[n] = '\0';
        if (!eol) { t += strlen(t); break; }
        t = eol + 1;
    }
    strip_tags(cue->text);
    return t;
}

int subs_load(const char *item_id, const char *media_source_id, int stream_index)
{
    subs_clear();
    if (!item_id || !item_id[0] || stream_index < 0) return -1;

    if (!s_cues) {
        s_cues = (SubCue *)malloc(sizeof(SubCue) * SUB_MAX_CUES);
        if (!s_cues) { plog("subs: out of memory for the cue table"); return -1; }
    }

    // Ask for SubRip whatever the source format is: Jellyfin converts ASS,
    // SSA and WebVTT server-side, which loses styling but keeps the words and
    // the timing -- and costs no video transcode, which is the entire point.
    char url[640];
    snprintf(url, sizeof(url),
             "%s/Videos/%s/%s/Subtitles/%d/Stream.srt",
             g_server, item_id,
             (media_source_id && media_source_id[0]) ? media_source_id : item_id,
             stream_index);

    static char *body = NULL;
    if (!body) {
        body = (char *)malloc(RESPONSE_SIZE);
        if (!body) { plog("subs: out of memory for the download"); return -1; }
    }
    const int rc = http_request(HTTP_GET, url, NULL, g_token, body, RESPONSE_SIZE);
    if (rc <= 0) {
        char b[96];
        snprintf(b, sizeof(b), "subs: fetch failed rc=%d idx=%d", rc, stream_index);
        plog(b);
        return -1;
    }

    const char *p = body;
    while (s_n < SUB_MAX_CUES) {
        SubCue *c = &s_cues[s_n];
        const char *next = parse_block(p, c);
        if (!next) break;
        p = next;
        // Drop cues that say nothing or end before they start rather than
        // carrying them into the lookup.
        if (c->text[0] && c->end_ms > c->start_ms) s_n++;
    }

    char b[128];
    snprintf(b, sizeof(b), "subs: loaded %d cues (%d bytes) idx=%d%s",
             s_n, rc, stream_index,
             (s_n == SUB_MAX_CUES) ? " [TABLE FULL]" : "");
    plog(b);
    s_cursor = 0;
    if (s_n <= 0) return -1;
    s_mode = SUB_TEXT;
    return s_n;
}

int subs_load_pgs(const char *item_id, const char *media_source_id, int stream_index)
{
    subs_clear();
    if (!item_id || !item_id[0] || stream_index < 0) return -1;

    if (!s_pgs_raw) {
        s_pgs_raw = (uint8_t *)malloc(PGS_SUP_MAX);
        if (!s_pgs_raw) { plog("subs: out of memory for the .sup buffer"); return -1; }
    }

    // Unlike Stream.srt, Jellyfin serves this format's raw bytes rather than
    // transcoding it -- there is nothing to transcode a bitmap subtitle
    // INTO that still counts as text, so .sup is the extracted elementary
    // stream verbatim (see subtitles_pgs.h's format PROVENANCE note).
    char url[640];
    snprintf(url, sizeof(url),
             "%s/Videos/%s/%s/Subtitles/%d/Stream.sup",
             g_server, item_id,
             (media_source_id && media_source_id[0]) ? media_source_id : item_id,
             stream_index);

    const int rc = http_fetch_binary(url, g_token, s_pgs_raw, PGS_SUP_MAX);
    if (rc <= 0) {
        char b[112];
        snprintf(b, sizeof(b), "subs: pgs fetch failed rc=%d idx=%d", rc, stream_index);
        plog(b);
        return -1;
    }
    if (rc >= PGS_SUP_MAX - 1) {
        // plog() itself truncates at 127 chars -- keep this short rather
        // than write a long explanation that never reaches the log file.
        char b[80];
        snprintf(b, sizeof(b),
                 "subs: pgs fetch hit %dMB cap -- likely TRUNCATED",
                 PGS_SUP_MAX / (1024 * 1024));
        plog(b);
    }
    s_pgs_raw_len = rc;

    const int n = pgs_build_index(s_pgs_raw, s_pgs_raw_len, &s_pgs_index);
    char b[112];
    snprintf(b, sizeof(b), "subs: pgs loaded %d epochs (%d bytes) idx=%d",
             n, s_pgs_raw_len, stream_index);
    plog(b);
    if (n <= 0) return -1;
    s_mode = SUB_PGS;
    return n;
}

static const char *text_at_locked(u64 pts_ms)
{
    if (s_n <= 0) return NULL;
    const u32 t = (u32)pts_ms;

    // Normal playback walks forward a cue at a time, so start from where we
    // left off.  If the clock has gone backwards -- a seek -- fall back to a
    // binary search rather than scanning the whole table.
    if (s_cursor >= s_n || s_cues[s_cursor].end_ms < t) {
        if (s_cursor < s_n && s_cues[s_cursor].end_ms < t &&
            s_cursor + 8 < s_n && s_cues[s_cursor + 8].end_ms >= t) {
            while (s_cursor < s_n && s_cues[s_cursor].end_ms < t) s_cursor++;
        } else {
            int lo = 0, hi = s_n - 1, best = 0;
            while (lo <= hi) {
                int mid = (lo + hi) / 2;
                if (s_cues[mid].end_ms < t) { lo = mid + 1; }
                else                        { best = mid; hi = mid - 1; }
            }
            s_cursor = best;
        }
    } else if (s_cursor > 0 && s_cues[s_cursor - 1].end_ms >= t) {
        int lo = 0, hi = s_n - 1, best = 0;
        while (lo <= hi) {
            int mid = (lo + hi) / 2;
            if (s_cues[mid].end_ms < t) { lo = mid + 1; }
            else                        { best = mid; hi = mid - 1; }
        }
        s_cursor = best;
    }

    if (s_cursor >= s_n) return NULL;
    const SubCue *c = &s_cues[s_cursor];
    if (t >= c->start_ms && t <= c->end_ms) return c->text;
    return NULL;
}

static const PgsBitmap *pgs_at_locked(u64 pts_ms)
{
    if (s_mode != SUB_PGS || s_pgs_index.n <= 0) return NULL;

    const int idx = pgs_find_epoch(&s_pgs_index, (u32)pts_ms);
    if (idx < 0) return NULL;

    const PgsEpoch *e = &s_pgs_index.epoch[idx];
    if (!e->has_object) return NULL;   // an explicit "hide subtitle" epoch

    if (idx == s_pgs_cur_epoch) return s_pgs_cur_ok ? &s_pgs_cur : NULL;

    // A new display set: decode it now. Grown on demand rather than
    // pre-allocated at PGS_MAX_W*PGS_MAX_H up front -- most cropped dialogue
    // bitmaps are far smaller than that worst case, so this only pays for
    // what a given disc's subtitles actually need (same reasoning
    // thumbnail_cache.cpp's slot sizing comment gives).
    if (!s_pgs_px) {
        s_pgs_px_cap = 800 * 140;   // a modest first guess; grows below if wrong
        s_pgs_px = (uint32_t *)malloc((size_t)s_pgs_px_cap * 4);
    }
    bool ok = s_pgs_px &&
        pgs_decode_epoch(s_pgs_raw, s_pgs_raw_len, e->pcs_offset,
                         s_pgs_px, s_pgs_px_cap, &s_pgs_cur);
    if (!ok && s_pgs_px && s_pgs_cur.width > 0 && s_pgs_cur.height > 0) {
        const long need = (long)s_pgs_cur.width * s_pgs_cur.height;
        if (need <= (long)PGS_MAX_W * PGS_MAX_H) {
            uint32_t *grown = (uint32_t *)realloc(s_pgs_px, (size_t)need * 4);
            if (grown) {
                s_pgs_px     = grown;
                s_pgs_px_cap = (int)need;
                ok = pgs_decode_epoch(s_pgs_raw, s_pgs_raw_len, e->pcs_offset,
                                      s_pgs_px, s_pgs_px_cap, &s_pgs_cur);
            }
        } else {
            char b[112];
            snprintf(b, sizeof(b),
                     "subs: pgs object %dx%d exceeds PGS_MAX_W/H -- skipped",
                     s_pgs_cur.width, s_pgs_cur.height);
            plog(b);
        }
    }

    s_pgs_cur_epoch = idx;
    s_pgs_cur_ok    = ok;
    return ok ? &s_pgs_cur : NULL;
}

const char *subs_text_at(u64 pts_ms)
{
    SUB_LOCK();
    const char *t = text_at_locked(pts_ms);
    SUB_UNLOCK();
    return t;                  // into the table: cues are only ever appended while it is read, and cleared with the reader stopped
}

const PgsBitmap *subs_pgs_at(u64 pts_ms)
{
    SUB_LOCK();
    const PgsBitmap *b = pgs_at_locked(pts_ms);
    SUB_UNLOCK();
    return b;
}

// ---- local files ----------------------------------------------------------------------------------------------

void subs_local_begin(bool pgs)
{
    sub_lock_init();
    SUB_LOCK();
    clear_locked();
    if (pgs && !s_pgs_raw) s_pgs_raw = (uint8_t *)malloc(PGS_SUP_MAX);
    if (!pgs && !s_cues) s_cues = (SubCue *)malloc(sizeof(SubCue) * SUB_MAX_CUES);
    if (pgs ? s_pgs_raw != NULL : s_cues != NULL) s_mode = pgs ? SUB_PGS : SUB_TEXT;
#ifndef JF_SUBTITLES_TEST
    else plog("subs: out of memory for a local subtitle track");
#endif
    SUB_UNLOCK();
}

void subs_local_restart(void)
{
    SUB_LOCK();
    const SubMode keep = s_mode;
    clear_locked();
    s_mode = keep;
    SUB_UNLOCK();
}

void subs_local_add_text(u32 start_ms, u32 end_ms, const char *text)
{
    if (!text || !text[0]) return;
    SUB_LOCK();
    if (s_mode == SUB_TEXT) {
        if (s_n >= SUB_MAX_CUES) {
            // More cues than the table holds (a long film, an anime): playback goes forward, so the older half goes.
            const int drop = SUB_MAX_CUES / 2;
            memmove(s_cues, s_cues + drop, sizeof(SubCue) * (size_t)(s_n - drop));
            s_n -= drop;
            s_cursor = 0;
        }
        SubCue *c = &s_cues[s_n];
        c->start_ms = start_ms;
        c->end_ms   = end_ms > start_ms ? end_ms : start_ms + 2500;
        snprintf(c->text, SUB_TEXT_LEN, "%s", text);
        s_n++;
    }
    SUB_UNLOCK();
}

// Display sets arrive in time order and only the present matters: when the buffer or the index would overflow, the
// older display sets go, all but the last two (the one on screen and the one before it).
static void pgs_make_room_locked(int len)
{
    if (s_pgs_raw_len + len <= PGS_SUP_MAX && s_pgs_index.n + 4 < PGS_MAX_EPOCHS) return;
    uint32_t from = (uint32_t)s_pgs_raw_len;                    // nothing indexed: nothing to keep
    if (s_pgs_index.n >= 2) from = s_pgs_index.epoch[s_pgs_index.n - 2].pcs_offset;
    else if (s_pgs_index.n == 1) from = s_pgs_index.epoch[0].pcs_offset;
    if (from > (uint32_t)s_pgs_raw_len) from = (uint32_t)s_pgs_raw_len;
    memmove(s_pgs_raw, s_pgs_raw + from, (size_t)s_pgs_raw_len - from);
    s_pgs_raw_len -= (int)from;
    s_pgs_index.n   = 0;
    s_pgs_scanned   = pgs_index_extend(s_pgs_raw, s_pgs_raw_len, 0, &s_pgs_index);
    s_pgs_cur_epoch = -1;
    s_pgs_cur_ok    = false;
}

void subs_local_add_pgs(const uint8_t *sup, int len)
{
    if (!sup || len <= 0) return;
    SUB_LOCK();
    if (s_mode == SUB_PGS && s_pgs_raw) pgs_make_room_locked(len);
    if (s_mode == SUB_PGS && s_pgs_raw && s_pgs_raw_len + len <= PGS_SUP_MAX) {
        memcpy(s_pgs_raw + s_pgs_raw_len, sup, (size_t)len);
        s_pgs_raw_len += len;
        s_pgs_scanned = pgs_index_extend(s_pgs_raw, s_pgs_raw_len, s_pgs_scanned, &s_pgs_index);
    }
    SUB_UNLOCK();
}
