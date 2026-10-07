// A short, readable name for one version (MediaSource) -- see version_summary.h.

#include "version_summary.h"
#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

// Case-insensitive search for `w` in s as a whole token: the characters on
// either side are not letters or digits.  Returns the match or NULL.
static const char *find_tok(const char *s, const char *w)
{
    const size_t n = strlen(w);
    for (const char *p = s; *p; p++) {
        size_t i = 0;
        while (i < n && p[i] && tolower((unsigned char)p[i]) == tolower((unsigned char)w[i])) i++;
        if (i != n) continue;
        const bool left_ok  = p == s || !isalnum((unsigned char)p[-1]);
        const bool right_ok = !isalnum((unsigned char)p[n]);
        if (left_ok && right_ok) return p;
    }
    return NULL;
}

static void add(char *out, size_t cap, const char *part)
{
    if (!part || !part[0]) return;
    const size_t len = strlen(out);
    if (len + 1 >= cap) return;
    snprintf(out + len, cap - len, "%s%s", len ? " \xC2\xB7 " : "", part);
}

void version_summary(const char *raw, char *out, size_t cap)
{
    if (!out || cap == 0) return;
    out[0] = 0;
    if (!raw) return;

    // resolution
    if (find_tok(raw, "2160p") || find_tok(raw, "4K") || find_tok(raw, "UHD")) add(out, cap, "4K");
    else if (find_tok(raw, "1080p") || find_tok(raw, "1080i"))                 add(out, cap, "1080p");
    else if (find_tok(raw, "720p"))                                            add(out, cap, "720p");
    else if (find_tok(raw, "576p") || find_tok(raw, "480p"))                   add(out, cap, "SD");

    // source
    if (find_tok(raw, "REMUX") || find_tok(raw, "BDRemux"))                    add(out, cap, "REMUX");
    else if (find_tok(raw, "BluRay") || find_tok(raw, "Blu-ray") || find_tok(raw, "BDRip") ||
             find_tok(raw, "BRRip"))                                           add(out, cap, "BluRay");
    else if (find_tok(raw, "WEB-DL") || find_tok(raw, "WEBDL"))                add(out, cap, "WEB-DL");
    else if (find_tok(raw, "WEBRip") || find_tok(raw, "WEB"))                  add(out, cap, "WEBRip");
    else if (find_tok(raw, "HDTV"))                                            add(out, cap, "HDTV");

    // 3D (the PS3 shows it squashed: worth knowing before picking it)
    if (find_tok(raw, "3D") || find_tok(raw, "H-OU") || find_tok(raw, "HSBS") ||
        find_tok(raw, "SBS") || find_tok(raw, "OU"))                           add(out, cap, "3D");

    // HDR
    if (find_tok(raw, "DV") || find_tok(raw, "DoVi") || find_tok(raw, "Dolby Vision")) add(out, cap, "DV");
    else if (find_tok(raw, "HDR10+") || find_tok(raw, "HDR10") || find_tok(raw, "HDR")) add(out, cap, "HDR");

    // video codec
    if (find_tok(raw, "HEVC") || find_tok(raw, "x265") || find_tok(raw, "H.265") ||
        find_tok(raw, "H265"))                                                 add(out, cap, "HEVC");
    else if (find_tok(raw, "AV1"))                                             add(out, cap, "AV1");
    else if (find_tok(raw, "AVC") || find_tok(raw, "x264") || find_tok(raw, "H.264") ||
             find_tok(raw, "H264"))                                            add(out, cap, "H.264");

    // audio, with its channels when stated
    {
        char a[32] = "";
        if (find_tok(raw, "TrueHD") && find_tok(raw, "Atmos"))  snprintf(a, sizeof a, "TrueHD Atmos");
        else if (find_tok(raw, "TrueHD"))                       snprintf(a, sizeof a, "TrueHD");
        else if (find_tok(raw, "DTS-HD MA") || find_tok(raw, "DTS-HD.MA") || find_tok(raw, "DTS-HD-MA") ||
                 (find_tok(raw, "DTS-HD") && find_tok(raw, "MA"))) snprintf(a, sizeof a, "DTS-HD MA");
        else if (find_tok(raw, "DTS:X") || find_tok(raw, "DTS-X")) snprintf(a, sizeof a, "DTS:X");
        else if (find_tok(raw, "DTS-HD"))                       snprintf(a, sizeof a, "DTS-HD");
        else if (find_tok(raw, "DTS"))                          snprintf(a, sizeof a, "DTS");
        else if (find_tok(raw, "DD+") || find_tok(raw, "DDP") || find_tok(raw, "EAC3") ||
                 find_tok(raw, "E-AC-3") || find_tok(raw, "DDP5"))  snprintf(a, sizeof a, "DD+");
        else if (find_tok(raw, "AC3") || find_tok(raw, "DD"))   snprintf(a, sizeof a, "DD");
        else if (find_tok(raw, "AAC"))                          snprintf(a, sizeof a, "AAC");
        else if (find_tok(raw, "FLAC"))                         snprintf(a, sizeof a, "FLAC");
        if (a[0]) {
            const char *ch = find_tok(raw, "7.1") ? " 7.1" : find_tok(raw, "5.1") ? " 5.1"
                           : find_tok(raw, "2.0") ? " 2.0" : "";
            char b[40]; snprintf(b, sizeof b, "%s%s", a, ch);
            add(out, cap, b);
        }
    }

    // size: "<number> GB"
    {
        const char *g = raw;
        while ((g = strstr(g, "GB")) != NULL) {
            const char *e = g;
            while (e > raw && e[-1] == ' ') e--;
            const char *b = e;
            while (b > raw && (isdigit((unsigned char)b[-1]) || b[-1] == '.')) b--;
            if (b < e && (g[2] == 0 || !isalnum((unsigned char)g[2]))) {
                char sz[24];
                snprintf(sz, sizeof sz, "%.*s GB", (int)(e - b), b);
                add(out, cap, sz);
                break;
            }
            g += 2;
        }
    }

    // language: the text after the globe emoji (U+1F30E), to the line's end
    {
        const char *l = strstr(raw, "\xF0\x9F\x8C\x8E");
        if (l) {
            l += 4;
            while (*l == ' ') l++;
            char lang[28]; int n = 0;
            // to the line's end, or the next emoji (the subtitle list, the file)
            while (*l && *l != '\n' && *l != '\r' && (unsigned char)*l != 0xF0 &&
                   n < (int)sizeof lang - 1) lang[n++] = *l++;
            while (n > 0 && (lang[n - 1] == ' ' || lang[n - 1] == '|' || lang[n - 1] == '/')) n--;
            lang[n] = 0;
            add(out, cap, lang);
        }
    }

    if (!out[0]) snprintf(out, cap, "Version");
}
