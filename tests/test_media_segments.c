// media_segments_parse against real and hostile /MediaSegments bodies.
#include <stdio.h>
#include <string.h>
#include <math.h>

#include "media_segments.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

int main(void) {
    MediaSegment s[MEDIA_SEGMENTS_MAX];

    // Verbatim shape from the user's server (Better Call Saul S01E06,
    // media source 058a35cf..., Gelato IntroDB), with a BOM.
    const char *real =
        "\xEF\xBB\xBF{\"Items\":[{\"Id\":\"5f1e\",\"ItemId\":\"058a35cf3cdd929b1d43d0be571d9176\","
        "\"Type\":\"Intro\",\"StartTicks\":2280000000,\"EndTicks\":2400000000}],"
        "\"TotalRecordCount\":1,\"StartIndex\":0}";
    int n = media_segments_parse(real, (int)strlen(real), s, MEDIA_SEGMENTS_MAX);
    CHECK(n == 1);
    CHECK(s[0].type == SEG_INTRO);
    CHECK(fabs(s[0].start_secs - 228.0) < 1e-6 && fabs(s[0].end_secs - 240.0) < 1e-6);
    CHECK(strcmp(media_segment_skip_label(s[0].type), "Skip Intro") == 0);

    // Several types, key order shuffled, whitespace, a nested object that
    // carries its own StartTicks which must NOT be read as the segment's.
    const char *multi =
        "{ \"TotalRecordCount\": 3, \"Items\": [\n"
        "  {\"EndTicks\": 900000000, \"Type\": \"Recap\", \"StartTicks\": 0},\n"
        "  {\"Type\":\"Outro\",\"Extra\":{\"StartTicks\":1},\"StartTicks\":25000000000,\"EndTicks\":26000000000},\n"
        "  {\"Type\":\"Mystery\",\"StartTicks\":10,\"EndTicks\":20}\n"
        "]}";
    n = media_segments_parse(multi, (int)strlen(multi), s, MEDIA_SEGMENTS_MAX);
    CHECK(n == 3);
    CHECK(s[0].type == SEG_RECAP && s[0].start_secs == 0.0 && fabs(s[0].end_secs - 90.0) < 1e-6);
    CHECK(s[1].type == SEG_OUTRO && fabs(s[1].start_secs - 2500.0) < 1e-6);
    CHECK(s[2].type == SEG_UNKNOWN && media_segment_skip_label(s[2].type) == NULL);

    // Empty, junk, truncated, backwards and capped.
    const char *empty = "{\"Items\":[],\"TotalRecordCount\":0,\"StartIndex\":0}";
    CHECK(media_segments_parse(empty, (int)strlen(empty), s, MEDIA_SEGMENTS_MAX) == 0);
    CHECK(media_segments_parse("<html>502</html>", 16, s, MEDIA_SEGMENTS_MAX) == 0);
    const char *trunc = "{\"Items\":[{\"Type\":\"Intro\",\"StartTicks\":1";
    CHECK(media_segments_parse(trunc, (int)strlen(trunc), s, MEDIA_SEGMENTS_MAX) == 0);
    const char *back = "{\"Items\":[{\"Type\":\"Intro\",\"StartTicks\":50,\"EndTicks\":10}]}";
    CHECK(media_segments_parse(back, (int)strlen(back), s, MEDIA_SEGMENTS_MAX) == 0);
    CHECK(media_segments_parse(multi, (int)strlen(multi), s, 2) == 2);
    CHECK(media_segments_parse(NULL, 0, s, MEDIA_SEGMENTS_MAX) == 0);

    printf("%s\n", fails ? "FAIL" : "PASS");
    return fails ? 1 : 0;
}
