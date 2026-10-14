// Live TV, console side.  See api_livetv.h.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ppu-types.h>
#include <sys/systime.h>
#include <sysutil/sysutil.h>

#include "api_livetv.h"
#include "jellyfin_api.h"
#include "json_lite.h"
#include "plog.h"

// Channels per request: 50 of them with logos and current programmes are well
// under the response cap.  Programme requests list their channels in the URL,
// and http_request keeps 512 bytes of path: 8 ids (33 bytes each) fit with the
// rest of the query, 15 do not.
#define CHANNEL_PAGE   50
#define GUIDE_PAGE      8
#define PAGE_BUF   (256 * 1024)

uint64_t jf_now_ticks(void) {
    u64 sec = 0, nsec = 0;
    sysGetCurrentTime(&sec, &nsec);
    return jf_ticks_from_unix((uint64_t)sec);
}

int jf_utc_offset_secs(void) {
    s32 tz = 0, summer = 0;
    if (sysUtilGetSystemParamInt(SYSUTIL_SYSTEMPARAM_ID_TIMEZONE, &tz) != 0) tz = 0;
    if (sysUtilGetSystemParamInt(SYSUTIL_SYSTEMPARAM_ID_SUMMERTIME, &summer) != 0) summer = 0;
    return (int)tz * 60 + (summer ? 3600 : 0);
}

int jf_fetch_channels(JFChannel *out, int max) {
    return jf_fetch_channels_range(out, max, 0, max, NULL);
}

int jf_fetch_channels_range(JFChannel *out, int max, int start, int count, int *total_out,
                            const char *search, const char *items_extra) {
    if (max > LIVETV_MAX_CHANNELS) max = LIVETV_MAX_CHANNELS;
    if (count > max) count = max;
    if (start < 0) start = 0;
    if (!g_server[0] || !g_userid[0] || !g_token[0] || count <= 0) return -1;
    char *buf = (char *)malloc(PAGE_BUF);
    if (!buf) return -1;
    int n = 0, total = 0;
    bool ok = true;
    do {
        char url[1024];
        const int want = count - n < CHANNEL_PAGE ? count - n : CHANNEL_PAGE;
        const bool filtered = items_extra && items_extra[0];
        if ((search && search[0]) || filtered) {
            char term[200] = "";
            if (search && search[0]) {
                char enc[160];
                url_encode_query(search, enc, sizeof enc);
                snprintf(term, sizeof term, "&searchTerm=%s", enc);
            }
            snprintf(url, sizeof url,
                     "%s/Users/%s/Items?Recursive=true&IncludeItemTypes=LiveTvChannel"
                     "%s%s&EnableImages=true&ImageTypeLimit=1&EnableUserData=true"
                     "&SortBy=SortName&StartIndex=%d&Limit=%d",
                     g_server, g_userid, term, filtered ? items_extra : "", start + n, want);
        } else {
            snprintf(url, sizeof url,
                     "%s/LiveTv/Channels?UserId=%s&EnableImages=true&ImageTypeLimit=1"
                     "&AddCurrentProgram=true&EnableUserData=true&SortBy=SortName"
                     "&StartIndex=%d&Limit=%d",
                     g_server, g_userid, start + n, want);
        }
        buf[0] = '\0';
        const int st = http_request(HTTP_GET, url, NULL, g_token, buf, PAGE_BUF);
        if (st != 200) {
            char b[80];
            snprintf(b, sizeof b, "livetv: channels http %d", st);
            plog(b);
            ok = n > 0;                      // keep what already arrived
            break;
        }
        bool trunc = false;
        const int got = livetv_parse_channels(buf, (int)strlen(buf), out + n, count - n,
                                              &total, &trunc);
        if (trunc) plog("livetv: channel reply truncated -- showing what arrived");
        n += got;
        if (got <= 0) break;
    } while (start + n < total && n < count);
    free(buf);
    if (!ok) return -1;
    if (total_out) *total_out = total;
    livetv_sort_channels(out, n);
    char b[80];
    snprintf(b, sizeof b, "livetv: %d channel(s) from %d of %d%s", n, start, total,
             (search && search[0]) ? " (search)" : (items_extra && items_extra[0]) ? " (filtered)" : "");
    plog(b);
    return n;
}

#define NUMBER_PAGE 200

int jf_fetch_channel_numbers(float *out, int max, int *total_out) {
    if (!g_server[0] || !g_userid[0] || !g_token[0] || max <= 0) return -1;
    char *buf = (char *)malloc(PAGE_BUF);
    JFChannel *page = (JFChannel *)malloc(sizeof(JFChannel) * NUMBER_PAGE);
    if (!buf || !page) { free(buf); free(page); return -1; }
    int n = 0, total = 0;
    bool ok = true;
    do {
        char url[512];
        snprintf(url, sizeof url,
                 "%s/LiveTv/Channels?UserId=%s&EnableImages=false&EnableUserData=false"
                 "&AddCurrentProgram=false&SortBy=SortName&StartIndex=%d&Limit=%d",
                 g_server, g_userid, n, NUMBER_PAGE);
        buf[0] = '\0';
        const int st = http_request(HTTP_GET, url, NULL, g_token, buf, PAGE_BUF);
        if (st != 200) {
            char b[80];
            snprintf(b, sizeof b, "livetv: numbers http %d", st);
            plog(b);
            ok = n > 0;
            break;
        }
        bool trunc = false;
        const int got = livetv_parse_channels(buf, (int)strlen(buf), page, NUMBER_PAGE, &total, &trunc);
        if (got <= 0) break;
        for (int i = 0; i < got && n < max; i++) {
            double v = -1;
            out[n++] = livetv_number_value(page[i].number, &v) ? (float)v : -1.0f;
        }
    } while (n < total && n < max);
    free(buf); free(page);
    if (!ok) return -1;
    if (total_out) *total_out = total;
    char b[80];
    snprintf(b, sizeof b, "livetv: %d channel number(s) of %d", n, total);
    plog(b);
    return n;
}

int jf_fetch_programs(const char *const *channel_ids, int n,
                      uint64_t from_ticks, uint64_t to_ticks,
                      JFProgram *out, int max) {
    if (!g_server[0] || !g_userid[0] || !g_token[0] || n <= 0 || max <= 0) return 0;
    char from[40], to[40];
    if (!jf_iso8601_from_ticks(from_ticks, from, sizeof from) ||
        !jf_iso8601_from_ticks(to_ticks, to, sizeof to)) return 0;
    char *buf = (char *)malloc(PAGE_BUF);
    if (!buf) return 0;
    int stored = 0;
    for (int i = 0; i < n && stored < max; i += GUIDE_PAGE) {
        char ids[GUIDE_PAGE * 41 + 1];
        ids[0] = '\0';
        for (int k = i; k < n && k < i + GUIDE_PAGE; k++) {
            if (!channel_ids[k] || !channel_ids[k][0]) continue;
            snprintf(ids + strlen(ids), sizeof ids - strlen(ids), "%s%s",
                     ids[0] ? "," : "", channel_ids[k]);
        }
        if (!ids[0]) continue;
        char url[512];
        const int ul = snprintf(url, sizeof url,
                 "%s/LiveTv/Programs?UserId=%s&ChannelIds=%s&MinEndDate=%s&MaxStartDate=%s"
                 "&SortBy=StartDate&EnableImages=false&Limit=400",
                 g_server, g_userid, ids, from, to);
        if (ul <= 0 || ul >= (int)sizeof url) continue;
        buf[0] = '\0';
        const int st = http_request(HTTP_GET, url, NULL, g_token, buf, PAGE_BUF);
        if (st != 200) {
            char b[80];
            snprintf(b, sizeof b, "livetv: programmes http %d", st);
            plog(b);
            continue;
        }
        bool trunc = false;
        int total = 0;
        stored += livetv_parse_programs(buf, (int)strlen(buf), out + stored, max - stored,
                                        &total, &trunc);
        if (trunc) plog("livetv: programme reply truncated -- showing what arrived");
    }
    free(buf);
    return stored;
}

bool jf_fetch_program_overview(const char *program_id, char *out, int cap) {
    if (!out || cap <= 0) return false;
    out[0] = '\0';
    if (!program_id || !program_id[0] || !g_server[0] || !g_userid[0]) return false;
    char *buf = (char *)malloc(64 * 1024);
    if (!buf) return false;
    char url[512];
    snprintf(url, sizeof url, "%s/LiveTv/Programs/%s?UserId=%s", g_server, program_id, g_userid);
    buf[0] = '\0';
    const int st = http_request(HTTP_GET, url, NULL, g_token, buf, 64 * 1024);
    bool ok = false;
    if (st == 200) {
        const char *end = buf + strlen(buf);
        const char *top = skip_ws(buf, end);
        const char *v = find_top_value(top, end, "Overview");
        if (v) ok = read_json_string(v, end, out, cap);
    }
    free(buf);
    return ok && out[0];
}

bool jf_livestream_close(const char *live_stream_id) {
    if (!live_stream_id || !live_stream_id[0] || !g_server[0]) return false;
    char enc[288], url[512];
    url_encode_query(live_stream_id, enc, sizeof enc);
    snprintf(url, sizeof url, "%s/LiveStreams/Close?LiveStreamId=%s", g_server, enc);
    static char resp[512];
    const int st = http_request(HTTP_POST, url, "", g_token, resp, sizeof resp);
    char b[96];
    snprintf(b, sizeof b, "livetv: close stream -> %d", st);
    plog(b);
    return st >= 200 && st < 300;
}
