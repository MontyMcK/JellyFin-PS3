// Per-item detail fetch and PlaybackInfo session ID.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "jellyfin_api.h"
#include "plog.h"

// Defined in jellyfin_api.cpp
int json_get_in_range(const char *start, int len,
                      const char *key, char *out, int out_size);

static double json_get_double(const char *json, const char *key, double def) {
    char search[64];
    snprintf(search, sizeof(search), "\"%s\":", key);
    const char *p = strstr(json, search);
    if (!p) return def;
    p += strlen(search);
    while (*p == ' ') p++;
    if (*p != '-' && (*p < '0' || *p > '9')) return def;
    return atof(p);
}

static void json_array_first_string(const char *json, const char *key,
                                     char *out, int out_size) {
    out[0] = '\0';
    char search[80];
    snprintf(search, sizeof(search), "\"%s\":", key);
    const char *p = strstr(json, search);
    if (!p) return;
    p += strlen(search);
    while (*p == ' ') p++;
    if (*p != '[') return;
    p++;
    while (*p == ' ') p++;
    if (*p != '"') return;
    p++;
    int i = 0;
    while (*p && *p != '"' && i < out_size - 1) out[i++] = *p++;
    out[i] = '\0';
}

static void json_array_strings_join(const char *json, const char *key,
                                     char *out, int out_size) {
    out[0] = '\0';
    char search[80];
    snprintf(search, sizeof(search), "\"%s\":", key);
    const char *p = strstr(json, search);
    if (!p) return;
    p += strlen(search);
    while (*p == ' ') p++;
    if (*p != '[') return;
    p++;
    int written = 0;
    while (*p && *p != ']') {
        while (*p && *p != '"' && *p != ']') p++;
        if (!*p || *p == ']') break;
        p++;  // skip opening "
        if (written > 0 && written < out_size - 3) {
            out[written++] = ','; out[written++] = ' '; out[written] = '\0';
        }
        while (*p && *p != '"' && written < out_size - 1)
            out[written++] = *p++;
        out[written] = '\0';
        if (*p == '"') p++;
    }
}

static void json_array_obj_names_join(const char *json, const char *array_key,
                                       char *out, int out_size) {
    out[0] = '\0';
    char search[80];
    snprintf(search, sizeof(search), "\"%s\":", array_key);
    const char *p = strstr(json, search);
    if (!p) return;
    p += strlen(search);
    while (*p == ' ') p++;
    if (*p != '[') return;
    p++;
    int written = 0;
    while (*p && *p != ']') {
        while (*p && *p != '{' && *p != ']') p++;
        if (!*p || *p == ']') break;
        const char *obj_start = p;
        int depth = 0; bool in_str = false, esc = false;
        while (*p) {
            char c = *p;
            if (esc) { esc = false; }
            else if (in_str) { if (c=='\\') esc=true; else if (c=='"') in_str=false; }
            else { if (c=='"') in_str=true; else if (c=='{') depth++; else if (c=='}') { if (!--depth) { p++; break; } } }
            p++;
        }
        int olen = (int)(p - obj_start);
        char name[128] = "";
        json_get_in_range(obj_start, olen, "Name", name, sizeof(name));
        if (name[0]) {
            if (written > 0 && written < out_size - 3) {
                out[written++] = ','; out[written++] = ' '; out[written] = '\0';
            }
            int nl = (int)strlen(name);
            if (nl > out_size - written - 1) nl = out_size - written - 1;
            memcpy(out + written, name, nl);
            written += nl;
            out[written] = '\0';
        }
    }
}

static void parse_media_streams(const char *json,
                                  char *video_out, int vlen,
                                  char *audio_out, int alen) {
    video_out[0] = '\0'; audio_out[0] = '\0';
    const char *arr = strstr(json, "\"MediaStreams\":");
    if (!arr) return;
    arr += sizeof("\"MediaStreams\":") - 1;
    while (*arr == ' ') arr++;
    if (*arr != '[') return;
    arr++;

    char first_audio[128] = "";
    bool found_video = false, found_def_audio = false;
    const char *p = arr;

    while (*p && *p != ']') {
        while (*p && *p != '{' && *p != ']') p++;
        if (!*p || *p == ']') break;
        const char *obj_start = p;
        int depth = 0; bool in_str = false, esc = false;
        while (*p) {
            char c = *p;
            if (esc) { esc = false; }
            else if (in_str) { if (c=='\\') esc=true; else if (c=='"') in_str=false; }
            else { if (c=='"') in_str=true; else if (c=='{') depth++; else if (c=='}') { if (!--depth) { p++; break; } } }
            p++;
        }
        int olen = (int)(p - obj_start);

        char stype[16] = "";
        json_get_in_range(obj_start, olen, "Type", stype, sizeof(stype));

        if (!found_video && strcmp(stype, "Video") == 0) {
            found_video = true;
            if (!json_get_in_range(obj_start, olen, "DisplayTitle", video_out, vlen) || !video_out[0]) {
                char codec_s[16] = "", vrange[16] = ""; int width = 0;
                json_get_in_range(obj_start, olen, "Codec",      codec_s, sizeof(codec_s));
                json_get_in_range(obj_start, olen, "VideoRange", vrange,  sizeof(vrange));
                const char *wp = obj_start, *wend = obj_start + olen;
                while (wp + 8 < wend) {
                    if (memcmp(wp, "\"Width\":", 8) == 0) { width = atoi(wp + 8); break; }
                    wp++;
                }
                const char *res = width >= 1920 ? "1080p" :
                                  width >= 1280 ? "720p"  :
                                  width >= 720  ? "480p"  : "";
                for (int i = 0; codec_s[i]; i++) codec_s[i] = (char)toupper((unsigned char)codec_s[i]);
                snprintf(video_out, vlen, "%s %s %s", res, codec_s, vrange);
                int vl = (int)strlen(video_out);
                while (vl > 0 && video_out[vl-1] == ' ') video_out[--vl] = '\0';
            }
        }

        if (strcmp(stype, "Audio") == 0) {
            char disp[128] = "";
            json_get_in_range(obj_start, olen, "DisplayTitle", disp, sizeof(disp));
            if (!first_audio[0] && disp[0]) snprintf(first_audio, sizeof(first_audio), "%s", disp);
            if (!found_def_audio) {
                const char *dp = obj_start, *dend = obj_start + olen;
                while (dp + 12 < dend) {
                    if (memcmp(dp, "\"IsDefault\":", 12) == 0) {
                        dp += 12;
                        while (*dp == ' ') dp++;
                        if (memcmp(dp, "true", 4) == 0 && disp[0]) {
                            found_def_audio = true;
                            snprintf(audio_out, alen, "%s", disp);
                        }
                        break;
                    }
                    dp++;
                }
            }
        }
    }

    if (!found_def_audio && first_audio[0])
        snprintf(audio_out, alen, "%s", first_audio);
}

// Integer field within a JSON object slice; def on missing/non-numeric.
static int json_get_int_in_range(const char *start, int len,
                                 const char *key, int def) {
    char search[48];
    int slen = snprintf(search, sizeof(search), "\"%s\":", key);
    const char *p = start, *end = start + len;
    while (p + slen < end) {
        if (memcmp(p, search, slen) == 0) {
            p += slen;
            while (p < end && *p == ' ') p++;
            if (p < end && (*p == '-' || (*p >= '0' && *p <= '9')))
                return atoi(p);
            return def;
        }
        p++;
    }
    return def;
}

// Bool field within a JSON object slice; false on missing.
static bool json_get_bool_in_range(const char *start, int len,
                                   const char *key) {
    char search[48];
    int slen = snprintf(search, sizeof(search), "\"%s\":", key);
    const char *p = start, *end = start + len;
    while (p + slen < end) {
        if (memcmp(p, search, slen) == 0) {
            p += slen;
            while (p < end && *p == ' ') p++;
            return (p + 4 <= end && memcmp(p, "true", 4) == 0);
        }
        p++;
    }
    return false;
}

bool jellyfin_fetch_tracks(const char *item_id, JFTracks *out) {
    memset(out, 0, sizeof(*out));

    char url[512];
    snprintf(url, sizeof(url),
        "%s/Users/%s/Items/%s?Fields=MediaStreams",
        g_server, g_userid, item_id);

    int status = http_request(0, url, NULL, g_token, responseBuffer, RESPONSE_SIZE);
    if (status != 200) {
        char buf[64];
        snprintf(buf, sizeof(buf), "fetch_tracks: http %d", status);
        plog(buf);
        return false;
    }

    const char *arr = strstr(responseBuffer, "\"MediaStreams\":");
    if (!arr) { plog("fetch_tracks: no MediaStreams"); return false; }
    arr += sizeof("\"MediaStreams\":") - 1;
    while (*arr == ' ') arr++;
    if (*arr != '[') return false;
    arr++;

    const char *p = arr;
    while (*p && *p != ']') {
        while (*p && *p != '{' && *p != ']') p++;
        if (!*p || *p == ']') break;
        const char *obj_start = p;
        int depth = 0; bool in_str = false, esc = false;
        while (*p) {
            char c = *p;
            if (esc) { esc = false; }
            else if (in_str) { if (c=='\\') esc=true; else if (c=='"') in_str=false; }
            else { if (c=='"') in_str=true; else if (c=='{') depth++; else if (c=='}') { if (!--depth) { p++; break; } } }
            p++;
        }
        int olen = (int)(p - obj_start);

        char stype[16] = "";
        json_get_in_range(obj_start, olen, "Type", stype, sizeof(stype));
        int idx = json_get_int_in_range(obj_start, olen, "Index", -1);
        if (idx < 0) continue;

        char disp[64] = "";
        json_get_in_range(obj_start, olen, "DisplayTitle", disp, sizeof(disp));
        if (!disp[0])
            json_get_in_range(obj_start, olen, "Language", disp, sizeof(disp));

        if (strcmp(stype, "Audio") == 0 && out->n_audio < JF_MAX_STREAMS) {
            JFStream *s = &out->audio[out->n_audio];
            s->index = idx;
            snprintf(s->label, sizeof(s->label), "%s",
                     disp[0] ? disp : "Audio");
            if (json_get_bool_in_range(obj_start, olen, "IsDefault"))
                out->default_audio = out->n_audio;
            out->n_audio++;
        } else if (strcmp(stype, "Subtitle") == 0 && out->n_subs < JF_MAX_STREAMS) {
            JFStream *s = &out->subs[out->n_subs];
            s->index = idx;
            snprintf(s->label, sizeof(s->label), "%s",
                     disp[0] ? disp : "Subtitle");
            out->n_subs++;
        }
    }

    char buf[64];
    snprintf(buf, sizeof(buf), "fetch_tracks: %d audio, %d subs",
             out->n_audio, out->n_subs);
    plog(buf);
    return true;
}

bool jellyfin_fetch_item_detail(const char *item_id, XMBItemDetail *out) {
    memset(out, 0, sizeof(*out));

    char url[512];
    snprintf(url, sizeof(url),
        "%s/Users/%s/Items/%s"
        "?Fields=Overview,Taglines,People,OfficialRating,CommunityRating"
        ",CriticRating,Genres,Studios,MediaStreams",
        g_server, g_userid, item_id);

    int status = http_request(0, url, NULL, g_token, responseBuffer, RESPONSE_SIZE);
    if (status != 200) {
        char buf[64];
        snprintf(buf, sizeof(buf), "item_detail: http %d", status);
        plog(buf);
        return false;
    }

    const char *resp = responseBuffer;

    json_get_string(resp, "Overview",       out->overview,        sizeof(out->overview));
    json_get_string(resp, "OfficialRating", out->official_rating, sizeof(out->official_rating));

    double cr = json_get_double(resp, "CommunityRating", -1.0);
    if (cr >= 0.0) snprintf(out->community_rating, sizeof(out->community_rating), "%.1f", cr);

    double xr = json_get_double(resp, "CriticRating", -1.0);
    if (xr >= 0.0) snprintf(out->critic_rating, sizeof(out->critic_rating), "%.0f%%", xr);

    json_array_first_string(resp,   "Taglines", out->tagline, sizeof(out->tagline));
    json_array_strings_join(resp,   "Genres",   out->genres,  sizeof(out->genres));
    json_array_obj_names_join(resp, "Studios",  out->studios, sizeof(out->studios));

    parse_media_streams(resp, out->video_info, sizeof(out->video_info),
                              out->audio_info, sizeof(out->audio_info));
    plog("item_detail: ok");
    return true;
}

void jellyfin_stop_transcode(const char *session_id) {
    if (!g_server[0] || !session_id || !session_id[0]) return;
    char url[512];
    snprintf(url, sizeof(url),
        "%s/Videos/ActiveEncodings?deviceId=ps3&playSessionId=%s",
        g_server, session_id);
    int status = http_request(HTTP_DELETE, url, NULL, g_token,
                              responseBuffer, RESPONSE_SIZE);
    char buf[80];
    snprintf(buf, sizeof(buf), "stop_transcode: http %d session=%s", status, session_id);
    plog(buf);
}

bool jellyfin_get_play_session_id(const char *item_id,
                                   char *out_session_id, int out_len,
                                   unsigned *out_total_secs) {
    if (out_total_secs) *out_total_secs = 0;
    if (!g_server[0] || !g_userid[0] || !g_token[0]) {
        plog("playbackinfo: missing server/user/token");
        return false;
    }

    char url[768];
    snprintf(url, sizeof(url),
        "%s/Items/%s/PlaybackInfo"
        "?UserId=%s"
        "&MaxStreamingBitrate=8000000"
        "&StartTimeTicks=0"
        "&AutoOpenLiveStream=true",
        g_server, item_id, g_userid);

    // Stored in read-only data — avoids putting ~700 bytes on the stack.
    static const char body[] =
        "{\"DeviceProfile\":{"
          "\"Name\":\"PS3\","
          "\"MaxStreamingBitrate\":8000000,"
          "\"MaxStaticBitrate\":8000000,"
          "\"MusicStreamingTranscodingBitrate\":192000,"
          "\"DirectPlayProfiles\":[],"
          "\"TranscodingProfiles\":[{"
            "\"Type\":\"Video\","
            "\"Container\":\"ts\","
            "\"VideoCodec\":\"h264\","
            "\"AudioCodec\":\"mp3\","
            "\"Protocol\":\"http\","
            "\"Context\":\"Streaming\","
            "\"MaxAudioChannels\":\"2\""
          "}],"
          "\"CodecProfiles\":[{"
            "\"Type\":\"Video\","
            "\"Codec\":\"h264\","
            "\"Conditions\":["
              "{\"Condition\":\"EqualsAny\",\"Property\":\"VideoProfile\","
               "\"Value\":\"baseline\",\"IsRequired\":true},"
              "{\"Condition\":\"LessThanEqual\",\"Property\":\"VideoLevel\","
               "\"Value\":\"31\",\"IsRequired\":true},"
              "{\"Condition\":\"LessThanEqual\",\"Property\":\"Width\","
               "\"Value\":\"1280\",\"IsRequired\":true},"
              "{\"Condition\":\"LessThanEqual\",\"Property\":\"Height\","
               "\"Value\":\"720\",\"IsRequired\":true},"
              "{\"Condition\":\"LessThanEqual\",\"Property\":\"VideoBitrate\","
               "\"Value\":\"4000000\",\"IsRequired\":true}"
            "]"
          "},{"
            "\"Type\":\"VideoAudio\","
            "\"Codec\":\"mp3\","
            "\"Conditions\":["
              "{\"Condition\":\"LessThanEqual\",\"Property\":\"AudioChannels\","
               "\"Value\":\"2\",\"IsRequired\":false},"
              "{\"Condition\":\"Equals\",\"Property\":\"AudioSampleRate\","
               "\"Value\":\"48000\",\"IsRequired\":false}"
            "]"
          "}],"
          "\"ContainerProfiles\":[],"
          // No on-device subtitle renderer: ask the server to burn subs into
          // the video (Method=Encode) for every common format.
          "\"SubtitleProfiles\":["
            "{\"Format\":\"subrip\",\"Method\":\"Encode\"},"
            "{\"Format\":\"srt\",\"Method\":\"Encode\"},"
            "{\"Format\":\"ass\",\"Method\":\"Encode\"},"
            "{\"Format\":\"ssa\",\"Method\":\"Encode\"},"
            "{\"Format\":\"pgssub\",\"Method\":\"Encode\"},"
            "{\"Format\":\"dvdsub\",\"Method\":\"Encode\"},"
            "{\"Format\":\"vtt\",\"Method\":\"Encode\"}"
          "]"
        "}}";

    int status = http_request(1, url, body, g_token, responseBuffer, RESPONSE_SIZE);
    if (status != 200) {
        char buf[80];
        snprintf(buf, sizeof(buf), "playbackinfo: http status %d", status);
        plog(buf);
        char errbuf[280];
        snprintf(errbuf, sizeof(errbuf), "playbackinfo_err: %.256s", responseBuffer);
        plog(errbuf);
        return false;
    }

    // Media duration (RunTimeTicks is in 100-ns units → 10,000,000 ticks/sec).
    if (out_total_secs) {
        double ticks = json_get_double(responseBuffer, "RunTimeTicks", 0.0);
        if (ticks > 0.0) *out_total_secs = (unsigned)(ticks / 10000000.0);
        char buf[64];
        snprintf(buf, sizeof(buf), "playbackinfo: runtime=%us", *out_total_secs);
        plog(buf);
    }

    if (!json_get_string(responseBuffer, "PlaySessionId", out_session_id, out_len)) {
        plog("playbackinfo: PlaySessionId not found in response");
        return false;
    }

    char buf[96];
    snprintf(buf, sizeof(buf), "playbackinfo: session=%s", out_session_id);
    plog(buf);
    return true;
}
