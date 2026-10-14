// Host test for source/local/local_names.c: how the Media browser names a file, and the size,
// duration and resume texts.
//
//   make -f Makefile.host test_local_names && ./test_local_names

#include "local_names.h"

#include <stdio.h>
#include <string.h>
#include <string>

static int s_checks = 0, s_failed = 0;
#define CHECK(cond) do { s_checks++; if (!(cond)) { s_failed++; \
    printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

static void names() {
    printf("- file names\n");
    struct { const char *file, *title; int year; } t[] = {
        { "The.Movie.Name.2019.1080p.BluRay.x264-GRP.mkv", "The Movie Name", 2019 },
        { "The_Movie_Name_(2019).mkv", "The Movie Name", 2019 },
        { "Blade.Runner.2049.2017.2160p.UHD.BluRay.REMUX.HDR.HEVC.TrueHD.Atmos.7.1-GRP.mkv", "Blade Runner 2049", 2017 },
        { "2001.A.Space.Odyssey.1968.720p.BluRay.DTS.x264.m2ts", "2001 A Space Odyssey", 1968 },
        { "Some Film [1999] 1080p.ts", "Some Film", 1999 },
        { "Plain Name.mkv", "Plain Name", 0 },
        { "Plain Name", "Plain Name", 0 },
        { "Show.Name.S01E02.Episode.Title.1080p.WEB-DL.DD5.1.H.264-GRP.mkv", "Show Name S01E02 Episode Title", 0 },
        { "1080p.mkv", "1080p", 0 },                              // nothing but a tag: the words stay
        { "2012.mkv", "2012", 0 },                                // a year as the first word is a title
        { "Movie.Title.1999.mkv", "Movie Title", 1999 },
        { "Movie.Title.1899.mkv", "Movie Title 1899", 0 },        // not a plausible year
        { "A.Film.With.Ex-Wife.mkv", "A Film With Ex-Wife", 0 },
        { "Marvel's.Agents.2013.x264.mkv", "Marvel's Agents", 2013 },
        { "Film.Name.2020.EXTENDED.1080p.mkv", "Film Name", 2020 },
        { "Name.Of.A.Film.web.mkv", "Name Of A Film web", 0 },    // "web" alone is a real word, not a tag
        { "...mkv", "...mkv", 0 },                                // no words at all: the file name as it is
        { "Caf\xC3\xA9.Society.2016.mkv", "Caf\xC3\xA9 Society", 2016 },
        { "x264.mkv", "x264", 0 },
    };
    for (auto &x : t) {
        LocalTitle lt;
        local_clean_name(x.file, &lt);
        if (strcmp(lt.title, x.title) || lt.year != x.year) printf("  %s -> '%s' %d (want '%s' %d)\n", x.file, lt.title, lt.year, x.title, x.year);
        CHECK(!strcmp(lt.title, x.title) && lt.year == x.year);
    }

    LocalTitle lt;
    local_clean_name("The.Movie.Name.2019.1080p.mkv", &lt);
    char line[160];
    local_title_line(&lt, line, sizeof line);
    CHECK(!strcmp(line, "The Movie Name (2019)"));
    local_clean_name("Plain.mkv", &lt);
    local_title_line(&lt, line, sizeof line);
    CHECK(!strcmp(line, "Plain"));

    // robustness: empty, NULL, very long, many words
    local_clean_name("", &lt);
    CHECK(lt.title[0] == '\0' && lt.year == 0);
    local_clean_name(nullptr, &lt);
    CHECK(lt.title[0] == '\0');
    char big[1200];
    memset(big, 'a', sizeof big - 1);
    big[sizeof big - 1] = '\0';
    local_clean_name(big, &lt);
    CHECK(strlen(lt.title) > 0 && strlen(lt.title) < sizeof lt.title);
    std::string many;
    for (int i = 0; i < 100; i++) many += "word.";
    many += "mkv";
    local_clean_name(many.c_str(), &lt);
    CHECK(strlen(lt.title) > 0 && strlen(lt.title) < sizeof lt.title);
    // a title cut inside a UTF-8 sequence would show a broken glyph
    std::string wide;
    for (int i = 0; i < 80; i++) wide += "\xC3\xA9";                      // 160 bytes: one 160-byte word
    wide += ".mkv";
    local_clean_name(wide.c_str(), &lt);
    bool whole = true;
    for (const unsigned char *p = (const unsigned char *)lt.title; *p; ) {
        if (*p == 0xC3 && (p[1] & 0xC0) == 0x80) p += 2; else { whole = false; break; }
    }
    CHECK(whole && lt.title[0]);
}

static void sizes() {
    printf("- sizes, durations, resume\n");
    char b[64];
    struct { uint64_t n; const char *s; } sz[] = {
        { 0, "0 B" }, { 1, "1 B" }, { 1023, "1023 B" }, { 1024, "1 KB" }, { 37 * 1024, "37 KB" },
        { 812ull * 1024 * 1024, "812 MB" }, { 1024ull * 1024 * 1024, "1.0 GB" },
        { 4509715660ull, "4.2 GB" }, { 1ull << 40, "1.0 TB" }, { 2ull << 40, "2.0 TB" },
    };
    for (auto &x : sz) {
        local_format_size(x.n, b, sizeof b);
        if (strcmp(b, x.s)) printf("  %llu -> %s (want %s)\n", (unsigned long long)x.n, b, x.s);
        CHECK(!strcmp(b, x.s));
    }
    struct { uint32_t s; const char *t; } du[] = {
        { 0, "" }, { 1, "under a minute" }, { 59, "under a minute" }, { 60, "1 min" }, { 89, "1 min" }, { 90, "2 min" },
        { 47 * 60, "47 min" }, { 3600, "1 h" }, { 3600 + 29, "1 h" }, { 3600 + 31, "1 h 1 min" },
        { 6720, "1 h 52 min" }, { 7200, "2 h" }, { 3 * 3600 + 25 * 60, "3 h 25 min" },
    };
    for (auto &x : du) {
        local_format_duration(x.s, b, sizeof b);
        if (strcmp(b, x.t)) printf("  %u s -> '%s' (want '%s')\n", x.s, b, x.t);
        CHECK(!strcmp(b, x.t));
    }
    local_format_resume(245, b, sizeof b);
    CHECK(!strcmp(b, "Resume from 4:05"));
    local_format_resume(3600 + 12 * 60 + 30, b, sizeof b);
    CHECK(!strcmp(b, "Resume from 1:12:30"));
    local_format_resume(59, b, sizeof b);
    CHECK(!strcmp(b, "Resume from 0:59"));
    char tiny[4];
    local_format_size(4509715660ull, tiny, sizeof tiny);                  // cut, still terminated
    CHECK(strlen(tiny) == 3);
    local_format_size(5, tiny, 0);                                        // no room: nothing written
}

int main() {
    names();
    sizes();
    printf("local names: %d checks, %d failed\n", s_checks, s_failed);
    return s_failed ? 1 : 0;
}
