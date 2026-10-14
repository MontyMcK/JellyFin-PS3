#pragma once
// Small JSON readers shared by the hand-rolled API parsers (media_sources.cpp,
// livetv.cpp).  Pure: no PS3 headers, so the parsers that use them host-test.
//
// They read a buffer in place: find_top_value() locates a DIRECT child of an
// object (braces inside strings and keys of nested objects cannot be mistaken
// for it), and the read_* functions decode the value it points at.

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static inline const char *skip_ws(const char *p, const char *end) {
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) p++;
    return p;
}

static inline int hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static inline void append_utf8(char *out, int cap, int *used, unsigned cp) {
    unsigned char b[3]; int n = 0;
    if (cp <= 0x7f) { b[0] = (unsigned char)cp; n = 1; }
    else if (cp <= 0x7ff) {
        b[0] = (unsigned char)(0xc0 | (cp >> 6));
        b[1] = (unsigned char)(0x80 | (cp & 0x3f)); n = 2;
    } else {
        b[0] = (unsigned char)(0xe0 | (cp >> 12));
        b[1] = (unsigned char)(0x80 | ((cp >> 6) & 0x3f));
        b[2] = (unsigned char)(0x80 | (cp & 0x3f)); n = 3;
    }
    if (*used + n >= cap) return;
    for (int i = 0; i < n; i++) out[(*used)++] = (char)b[i];
}

static inline bool read_json_string(const char *p, const char *end,
                             char *out, int cap) {
    if (!out || cap <= 0) return false;
    out[0] = '\0';
    p = skip_ws(p, end);
    if (p >= end || *p != '"') return false;
    p++;
    int used = 0;
    while (p < end && *p != '"') {
        unsigned char c = (unsigned char)*p++;
        if (c != '\\') {
            if (used + 1 < cap) out[used++] = (char)c;
            continue;
        }
        if (p >= end) break;
        char esc = *p++;
        char decoded = 0;
        switch (esc) {
        case '"': decoded = '"'; break;
        case '\\': decoded = '\\'; break;
        case '/':  decoded = '/';  break;
        case 'b':  decoded = '\b'; break;
        case 'f':  decoded = '\f'; break;
        case 'n':  decoded = '\n'; break;
        case 'r':  decoded = '\r'; break;
        case 't':  decoded = '\t'; break;
        case 'u': {
            if (p + 4 > end) break;
            int h0 = hex_nibble(p[0]), h1 = hex_nibble(p[1]);
            int h2 = hex_nibble(p[2]), h3 = hex_nibble(p[3]);
            if (h0 >= 0 && h1 >= 0 && h2 >= 0 && h3 >= 0) {
                append_utf8(out, cap, &used,
                            (unsigned)((h0 << 12) | (h1 << 8) | (h2 << 4) | h3));
                p += 4;
            }
            continue;
        }
        default: decoded = esc; break;
        }
        if (decoded && used + 1 < cap) out[used++] = decoded;
    }
    out[used] = '\0';
    return true;
}

// Find a direct child key in an object.  Nested MediaStreams keys cannot be
// mistaken for source fields, and braces inside strings are ignored.
static inline const char *find_top_value(const char *obj, const char *end,
                                  const char *key) {
    if (!obj || obj >= end || *obj != '{') return NULL;
    const int key_len = (int)strlen(key);
    int depth = 0;
    const char *p = obj;
    while (p < end) {
        if (*p == '"') {
            const char *q = p + 1;
            bool esc = false;
            while (q < end) {
                if (esc) esc = false;
                else if (*q == '\\') esc = true;
                else if (*q == '"') break;
                q++;
            }
            if (depth == 1 && q < end && q - (p + 1) == key_len &&
                memcmp(p + 1, key, key_len) == 0) {
                const char *v = skip_ws(q + 1, end);
                if (v < end && *v == ':') return skip_ws(v + 1, end);
            }
            p = (q < end) ? q + 1 : end;
            continue;
        }
        if (*p == '{' || *p == '[') depth++;
        else if (*p == '}' || *p == ']') depth--;
        p++;
    }
    return NULL;
}

static inline long long read_json_int(const char *p, const char *end, long long def) {
    p = skip_ws(p, end);
    if (p >= end || (*p != '-' && (*p < '0' || *p > '9'))) return def;
    return strtoll(p, NULL, 10);
}

static inline bool read_json_bool(const char *p, const char *end) {
    p = skip_ws(p, end);
    return p + 4 <= end && memcmp(p, "true", 4) == 0;
}

static inline const char *object_end(const char *start, const char *limit) {
    if (!start || start >= limit || *start != '{') return NULL;
    int depth = 0; bool in_string = false, escaped = false;
    for (const char *p = start; p < limit; p++) {
        char c = *p;
        if (escaped) escaped = false;
        else if (in_string) {
            if (c == '\\') escaped = true;
            else if (c == '"') in_string = false;
        } else if (c == '"') in_string = true;
        else if (c == '{') depth++;
        else if (c == '}' && --depth == 0) return p + 1;
    }
    return NULL;
}
