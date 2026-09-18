// On-device subtitle rendering — see subtitles.h for why.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "subtitles.h"
// The host test (tests/test_subtitles.cpp) compiles THIS file with stubs for
// the console-only dependencies, so the parser under test is the one the PS3
// actually runs rather than a copy that can drift away from it.
#ifndef JF_SUBTITLES_TEST
#include "http.h"
#include "jellyfin_api.h"
#include "plog.h"
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

bool subs_active(void) { return s_n > 0; }

void subs_reset_cursor(void) { s_cursor = 0; }

void subs_clear(void)
{
    s_n = 0;
    s_cursor = 0;
    // The table itself is kept: switching tracks mid-film should not have to
    // find 850 KB again on a heap that VDEC and the ring have already carved
    // up, and holding it costs nothing the player was going to use.
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
    return s_n > 0 ? s_n : -1;
}

const char *subs_text_at(u64 pts_ms)
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
