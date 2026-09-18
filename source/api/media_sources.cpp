// PlaybackInfo.MediaSources parser.  This file deliberately has no PS3-only
// dependencies so the exact parser shipped in the PKG can be host-tested.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "jellyfin_api.h"

static const char *skip_ws(const char *p, const char *end) {
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) p++;
    return p;
}

static int hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void append_utf8(char *out, int cap, int *used, unsigned cp) {
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

static bool read_json_string(const char *p, const char *end,
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
static const char *find_top_value(const char *obj, const char *end,
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

static long long read_json_int(const char *p, const char *end, long long def) {
    p = skip_ws(p, end);
    if (p >= end || (*p != '-' && (*p < '0' || *p > '9'))) return def;
    return strtoll(p, NULL, 10);
}

static bool read_json_bool(const char *p, const char *end) {
    p = skip_ws(p, end);
    return p + 4 <= end && memcmp(p, "true", 4) == 0;
}

static const char *object_end(const char *start, const char *limit) {
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

// Defined HERE, in the file that sets it, rather than beside the other API
// globals: this parser is deliberately free of PS3-only dependencies so it
// can be host-tested standalone (tests/test_media_sources.cpp), and putting
// the definition in api_auth.cpp broke that link.
int g_source_fps_milli = 0;

static void parse_tracks(const char *source, const char *source_end,
                         JFTracks *out, char *first_video, int video_cap) {
    memset(out, 0, sizeof(*out));
    if (first_video && video_cap > 0) first_video[0] = '\0';
    g_source_fps_milli = 0;   // re-learned from this source's Video stream
    const char *arr = find_top_value(source, source_end, "MediaStreams");
    if (!arr || arr >= source_end || *arr != '[') return;
    const char *p = arr + 1;
    while (p < source_end) {
        p = skip_ws(p, source_end);
        if (p >= source_end || *p == ']') break;
        if (*p != '{') { p++; continue; }
        const char *oe = object_end(p, source_end);
        if (!oe) break;

        char type[16] = "", display[128] = "", language[32] = "";
        const char *v = find_top_value(p, oe, "Type");
        if (v) read_json_string(v, oe, type, sizeof(type));
        v = find_top_value(p, oe, "DisplayTitle");
        if (v) read_json_string(v, oe, display, sizeof(display));
        v = find_top_value(p, oe, "Language");
        if (v) read_json_string(v, oe, language, sizeof(language));
        v = find_top_value(p, oe, "Index");
        int index = v ? (int)read_json_int(v, oe, -1) : -1;

        if (strcmp(type, "Video") == 0) {
            if (first_video && !first_video[0] && display[0])
                snprintf(first_video, video_cap, "%s", display);
            // Frame rate, for the player timing when VDEC reports no
            // frame-rate code -- which is exactly what a stream copy does.
            // Kept as milli-fps so it stays integer: 23.976025 -> 23976.
            if (!g_source_fps_milli) {
                const char *fr = find_top_value(p, oe, "RealFrameRate");
                if (!fr) fr = find_top_value(p, oe, "AverageFrameRate");
                if (fr) {
                    char numbuf[32]; int k = 0;
                    const char *q = fr;
                    while (q < oe && k < (int)sizeof(numbuf) - 1 &&
                           ((*q >= '0' && *q <= '9') || *q == '.'))
                        numbuf[k++] = *q++;
                    numbuf[k] = '\0';
                    if (numbuf[0]) {
                        double f = atof(numbuf);
                        if (f > 1.0 && f < 1000.0)
                            g_source_fps_milli = (int)(f * 1000.0 + 0.5);
                    }
                }
            }
        } else if (index >= 0 && strcmp(type, "Audio") == 0 &&
                   out->n_audio < JF_MAX_STREAMS) {
            int pos = out->n_audio++;
            out->audio[pos].index = index;
            snprintf(out->audio[pos].label, sizeof(out->audio[pos].label), "%s",
                     display[0] ? display : (language[0] ? language : "Audio"));
            v = find_top_value(p, oe, "IsDefault");
            if (v && read_json_bool(v, oe)) out->default_audio = pos;
        } else if (index >= 0 && strcmp(type, "Subtitle") == 0 &&
                   out->n_subs < JF_MAX_STREAMS) {
            int pos = out->n_subs++;
            out->subs[pos].index = index;
            snprintf(out->subs[pos].label, sizeof(out->subs[pos].label), "%s",
                     display[0] ? display : (language[0] ? language : "Subtitle"));
            out->subs[pos].codec[0] = ' ';
            v = find_top_value(p, oe, "Codec");
            if (v) read_json_string(v, oe, out->subs[pos].codec,
                                    sizeof(out->subs[pos].codec));
        }
        p = oe;
    }
}

// Case-insensitive substring search (the labels come from whatever named the
// file, so "1080P" and "1080p" both turn up).
static bool contains_ci(const char *hay, const char *needle) {
    if (!hay || !needle || !needle[0]) return false;
    for (const char *h = hay; *h; h++) {
        const char *a = h, *b = needle;
        while (*a && *b) {
            char ca = (*a >= 'A' && *a <= 'Z') ? (char)(*a - 'A' + 'a') : *a;
            char cb = (*b >= 'A' && *b <= 'Z') ? (char)(*b - 'A' + 'a') : *b;
            if (ca != cb) break;
            a++; b++;
        }
        if (!*b) return true;
    }
    return false;
}

// Append `tag` to a label unless it is already in there.
static void label_append(char *label, int cap, const char *tag) {
    if (contains_ci(label, tag)) return;
    int len = (int)strlen(label);
    if (len + 4 + (int)strlen(tag) < cap)
        snprintf(label + len, (size_t)(cap - len), " \xB7 %s", tag);
}

// Make the resolution and video codec visible in a version's label.
//
// A source plugin names its entries after the release ("Show.S01E01.WEB-DL
// .DDP5.1.H.264-GROUP") and most such names carry the resolution, but not all
// do — and with a long list the choice that matters is which resolution and
// which codec, because both decide how much work the SERVER has to do.  The
// PS3 itself is unaffected: video is always transcoded to H.264 for it
// (AllowVideoStreamCopy=false), so a 4K HEVC source plays — it just makes the
// server re-encode 4K HEVC in real time, which is where "stream connection
// failed" after a long wait comes from.  Picking a 1080p H.264 source is
// dramatically cheaper, so it is worth being able to see which is which.
//
// Never invents: anything the video stream does not report is not added.
static void source_tag_video(char *label, int cap, const char *video) {
    if (!label || !video || !video[0]) return;

    static const char *kRes[] = { "2160p", "4K", "1440p", "1080p", "720p",
                                  "576p", "480p", "360p" };
    for (unsigned i = 0; i < sizeof(kRes) / sizeof(kRes[0]); i++) {
        if (!contains_ci(video, kRes[i])) continue;
        // "4K" and "2160p" mean the same thing to a reader; do not add one
        // when the other is already present.
        if (i <= 1 && (contains_ci(label, "2160p") || contains_ci(label, "4K")))
            break;
        label_append(label, cap, kRes[i]);
        break;
    }

    // Codec.  The PS3's own decoder handles H.264/AVC (the two names are the
    // same codec) and nothing newer — no HEVC, no AV1.  That is not what
    // decides whether a source plays here, because the server always
    // transcodes video to H.264 for this client, but it decides how HARD the
    // server has to work: re-encoding 4K HEVC or AV1 in real time is what
    // produces stalls and the two-minute "connection failed".  So name the
    // codec when the release name does not.
    //
    // H.264 sources are named "x264", "H.264" or "AVC" interchangeably; any
    // of those counts as already said.
    if (contains_ci(video, "AV1")) {
        if (!contains_ci(label, "AV1")) label_append(label, cap, "AV1");
    } else if (contains_ci(video, "HEVC") || contains_ci(video, "H265") ||
               contains_ci(video, "H.265") || contains_ci(video, "X265")) {
        if (!contains_ci(label, "265") && !contains_ci(label, "HEVC"))
            label_append(label, cap, "HEVC");
    } else if (contains_ci(video, "H264") || contains_ci(video, "H.264") ||
               contains_ci(video, "AVC") || contains_ci(video, "X264")) {
        if (!contains_ci(label, "264") && !contains_ci(label, "AVC"))
            label_append(label, cap, "H.264");
    }
}

static bool parse_source(const char *obj, const char *end, JFMediaSource *out) {
    memset(out, 0, sizeof(*out));
    const char *v = find_top_value(obj, end, "Id");
    if (v) read_json_string(v, end, out->id, sizeof(out->id));
    v = find_top_value(obj, end, "LiveStreamId");
    if (v) read_json_string(v, end, out->live_stream_id,
                            sizeof(out->live_stream_id));
    v = find_top_value(obj, end, "Name");
    if (v) read_json_string(v, end, out->label, sizeof(out->label));
    v = find_top_value(obj, end, "RunTimeTicks");
    long long ticks = v ? read_json_int(v, end, 0) : 0;
    if (ticks > 0) out->runtime_secs = (unsigned)(ticks / 10000000LL);

    char video[64] = "";
    parse_tracks(obj, end, &out->tracks, video, sizeof(video));
    if (!out->label[0])
        snprintf(out->label, sizeof(out->label), "%s",
                 video[0] ? video : "Version");
    else
        source_tag_video(out->label, sizeof(out->label), video);
    return out->id[0] != '\0';
}

static const char *media_sources_array(const char *json, const char **end) {
    if (!json) return NULL;
    const char *p = strstr(json, "\"MediaSources\"");
    if (!p) return NULL;
    p += sizeof("\"MediaSources\"") - 1;
    while (*p && *p != ':') p++;
    if (*p != ':') return NULL;
    p = skip_ws(p + 1, p + strlen(p));
    if (*p != '[') return NULL;
    *end = json + strlen(json);
    return p + 1;
}

int jellyfin_parse_media_sources(const char *json, JFMediaSources *out) {
    if (!out) return 0;
    memset(out, 0, sizeof(*out));
    const char *limit = NULL;
    const char *p = media_sources_array(json, &limit);
    if (!p) return 0;
    while (p < limit && out->n_sources < JF_MAX_SOURCES) {
        p = skip_ws(p, limit);
        if (p >= limit || *p == ']') break;
        if (*p != '{') { p++; continue; }
        const char *oe = object_end(p, limit);
        if (!oe) break;
        if (parse_source(p, oe, &out->source[out->n_sources]))
            out->n_sources++;
        p = oe;
    }

    // Duplicate names are common for local versions.  Keep the server's name
    // where it is useful, but make identical entries distinguishable.
    bool duplicate[JF_MAX_SOURCES] = { false };
    int ordinal[JF_MAX_SOURCES] = { 0 };
    for (int i = 0; i < out->n_sources; i++) {
        ordinal[i] = 1;
        for (int j = 0; j < out->n_sources; j++) {
            if (strcmp(out->source[i].label, out->source[j].label) == 0) {
                if (j != i) duplicate[i] = true;
                if (j < i) ordinal[i]++;
            }
        }
    }
    for (int i = 0; i < out->n_sources; i++) {
        if (duplicate[i]) {
            char base[112];
            snprintf(base, sizeof(base), "%s", out->source[i].label);
            snprintf(out->source[i].label, sizeof(out->source[i].label),
                     "%s (%d)", base, ordinal[i]);
        }
    }
    return out->n_sources;
}

bool jellyfin_parse_selected_media_source(const char *json,
                                           const char *requested_id,
                                           JFMediaSource *out) {
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    const char *limit = NULL;
    const char *p = media_sources_array(json, &limit);
    if (!p) return false;
    JFMediaSource first;
    bool have_first = false;
    while (p < limit) {
        p = skip_ws(p, limit);
        if (p >= limit || *p == ']') break;
        if (*p != '{') { p++; continue; }
        const char *oe = object_end(p, limit);
        if (!oe) break;
        JFMediaSource cur;
        if (parse_source(p, oe, &cur)) {
            if (!have_first) { first = cur; have_first = true; }
            if (requested_id && requested_id[0] &&
                strcmp(cur.id, requested_id) == 0) {
                *out = cur;
                return true;
            }
        }
        p = oe;
    }
    if (have_first) { *out = first; return true; }
    return false;
}

// Text subtitle formats can be fetched as SubRip and drawn on the console;
// bitmap ones cannot. Everything not recognised is treated as a bitmap, so an
// unknown format falls back to the burn-in that has always worked rather than
// to an empty overlay.
static bool ieq(const char *a, const char *b)
{
    for (; *a && *b; a++, b++) {
        char x = *a, y = *b;
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y) return false;
    }
    return *a == *b;
}

bool jf_sub_is_text(const char *codec)
{
    if (!codec || !codec[0]) return false;
    static const char *kText[] = { "subrip", "srt", "ass", "ssa", "vtt",
                                   "webvtt", "text", "mov_text", "sami" };
    for (unsigned i = 0; i < sizeof(kText)/sizeof(kText[0]); i++)
        if (ieq(codec, kText[i])) return true;
    return false;
}
