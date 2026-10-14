// ItemFacts parser (api_facts.h).  No PS3-only dependencies, so the exact
// parser shipped in the PKG is host-tested (tests/test_facts.cpp).
//
// The words are this client's, not Jellyfin's DisplayTitle.  DisplayTitle
// leads with the language and spells codecs its own way ("English - DTS-HD
// MA - 5.1 - Default"); the strip wants the format alone, in the canvas's
// spelling: "DTS-HD MA 5.1", "TRUEHD ATMOS 7.1", "AC-3 5.1".

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "api_facts.h"
#include "json_unescape.h"

// --- a flat-object field reader ------------------------------------------------
// Jellyfin's JSON is compact ("key":value, no spaces), which every other
// parser in this client relies on too.

static bool get_str(const char *o, int n, const char *key, char *out, int cap) {
    char pat[48];
    int  pl = snprintf(pat, sizeof pat, "\"%s\":\"", key);
    out[0] = '\0';
    for (const char *p = o, *end = o + n; p + pl <= end; p++) {
        if (memcmp(p, pat, (size_t)pl) != 0) continue;
        p += pl;
        json_unescape(p, end, out, cap);
        return true;
    }
    return false;
}

static long long get_ll(const char *o, int n, const char *key, long long def) {
    char pat[48];
    int  pl = snprintf(pat, sizeof pat, "\"%s\":", key);
    for (const char *p = o, *end = o + n; p + pl < end; p++) {
        if (memcmp(p, pat, (size_t)pl) != 0) continue;
        p += pl;
        if (*p == '-' || (*p >= '0' && *p <= '9')) return atoll(p);
        return def;
    }
    return def;
}

static bool get_true(const char *o, int n, const char *key) {
    char pat[48];
    int  pl = snprintf(pat, sizeof pat, "\"%s\":true", key);
    for (const char *p = o, *end = o + n; p + pl <= end; p++)
        if (memcmp(p, pat, (size_t)pl) == 0) return true;
    return false;
}

static void upper(char *s) {
    for (; *s; s++) *s = (char)toupper((unsigned char)*s);
}

static bool has_ci(const char *hay, const char *needle) {
    size_t nl = strlen(needle);
    for (; *hay; hay++) {
        size_t i = 0;
        while (i < nl && hay[i] &&
               tolower((unsigned char)hay[i]) == tolower((unsigned char)needle[i])) i++;
        if (i == nl) return true;
    }
    return false;
}

// --- words --------------------------------------------------------------------

static const char *channels_word(int ch, char *buf, int cap) {
    switch (ch) {
    case 1: return "1.0";
    case 2: return "2.0";
    case 3: return "2.1";
    case 6: return "5.1";
    case 7: return "6.1";
    case 8: return "7.1";
    default:
        if (ch <= 0) return "";
        snprintf(buf, (size_t)cap, "%dCH", ch);
        return buf;
    }
}

// Format name for an audio stream, and whether it is lossless.
static const char *audio_format(const char *codec, const char *profile,
                                const char *title, bool *lossless) {
    const bool atmos = has_ci(profile, "atmos") || has_ci(title, "atmos");
    *lossless = false;
    if (!strcmp(codec, "truehd")) {
        *lossless = true;
        return atmos ? "TRUEHD ATMOS" : "TRUEHD";
    }
    if (!strcmp(codec, "dts")) {
        // DTS:X rides on an MA core, so it is lossless too.
        if (has_ci(profile, "dts:x") || has_ci(title, "dts:x")) { *lossless = true; return "DTS:X"; }
        if (has_ci(profile, "ma"))       { *lossless = true; return "DTS-HD MA"; }
        if (has_ci(profile, "hra") || has_ci(profile, "hi res") || has_ci(profile, "hr"))
            return "DTS-HD HRA";
        if (has_ci(profile, "express")) return "DTS EXPRESS";
        return "DTS";
    }
    if (!strcmp(codec, "eac3"))  return atmos ? "E-AC-3 ATMOS" : "E-AC-3";
    if (!strcmp(codec, "ac3"))   return "AC-3";
    if (!strcmp(codec, "aac"))   return "AAC";
    if (!strcmp(codec, "mp3"))   return "MP3";
    if (!strcmp(codec, "mp2"))   return "MP2";
    if (!strcmp(codec, "opus"))  return "OPUS";
    if (!strcmp(codec, "vorbis"))return "VORBIS";
    if (!strcmp(codec, "flac"))  { *lossless = true; return "FLAC"; }
    if (!strcmp(codec, "alac"))  { *lossless = true; return "ALAC"; }
    if (!strncmp(codec, "pcm", 3)) { *lossless = true; return "LPCM"; }
    return NULL;   // caller upper-cases the codec
}

static void audio_label(const char *o, int n, char *out, int cap, bool *lossless) {
    char codec[24], profile[64], title[128], chb[16];
    get_str(o, n, "Codec", codec, sizeof codec);
    get_str(o, n, "Profile", profile, sizeof profile);
    get_str(o, n, "DisplayTitle", title, sizeof title);
    for (char *c = codec; *c; c++) *c = (char)tolower((unsigned char)*c);
    const char *fmt = audio_format(codec, profile, title, lossless);
    char cu[24];
    snprintf(cu, sizeof cu, "%s", codec);
    upper(cu);
    const char *ch = channels_word((int)get_ll(o, n, "Channels", 0), chb, sizeof chb);
    snprintf(out, (size_t)cap, "%s%s%s", fmt ? fmt : cu, ch[0] ? " " : "", ch);
}

static void video_label(const char *o, int n, char *out, int cap) {
    char codec[24], rtype[24], range[16];
    get_str(o, n, "Codec", codec, sizeof codec);
    get_str(o, n, "VideoRangeType", rtype, sizeof rtype);
    get_str(o, n, "VideoRange", range, sizeof range);
    for (char *c = codec; *c; c++) *c = (char)tolower((unsigned char)*c);
    const int w = (int)get_ll(o, n, "Width", 0), h = (int)get_ll(o, n, "Height", 0);

    char res[12] = "";
    if (w >= 3200 || h >= 1800)      snprintf(res, sizeof res, "4K");
    else if (w >= 1900 || h >= 1000) snprintf(res, sizeof res, "1080P");
    else if (w >= 1200 || h >= 700)  snprintf(res, sizeof res, "720P");
    else if (h > 0)                  snprintf(res, sizeof res, "%dP", h);

    const char *cn =
        !strcmp(codec, "h264") ? "H.264" :
        (!strcmp(codec, "hevc") || !strcmp(codec, "h265")) ? "HEVC" :
        !strcmp(codec, "mpeg2video") ? "MPEG-2" :
        !strcmp(codec, "vc1") ? "VC-1" :
        !strcmp(codec, "av1") ? "AV1" :
        !strcmp(codec, "vp9") ? "VP9" :
        !strcmp(codec, "mpeg4") ? "MPEG-4" : NULL;
    char cu[24];
    snprintf(cu, sizeof cu, "%s", codec);
    upper(cu);

    // Dolby Vision before HDR10: a DV file often carries an HDR10 base too.
    const char *hdr =
        has_ci(rtype, "dovi") ? "DV" :
        has_ci(rtype, "hdr10plus") ? "HDR10+" :
        has_ci(rtype, "hdr10") ? "HDR10" :
        has_ci(rtype, "hlg") ? "HLG" :
        (has_ci(range, "hdr") && !has_ci(range, "sdr")) ? "HDR" : NULL;

    snprintf(out, (size_t)cap, "%s%s%s%s%s", res, res[0] ? " " : "",
             cn ? cn : cu, hdr ? " " : "", hdr ? hdr : "");
}

static void subs_label(const char *o, int n, char *out, int cap) {
    char codec[24], lang[16];
    get_str(o, n, "Codec", codec, sizeof codec);
    get_str(o, n, "Language", lang, sizeof lang);
    for (char *c = codec; *c; c++) *c = (char)tolower((unsigned char)*c);
    const char *cn =
        !strcmp(codec, "pgssub") ? "PGS" :
        (!strcmp(codec, "subrip") || !strcmp(codec, "srt")) ? "SRT" :
        (!strcmp(codec, "ass") || !strcmp(codec, "ssa")) ? "ASS" :
        (!strcmp(codec, "dvdsub") || !strcmp(codec, "dvd_subtitle")) ? "VOBSUB" :
        !strcmp(codec, "webvtt") ? "VTT" :
        !strcmp(codec, "mov_text") ? "TX3G" : NULL;
    char cu[24];
    snprintf(cu, sizeof cu, "%s", codec);
    upper(cu);
    upper(lang);
    snprintf(out, (size_t)cap, "%.12s%s%.15s", cn ? cn : cu, lang[0] ? " " : "", lang);
}

// --- the walk -------------------------------------------------------------------

// The synopsis and the first four actors (the peek's back face).  People
// objects are walked like MediaStreams; only Type "Actor" counts, in the
// order the server bills them.
static void about_parse(const char *json, int jl, ItemFacts *out) {
    char ov[1024];
    if (get_str(json, jl, "Overview", ov, sizeof ov) && ov[0]) {
        // Cut at a word boundary with an ellipsis if it will not fit, never
        // mid-character (json_unescape already kept characters whole).
        const int cap = (int)sizeof out->overview;
        if ((int)strlen(ov) < cap) {
            snprintf(out->overview, sizeof out->overview, "%s", ov);
        } else {
            int cut = cap - 4;
            while (cut > 0 && ov[cut] != ' ') cut--;
            if (cut <= 0) cut = cap - 4;
            while (cut > 0 && ((unsigned char)ov[cut] & 0xC0) == 0x80) cut--;
            memcpy(out->overview, ov, (size_t)cut);
            memcpy(out->overview + cut, "...", 4);
        }
    }
    get_str(json, jl, "OfficialRating", out->rating, sizeof out->rating);

    const char *arr = strstr(json, "\"People\":[");
    if (!arr) return;
    const char *p = arr + 10;
    while (*p && *p != ']' && out->n_cast < 4) {
        while (*p && *p != '{' && *p != ']') p++;
        if (*p != '{') break;
        const char *o = p;
        int depth = 0;
        bool in_str = false, esc = false;
        while (*p) {
            const char c = *p;
            if (esc) esc = false;
            else if (in_str) { if (c == '\\') esc = true; else if (c == '"') in_str = false; }
            else if (c == '"') in_str = true;
            else if (c == '{') depth++;
            else if (c == '}' && --depth == 0) { p++; break; }
            p++;
        }
        const int n = (int)(p - o);
        char type[16];
        get_str(o, n, "Type", type, sizeof type);
        if (strcmp(type, "Actor") != 0) continue;
        const int k = out->n_cast;
        get_str(o, n, "Name", out->cast[k], sizeof out->cast[k]);
        get_str(o, n, "Id",   out->cast_id[k], sizeof out->cast_id[k]);
        if (out->cast[k][0]) out->n_cast++;
    }
}

void facts_parse(const char *json, ItemFacts *out) {
    memset(out, 0, sizeof *out);
    if (!json) return;
    const int jl = (int)strlen(json);
    about_parse(json, jl, out);

    char cont[64];
    if (get_str(json, jl, "Container", cont, sizeof cont)) {
        // A multi-name container ("mov,mp4,m4a,3gp,3g2,mj2") is an MP4.
        if (strstr(cont, "mp4")) snprintf(cont, sizeof cont, "mp4");
        char *comma = strchr(cont, ',');
        if (comma) *comma = '\0';
        snprintf(out->container, sizeof out->container, "%.11s", cont);
        upper(out->container);
    }
    const long long ticks = get_ll(json, jl, "RunTimeTicks", 0);
    if (ticks > 0) out->runtime_secs = (u32)(ticks / 10000000LL);

    const char *arr = strstr(json, "\"MediaStreams\":[");
    if (!arr) return;
    const char *p = arr + 16;

    // The first two audio tracks and the default one, whichever it is.
    char a0[40] = "", a1[40] = "", adef[40] = "";
    bool ll0 = false, lldef = false, have_video = false;
    int  n_audio = 0, def = -1;
    char first_sub[32] = "", def_sub[32] = "";

    while (*p && *p != ']') {
        while (*p && *p != '{' && *p != ']') p++;
        if (*p != '{') break;
        const char *o = p;
        int depth = 0;
        bool in_str = false, esc = false;
        while (*p) {
            const char c = *p;
            if (esc) esc = false;
            else if (in_str) { if (c == '\\') esc = true; else if (c == '"') in_str = false; }
            else if (c == '"') in_str = true;
            else if (c == '{') depth++;
            else if (c == '}' && --depth == 0) { p++; break; }
            p++;
        }
        const int n = (int)(p - o);

        char type[16];
        get_str(o, n, "Type", type, sizeof type);
        if (!strcmp(type, "Video") && !have_video) {
            video_label(o, n, out->video, sizeof out->video);
            have_video = true;
        } else if (!strcmp(type, "Audio")) {
            char lab[40];
            bool ll = false;
            audio_label(o, n, lab, sizeof lab, &ll);
            if (n_audio == 0) { snprintf(a0, sizeof a0, "%s", lab); ll0 = ll; }
            if (n_audio == 1)   snprintf(a1, sizeof a1, "%s", lab);
            if (def < 0 && get_true(o, n, "IsDefault")) {
                def = n_audio;
                snprintf(adef, sizeof adef, "%s", lab);
                lldef = ll;
            }
            n_audio++;
        } else if (!strcmp(type, "Subtitle")) {
            char lab[32];
            subs_label(o, n, lab, sizeof lab);
            if (!out->n_subs) snprintf(first_sub, sizeof first_sub, "%s", lab);
            if (!def_sub[0] && get_true(o, n, "IsDefault"))
                snprintf(def_sub, sizeof def_sub, "%s", lab);
            out->n_subs++;
        }
    }

    // The main track is the default one, or the first when none is marked;
    // the other is the first track that is not the main one.
    const char *other;
    if (def >= 0) {
        snprintf(out->audio, sizeof out->audio, "%s", adef);
        out->lossless = lldef;
        other = def == 0 ? a1 : a0;
    } else {
        snprintf(out->audio, sizeof out->audio, "%s", a0);
        out->lossless = ll0;
        other = a1;
    }
    if (n_audio > 2 && other[0])
        snprintf(out->audio_more, sizeof out->audio_more, "+ %s +%d",
                 other, n_audio - 2);
    else if (n_audio > 1 && other[0])
        snprintf(out->audio_more, sizeof out->audio_more, "+ %s", other);
    snprintf(out->subs, sizeof out->subs, "%s",
             def_sub[0] ? def_sub : first_sub[0] ? first_sub : "NONE");
}
