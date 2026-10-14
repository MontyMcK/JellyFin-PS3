// Trickplay manifest parser -- source/api/trickplay_info.c compiled straight
// into this test (same convention as test_media_sources.cpp). Exercises the
// real two-level dict shape (MediaSourceId -> width -> TrickplayInfo), a
// title with NO trickplay data, and a title with a source id that has no
// trickplay entry (e.g. a version that predates the feature being enabled).

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../source/api/trickplay_info.h"

static int failures = 0;
static void check(bool ok, const char *what) {
    printf("  %-58s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) failures++;
}

// A real Items/{id}?Fields=Trickplay response embeds Trickplay alongside
// everything else on the item DTO -- this fixture keeps a couple of
// unrelated sibling fields so the scanner is proven not to wander into them.
static const char kItem[] =
    "{\"Name\":\"Some Movie\",\"Id\":\"item-1\","
    "\"Trickplay\":{"
      "\"source-a\":{"
        "\"320\":{\"Width\":320,\"Height\":180,\"TileWidth\":10,"
                 "\"TileHeight\":10,\"ThumbnailCount\":519,"
                 "\"Interval\":10000,\"Bandwidth\":5752}"
      "},"
      "\"source-b\":{}"
    "},"
    "\"RunTimeTicks\":72000000000}";

static const char kNoTrickplay[] =
    "{\"Name\":\"No Trickplay Yet\",\"Trickplay\":{}}";

static const char kMissingKey[] =
    "{\"Name\":\"Old response, field omitted entirely\"}";

int main(void) {
    TrickplayInfo info;

    assert(trickplay_parse_info(kItem, "source-a", &info));
    check(info.width == 320,        "source-a: Width (pixel size) = 320");
    check(info.height == 180,       "source-a: Height (pixel size) = 180");
    check(info.tile_cols == 10,     "source-a: TileWidth (grid cols) = 10");
    check(info.tile_rows == 10,     "source-a: TileHeight (grid rows) = 10");
    check(info.thumbnail_count == 519, "source-a: ThumbnailCount = 519");
    check(info.interval_ms == 10000,   "source-a: Interval = 10000ms");
    check(strcmp(info.width_key, "320") == 0, "source-a: width key text = \"320\"");

    check(!trickplay_parse_info(kItem, "source-b", &info),
         "source-b: empty inner dict -> no info");
    check(!trickplay_parse_info(kItem, "source-does-not-exist", &info),
         "unknown media source id -> no info");
    check(!trickplay_parse_info(kNoTrickplay, "source-a", &info),
         "title with Trickplay:{} -> no info");
    check(!trickplay_parse_info(kMissingKey, "source-a", &info),
         "response missing the Trickplay field entirely -> no info");
    check(!trickplay_parse_info(NULL, "source-a", &info),
         "NULL json handled without crashing");
    check(!trickplay_parse_info(kItem, NULL, &info),
         "NULL media_source_id handled without crashing");

    puts(failures == 0 ? "trickplay manifest parser: synthetic ok"
                       : "trickplay manifest parser: FAILURES");
    return failures ? 1 : 0;
}
