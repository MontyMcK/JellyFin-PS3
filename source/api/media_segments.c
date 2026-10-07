// See media_segments.h.  Hand-rolled scanner in the style of
// trickplay_info.c: matches only direct child keys of each segment object.

#include "media_segments.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

static const char *skip_ws(const char *p, const char *end) {
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) p++;
    return p;
}

// End of the string/object/array starting at p, or NULL if unterminated.
static const char *value_end(const char *p, const char *end) {
    if (p >= end) return NULL;
    char open = *p;
    if (open == '"') {
        bool esc = false;
        for (p++; p < end; p++) {
            if (esc) esc = false;
            else if (*p == '\\') esc = true;
            else if (*p == '"') return p + 1;
        }
        return NULL;
    }
    if (open != '{' && open != '[') {
        // A scalar: runs to the next ',' '}' or ']'.
        while (p < end && *p != ',' && *p != '}' && *p != ']') p++;
        return p;
    }
    char close = (open == '{') ? '}' : ']';
    int depth = 0;
    bool in_str = false, esc = false;
    for (; p < end; p++) {
        char c = *p;
        if (esc) { esc = false; continue; }
        if (in_str) { if (c == '\\') esc = true; else if (c == '"') in_str = false; continue; }
        if (c == '"') in_str = true;
        else if (c == open) depth++;
        else if (c == close && --depth == 0) return p + 1;
    }
    return NULL;
}

// Value of the direct child `key` of the object [obj, obj_end), or NULL.
static const char *find_child(const char *obj, const char *obj_end, const char *key) {
    if (obj >= obj_end || *obj != '{') return NULL;
    const size_t key_len = strlen(key);
    const char *p = obj + 1;
    for (;;) {
        p = skip_ws(p, obj_end);
        if (p >= obj_end || *p != '"') return NULL;
        const char *kend = value_end(p, obj_end);
        if (!kend) return NULL;
        const bool match = (size_t)(kend - p - 2) == key_len && memcmp(p + 1, key, key_len) == 0;
        p = skip_ws(kend, obj_end);
        if (p >= obj_end || *p != ':') return NULL;
        p = skip_ws(p + 1, obj_end);
        if (match) return p;
        const char *vend = value_end(p, obj_end);
        if (!vend) return NULL;
        p = skip_ws(vend, obj_end);
        if (p < obj_end && *p == ',') p++;
    }
}

static bool str_is(const char *v, const char *end, const char *s) {
    const size_t n = strlen(s);
    return v && v < end && *v == '"' && (size_t)(end - v) > n + 1 &&
           memcmp(v + 1, s, n) == 0 && v[n + 1] == '"';
}

static MediaSegmentType type_of(const char *v, const char *end) {
    if (str_is(v, end, "Intro"))      return SEG_INTRO;
    if (str_is(v, end, "Recap"))      return SEG_RECAP;
    if (str_is(v, end, "Outro"))      return SEG_OUTRO;
    if (str_is(v, end, "Preview"))    return SEG_PREVIEW;
    if (str_is(v, end, "Commercial")) return SEG_COMMERCIAL;
    return SEG_UNKNOWN;
}

static bool ticks_of(const char *v, const char *end, double *secs) {
    if (!v || v >= end || (*v != '-' && (*v < '0' || *v > '9'))) return false;
    char buf[32];
    int n = 0;
    while (v < end && n < 31 && (*v == '-' || (*v >= '0' && *v <= '9'))) buf[n++] = *v++;
    buf[n] = 0;
    *secs = (double)strtoll(buf, NULL, 10) / 1e7;
    return true;
}

int media_segments_parse(const char *json, int len, MediaSegment *out, int max) {
    if (!json || len <= 0 || max <= 0) return 0;
    const char *end = json + len;
    const char *p = skip_ws(json, end);
    if (len >= 3 && (unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBB &&
        (unsigned char)p[2] == 0xBF)
        p = skip_ws(p + 3, end);            // Jellyfin's UTF-8 BOM
    const char *root_end = value_end(p, end);
    if (!root_end || *p != '{') return 0;
    const char *items = find_child(p, root_end, "Items");
    if (!items || *items != '[') return 0;
    const char *items_end = value_end(items, root_end);
    if (!items_end) return 0;

    int n = 0;
    const char *q = items + 1;
    while (n < max) {
        q = skip_ws(q, items_end);
        if (q >= items_end || *q != '{') break;
        const char *oend = value_end(q, items_end);
        if (!oend) break;
        double s, e;
        if (ticks_of(find_child(q, oend, "StartTicks"), oend, &s) &&
            ticks_of(find_child(q, oend, "EndTicks"), oend, &e) && e > s) {
            out[n].type = type_of(find_child(q, oend, "Type"), oend);
            out[n].start_secs = s;
            out[n].end_secs = e;
            n++;
        }
        q = skip_ws(oend, items_end);
        if (q < items_end && *q == ',') q++;
    }
    return n;
}

const char *media_segment_skip_label(MediaSegmentType t) {
    switch (t) {
    case SEG_INTRO:      return "Skip Intro";
    case SEG_RECAP:      return "Skip Recap";
    case SEG_OUTRO:      return "Skip Credits";
    case SEG_PREVIEW:    return "Skip Preview";
    case SEG_COMMERCIAL: return "Skip Ad";
    default:             return NULL;
    }
}
