// The pure half of chapters.h.
#include <stdio.h>
#include <string.h>

#include "chapters.h"
#include "json_lite.h"

int jf_parse_chapters(const char *json, int len, JFChapters *out) {
    memset(out, 0, sizeof *out);
    if (!json || len <= 0) return 0;
    const char *end = json + len;
    const char *root = skip_ws(json, end);
    if (end - root >= 3 && (unsigned char)root[0] == 0xEF && (unsigned char)root[1] == 0xBB &&
        (unsigned char)root[2] == 0xBF)
        root = skip_ws(root + 3, end);                       // Jellyfin's JSON can carry a BOM
    if (root >= end || *root != '{') return 0;

    const char *p = find_top_value(root, end, "Chapters");
    if (!p || p >= end || *p != '[') return 0;
    p++;
    while (p < end && out->n < JF_MAX_CHAPTERS) {
        p = skip_ws(p, end);
        if (p >= end || *p == ']') break;
        if (*p == ',') { p++; continue; }
        if (*p != '{') break;
        const char *oe = object_end(p, end);
        if (!oe) break;                                       // cut short: keep what is whole
        const char *t = find_top_value(p, oe, "StartPositionTicks");
        if (t) {
            const long long ticks = read_json_int(t, oe, -1);
            const double secs = (double)ticks / 1e7;
            if (ticks >= 0 && (out->n == 0 || secs > out->ch[out->n - 1].start_secs + 0.5)) {
                JFChapter &c = out->ch[out->n];
                c.start_secs = secs;
                c.name[0] = '\0';
                const char *nv = find_top_value(p, oe, "Name");
                if (nv) read_json_string(nv, oe, c.name, (int)sizeof c.name);
                out->n++;
            }
        }
        p = oe;
    }
    return out->n;
}

int jf_chapter_index_at(const JFChapters *c, double pos_secs) {
    int at = -1;
    if (!c) return at;
    for (int i = 0; i < c->n; i++)
        if (c->ch[i].start_secs <= pos_secs + 0.5) at = i;
    return at;
}

// Bytes of `s` that fit in `max` without cutting a UTF-8 character.
static int utf8_prefix(const char *s, int max) {
    int n = 0;
    while (s[n] && n < max) n++;
    if (!s[n]) return n;
    while (n > 0 && ((unsigned char)s[n] & 0xC0) == 0x80) n--;   // landed inside a character
    return n;
}

void jf_chapter_label(const JFChapter *c, int index, const char *unnamed_fmt,
                      char *out, int cap) {
    if (!out || cap <= 0) return;
    out[0] = '\0';
    if (!c) return;
    char name[56];
    if (c->name[0]) {
        const int k = utf8_prefix(c->name, 30);
        memcpy(name, c->name, (size_t)k);
        name[k] = '\0';
        if (c->name[k]) snprintf(name + k, sizeof name - (size_t)k, "...");
    } else {
        snprintf(name, sizeof name, unnamed_fmt ? unnamed_fmt : "%d", index + 1);
    }
    const long long t = (long long)(c->start_secs + 0.5);
    const long long h = t / 3600, m = (t / 60) % 60, s = t % 60;
    if (h > 0) snprintf(out, (size_t)cap, "%d  %s  %lld:%02lld:%02lld", index + 1, name, h, m, s);
    else       snprintf(out, (size_t)cap, "%d  %s  %lld:%02lld", index + 1, name, m, s);
}
