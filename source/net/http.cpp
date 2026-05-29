#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ppu-types.h>
#include <net/net.h>
#include <netinet/in.h>
#include <sysmodule/sysmodule.h>

#include "http.h"

static char s_raw_buf[RESPONSE_SIZE + 4096];
static char s_bin_buf[64*1024];

static void url_parse(const char *url, char *host, int hsz,
                      int *port, char *path, int psz) {
    const char *p = url;
    if (strncmp(p, "http://", 7) == 0) p += 7;
    const char *h = p;
    while (*p && *p != ':' && *p != '/') p++;
    int hl = p - h; if (hl >= hsz) hl = hsz - 1;
    memcpy(host, h, hl); host[hl] = '\0';
    *port = 8096;
    if (*p == ':') { p++; *port = atoi(p); while (*p && *p != '/') p++; }
    strncpy(path, *p ? p : "/", psz - 1); path[psz - 1] = '\0';
}

int http_init(void) {
    int ret;
    ret = sysModuleLoad(SYSMODULE_NET); if (ret < 0) return ret;
    ret = netInitialize();              if (ret < 0) { sysModuleUnload(SYSMODULE_NET); return ret; }
    return HTTP_SUCCESS;
}

void http_end(void) {
    netDeinitialize();
    sysModuleUnload(SYSMODULE_NET);
}

int http_request(int is_post, const char *url, const char *body,
                 const char *token, char *out, int out_size) {
    char host[256]; int port; char path[512];
    url_parse(url, host, sizeof(host), &port, path, sizeof(path));

    int sock = netSocket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock < 0) return -1;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons((u16)port);
    unsigned na=0, nb=0, nc=0, nd=0;
    sscanf(host, "%u.%u.%u.%u", &na, &nb, &nc, &nd);
    addr.sin_addr.s_addr = htonl((na<<24)|(nb<<16)|(nc<<8)|nd);

    if (netConnect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        netClose(sock); return -1;
    }

    char auth[400];
    if (token && token[0])
        snprintf(auth, sizeof(auth),
            "MediaBrowser Token=\"%s\"", token);
    else
        snprintf(auth, sizeof(auth),
            "MediaBrowser Client=\"PS3\", Device=\"PS3\","
            " DeviceId=\"ps3\", Version=\"0.1\"");

    int blen = (is_post && body) ? (int)strlen(body) : 0;

    char req[2048]; int rlen = 0;
    rlen += snprintf(req+rlen, sizeof(req)-rlen,
        "%s %s HTTP/1.1\r\n"
        "Host: %s:%d\r\n"
        "Authorization: %s\r\n"
        "Accept: application/json\r\n"
        "User-Agent: " HTTP_USER_AGENT "\r\n",
        is_post ? "POST" : "GET", path, host, port, auth);
    if (blen > 0)
        rlen += snprintf(req+rlen, sizeof(req)-rlen,
            "Content-Type: application/json\r\n"
            "Content-Length: %d\r\n", blen);
    rlen += snprintf(req+rlen, sizeof(req)-rlen, "Connection: close\r\n\r\n");

    netSend(sock, req, rlen, 0);
    if (blen > 0) netSend(sock, body, blen, 0);

    int total = 0, n;
    memset(s_raw_buf, 0, sizeof(s_raw_buf));
    while (total < (int)sizeof(s_raw_buf) - 1) {
        n = netRecv(sock, s_raw_buf + total, sizeof(s_raw_buf) - total - 1, 0);
        if (n <= 0) break;
        total += n;
    }
    s_raw_buf[total] = '\0';
    netClose(sock);

    int status = -1;
    if (strncmp(s_raw_buf, "HTTP/", 5) == 0) {
        char *sp = strchr(s_raw_buf, ' ');
        if (sp) status = atoi(sp + 1);
    }

    memset(out, 0, out_size);
    char *bp = strstr(s_raw_buf, "\r\n\r\n");
    if (bp) {
        bp += 4;
        int body_bytes = total - (int)(bp - s_raw_buf);
        if (body_bytes < 0)       body_bytes = 0;
        if (body_bytes > out_size-1) body_bytes = out_size - 1;
        memcpy(out, bp, body_bytes);
        out[body_bytes] = '\0';
    }

    return status;
}

int http_fetch_binary(const char *url, const char *token,
                      uint8_t *out, int out_size) {
    char host[256]; int port; char path[512];
    url_parse(url, host, sizeof(host), &port, path, sizeof(path));

    int sock = netSocket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock < 0) return -1;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons((u16)port);
    unsigned na=0, nb=0, nc=0, nd=0;
    sscanf(host, "%u.%u.%u.%u", &na, &nb, &nc, &nd);
    addr.sin_addr.s_addr = htonl((na<<24)|(nb<<16)|(nc<<8)|nd);

    if (netConnect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        netClose(sock); return -1;
    }

    char auth[400];
    if (token && token[0])
        snprintf(auth, sizeof(auth), "MediaBrowser Token=\"%s\"", token);
    else
        snprintf(auth, sizeof(auth),
            "MediaBrowser Client=\"PS3\", Device=\"PS3\","
            " DeviceId=\"ps3\", Version=\"0.1\"");

    char req[2048]; int rlen = 0;
    rlen += snprintf(req+rlen, sizeof(req)-rlen,
        "GET %s HTTP/1.1\r\n"
        "Host: %s:%d\r\n"
        "Authorization: %s\r\n"
        "Accept: image/jpeg,image/*\r\n"
        "User-Agent: " HTTP_USER_AGENT "\r\n"
        "Connection: close\r\n\r\n",
        path, host, port, auth);
    netSend(sock, req, rlen, 0);

    int total = 0, n;
    while (total < (int)sizeof(s_bin_buf) - 1) {
        n = netRecv(sock, s_bin_buf + total, sizeof(s_bin_buf) - total - 1, 0);
        if (n <= 0) break;
        total += n;
    }
    netClose(sock);

    int status = -1;
    if (strncmp(s_bin_buf, "HTTP/", 5) == 0) {
        char *sp = strchr(s_bin_buf, ' ');
        if (sp) status = atoi(sp + 1);
    }
    if (status != 200) return -1;

    char *bp = strstr(s_bin_buf, "\r\n\r\n");
    if (!bp) return -1;
    bp += 4;
    int body_bytes = total - (int)(bp - s_bin_buf);
    if (body_bytes <= 0) return -1;
    if (body_bytes > out_size) body_bytes = out_size;
    memcpy(out, bp, body_bytes);
    return body_bytes;
}
