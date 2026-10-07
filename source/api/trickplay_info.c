// See trickplay_info.h. Small hand-rolled scanner in the same style as
// media_sources.cpp: brace-depth aware, matches only DIRECT child keys of
// an object so a nested object's keys are never mistaken for the parent's.

#include "trickplay_info.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static const char *skip_ws(const char *p, const char *end) {
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) p++;
    return p;
}

// End of the object/array/string starting at *p (which must be '{', '[' or
// '"'), or NULL if it runs off the end unterminated.
static const char *value_end(const char *p, const char *end) {
    if (p >= end) return NULL;
    char open = *p;
    if (open == '"') {
        p++;
        bool esc = false;
        while (p < end) {
            if (esc) esc = false;
            else if (*p == '\\') esc = true;
            else if (*p == '"') return p + 1;
            p++;
        }
        return NULL;
    }
    if (open != '{' && open != '[') return NULL;
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

// Direct child of the object [obj, obj_end) named `key` -- returns a pointer
// to its value (whatever follows the ':'), or NULL if absent. Never
// descends into a nested object/array's own keys.
static const char *find_child(const char *obj, const char *obj_end,
                              const char *key) {
    if (obj >= obj_end || *obj != '{') return NULL;
    const int key_len = (int)strlen(key);
    const char *p = obj + 1;
    while (p < obj_end) {
        p = skip_ws(p, obj_end);
        if (p >= obj_end || *p == '}') return NULL;
        if (*p != '"') return NULL;   // malformed: key must be a string
        const char *kend = value_end(p, obj_end);
        if (!kend) return NULL;
        bool match = (kend - p - 2 == key_len) && memcmp(p + 1, key, key_len) == 0;
        p = skip_ws(kend, obj_end);
        if (p >= obj_end || *p != ':') return NULL;
        p = skip_ws(p + 1, obj_end);
        if (match) return p;
        const char *vend = value_end(p, obj_end);
        if (vend) { p = vend; }
        else { while (p < obj_end && *p != ',' && *p != '}') p++; }
        p = skip_ws(p, obj_end);
        if (p < obj_end && *p == ',') p++;
    }
    return NULL;
}

// The FIRST direct child key of [obj, obj_end), any name -- for the
// width-keyed inner dictionary, whose key we don't know in advance.
// Writes the key text (unescaped-enough for a plain numeric key) into
// key_out and returns a pointer to its value, or NULL if the object is empty.
static const char *first_child(const char *obj, const char *obj_end,
                               char *key_out, int key_cap) {
    if (obj >= obj_end || *obj != '{') return NULL;
    const char *p = skip_ws(obj + 1, obj_end);
    if (p >= obj_end || *p != '"') return NULL;
    const char *kend = value_end(p, obj_end);
    if (!kend) return NULL;
    int klen = (int)(kend - p - 2);
    if (klen < 0) klen = 0;
    if (klen >= key_cap) klen = key_cap - 1;
    memcpy(key_out, p + 1, (size_t)klen);
    key_out[klen] = '\0';
    p = skip_ws(kend, obj_end);
    if (p >= obj_end || *p != ':') return NULL;
    return skip_ws(p + 1, obj_end);
}

static int read_int(const char *p, const char *end, int def) {
    p = skip_ws(p, end);
    if (p >= end || (*p != '-' && (*p < '0' || *p > '9'))) return def;
    return (int)strtol(p, NULL, 10);
}

bool trickplay_parse_info(const char *json, const char *media_source_id,
                          TrickplayInfo *out) {
    memset(out, 0, sizeof(*out));
    if (!json || !media_source_id || !media_source_id[0]) return false;

    const char *tp_key = strstr(json, "\"Trickplay\"");
    if (!tp_key) return false;
    const char *p = strchr(tp_key, ':');
    if (!p) return false;
    p = skip_ws(p + 1, json + strlen(json));
    if (p >= json + strlen(json) || *p != '{') return false;   // null/absent
    const char *tp_end = value_end(p, json + strlen(json));
    if (!tp_end) return false;
    const char *tp_obj = p;

    const char *src_val = find_child(tp_obj, tp_end, media_source_id);
    if (!src_val || *src_val != '{') return false;
    const char *src_end = value_end(src_val, tp_end);
    if (!src_end) return false;

    char width_key[8];
    const char *info_val = first_child(src_val, src_end, width_key, sizeof(width_key));
    if (!info_val || *info_val != '{') return false;
    const char *info_end = value_end(info_val, src_end);
    if (!info_end) return false;

    const char *v;
    v = find_child(info_val, info_end, "Width");
    out->width = v ? read_int(v, info_end, 0) : 0;
    v = find_child(info_val, info_end, "Height");
    out->height = v ? read_int(v, info_end, 0) : 0;
    v = find_child(info_val, info_end, "TileWidth");
    out->tile_cols = v ? read_int(v, info_end, 0) : 0;
    v = find_child(info_val, info_end, "TileHeight");
    out->tile_rows = v ? read_int(v, info_end, 0) : 0;
    v = find_child(info_val, info_end, "ThumbnailCount");
    out->thumbnail_count = v ? read_int(v, info_end, 0) : 0;
    v = find_child(info_val, info_end, "Interval");
    out->interval_ms = v ? read_int(v, info_end, 0) : 0;
    snprintf(out->width_key, sizeof(out->width_key), "%s", width_key);

    return out->width > 0 && out->height > 0 &&
           out->tile_cols > 0 && out->tile_rows > 0 && out->interval_ms > 0;
}
