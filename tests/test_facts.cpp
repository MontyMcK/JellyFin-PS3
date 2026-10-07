// Host test for source/api/facts_parse.cpp -- the words on the Home queue's
// tech strip and the detail page's audio chip.
//
//   mingw32-make -s -B -f Makefile.host test_facts && ./test_facts.exe

#include <stdio.h>
#include <string.h>

#include "api_facts.h"

static int s_fail = 0;

static void eq(const char *what, const char *got, const char *want) {
    if (strcmp(got, want) != 0) {
        printf("FAIL %s: got \"%s\", want \"%s\"\n", what, got, want);
        s_fail++;
    }
}
static void yes(const char *what, bool v) {
    if (!v) { printf("FAIL %s\n", what); s_fail++; }
}

int main(void) {
    // 1. A Blu-ray remux: DTS-HD MA default, an AC-3 commentary, PGS subs.
    {
        const char *j =
            "{\"Items\":[{\"Name\":\"Interstellar\",\"Id\":\"abc\",\"Container\":\"mkv\","
            "\"RunTimeTicks\":101400000000,\"MediaStreams\":["
            "{\"Codec\":\"h264\",\"Type\":\"Video\",\"Width\":1920,\"Height\":1080,"
            "\"VideoRange\":\"SDR\",\"VideoRangeType\":\"SDR\",\"Index\":0},"
            "{\"Codec\":\"dts\",\"Profile\":\"DTS-HD MA\",\"Channels\":6,\"Type\":\"Audio\","
            "\"DisplayTitle\":\"English - DTS-HD MA - 5.1 - Default\",\"IsDefault\":true},"
            "{\"Codec\":\"ac3\",\"Channels\":6,\"Type\":\"Audio\",\"IsDefault\":false,"
            "\"DisplayTitle\":\"English - Dolby Digital - 5.1\"},"
            "{\"Codec\":\"PGSSUB\",\"Language\":\"eng\",\"Type\":\"Subtitle\",\"IsDefault\":false},"
            "{\"Codec\":\"subrip\",\"Language\":\"fre\",\"Type\":\"Subtitle\",\"IsDefault\":false}"
            "]}],\"TotalRecordCount\":1}";
        ItemFacts f;
        facts_parse(j, &f);
        eq("1 container", f.container, "MKV");
        eq("1 video", f.video, "1080P H.264");
        eq("1 audio", f.audio, "DTS-HD MA 5.1");
        eq("1 audio_more", f.audio_more, "+ AC-3 5.1");
        eq("1 subs", f.subs, "PGS ENG");
        yes("1 lossless", f.lossless);
        yes("1 n_subs", f.n_subs == 2);
        yes("1 runtime", f.runtime_secs == 10140);
    }
    // 2. 4K HDR10 TrueHD Atmos 7.1 as the SECOND track, marked default; no subs.
    {
        const char *j =
            "{\"Items\":[{\"Container\":\"mov,mp4,m4a,3gp,3g2,mj2\",\"MediaStreams\":["
            "{\"Codec\":\"hevc\",\"Type\":\"Video\",\"Width\":3840,\"Height\":2160,"
            "\"VideoRange\":\"HDR\",\"VideoRangeType\":\"HDR10\"},"
            "{\"Codec\":\"eac3\",\"Channels\":6,\"Type\":\"Audio\",\"IsDefault\":false},"
            "{\"Codec\":\"truehd\",\"Profile\":\"TrueHD + Dolby Atmos\",\"Channels\":8,"
            "\"Type\":\"Audio\",\"IsDefault\":true},"
            "{\"Codec\":\"aac\",\"Channels\":2,\"Type\":\"Audio\",\"IsDefault\":false}"
            "]}]}";
        ItemFacts f;
        facts_parse(j, &f);
        eq("2 container", f.container, "MP4");
        eq("2 video", f.video, "4K HEVC HDR10");
        eq("2 audio", f.audio, "TRUEHD ATMOS 7.1");
        eq("2 audio_more", f.audio_more, "+ E-AC-3 5.1 +1");
        eq("2 subs", f.subs, "NONE");
        yes("2 lossless", f.lossless);
    }
    // 3. No default flag anywhere: the first track leads.  A lossy AAC stereo
    //    web file at 720p.
    {
        const char *j =
            "{\"Container\":\"mp4\",\"MediaStreams\":["
            "{\"Codec\":\"h264\",\"Type\":\"Video\",\"Width\":1280,\"Height\":720},"
            "{\"Codec\":\"aac\",\"Channels\":2,\"Type\":\"Audio\"}]}";
        ItemFacts f;
        facts_parse(j, &f);
        eq("3 video", f.video, "720P H.264");
        eq("3 audio", f.audio, "AAC 2.0");
        eq("3 audio_more", f.audio_more, "");
        yes("3 lossy", !f.lossless);
    }
    // 4. DTS:X, Dolby Vision, an unknown codec kept in capitals, SD height.
    {
        const char *j =
            "{\"MediaStreams\":["
            "{\"Codec\":\"hevc\",\"Type\":\"Video\",\"Width\":3840,\"Height\":1600,"
            "\"VideoRangeType\":\"DOVIWithHDR10\"},"
            "{\"Codec\":\"dts\",\"Profile\":\"DTS-HD MA + DTS:X\",\"Channels\":8,"
            "\"Type\":\"Audio\",\"IsDefault\":true},"
            "{\"Codec\":\"wmapro\",\"Channels\":6,\"Type\":\"Audio\"}]}";
        ItemFacts f;
        facts_parse(j, &f);
        eq("4 video", f.video, "4K HEVC DV");
        eq("4 audio", f.audio, "DTS:X 7.1");
        eq("4 audio_more", f.audio_more, "+ WMAPRO 5.1");
        yes("4 lossless", f.lossless);
        const char *sd = "{\"MediaStreams\":[{\"Codec\":\"mpeg2video\",\"Type\":\"Video\","
                         "\"Width\":720,\"Height\":576}]}";
        facts_parse(sd, &f);
        eq("4 sd", f.video, "576P MPEG-2");
    }
    // 5. Nothing useful: no streams, a null document.
    {
        ItemFacts f;
        facts_parse("{\"Items\":[{\"Name\":\"x\"}]}", &f);
        eq("5 audio", f.audio, "");
        eq("5 video", f.video, "");
        facts_parse(NULL, &f);
        eq("5 null", f.container, "");
    }

    // 6. The peek's back face: synopsis (with Jellyfin's escapes), rating,
    //    the first four ACTORS only, in billing order.  A Series: no streams.
    {
        const char *j =
            "{\"Items\":[{\"Name\":\"Dark\",\"Id\":\"s1\",\"Type\":\"Series\","
            "\"Overview\":\"A missing child sets four families on a frantic hunt. "
            "It\\u0027s \\u0022time\\u0022 \\u002B more.\","
            "\"OfficialRating\":\"TV-MA\",\"People\":["
            "{\"Name\":\"Baran bo Odar\",\"Id\":\"d1\",\"Type\":\"Director\"},"
            "{\"Name\":\"Louis Hofmann\",\"Id\":\"p1\",\"Role\":\"Jonas\",\"Type\":\"Actor\"},"
            "{\"Name\":\"Lisa Vicari\",\"Id\":\"p2\",\"Type\":\"Actor\"},"
            "{\"Name\":\"Maja Sch\\u00F6ne\",\"Id\":\"p3\",\"Type\":\"Actor\"},"
            "{\"Name\":\"Oliver Masucci\",\"Id\":\"p4\",\"Type\":\"Actor\"},"
            "{\"Name\":\"Karoline Eichhorn\",\"Id\":\"p5\",\"Type\":\"Actor\"}]}]}";
        ItemFacts f;
        facts_parse(j, &f);
        eq("6 overview", f.overview,
           "A missing child sets four families on a frantic hunt. It's \"time\" + more.");
        eq("6 rating", f.rating, "TV-MA");
        yes("6 four actors", f.n_cast == 4);
        eq("6 cast0", f.cast[0], "Louis Hofmann");
        eq("6 cast0 id", f.cast_id[0], "p1");
        eq("6 cast2 utf8", f.cast[2], "Maja Sch\xC3\xB6ne");
        eq("6 cast3", f.cast[3], "Oliver Masucci");
        eq("6 no audio", f.audio, "");
    }
    // 7. A long synopsis is cut at a word boundary with an ellipsis.
    {
        static char j[2048];
        char ov[1000]; int n = 0;
        while (n < 900) n += snprintf(ov + n, sizeof ov - n, "word%d ", n);
        snprintf(j, sizeof j, "{\"Overview\":\"%s\"}", ov);
        ItemFacts f;
        facts_parse(j, &f);
        const size_t L = strlen(f.overview);
        yes("7 fits", L < sizeof f.overview);
        yes("7 ellipsis", L >= 3 && !strcmp(f.overview + L - 3, "..."));
        yes("7 word boundary", L >= 4 && f.overview[L - 4] != ' ' &&
                               strstr(ov, f.overview) == NULL);
    }

    if (s_fail) { printf("test_facts: %d FAILED\n", s_fail); return 1; }
    printf("test_facts: all passed\n");
    return 0;
}
