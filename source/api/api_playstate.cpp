// Playback-state reporting — POST /Sessions/Playing{,/Progress,/Stopped}.
// Keeps the server's Continue Watching list and resume positions in sync
// with what the PS3 plays.  Responses are empty (204); use a small local
// buffer instead of the shared responseBuffer since the progress reports
// come from their own thread.

#include <stdio.h>

#include <ppu-types.h>

#include "jellyfin_api.h"
#include "plog.h"

static void post_playstate(const char *endpoint, const char *item_id,
                           const char *session_id, u64 pos_ticks, bool paused) {
    if (!g_server[0] || !g_token[0] || !item_id || !item_id[0]) return;

    char url[512];
    snprintf(url, sizeof(url), "%s%s", g_server, endpoint);

    char body[512];
    snprintf(body, sizeof(body),
        "{\"ItemId\":\"%s\",\"PlaySessionId\":\"%s\","
        "\"PositionTicks\":%llu,\"IsPaused\":%s,"
        "\"PlayMethod\":\"Transcode\",\"CanSeek\":true}",
        item_id, session_id ? session_id : "",
        (unsigned long long)pos_ticks, paused ? "true" : "false");

    char resp[256];
    int status = http_request(HTTP_POST, url, body, g_token, resp, sizeof(resp));

    static int s_log = 0;
    if (s_log < 12 || (status != 200 && status != 204)) {
        if (s_log < 32) {
            s_log++;
            char buf[112];
            snprintf(buf, sizeof(buf), "playstate: %s http=%d pos=%llus",
                     endpoint, status,
                     (unsigned long long)(pos_ticks / 10000000ULL));
            plog(buf);
        }
    }
}

void jellyfin_report_playing(const char *item_id, const char *session_id,
                             unsigned long long pos_ticks) {
    post_playstate("/Sessions/Playing", item_id, session_id, pos_ticks, false);
}

void jellyfin_report_progress(const char *item_id, const char *session_id,
                              unsigned long long pos_ticks, bool paused) {
    post_playstate("/Sessions/Playing/Progress", item_id, session_id,
                   pos_ticks, paused);
}

void jellyfin_report_stopped(const char *item_id, const char *session_id,
                             unsigned long long pos_ticks) {
    post_playstate("/Sessions/Playing/Stopped", item_id, session_id,
                   pos_ticks, false);
}
