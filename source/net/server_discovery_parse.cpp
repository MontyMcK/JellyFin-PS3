// The pure half of server_discovery.h: reply parsing and URL rules.
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "server_discovery.h"
#include "../api/json_lite.h"

bool sd_parse_reply(const char *buf, int len, const char *from_ip, SdServer *out) {
    memset(out, 0, sizeof *out);
    if (!buf || len <= 0) return false;
    const char *end = buf + len;
    const char *obj = skip_ws(buf, end);
    if (obj + 3 < end && (unsigned char)obj[0] == 0xEF && (unsigned char)obj[1] == 0xBB
        && (unsigned char)obj[2] == 0xBF)
        obj = skip_ws(obj + 3, end);   // Jellyfin's JSON can carry a BOM
    if (obj >= end || *obj != '{') return false;

    const char *v;
    if ((v = find_top_value(obj, end, "Address")))
        read_json_string(v, end, out->address, sizeof out->address);
    if ((v = find_top_value(obj, end, "Id")))
        read_json_string(v, end, out->id, sizeof out->id);
    if ((v = find_top_value(obj, end, "Name")))
        read_json_string(v, end, out->name, sizeof out->name);

    if (strncasecmp(out->address, "http://", 7) != 0 &&
        strncasecmp(out->address, "https://", 8) != 0) {
        out->address[0] = '\0';
    }
    int al = (int)strlen(out->address);
    while (al > 0 && out->address[al - 1] == '/') out->address[--al] = '\0';
    if (!out->address[0] || !out->id[0]) return false;

    if (from_ip && from_ip[0])
        sd_address_with_host(out->address, from_ip, out->alt, sizeof out->alt);
    if (out->alt[0] && sd_same_server_url(out->alt, out->address)) out->alt[0] = '\0';
    return true;
}

namespace {

struct Url {
    char scheme[8];
    char host[128];
    int  port;
    char path[128];
};

bool split(const char *u, Url *o) {
    memset(o, 0, sizeof *o);
    if (!u) return false;
    const char *p;
    if      (strncasecmp(u, "http://", 7) == 0)  { snprintf(o->scheme, sizeof o->scheme, "http");  p = u + 7; o->port = 80; }
    else if (strncasecmp(u, "https://", 8) == 0) { snprintf(o->scheme, sizeof o->scheme, "https"); p = u + 8; o->port = 443; }
    else return false;
    const char *h0 = p;
    while (*p && *p != ':' && *p != '/') p++;
    const int hl = (int)(p - h0);
    if (hl <= 0 || hl >= (int)sizeof o->host) return false;
    for (int i = 0; i < hl; i++) o->host[i] = (char)tolower((unsigned char)h0[i]);
    if (*p == ':') {
        p++;
        int port = 0, digits = 0;
        while (*p >= '0' && *p <= '9' && digits < 6) { port = port * 10 + (*p++ - '0'); digits++; }
        if (digits == 0 || port <= 0 || port > 65535) return false;
        o->port = port;
    }
    if (*p && *p != '/') return false;
    snprintf(o->path, sizeof o->path, "%s", p);
    int pl = (int)strlen(o->path);
    while (pl > 0 && o->path[pl - 1] == '/') o->path[--pl] = '\0';
    return true;
}

}   // namespace

bool sd_address_with_host(const char *address, const char *host, char *out, int cap) {
    if (out && cap > 0) out[0] = '\0';
    Url u;
    if (!out || cap <= 0 || !host || !host[0] || !split(address, &u)) return false;
    const int n = snprintf(out, cap, "%s://%s:%d%s", u.scheme, host, u.port, u.path);
    if (n < 0 || n >= cap) { out[0] = '\0'; return false; }
    return true;
}

bool sd_same_server_url(const char *a, const char *b) {
    Url x, y;
    if (!split(a, &x) || !split(b, &y)) return false;
    return strcmp(x.scheme, y.scheme) == 0 && strcmp(x.host, y.host) == 0 &&
           x.port == y.port && strcmp(x.path, y.path) == 0;
}

int sd_candidates(const SdServer *servers, int n, const char *old_server,
                  char (*out)[160], int max) {
    int k = 0;
    for (int i = 0; i < n && k < max; i++) {
        const char *c[2] = { servers[i].alt, servers[i].address };   // where the reply came from first: it is proven to reach us
        for (int j = 0; j < 2 && k < max; j++) {
            if (!c[j][0]) continue;
            if (strncasecmp(c[j], "http://", 7) != 0) continue;   // see sd_find_moved
            if (old_server && sd_same_server_url(c[j], old_server)) continue;
            bool dup = false;
            for (int m = 0; m < k; m++) if (sd_same_server_url(out[m], c[j])) dup = true;
            if (dup) continue;
            snprintf(out[k++], 160, "%s", c[j]);
        }
    }
    return k;
}
