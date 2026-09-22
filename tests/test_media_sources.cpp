#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "jellyfin_api.h"

static const char kPlaybackInfo[] =
    "{\"MediaSources\":["
      "{\"Protocol\":\"File\",\"Id\":\"source-a\","
       "\"Name\":\"Same cut\",\"RunTimeTicks\":600000000,"
       "\"MediaStreams\":["
         "{\"DisplayTitle\":\"720p H264 SDR\",\"Type\":\"Video\",\"Index\":0},"
         "{\"DisplayTitle\":\"English - DTS - 5.1\",\"IsDefault\":true,"
          "\"Type\":\"Audio\",\"Index\":1},"
         "{\"DisplayTitle\":\"English - SRT\",\"Type\":\"Subtitle\",\"Index\":2}"
       "]},"
      "{\"Id\":\"source-b\",\"LiveStreamId\":\"live-b\","
       "\"Name\":\"Same cut\",\"RunTimeTicks\":720000000,"
       "\"MediaStreams\":["
         "{\"Name\":\"nested-name-must-not-win\","
          "\"DisplayTitle\":\"1080p H264 SDR\",\"Type\":\"Video\",\"Index\":0},"
         "{\"Language\":\"Japanese\",\"Type\":\"Audio\",\"Index\":4}"
       "]},"
      "{\"Id\":\"source-c\",\"Name\":\"\\u26a1 4K Web\","
       "\"MediaStreams\":[]}"
    "],\"PlaySessionId\":\"session\"}";

int main(int argc, char **argv) {
    JFMediaSources all;
    assert(jellyfin_parse_media_sources(kPlaybackInfo, &all) == 3);
    assert(all.n_sources == 3);
    assert(strcmp(all.source[0].id, "source-a") == 0);
    // Version labels carry resolution and codec (source_tag_video), which is
    // the whole point of the row: two cuts of the same title have to be
    // tellable apart before you pick one.  The bare "(1)"/"(2)" numbering is
    // only the fallback for when even those tags collide.
    assert(strstr(all.source[0].label, "Same cut") != NULL);
    assert(strstr(all.source[0].label, "720p")     != NULL);
    assert(strstr(all.source[0].label, "H.264")    != NULL);
    assert(strstr(all.source[1].label, "Same cut") != NULL);
    assert(strstr(all.source[1].label, "1080p")    != NULL);
    assert(strcmp(all.source[0].label, all.source[1].label) != 0);
    assert(all.source[0].runtime_secs == 60);
    assert(all.source[1].runtime_secs == 72);
    assert(all.source[0].tracks.n_audio == 1);
    assert(all.source[0].tracks.default_audio == 0);
    assert(all.source[0].tracks.audio[0].index == 1);
    assert(all.source[0].tracks.n_subs == 1);
    assert(all.source[0].tracks.subs[0].index == 2);
    assert(strcmp(all.source[1].live_stream_id, "live-b") == 0);
    assert(strcmp(all.source[1].tracks.audio[0].label, "Japanese") == 0);
    assert(strstr(all.source[2].label, "4K Web") != NULL);

    JFMediaSource selected;
    assert(jellyfin_parse_selected_media_source(kPlaybackInfo, "source-b",
                                                 &selected));
    assert(strcmp(selected.id, "source-b") == 0);
    assert(strcmp(selected.live_stream_id, "live-b") == 0);
    assert(selected.tracks.audio[0].index == 4);

    puts("media source parser: synthetic ok");

    // Optional real item-DTO/PlaybackInfo fixture (used by the WSL check).
    if (argc == 2) {
        FILE *f = fopen(argv[1], "rb");
        assert(f != NULL);
        assert(fseek(f, 0, SEEK_END) == 0);
        long size = ftell(f);
        assert(size > 0);
        rewind(f);
        char *json = (char *)malloc((size_t)size + 1);
        assert(json != NULL);
        assert(fread(json, 1, (size_t)size, f) == (size_t)size);
        fclose(f);
        json[size] = '\0';
        JFMediaSources real;
        int count = jellyfin_parse_media_sources(json, &real);
        assert(count >= 2);
        assert(real.source[0].id[0] && real.source[1].id[0]);
        assert(real.source[0].tracks.n_audio > 0);
        assert(real.source[1].tracks.n_audio > 0);
        printf("media source parser: real MediaSources JSON ok (%d sources)\n",
               count);
        free(json);
    }
    return 0;
}
