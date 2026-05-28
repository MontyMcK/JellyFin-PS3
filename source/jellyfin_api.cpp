// Shared utilities: global session state, JSON helpers, item helpers.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "jellyfin_api.h"

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
int json_get_in_range(const char *start, int len,
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

int json_get_int(const char *json, const char *key, int def) {
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
