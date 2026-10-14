// Host test for Live TV parsing and arithmetic (source/api/livetv.cpp).
//
// The two JSON fixtures are written by hand in the shape a Jellyfin 10.11
// server answers /LiveTv/Channels and /LiveTv/Programs with; they hold no
// token, user id or address.  They cover what the real replies carry that the
// parser must get right: escapes and non-ASCII in names, braces inside
// strings, nested ImageTags / UserData / CurrentProgram, a channel with no
// number, a programme with no episode title, overlapping, gapped and
// zero-length programmes, a zone offset, and a reply cut short.
//
//   make -f Makefile.host test_livetv && ./test_livetv
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "livetv.h"

static int s_checks = 0, s_failed = 0;
#define CHECK(cond) do { s_checks++; if (!(cond)) { s_failed++; \
    printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

static char *slurp(const char *path, int *len) {
    FILE *f = fopen(path, "rb");
    if (!f) { printf("cannot open %s\n", path); exit(2); }
    fseek(f, 0, SEEK_END);
    *len = (int)ftell(f);
    fseek(f, 0, SEEK_SET);
    char *b = (char *)malloc((size_t)*len + 1);
    if (fread(b, 1, (size_t)*len, f) != (size_t)*len) exit(2);
    b[*len] = '\0';
    fclose(f);
    return b;
}

// An independent reference: glibc's timegm.
static uint64_t ref_ticks(int Y, int M, int D, int h, int m, int s, uint64_t frac = 0) {
    struct tm t;
    memset(&t, 0, sizeof t);
    t.tm_year = Y - 1900; t.tm_mon = M - 1; t.tm_mday = D;
    t.tm_hour = h; t.tm_min = m; t.tm_sec = s;
    return jf_ticks_from_unix((uint64_t)timegm(&t)) + frac;
}

static void time_parsing(void) {
    printf("- iso8601\n");
    CHECK(jf_ticks_from_iso8601("1970-01-01T00:00:00Z") == 621355968000000000ULL);
    CHECK(jf_ticks_from_iso8601("2026-10-03T19:30:00.0000000Z") == ref_ticks(2026, 10, 3, 19, 30, 0));
    CHECK(jf_ticks_from_iso8601("2026-10-03T19:30:00Z") == ref_ticks(2026, 10, 3, 19, 30, 0));
    CHECK(jf_ticks_from_iso8601("2026-10-03T19:30:00") == ref_ticks(2026, 10, 3, 19, 30, 0));   // no zone: UTC
    CHECK(jf_ticks_from_iso8601("2026-10-03T19:30:00.0000001Z") == ref_ticks(2026, 10, 3, 19, 30, 0, 1));
    CHECK(jf_ticks_from_iso8601("2026-10-03T19:30:00.5Z") == ref_ticks(2026, 10, 3, 19, 30, 0, 5000000));
    CHECK(jf_ticks_from_iso8601("2026-10-03T19:30:00.123456789Z") == ref_ticks(2026, 10, 3, 19, 30, 0, 1234567));
    // Offsets are subtracted: 21:00+02:00 is 19:00 UTC.
    CHECK(jf_ticks_from_iso8601("2026-10-03T21:00:00+02:00") == ref_ticks(2026, 10, 3, 19, 0, 0));
    CHECK(jf_ticks_from_iso8601("2026-10-03T14:00:00-05:00") == ref_ticks(2026, 10, 3, 19, 0, 0));
    CHECK(jf_ticks_from_iso8601("2026-10-03T19:30:00+05:30") == ref_ticks(2026, 10, 3, 14, 0, 0));
    CHECK(jf_ticks_from_iso8601("2026-10-03T21:00:00+0200") == ref_ticks(2026, 10, 3, 19, 0, 0));
    // Calendar edges.
    CHECK(jf_ticks_from_iso8601("2024-02-29T23:59:59Z") == ref_ticks(2024, 2, 29, 23, 59, 59));
    CHECK(jf_ticks_from_iso8601("2026-12-31T23:59:59Z") == ref_ticks(2026, 12, 31, 23, 59, 59));
    CHECK(jf_ticks_from_iso8601("2000-03-01T00:00:00Z") == ref_ticks(2000, 3, 1, 0, 0, 0));
    CHECK(jf_ticks_from_iso8601("2026-10-03 19:30:00Z") == ref_ticks(2026, 10, 3, 19, 30, 0));
    // Not timestamps.
    CHECK(jf_ticks_from_iso8601(NULL) == 0);
    CHECK(jf_ticks_from_iso8601("") == 0);
    CHECK(jf_ticks_from_iso8601("garbage") == 0);
    CHECK(jf_ticks_from_iso8601("2026-10-03") == 0);
    CHECK(jf_ticks_from_iso8601("2026-13-01T00:00:00Z") == 0);
    CHECK(jf_ticks_from_iso8601("2026-10-03T25:00:00Z") == 0);
    CHECK(jf_ticks_from_iso8601("20261003T193000Z") == 0);

    // And back, in the form the server's date filters take.
    char b[40];
    const uint64_t t = ref_ticks(2026, 10, 3, 19, 30, 0);
    CHECK(jf_iso8601_from_ticks(t, b, sizeof b) > 0 && !strcmp(b, "2026-10-03T19:30:00.0000000Z"));
    CHECK(jf_iso8601_from_ticks(t + 5000000, b, sizeof b) > 0 && !strcmp(b, "2026-10-03T19:30:00.5000000Z"));
    CHECK(jf_iso8601_from_ticks(jf_ticks_from_iso8601("2024-02-29T23:59:59.0000001Z"), b, sizeof b) > 0 &&
          !strcmp(b, "2024-02-29T23:59:59.0000001Z"));
    CHECK(jf_iso8601_from_ticks(621355968000000000ULL, b, sizeof b) > 0 && !strcmp(b, "1970-01-01T00:00:00.0000000Z"));
    CHECK(jf_iso8601_from_ticks(t, b, 10) == 0);            // too small a buffer
}

static void channels(void) {
    printf("- channels\n");
    int len;
    char *json = slurp("fixtures/livetv_channels.json", &len);
    JFChannel c[8];
    int total = -1;
    bool trunc = true;
    const int n = livetv_parse_channels(json, len, c, 8, &total, &trunc);
    CHECK(n == 4 && total == 4 && !trunc);
    CHECK(!strcmp(c[0].id, "aa11aa11aa11aa11aa11aa11aa11aa11") && !strcmp(c[0].name, "NASA TV Public"));
    CHECK(!strcmp(c[0].number, "1") && !strcmp(c[0].logo_tag, "tag0001") && c[0].favourite);
    CHECK(!strcmp(c[0].now_title, "Live from the Station"));
    CHECK(c[0].now_start_ticks == ref_ticks(2026, 10, 3, 18, 0, 0));
    CHECK(c[0].now_end_ticks == ref_ticks(2026, 10, 3, 19, 0, 0));
    // No number given: the empty string; no ImageTags.Primary: no logo.
    CHECK(!strcmp(c[1].name, "NASA TV Media") && !strcmp(c[1].number, "2"));
    CHECK(c[1].logo_tag[0] == '\0' && !c[1].favourite && c[1].now_title[0] == '\0');
    CHECK(c[1].now_start_ticks == 0 && c[1].now_end_ticks == 0);
    // Escapes, non-ASCII, braces inside a string, a zone-less fraction.
    CHECK(!strcmp(c[2].name, "Caf\xC3\xA9 \"Red\" Bull TV"));
    CHECK(!strcmp(c[2].now_title, "Braces {and} [brackets] in a title"));
    CHECK(c[2].now_start_ticks == ref_ticks(2026, 10, 3, 19, 30, 0));
    CHECK(c[2].now_end_ticks == ref_ticks(2026, 10, 3, 20, 15, 30, 5000000));
    CHECK(!strcmp(c[2].logo_tag, "tag0003"));       // not the sibling Thumb tag
    CHECK(c[3].number[0] == '\0' && !strcmp(c[3].logo_tag, "tag0004"));

    // A cap on how many are kept.
    JFChannel two[2];
    CHECK(livetv_parse_channels(json, len, two, 2, &total, &trunc) == 2 && total == 4);

    // A reply cut off by the response cap: what came whole, and a flag.
    JFChannel part[8];
    const int cut = len * 6 / 10;
    const int pn = livetv_parse_channels(json, cut, part, 8, &total, &trunc);
    CHECK(pn >= 1 && pn < 4 && trunc);
    CHECK(pn == 0 || !strcmp(part[0].name, "NASA TV Public"));

    // Nothing there.
    CHECK(livetv_parse_channels("", 0, c, 8, &total, &trunc) == 0 && total == 0);
    CHECK(livetv_parse_channels("{\"Items\":[],\"TotalRecordCount\":0}", 32, c, 8, &total, NULL) == 0);
    CHECK(livetv_parse_channels("{\"Error\":\"x\"}", 13, c, 8, &total, NULL) == 0);
    CHECK(livetv_parse_channels("not json", 8, c, 8, &total, NULL) == 0);
    CHECK(livetv_parse_channels(NULL, 0, c, 8, NULL, NULL) == 0);
    free(json);
}

static void programmes(void) {
    printf("- programmes\n");
    int len;
    char *json = slurp("fixtures/livetv_programs.json", &len);
    JFProgram p[8];
    int total = 0;
    bool trunc = true;
    const int n = livetv_parse_programs(json, len, p, 8, &total, &trunc);
    CHECK(n == 6 && total == 6 && !trunc);
    CHECK(!strcmp(p[0].name, "Morning Report") && p[0].is_news && p[0].is_live && !p[0].is_series);
    CHECK(p[0].episode_title[0] == '\0');
    CHECK(!strcmp(p[1].episode_title, "Expedition Update") && p[1].is_series && p[1].is_premiere);
    CHECK(!strcmp(p[1].channel_id, "aa11aa11aa11aa11aa11aa11aa11aa11"));
    CHECK(p[2].is_movie && p[3].is_kids && p[5].is_sports);
    CHECK(p[4].start_ticks == p[4].end_ticks && p[4].start_ticks != 0);   // zero length
    // 21:00+02:00 is 19:00Z.
    CHECK(p[5].start_ticks == ref_ticks(2026, 10, 3, 19, 0, 0));
    CHECK(p[5].end_ticks == ref_ticks(2026, 10, 3, 20, 0, 0));

    // What is on when: one channel's programmes, out of order allowed.
    JFProgram ch[5] = { p[0], p[1], p[2], p[3], p[4] };
    CHECK(livetv_programme_at(ch, 5, ref_ticks(2026, 10, 3, 17, 30, 0)) == 0);
    CHECK(livetv_programme_at(ch, 5, ref_ticks(2026, 10, 3, 18, 0, 0)) == 1);   // start is inclusive
    CHECK(livetv_programme_at(ch, 5, ref_ticks(2026, 10, 3, 18, 59, 59)) == 1);
    CHECK(livetv_programme_at(ch, 5, ref_ticks(2026, 10, 3, 19, 0, 0)) == -1);  // end is exclusive; zero length never airs
    CHECK(livetv_programme_at(ch, 5, ref_ticks(2026, 10, 3, 19, 45, 0)) == -1); // the gap
    CHECK(livetv_programme_at(ch, 5, ref_ticks(2026, 10, 3, 20, 15, 0)) == 2);
    CHECK(livetv_programme_at(ch, 5, ref_ticks(2026, 10, 3, 20, 45, 0)) == 3);  // overlap: the later start
    CHECK(livetv_programme_at(ch, 5, ref_ticks(2026, 10, 3, 21, 15, 0)) == 3);
    CHECK(livetv_programme_at(ch, 5, ref_ticks(2026, 10, 3, 22, 0, 0)) == -1);
    CHECK(livetv_programme_at(ch, 0, ref_ticks(2026, 10, 3, 18, 0, 0)) == -1);
    free(json);
}

static void list_rules(void) {
    printf("- list rules\n");
    int len;
    char *json = slurp("fixtures/livetv_channels.json", &len);
    JFChannel c[8];
    int total = 0;
    int n = livetv_parse_channels(json, len, c, 8, &total, NULL);
    // Reverse them, so the sort has work to do.
    for (int i = 0; i < n / 2; i++) { JFChannel t = c[i]; c[i] = c[n - 1 - i]; c[n - 1 - i] = t; }
    livetv_sort_channels(c, n);
    // The favourite first, then 2, then 10 (numerically, not as text), then the
    // channel with no number.
    CHECK(!strcmp(c[0].name, "NASA TV Public"));
    CHECK(!strcmp(c[1].number, "2"));
    CHECK(!strcmp(c[2].number, "10"));
    CHECK(c[3].number[0] == '\0' && !strcmp(c[3].name, "Bipbop Test Pattern"));
    // Equal numbers fall back to the name, and a sort of a sorted list is a no-op.
    JFChannel eq[3];
    memset(eq, 0, sizeof eq);
    snprintf(eq[0].name, sizeof eq[0].name, "Zulu");  snprintf(eq[0].number, sizeof eq[0].number, "5");
    snprintf(eq[1].name, sizeof eq[1].name, "Alpha"); snprintf(eq[1].number, sizeof eq[1].number, "5");
    snprintf(eq[2].name, sizeof eq[2].name, "Mid");   snprintf(eq[2].number, sizeof eq[2].number, "5.1");
    livetv_sort_channels(eq, 3);
    CHECK(!strcmp(eq[0].name, "Alpha") && !strcmp(eq[1].name, "Zulu") && !strcmp(eq[2].name, "Mid"));
    livetv_sort_channels(eq, 3);
    CHECK(!strcmp(eq[0].name, "Alpha") && !strcmp(eq[1].name, "Zulu"));
    free(json);

    // Progress through a programme.
    const uint64_t s = ref_ticks(2026, 10, 3, 18, 0, 0), e = ref_ticks(2026, 10, 3, 19, 0, 0);
    CHECK(livetv_progress_permille(s - 1, s, e) == 0);
    CHECK(livetv_progress_permille(s, s, e) == 0);
    CHECK(livetv_progress_permille(ref_ticks(2026, 10, 3, 18, 30, 0), s, e) == 500);
    CHECK(livetv_progress_permille(ref_ticks(2026, 10, 3, 18, 45, 0), s, e) == 750);
    CHECK(livetv_progress_permille(e, s, e) == 1000 && livetv_progress_permille(e + 1, s, e) == 1000);
    CHECK(livetv_progress_permille(s, 0, 0) == -1 && livetv_progress_permille(s, e, s) == -1);
    CHECK(livetv_progress_permille(s, s, s) == -1);

    // Channel up and down, wrapping.
    CHECK(livetv_step_channel(0, 4, +1) == 1 && livetv_step_channel(3, 4, +1) == 0);
    CHECK(livetv_step_channel(0, 4, -1) == 3 && livetv_step_channel(2, 4, -1) == 1);
    CHECK(livetv_step_channel(-1, 4, +1) == 0 && livetv_step_channel(-1, 4, -1) == 3);
    CHECK(livetv_step_channel(9, 4, +1) == 0 && livetv_step_channel(9, 4, -1) == 3);
    CHECK(livetv_step_channel(0, 1, +1) == 0 && livetv_step_channel(0, 1, -1) == 0);
    CHECK(livetv_step_channel(0, 0, +1) == -1);

    // Rapid presses open only the last channel.
    CHECK(!livetv_debounce_ready(1000000, 1200000, 600000));     // 200 ms after a press
    CHECK(!livetv_debounce_ready(1000000, 1599999, 600000));
    CHECK(livetv_debounce_ready(1000000, 1600000, 600000));
    CHECK(livetv_debounce_ready(1000000, 5000000, 600000));
    CHECK(!livetv_debounce_ready(2000000, 1000000, 600000));     // a clock that stepped back
}

static void clock_format(void) {
    printf("- clock\n");
    char b[16];
    const uint64_t t = jf_ticks_from_iso8601("2026-10-03T19:30:00Z");
    livetv_format_hm(t, 0, b, sizeof b);            CHECK(!strcmp(b, "19:30"));
    livetv_format_hm(t, 3600, b, sizeof b);         CHECK(!strcmp(b, "20:30"));      // summer time
    livetv_format_hm(t, -5 * 3600, b, sizeof b);    CHECK(!strcmp(b, "14:30"));
    livetv_format_hm(t, 5 * 3600 + 1800, b, sizeof b); CHECK(!strcmp(b, "01:00"));   // past midnight
    livetv_format_hm(t, -20 * 3600, b, sizeof b);   CHECK(!strcmp(b, "23:30"));      // before it
}

// Paging a long list by category.  The bases are the ones the tab uses.
static void page_ranges(void) {
    printf("- page ranges\n");
    const int bases[] = { 1, 101, 151, 1000, 9000 };
    int first[8], count[8], cat[8];

    // A list as the server sorts it: 1-3 (page 0), 101-104 (page 1), nothing in
    // 151-999, 1000-1001 (page 3), 9000- (page 4).  The empty category has no page.
    const float a[] = { 1, 2, 3, 101, 102, 103, 104, 1000, 1001, 9000, 9001, 9002 };
    int np = livetv_page_ranges(a, 12, bases, 5, first, count, cat, 8);
    CHECK(np == 4);
    CHECK(first[0] == 0  && count[0] == 3 && cat[0] == 0);
    CHECK(first[1] == 3  && count[1] == 4 && cat[1] == 1);
    CHECK(first[2] == 7  && count[2] == 2 && cat[2] == 3);
    CHECK(first[3] == 9  && count[3] == 3 && cat[3] == 4);

    // Numbers below the first base, and a channel without a number (-1), join
    // the page they sit in front of.
    const float b[] = { -1, 0.5f, 1, 2, 120 };
    np = livetv_page_ranges(b, 5, bases, 5, first, count, cat, 8);
    CHECK(np == 2 && first[0] == 0 && count[0] == 4 && cat[0] == 0);
    CHECK(first[1] == 4 && count[1] == 1 && cat[1] == 1);

    // Fractions stay with their whole number's block; the last page runs to the end.
    const float c[] = { 100.5f, 101, 150.9f, 151, 99999 };
    np = livetv_page_ranges(c, 5, bases, 5, first, count, cat, 8);
    CHECK(np == 4);
    CHECK(first[0] == 0 && count[0] == 1 && cat[0] == 0);
    CHECK(first[1] == 1 && count[1] == 2 && cat[1] == 1);
    CHECK(first[2] == 3 && count[2] == 1 && cat[2] == 2);
    CHECK(first[3] == 4 && count[3] == 1 && cat[3] == 4);

    // Every channel lands on exactly one page, in order, and `max` is respected.
    int total = 0;
    np = livetv_page_ranges(a, 12, bases, 5, first, count, cat, 8);
    for (int i = 0; i < np; i++) { CHECK(first[i] == total); total += count[i]; }
    CHECK(total == 12);
    CHECK(livetv_page_ranges(a, 12, bases, 5, first, count, cat, 2) == 2);

    // Nothing to page.
    CHECK(livetv_page_ranges(a, 0, bases, 5, first, count, cat, 8) == 0);
    CHECK(livetv_page_ranges(a, 12, bases, 0, first, count, cat, 8) == 0);
    CHECK(livetv_page_ranges(NULL, 12, bases, 5, first, count, cat, 8) == 0);

    double v = 0;
    CHECK(livetv_number_value("101", &v) && v == 101);
    CHECK(livetv_number_value("5.1", &v) && v > 5.09 && v < 5.11);
    CHECK(!livetv_number_value("", &v) && !livetv_number_value("abc", &v));
}

int main(void) {
    time_parsing();
    clock_format();
    channels();
    programmes();
    list_rules();
    page_ranges();
    printf("live tv: %d checks, %d failed\n", s_checks, s_failed);
    return s_failed ? 1 : 0;
}
