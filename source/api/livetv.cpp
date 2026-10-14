// Live TV parsing and arithmetic.  See livetv.h.  No PS3 dependencies.

#include "livetv.h"
#include "json_lite.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------------------
//  Time
// ---------------------------------------------------------------------------

#define TICKS_PER_SEC     10000000ULL
// Seconds from 0001-01-01 to 1970-01-01 (719,162 days).
#define UNIX_EPOCH_SECS   62135596800ULL

uint64_t jf_ticks_from_unix(uint64_t unix_secs) {
    return (unix_secs + UNIX_EPOCH_SECS) * TICKS_PER_SEC;
}

void livetv_format_hm(uint64_t ticks, int utc_offset_secs, char *out, int cap) {
    const int64_t unix_secs = (int64_t)(ticks / TICKS_PER_SEC) - (int64_t)UNIX_EPOCH_SECS
                              + utc_offset_secs;
    int64_t day_secs = unix_secs % 86400;
    if (day_secs < 0) day_secs += 86400;
    snprintf(out, (size_t)cap, "%02d:%02d", (int)(day_secs / 3600), (int)((day_secs / 60) % 60));
}

// Days since 1970-01-01 for a civil date (Howard Hinnant's algorithm).
static int64_t days_from_civil(int y, unsigned m, unsigned d) {
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

static void civil_from_days(int64_t z, int *y, unsigned *m, unsigned *d) {
    z += 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = (unsigned)(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const int yy = (int)yoe + (int)(era * 400);
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    *d = doy - (153 * mp + 2) / 5 + 1;
    *m = mp + (mp < 10 ? 3 : -9);
    *y = yy + (*m <= 2);
}

static bool two_digits(const char *p, int *out) {
    if (p[0] < '0' || p[0] > '9' || p[1] < '0' || p[1] > '9') return false;
    *out = (p[0] - '0') * 10 + (p[1] - '0');
    return true;
}

uint64_t jf_ticks_from_iso8601(const char *s) {
    if (!s) return 0;
    int Y = 0, M = 0, D = 0, h = 0, mi = 0, sec = 0;
    // YYYY-MM-DDTHH:MM:SS
    for (int i = 0; i < 4; i++) {
        if (s[i] < '0' || s[i] > '9') return 0;
        Y = Y * 10 + (s[i] - '0');
    }
    if (s[4] != '-' || !two_digits(s + 5, &M) || s[7] != '-' || !two_digits(s + 8, &D) ||
        (s[10] != 'T' && s[10] != ' ') || !two_digits(s + 11, &h) || s[13] != ':' ||
        !two_digits(s + 14, &mi) || s[16] != ':' || !two_digits(s + 17, &sec))
        return 0;
    if (M < 1 || M > 12 || D < 1 || D > 31 || h > 23 || mi > 59 || sec > 60) return 0;
    const char *p = s + 19;
    uint64_t frac = 0;                          // in 100 ns
    if (*p == '.') {
        p++;
        int digits = 0;
        while (*p >= '0' && *p <= '9') {
            if (digits < 7) { frac = frac * 10 + (uint64_t)(*p - '0'); digits++; }
            p++;
        }
        while (digits < 7) { frac *= 10; digits++; }
    }
    int64_t offset = 0;                         // seconds to ADD to get UTC
    if (*p == '+' || *p == '-') {
        int oh = 0, om = 0;
        if (!two_digits(p + 1, &oh)) return 0;
        const char *q = p + 3;
        if (*q == ':') q++;
        if (q[0] >= '0' && q[0] <= '9') two_digits(q, &om);
        offset = (int64_t)(oh * 3600 + om * 60) * (*p == '+' ? -1 : 1);
    }
    const int64_t days = days_from_civil(Y, (unsigned)M, (unsigned)D);
    const int64_t unix_secs = days * 86400 + h * 3600 + mi * 60 + sec + offset;
    if (unix_secs < -(int64_t)UNIX_EPOCH_SECS) return 0;
    return ((uint64_t)(unix_secs + (int64_t)UNIX_EPOCH_SECS)) * TICKS_PER_SEC + frac;
}

int jf_iso8601_from_ticks(uint64_t ticks, char *out, int cap) {
    if (!out || cap < 29) return 0;
    const uint64_t secs = ticks / TICKS_PER_SEC;
    const uint64_t frac = ticks % TICKS_PER_SEC;
    if (secs < UNIX_EPOCH_SECS) return 0;
    const int64_t unix_secs = (int64_t)(secs - UNIX_EPOCH_SECS);
    int64_t days = unix_secs / 86400;
    int64_t rem  = unix_secs % 86400;
    if (rem < 0) { rem += 86400; days--; }
    int y; unsigned m, d;
    civil_from_days(days, &y, &m, &d);
    return snprintf(out, (size_t)cap, "%04d-%02u-%02uT%02d:%02d:%02d.%07lluZ",
                    y, m, d, (int)(rem / 3600), (int)((rem / 60) % 60), (int)(rem % 60),
                    (unsigned long long)frac);
}

// ---------------------------------------------------------------------------
//  The list's rules
// ---------------------------------------------------------------------------

// Channel numbers are strings ("2", "5.1", "101"): compare by value, a number
// that does not parse sorting after those that do.
static bool number_value(const char *s, double *v) {
    if (!s[0]) return false;
    char *end = NULL;
    *v = strtod(s, &end);
    return end != s;
}

bool livetv_number_value(const char *s, double *v) { return number_value(s, v); }

int livetv_page_ranges(const float *nums, int n, const int *bases, int n_bases,
                       int *first, int *count, int *cat, int max) {
    if (!nums || !bases || !first || !count || !cat || n <= 0 || n_bases <= 0 || max <= 0) return 0;
    int pages = 0, i = 0;
    for (int k = 0; k < n_bases && pages < max; k++) {
        const bool last = k + 1 >= n_bases;
        const int start = i;
        while (i < n && (last || nums[i] < (float)bases[k + 1])) i++;
        if (i > start) { first[pages] = start; count[pages] = i - start; cat[pages] = k; pages++; }
    }
    return pages;
}

static int channel_cmp(const JFChannel *a, const JFChannel *b) {
    if (a->favourite != b->favourite) return a->favourite ? -1 : 1;
    double va = 0, vb = 0;
    const bool ha = number_value(a->number, &va), hb = number_value(b->number, &vb);
    if (ha != hb) return ha ? -1 : 1;
    if (ha && va != vb) return va < vb ? -1 : 1;
    return strcmp(a->name, b->name);
}

void livetv_sort_channels(JFChannel *c, int n) {
    // Insertion sort: stable, and the list is a few hundred at most.
    for (int i = 1; i < n; i++) {
        JFChannel t = c[i];
        int j = i - 1;
        while (j >= 0 && channel_cmp(&c[j], &t) > 0) { c[j + 1] = c[j]; j--; }
        c[j + 1] = t;
    }
}

int livetv_progress_permille(uint64_t now, uint64_t start, uint64_t end) {
    if (!start || !end || end <= start) return -1;
    if (now <= start) return 0;
    if (now >= end) return 1000;
    return (int)((now - start) * 1000 / (end - start));
}

int livetv_step_channel(int cur, int n, int dir) {
    if (n <= 0) return -1;
    if (cur < 0) return dir >= 0 ? 0 : n - 1;
    if (cur >= n) return dir >= 0 ? 0 : n - 1;
    return ((cur + (dir >= 0 ? 1 : -1)) % n + n) % n;
}

int livetv_programme_at(const JFProgram *progs, int n, uint64_t t) {
    int best = -1;
    for (int i = 0; i < n; i++) {
        if (progs[i].end_ticks <= progs[i].start_ticks) continue;     // never airs
        if (progs[i].start_ticks > t || progs[i].end_ticks <= t) continue;
        if (best < 0 || progs[i].start_ticks > progs[best].start_ticks) best = i;
    }
    return best;
}

bool livetv_debounce_ready(uint64_t last_press_us, uint64_t now_us, uint64_t quiet_us) {
    return now_us >= last_press_us && now_us - last_press_us >= quiet_us;
}

// ---------------------------------------------------------------------------
//  Parsing
// ---------------------------------------------------------------------------

static void str_field(const char *obj, const char *end, const char *key,
                      char *out, int cap) {
    out[0] = '\0';
    const char *v = find_top_value(obj, end, key);
    if (v) read_json_string(v, end, out, cap);
}

static bool bool_field(const char *obj, const char *end, const char *key) {
    const char *v = find_top_value(obj, end, key);
    return v && read_json_bool(v, end);
}

static uint64_t time_field(const char *obj, const char *end, const char *key) {
    char buf[48];
    str_field(obj, end, key, buf, sizeof buf);
    return buf[0] ? jf_ticks_from_iso8601(buf) : 0;
}

// The Items array of a reply: points at its first '{' search start, or NULL.
static const char *items_start(const char *json, const char *end, const char **arr_end) {
    const char *top = skip_ws(json, end);
    const char *v = find_top_value(top, end, "Items");
    if (!v || v >= end || *v != '[') return NULL;
    *arr_end = end;
    return v + 1;
}

// Calls fn for every object in the array at p; stops at max.  Returns the
// count and whether the buffer ended inside an object.
typedef void (*ObjFn)(const char *obj, const char *oend, void *dst);

static int each_object(const char *p, const char *end, int max, void *out,
                       size_t stride, ObjFn fn, bool *truncated) {
    int n = 0;
    if (truncated) *truncated = false;
    while (p < end && n < max) {
        p = skip_ws(p, end);
        if (p >= end || *p == ']') break;
        if (*p == ',') { p++; continue; }
        if (*p != '{') { p++; continue; }
        const char *oe = object_end(p, end);
        if (!oe) { if (truncated) *truncated = true; break; }
        fn(p, oe, (char *)out + (size_t)n * stride);
        n++;
        p = oe;
    }
    return n;
}

static void channel_fn(const char *o, const char *e, void *dst) {
    JFChannel *c = (JFChannel *)dst;
    memset(c, 0, sizeof *c);
    str_field(o, e, "Id", c->id, sizeof c->id);
    str_field(o, e, "Name", c->name, sizeof c->name);
    str_field(o, e, "ChannelNumber", c->number, sizeof c->number);
    if (!c->number[0]) str_field(o, e, "Number", c->number, sizeof c->number);
    const char *tags = find_top_value(o, e, "ImageTags");
    if (tags && *tags == '{') {
        const char *te = object_end(tags, e);
        if (te) str_field(tags, te, "Primary", c->logo_tag, sizeof c->logo_tag);
    }
    const char *ud = find_top_value(o, e, "UserData");
    if (ud && *ud == '{') {
        const char *ue = object_end(ud, e);
        if (ue) c->favourite = bool_field(ud, ue, "IsFavorite");
    }
    const char *cp = find_top_value(o, e, "CurrentProgram");
    if (cp && *cp == '{') {
        const char *ce = object_end(cp, e);
        if (ce) {
            str_field(cp, ce, "Name", c->now_title, sizeof c->now_title);
            c->now_start_ticks = time_field(cp, ce, "StartDate");
            c->now_end_ticks   = time_field(cp, ce, "EndDate");
        }
    }
}

static void program_fn(const char *o, const char *e, void *dst) {
    JFProgram *p = (JFProgram *)dst;
    memset(p, 0, sizeof *p);
    str_field(o, e, "Id", p->id, sizeof p->id);
    str_field(o, e, "ChannelId", p->channel_id, sizeof p->channel_id);
    str_field(o, e, "Name", p->name, sizeof p->name);
    str_field(o, e, "EpisodeTitle", p->episode_title, sizeof p->episode_title);
    p->start_ticks = time_field(o, e, "StartDate");
    p->end_ticks   = time_field(o, e, "EndDate");
    p->is_movie    = bool_field(o, e, "IsMovie");
    p->is_series   = bool_field(o, e, "IsSeries");
    p->is_news     = bool_field(o, e, "IsNews");
    p->is_sports   = bool_field(o, e, "IsSports");
    p->is_kids     = bool_field(o, e, "IsKids");
    p->is_live     = bool_field(o, e, "IsLive");
    p->is_premiere = bool_field(o, e, "IsPremiere");
}

static int parse_items(const char *json, int len, void *out, int max, size_t stride,
                       ObjFn fn, int *total, bool *truncated) {
    if (truncated) *truncated = false;
    if (!json || len <= 0 || !out || max <= 0) { if (total) *total = 0; return 0; }
    const char *end = json + len;
    const char *arr_end = end;
    const char *p = items_start(json, end, &arr_end);
    int n = 0;
    if (p) n = each_object(p, end, max, out, stride, fn, truncated);
    if (total) {
        const char *top = skip_ws(json, end);
        const char *v = find_top_value(top, end, "TotalRecordCount");
        *total = v ? (int)read_json_int(v, end, n) : n;
    }
    return n;
}

int livetv_parse_channels(const char *json, int len, JFChannel *out, int max,
                          int *total, bool *truncated) {
    return parse_items(json, len, out, max, sizeof(JFChannel), channel_fn, total, truncated);
}

int livetv_parse_programs(const char *json, int len, JFProgram *out, int max,
                          int *total, bool *truncated) {
    return parse_items(json, len, out, max, sizeof(JFProgram), program_fn, total, truncated);
}
