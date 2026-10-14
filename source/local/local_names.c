// local_names: see local_names.h.

#include "i18n.h"
#include "local_names.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#define MAX_TOKENS 48
#define TOKEN_MAX  48

static bool is_sep(char c) {
    return c == '.' || c == '_' || c == ' ' || c == '(' || c == ')' || c == '[' || c == ']' || c == '{' || c == '}';
}

// Words that start the release's own description, not the title.  Matched whole, in lower case; a
// token with a hyphen ("x264-GRP", "DTS-HD") also matches by its first part.  Short words that
// are also real words in titles ("web", "cam", "ts", "dv", "multi") are left out on purpose.
static const char *const TAGS[] = {
    "480p", "576p", "720p", "1080p", "1080i", "2160p", "4k", "uhd", "hdr", "hdr10", "hdr10+",
    "bluray", "blu-ray", "bdrip", "brrip", "bdremux", "remux", "webrip", "web-dl", "webdl", "hdtv",
    "dvdrip", "dvdscr", "hdrip", "hdcam",
    "x264", "x265", "h264", "h265", "hevc", "avc", "xvid", "divx", "10bit", "10-bit", "8bit",
    "aac", "ac3", "eac3", "dd5", "ddp5", "dts", "dts-hd", "dtshd", "truehd", "atmos", "flac",
    "repack", "unrated", "uncut", "remastered", "imax", "extended",
    "amzn", "dsnp", "hmax", "atvp",
};

static bool is_tag(const char *tok) {
    char low[TOKEN_MAX];
    size_t n = strlen(tok);
    if (n >= sizeof low) n = sizeof low - 1;
    for (size_t i = 0; i < n; i++) low[i] = (char)tolower((unsigned char)tok[i]);
    low[n] = '\0';
    for (size_t t = 0; t < sizeof TAGS / sizeof TAGS[0]; t++)
        if (strcmp(low, TAGS[t]) == 0) return true;
    char *dash = strchr(low, '-');
    if (dash && dash != low) {
        *dash = '\0';
        for (size_t t = 0; t < sizeof TAGS / sizeof TAGS[0]; t++)
            if (strcmp(low, TAGS[t]) == 0) return true;
    }
    return false;
}

static int year_of(const char *tok) {
    if (strlen(tok) != 4) return 0;
    for (int i = 0; i < 4; i++) if (!isdigit((unsigned char)tok[i])) return 0;
    const int y = (tok[0] - '0') * 1000 + (tok[1] - '0') * 100 + (tok[2] - '0') * 10 + (tok[3] - '0');
    return y >= 1900 && y <= 2099 ? y : 0;
}

// How many bytes of s (at most max) can be copied without cutting a UTF-8 sequence in half.
static int clip_utf8(const char *s, int max) {
    int n = 0;
    while (n < max && s[n]) n++;
    if (n == max && s[n]) while (n > 0 && ((unsigned char)s[n] & 0xC0) == 0x80) n--;
    return n;
}

void local_clean_name(const char *file_name, LocalTitle *out) {
    memset(out, 0, sizeof *out);
    if (!file_name) return;
    char name[256];
    snprintf(name, sizeof name, "%.*s", clip_utf8(file_name, (int)sizeof name - 1), file_name);

    // the extension: a last dot with up to 5 letters or digits after it
    char *dot = strrchr(name, '.');
    if (dot && dot != name && strlen(dot + 1) >= 1 && strlen(dot + 1) <= 5) {
        bool alnum = true;
        for (const char *p = dot + 1; *p; p++) if (!isalnum((unsigned char)*p)) alnum = false;
        if (alnum) *dot = '\0';
    }

    // words
    char tok[MAX_TOKENS][TOKEN_MAX];
    int n = 0;
    for (const char *p = name; *p && n < MAX_TOKENS; ) {
        while (*p && is_sep(*p)) p++;
        if (!*p) break;
        int len = 0;
        while (p[len] && !is_sep(p[len])) len++;
        const int take = clip_utf8(p, len < TOKEN_MAX - 1 ? len : TOKEN_MAX - 1);
        memcpy(tok[n], p, (size_t)take);
        tok[n][take] = '\0';
        n++;
        p += len;
    }
    if (n == 0) { snprintf(out->title, sizeof out->title, "%s", file_name); return; }

    int end = n;                                                    // the first release tag
    for (int i = 0; i < n; i++) if (is_tag(tok[i])) { end = i; break; }
    int year_at = -1;                                               // the last year before it, never the first word
    for (int i = 1; i < end; i++) if (year_of(tok[i])) year_at = i;

    int stop = year_at >= 0 ? year_at : end;
    if (stop == 0) stop = n;                                         // nothing but tags: keep the words as they are
    char *o = out->title;
    const size_t cap = sizeof out->title;
    size_t len = 0;
    for (int i = 0; i < stop; i++) {
        const size_t tl = strlen(tok[i]);
        if (len + tl + 2 > cap) break;
        if (len) o[len++] = ' ';
        memcpy(o + len, tok[i], tl);
        len += tl;
    }
    o[len] = '\0';
    if (year_at >= 0) out->year = year_of(tok[year_at]);
}

void local_title_line(const LocalTitle *t, char *out, int cap) {
    if (cap <= 0) return;
    if (t->year) snprintf(out, (size_t)cap, "%s (%d)", t->title, t->year);
    else snprintf(out, (size_t)cap, "%s", t->title);
}

void local_format_size(uint64_t bytes, char *out, int cap) {
    if (cap <= 0) return;
    const double KB = 1024.0, MB = KB * 1024.0, GB = MB * 1024.0, TB = GB * 1024.0;
    const double b = (double)bytes;
    if (b >= TB)      snprintf(out, (size_t)cap, "%.1f TB", b / TB);
    else if (b >= GB) snprintf(out, (size_t)cap, "%.1f GB", b / GB);
    else if (b >= MB) snprintf(out, (size_t)cap, "%.0f MB", b / MB);
    else if (b >= KB) snprintf(out, (size_t)cap, "%.0f KB", b / KB);
    else              snprintf(out, (size_t)cap, "%u B", (unsigned)bytes);
}

void local_format_duration(uint32_t secs, char *out, int cap) {
    if (cap <= 0) return;
    if (secs == 0) { out[0] = '\0'; return; }
    if (secs < 60) { snprintf(out, (size_t)cap, TR("under a minute")); return; }
    const uint32_t mins = (secs + 30) / 60;
    if (mins < 60) snprintf(out, (size_t)cap, TR("%u min"), (unsigned)mins);
    else if (mins % 60 == 0) snprintf(out, (size_t)cap, TR("%u h"), (unsigned)(mins / 60));
    else snprintf(out, (size_t)cap, TR("%u h %u min"), (unsigned)(mins / 60), (unsigned)(mins % 60));
}

void local_format_resume(uint32_t secs, char *out, int cap) {
    if (cap <= 0) return;
    if (secs >= 3600) snprintf(out, (size_t)cap, TR("Resume from %u:%02u:%02u"), (unsigned)(secs / 3600), (unsigned)(secs / 60 % 60), (unsigned)(secs % 60));
    else snprintf(out, (size_t)cap, TR("Resume from %u:%02u"), (unsigned)(secs / 60), (unsigned)(secs % 60));
}
