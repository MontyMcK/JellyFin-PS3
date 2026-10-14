// XMB JSON parsing helpers (local, avoids changing jellyfin_api.cpp).

#include "i18n.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ui_internal.h"
#include "json_unescape.h"   // \u0027 & co. -> UTF-8

int xmb_json_str_range(const char *start, int len,
                        const char *key, char *out, int out_size) {
    char search[64];
    snprintf(search, sizeof(search), "\"%s\":\"", key);
    int slen = strlen(search);
    const char *p = start, *end = start + len;
    while (p + slen <= end) {
        if (memcmp(p, search, slen) == 0) {
            p += slen;
            json_unescape(p, end, out, out_size);
            return 1;
        }
        p++;
    }
    out[0] = '\0';
    return 0;
}

// As xmb_json_str_range, but only the object's OWN key (depth 1 of the
// object at start), never one inside a nested object or array.  A search
// reply carries each title's MediaSources inline, and their streams'
// "Type":"Video" came before the title's "Type":"Movie" -- so a film found by
// search read as a "Video" and its details page never offered versions
// (2026-09-27).
static int xmb_json_top_str_range(const char *start, int len,
                                  const char *key, char *out, int out_size) {
    char search[64];
    snprintf(search, sizeof(search), "\"%s\":\"", key);
    const int slen = strlen(search);
    const char *p = start, *end = start + len;
    int depth = 0;
    bool in_str = false, esc = false;
    for (; p < end; p++) {
        const char c = *p;
        if (in_str) {
            if (esc) esc = false;
            else if (c == '\\') esc = true;
            else if (c == '"') in_str = false;
            continue;
        }
        if (c == '{' || c == '[') { depth++; continue; }
        if (c == '}' || c == ']') { depth--; continue; }
        if (c != '"') continue;
        if (depth == 1 && p + slen <= end && memcmp(p, search, slen) == 0) {
            json_unescape(p + slen, end, out, out_size);
            return 1;
        }
        in_str = true;
    }
    out[0] = '\0';
    return 0;
}

int xmb_json_int_range(const char *start, int len,
                        const char *key, int def) {
    char search[64];
    snprintf(search, sizeof(search), "\"%s\":", key);
    int slen = strlen(search);
    const char *p = start, *end = start + len;
    while (p + slen <= end) {
        if (memcmp(p, search, slen) == 0) {
            p += slen;
            while (p < end && *p == ' ') p++;
            if (*p < '0' || *p > '9') return def;
            return atoi(p);
        }
        p++;
    }
    return def;
}

long long xmb_json_ll_range(const char *start, int len,
                              const char *key, long long def) {
    char search[64];
    snprintf(search, sizeof(search), "\"%s\":", key);
    int slen = strlen(search);
    const char *p = start, *end = start + len;
    while (p + slen <= end) {
        if (memcmp(p, search, slen) == 0) {
            p += slen;
            while (p < end && *p == ' ') p++;
            if (*p < '0' || *p > '9') return def;
            long long v = 0;
            while (p < end && *p >= '0' && *p <= '9') v = v * 10 + (*p++ - '0');
            return v;
        }
        p++;
    }
    return def;
}

int xmb_json_first_arr_str(const char *start, int len,
                             const char *key, char *out, int out_size) {
    char search[64];
    snprintf(search, sizeof(search), "\"%s\":[\"", key);
    int slen = strlen(search);
    const char *p = start, *end = start + len;
    while (p + slen <= end) {
        if (memcmp(p, search, slen) == 0) {
            p += slen;
            json_unescape(p, end, out, out_size);
            return 1;
        }
        p++;
    }
    out[0] = '\0';
    return 0;
}

// Gelato's per-stream items.  When Gelato (the debrid plugin feeding this
// server's libraries) syncs a title's streams -- which it does the moment the
// title is opened, taking 6-8 s -- it creates one hidden-ish item PER STREAM
// (16 for a film), tagged "gelato-stream", with the film's own name, poster
// and provider ids and a null DateCreated.  For a while they list as ordinary
// children of the library, so the Movies grid showed a recently opened film
// sixteen times over, and every copy failed to play (PlaybackInfo on a stream
// item hits Gelato's .strm-path bug: HTTP 500).  They are never something to
// browse: the title's own item carries every stream as a version.  So they
// are dropped here, for every list -- which is why the listing queries ask
// for Tags.  Bounded search; the object is not NUL-terminated at olen.
static bool is_gelato_stream(const char *obj, int olen) {
    static const char pat[] = "\"gelato-stream\"";
    const int n = (int)sizeof(pat) - 1;
    for (int i = 0; i + n <= olen; i++)
        if (obj[i] == '"' && memcmp(obj + i, pat, (size_t)n) == 0) return true;
    return false;
}
static unsigned s_gelato_dropped = 0;
unsigned xmb_gelato_streams_dropped(void) { return s_gelato_dropped; }

int parse_xmb_items(const char *json, XMBItem *arr, int max) {
    return parse_xmb_items_each(json, arr, max, NULL, NULL);
}

int parse_xmb_items_each(const char *json, XMBItem *arr, int max,
                         XMBItemEach each, void *ctx) {
    const char *p = strstr(json, "\"Items\":[");
    if (!p) return 0;
    p += 9;

    int count = 0;
    while (count < max && *p) {
        while (*p && *p != '{' && *p != ']') p++;
        if (!*p || *p == ']') break;

        const char *obj = p;
        int  depth = 0;
        bool in_str = false, esc = false;
        while (*p) {
            char c = *p;
            if (esc) { esc = false; }
            else if (in_str) { if (c == '\\') esc = true; else if (c == '"') in_str = false; }
            else { if (c == '"') in_str = true; else if (c == '{') depth++; else if (c == '}') { if (--depth == 0) { p++; break; } } }
            p++;
        }
        int olen = (int)(p - obj);

        if (is_gelato_stream(obj, olen)) { s_gelato_dropped++; continue; }

        XMBItem it; memset(&it, 0, sizeof(it));
        xmb_json_str_range(obj, olen, "Id",   it.id,   sizeof(it.id));
        xmb_json_str_range(obj, olen, "Name", it.name, sizeof(it.name));
        if (!xmb_json_top_str_range(obj, olen, "Type", it.type, sizeof(it.type)))
            xmb_json_str_range(obj, olen, "Type", it.type, sizeof(it.type));
        if (!it.id[0]) continue;

        // ImageTags carries a "Thumb" only when the item really has wide banner
        // art (movies usually do, episodes usually don't).  Landscape cards use
        // it instead of the portrait Primary poster; see ui_home.cpp.  The key
        // search is for the quoted "Thumb", so "ParentThumbItemId" can't match.
        char thumb_tag[8];
        it.has_thumb = xmb_json_str_range(obj, olen, "Thumb",
                                          thumb_tag, sizeof(thumb_tag)) ? 1 : 0;

        bool is_album = strcmp(it.type, "MusicAlbum") == 0 ||
                        strcmp(it.type, "Playlist")   == 0;
        bool is_audio = strcmp(it.type, "Audio")      == 0;

        int year = xmb_json_int_range(obj, olen, "ProductionYear", 0);
        if (year > 0) snprintf(it.year_str, sizeof(it.year_str), "%d", year);

        long long ticks = xmb_json_ll_range(obj, olen, "RunTimeTicks", 0);
        if (is_album) {
            // Albums/playlists show a track count, not a duration.
            int n = xmb_json_int_range(obj, olen, "ChildCount", 0);
            if (n > 0)
                snprintf(it.duration_str, sizeof(it.duration_str),
                         n == 1 ? TR("%d Track") : TR("%d Tracks"), n);
        } else if (is_audio && ticks > 0) {
            int secs = (int)(ticks / 10000000LL);
            it.dur_secs = (u32)secs;
            snprintf(it.duration_str, sizeof(it.duration_str), "%d:%02d",
                     secs / 60, secs % 60);
        } else if (ticks > 0) {
            int total_min = (int)(ticks / 600000000LL);
            int h = total_min / 60, m = total_min % 60;
            if (h > 0) snprintf(it.duration_str, sizeof(it.duration_str), TR("%dh %dm"), h, m);
            else        snprintf(it.duration_str, sizeof(it.duration_str), TR("%dm"), m);
        }

        xmb_json_first_arr_str(obj, olen, "Genres", it.genre, sizeof(it.genre));

        // UserData (nested object, but the key names are unique within the
        // item): saved resume position + watched percentage.
        long long pos_ticks = xmb_json_ll_range(obj, olen,
                                                "PlaybackPositionTicks", 0);
        it.resume_secs = (u32)(pos_ticks / 10000000LL);
        int pct = xmb_json_int_range(obj, olen, "PlayedPercentage", 0);
        if (pct <= 0 && pos_ticks > 0 && ticks > 0)
            pct = (int)(pos_ticks * 100 / ticks);
        if (pct > 100) pct = 100;
        it.progress_pct = (u8)(pct > 0 ? pct : 0);

        char container[16] = "";
        xmb_json_str_range(obj, olen, "Container", container, sizeof(container));
        if (is_album) {
            // no codec chip on album cards
        } else if (is_audio) {
            for (int i = 0; container[i] && i < (int)sizeof(it.codec)-1; i++)
                it.codec[i] = (container[i] >= 'a' && container[i] <= 'z')
                                  ? container[i] - 32 : container[i];
        } else if (strstr(container, "hevc") || strstr(container, "h265") ||
                   strstr(container, "265"))
            strncpy(it.codec, "H.265", sizeof(it.codec)-1);
        else
            strncpy(it.codec, "H.264", sizeof(it.codec)-1);

        // "AlbumArtist" is the display string; per-track credits fall back
        // to the Artists array.  ("AlbumArtists":[ can't false-match — the
        // searcher requires the :"...  suffix.)
        xmb_json_str_range(obj, olen, "AlbumArtist", it.artist, sizeof(it.artist));
        if (!it.artist[0])
            xmb_json_first_arr_str(obj, olen, "Artists",
                                   it.artist, sizeof(it.artist));
        if (is_audio)
            xmb_json_str_range(obj, olen, "AlbumId",
                               it.album_id, sizeof(it.album_id));

        decode_unicode_escapes(it.name);
        decode_unicode_escapes(it.genre);
        decode_unicode_escapes(it.artist);
        if (each) each(count, obj, olen, ctx);
        arr[count++] = it;
    }
    return count;
}
