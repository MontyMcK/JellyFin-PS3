#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include <io/pad.h>
#include <sysutil/sysutil.h>

#include "jellyfin_api.h"
#include "plog.h"
#include "ui.h"
#include "player.h"

// -------------------------------------------------------
// Global session state
// -------------------------------------------------------

char g_server[256]        = "";
char g_username[64]       = "";
char g_token[256]         = "";
char g_userid[64]         = "";
char responseBuffer[RESPONSE_SIZE];

// -------------------------------------------------------
// JSON helpers
// -------------------------------------------------------

int json_get_string(const char *json, const char *key, char *out, int out_size) {
    char search[64];
    snprintf(search, sizeof(search), "\"%s\":\"", key);
    const char *p = strstr(json, search);
    if (!p) return 0;
    p += strlen(search);
    int i = 0;
    while (*p && *p != '"' && i < out_size-1) out[i++] = *p++;
    out[i] = '\0';
    return 1;
}

// Bounded version: searches within [start, start+len)
static int json_get_in_range(const char *start, int len,
                              const char *key, char *out, int out_size) {
    char search[64];
    snprintf(search, sizeof(search), "\"%s\":\"", key);
    int slen = strlen(search);
    const char *p = start, *end = start + len;
    while (p + slen <= end) {
        if (memcmp(p, search, slen) == 0) {
            p += slen;
            int i = 0;
            while (p < end && *p != '"' && i < out_size-1) out[i++] = *p++;
            out[i] = '\0';
            return 1;
        }
        p++;
    }
    out[0] = '\0';
    return 0;
}

static int json_get_int(const char *json, const char *key, int def) {
    char search[64];
    snprintf(search, sizeof(search), "\"%s\":", key);
    const char *p = strstr(json, search);
    if (!p) return def;
    p += strlen(search);
    while (*p == ' ') p++;
    if (*p < '0' || *p > '9') return def;
    return atoi(p);
}

void url_encode_query(const char *in, char *out, int out_size) {
    int j = 0;
    for (int i = 0; in[i] && j < out_size - 4; i++) {
        unsigned char c = (unsigned char)in[i];
        if (c == ' ') { out[j++]='%'; out[j++]='2'; out[j++]='0'; }
        else if ((c>='A'&&c<='Z')||(c>='a'&&c<='z')||(c>='0'&&c<='9')||
                  c=='-'||c=='_'||c=='.'||c=='~')
            out[j++] = (char)c;
        else { snprintf(out+j, out_size-j, "%%%02X", c); j += 3; }
    }
    out[j] = '\0';
}

// -------------------------------------------------------
// Item helpers
// -------------------------------------------------------

bool is_container(const char *t) {
    return strcmp(t,"CollectionFolder")==0 || strcmp(t,"UserView")==0 ||
           strcmp(t,"Folder")==0           || strcmp(t,"Series")==0   ||
           strcmp(t,"Season")==0           || strcmp(t,"MusicAlbum")==0 ||
           strcmp(t,"MusicArtist")==0      || strcmp(t,"BoxSet")==0;
}

// String-aware depth tracker: skips over { } inside quoted strings
// so inner arrays like "Studios":[] don't confuse the item boundary.
int parse_jf_items(const char *json, JFItem *arr, int max) {
    const char *p = strstr(json, "\"Items\":[");
    if (!p) return 0;
    p += 9;

    int count = 0;
    while (count < max && *p) {
        // Advance to next { or end of array ]
        while (*p && *p != '{' && *p != ']') p++;
        if (!*p || *p == ']') break;

        const char *obj_start = p;
        int  depth     = 0;
        bool in_string = false;
        bool escaped   = false;

        while (*p) {
            char c = *p;
            if (escaped) {
                escaped = false;
            } else if (in_string) {
                if (c == '\\') escaped = true;
                else if (c == '"') in_string = false;
            } else {
                if      (c == '"') in_string = true;
                else if (c == '{') depth++;
                else if (c == '}') { if (--depth == 0) { p++; break; } }
            }
            p++;
        }

        int olen = (int)(p - obj_start);
        JFItem tmp; memset(&tmp, 0, sizeof(tmp));
        json_get_in_range(obj_start, olen, "Id",   tmp.id,   sizeof(tmp.id));
        json_get_in_range(obj_start, olen, "Name", tmp.name, sizeof(tmp.name));
        json_get_in_range(obj_start, olen, "Type", tmp.type, sizeof(tmp.type));
        if (tmp.id[0]) arr[count++] = tmp;
    }
    return count;
}

// -------------------------------------------------------
// Config
// -------------------------------------------------------

void save_config(void) {
    FILE *f = fopen("/dev_hdd0/tmp/jellyfin_config.txt", "w");
    if (f) {
        fprintf(f, "%s\n%s\n%s\n%s\n", g_server, g_username, g_token, g_userid);
        fclose(f);
    }
}

int load_config(void) {
    FILE *f = fopen("/dev_hdd0/tmp/jellyfin_config.txt", "r");
    if (!f) return 0;
    fscanf(f, "%255s\n%63s\n%255s\n%63s\n", g_server, g_username, g_token, g_userid);
    fclose(f);
    int sl = strlen(g_server);
    if (sl > 0 && g_server[sl-1] == '/') g_server[sl-1] = '\0';
    return (g_server[0] && g_token[0]);
}

// -------------------------------------------------------
// Login
// -------------------------------------------------------

static void trim(char *s) {
    int len = strlen(s);
    while (len > 0 && (s[len-1]==' '||s[len-1]=='\n'||s[len-1]=='\r'||s[len-1]=='\t'))
        s[--len] = '\0';
    char *p = s;
    while (*p==' '||*p=='\n'||*p=='\r'||*p=='\t') p++;
    if (p != s) memmove(s, p, strlen(p)+1);
}

int do_login(void) {
    char password[64] = "";
    if (get_input(g_username, sizeof(g_username), "Username", false) != 1) return 0;
    if (get_input(password,   sizeof(password),   "Password", true)  != 1) return 0;
    trim(g_username);
    trim(password);

    drawHeader();
    drawText(40, 100, "Logging in...");
    drawTextf(40, 130, "Server: %s", g_server);
    drawTextf(40, 155, "User:   %s", g_username);
    flip();

    char url[512], body[256];
    snprintf(url,  sizeof(url),  "%s/Users/AuthenticateByName", g_server);
    snprintf(body, sizeof(body), "{\"Username\":\"%s\",\"Pw\":\"%s\"}",
             g_username, password);

    {
        FILE *dbg = fopen("/dev_hdd0/tmp/jf_debug.txt", "w");
        if (dbg) {
            fprintf(dbg, "URL: %s\nBody(%d): %s\n", url, (int)strlen(body), body);
            fclose(dbg);
        }
    }

    int status = http_request(1, url, body, NULL, responseBuffer, RESPONSE_SIZE);

    if (status == 200) {
        json_get_string(responseBuffer, "AccessToken", g_token,  sizeof(g_token));
        json_get_string(responseBuffer, "Id",          g_userid, sizeof(g_userid));
        if (g_token[0]) { save_config(); return 1; }
    }

    {
        FILE *dbg = fopen("/dev_hdd0/tmp/jf_debug.txt", "a");
        if (dbg) {
            fprintf(dbg, "Status: %d\nResponse(%d): %s\n",
                    status, (int)strlen(responseBuffer), responseBuffer);
            fclose(dbg);
        }
    }

    drawHeader();
    drawText(40, 100, "Login Failed!");
    drawTextf(40, 130, "Status: %d", status);
    if      (status == 401) drawText(40, 155, "Wrong username or password");
    else if (status == 404) drawText(40, 155, "Wrong server URL or path");
    else if (status == 400) drawText(40, 155, "Bad request (check credentials)");
    else if (status ==  -1) drawText(40, 155, "Could not reach server");
    if (responseBuffer[0]) {
        char snippet[64]; snprintf(snippet, sizeof(snippet), "%.60s", responseBuffer);
        drawTextf(40, 180, "%s", snippet);
    }
    drawText(40, 230, "Press X to try again");
    flip();

    init_btns();
    padInfo padinfo; padData paddata;
    while (running) {
        sysUtilCheckCallback();
        ioPadGetInfo(&padinfo);
        for (int i = 0; i < MAX_PADS; i++) {
            if (!padinfo.status[i]) continue;
            ioPadGetData(i, &paddata);
            update_buttons(&paddata);
            if (BTN_PRESSED(cross)) return 0;
        }
    }
    return 0;
}

// -------------------------------------------------------
// Library browser — private helpers
// -------------------------------------------------------

// Returns: >=0 selected, -1 back, -2 prev page, -3 next page
static int show_list(JFItem *arr, int count, const char *title,
                     bool has_prev, bool has_next) {
    int sel = 0, top = 0;
    init_btns();
    padInfo pi; padData pd;

    while (running) {
        sysUtilCheckCallback();
        ioPadGetInfo(&pi);
        for (int i = 0; i < MAX_PADS; i++) {
            if (!pi.status[i]) continue;
            ioPadGetData(i, &pd);
            update_buttons(&pd);
            if (BTN_PRESSED(up))   { if (sel > 0)         { sel--; if (sel < top)          top = sel; } }
            if (BTN_PRESSED(down)) { if (sel < count - 1) { sel++; if (sel >= top+JF_PAGE) top = sel-JF_PAGE+1; } }
            if (BTN_PRESSED(cross))              return sel;
            if (BTN_PRESSED(circle))             return -1;
            if (BTN_PRESSED(l1) && has_prev)     return -2;
            if (BTN_PRESSED(r1) && has_next)     return -3;
        }

        drawHeader();
        drawTextf(40, 65, "%.72s", title);
        if (count == 0) drawText(40, 87, "(no items)");

        int y = 87;
        for (int i = top; i < top+JF_PAGE && i < count; i++) {
            char line[80];
            const char *pfx = is_container(arr[i].type) ? ">" : "";
            if (i == sel) snprintf(line, sizeof(line), "[%s%.62s]", pfx, arr[i].name);
            else          snprintf(line, sizeof(line), " %s%.63s",  pfx, arr[i].name);
            drawText(40, y, line);
            y += LINE_HEIGHT;
        }

        char footer[128];
        snprintf(footer, sizeof(footer), "X:open  O:back  %d/%d%s%s",
                 count ? sel+1 : 0, count,
                 has_prev ? "  L1:prev" : "",
                 has_next ? "  R1:next" : "");
        drawText(40, 660, footer);
        flip();
    }
    return -1;
}

static void browse_level(const char *parent_id, const char *title, int depth) {
    if (depth > 6 || !running) return;

    int  start_index = 0;
    int  total_count = 0;
    bool need_fetch  = true;
    JFItem items[JF_MAX];
    int  count = 0;

    while (running) {
        if (need_fetch) {
            drawHeader();
            drawTextf(40, 100, "Loading %.60s...", title);
            flip();

            char url[512];
            if (!parent_id || !parent_id[0])
                snprintf(url, sizeof(url), "%s/Users/%s/Views", g_server, g_userid);
            else
                snprintf(url, sizeof(url),
                    "%s/Users/%s/Items?ParentId=%s&StartIndex=%d&Limit=%d"
                    "&SortBy=SortName&SortOrder=Ascending",
                    g_server, g_userid, parent_id, start_index, JF_MAX);

            int status = http_request(0, url, NULL, g_token, responseBuffer, RESPONSE_SIZE);
            count = 0;
            if (status == 200) {
                count       = parse_jf_items(responseBuffer, items, JF_MAX);
                total_count = json_get_int(responseBuffer, "TotalRecordCount", start_index + count);
            } else {
                drawHeader();
                drawTextf(40, 100, "Error loading items: %d", status);
                drawText(40, 130, "O: back");
                flip();
                init_btns();
                padInfo pi; padData pd;
                while (running) {
                    sysUtilCheckCallback(); ioPadGetInfo(&pi);
                    for (int i = 0; i < MAX_PADS; i++) {
                        if (!pi.status[i]) continue;
                        ioPadGetData(i, &pd); update_buttons(&pd);
                        if (BTN_PRESSED(circle)) return;
                    }
                }
                return;
            }
            need_fetch = false;
        }

        char page_title[128];
        if (total_count > JF_MAX)
            snprintf(page_title, sizeof(page_title), "%.46s [%d-%d/%d]",
                     title, start_index+1, start_index+count, total_count);
        else
            snprintf(page_title, sizeof(page_title), "%.72s", title);

        bool has_prev = (start_index > 0);
        bool has_next = (start_index + count < total_count);

        int sel = show_list(items, count, page_title, has_prev, has_next);

        if (sel == -1) return;
        if (sel == -2) { start_index -= JF_MAX; if (start_index < 0) start_index = 0; need_fetch = true; continue; }
        if (sel == -3) { start_index += JF_MAX;                                        need_fetch = true; continue; }

        if (is_container(items[sel].type))
            browse_level(items[sel].id, items[sel].name, depth + 1);
        else
            show_player(&items[sel]);
    }
}

// -------------------------------------------------------
// Public screens
// -------------------------------------------------------

void show_library_browser(void) {
    if (!g_userid[0]) {
        drawHeader();
        drawText(40, 100, "No user ID - please log out and log in again.");
        drawText(40, 130, "O: back");
        flip();
        init_btns();
        padInfo pi; padData pd;
        while (running) {
            sysUtilCheckCallback(); ioPadGetInfo(&pi);
            for (int i = 0; i < MAX_PADS; i++) {
                if (!pi.status[i]) continue;
                ioPadGetData(i, &pd); update_buttons(&pd);
                if (BTN_PRESSED(circle)) return;
            }
        }
        return;
    }
    browse_level(NULL, "Libraries", 0);
}

void show_search(void) {
    char query[64] = "";
    if (get_input(query, sizeof(query), "Search movies/shows", false) != 1) return;
    if (!query[0]) return;

    drawHeader();
    drawTextf(40, 100, "Searching: %.60s", query);
    flip();

    char encoded[192];
    url_encode_query(query, encoded, sizeof(encoded));

    char url[512];
    snprintf(url, sizeof(url),
        "%s/Users/%s/Items?searchTerm=%s&Recursive=true"
        "&IncludeItemTypes=Movie,Series,Episode"
        "&Limit=%d&SortBy=SortName&SortOrder=Ascending",
        g_server, g_userid, encoded, JF_MAX);

    int status = http_request(0, url, NULL, g_token, responseBuffer, RESPONSE_SIZE);
    JFItem items[JF_MAX]; int count = 0;
    if (status == 200) {
        count = parse_jf_items(responseBuffer, items, JF_MAX);
    } else {
        drawHeader();
        drawTextf(40, 100, "Search failed: %d", status);
        drawText(40, 130, "O: back");
        flip();
        init_btns();
        padInfo pi; padData pd;
        while (running) {
            sysUtilCheckCallback(); ioPadGetInfo(&pi);
            for (int i = 0; i < MAX_PADS; i++) {
                if (!pi.status[i]) continue;
                ioPadGetData(i, &pd); update_buttons(&pd);
                if (BTN_PRESSED(circle)) return;
            }
        }
        return;
    }

    while (running) {
        char result_title[96];
        snprintf(result_title, sizeof(result_title), "%.46s (%d results)", query, count);
        int sel = show_list(items, count, result_title, false, false);
        if (sel < 0) return;
        show_player(&items[sel]);
    }
}

void show_main_menu(void) {
    ui_run_xmb();
}

// -------------------------------------------------------
// PlaybackInfo — obtain a Jellyfin PlaySessionId
// -------------------------------------------------------

bool jellyfin_get_play_session_id(const char *item_id,
                                   char *out_session_id, int out_len) {
    if (!g_server[0] || !g_userid[0] || !g_token[0]) {
        plog("playbackinfo: missing server/user/token");
        return false;
    }

    char url[512];
    snprintf(url, sizeof(url), "%s/Users/%s/Items/%s/PlaybackInfo",
             g_server, g_userid, item_id);

    // Stored in read-only data — avoids putting ~700 bytes on the stack.
    static const char body[] =
        "{\"DeviceProfile\":{"
          "\"MaxStreamingBitrate\":8000000,"
          "\"TranscodingProfiles\":[{"
            "\"Container\":\"ts\","
            "\"Type\":\"Video\","
            "\"VideoCodec\":\"h264\","
            "\"AudioCodec\":\"mp3\","
            "\"Protocol\":\"http\","
            "\"Context\":\"Streaming\","
            "\"MaxAudioChannels\":\"2\""
          "}],"
          "\"DirectPlayProfiles\":[],"
          "\"CodecProfiles\":[{"
            "\"Type\":\"Video\","
            "\"Codec\":\"h264\","
            "\"Conditions\":["
              "{\"Condition\":\"LessThanEqual\",\"Property\":\"VideoProfile\","
               "\"Value\":\"baseline\",\"IsRequired\":false},"
              "{\"Condition\":\"LessThanEqual\",\"Property\":\"VideoLevel\","
               "\"Value\":\"31\",\"IsRequired\":false},"
              "{\"Condition\":\"LessThanEqual\",\"Property\":\"Width\","
               "\"Value\":\"960\",\"IsRequired\":false},"
              "{\"Condition\":\"LessThanEqual\",\"Property\":\"Height\","
               "\"Value\":\"540\",\"IsRequired\":false}"
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
          "\"SubtitleProfiles\":[]"
        "}}";

    int status = http_request(1, url, body, g_token, responseBuffer, RESPONSE_SIZE);
    if (status != 200) {
        char buf[64];
        snprintf(buf, sizeof(buf), "playbackinfo: http status %d", status);
        plog(buf);
        return false;
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
