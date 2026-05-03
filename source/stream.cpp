#include "stream.h"
#include "plog.h"
#include "jellyfin_api.h"
#include "http.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ppu-types.h>
#include <net/net.h>
#include <netinet/in.h>
#include <sys/socket.h>

static bool s_chunked      = false;
static int  s_chunk_remain = -1;
static char s_chdr[32];
static int  s_chdr_n      = 0;
static int  s_ctrail       = 0;

int stream_open(const char *url) {
    const char *p = url;
    if (strncmp(p, "http://", 7) == 0) p += 7;

    char host[256]; int port = 8096; char path[512];
    const char *h = p;
    while (*p && *p != ':' && *p != '/') p++;
    int hl = p - h; if (hl >= 255) hl = 255;
    memcpy(host, h, hl); host[hl] = '\0';
    if (*p == ':') { p++; port = atoi(p); while (*p && *p != '/') p++; }
    strncpy(path, *p ? p : "/", sizeof(path)-1); path[sizeof(path)-1] = '\0';

    int sock = netSocket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock < 0) return -1;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((u16)port);
    unsigned na=0,nb=0,nc=0,nd=0;
    sscanf(host, "%u.%u.%u.%u", &na, &nb, &nc, &nd);
    addr.sin_addr.s_addr = htonl((na<<24)|(nb<<16)|(nc<<8)|nd);

    if (netConnect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        netClose(sock); return -1;
    }

    char auth[400];
    if (g_token[0])
        snprintf(auth, sizeof(auth),
            "MediaBrowser Client=\"PS3\", Device=\"PS3\","
            " DeviceId=\"ps3\", Version=\"0.1\", Token=\"%s\"", g_token);
    else
        snprintf(auth, sizeof(auth),
            "MediaBrowser Client=\"PS3\", Device=\"PS3\","
            " DeviceId=\"ps3\", Version=\"0.1\"");

    char req[2048];
    int rlen = snprintf(req, sizeof(req),
        "GET %s HTTP/1.1\r\n"
        "Host: %s:%d\r\n"
        "X-Emby-Authorization: %s\r\n"
        "Accept: video/mp2t\r\n"
        "User-Agent: " HTTP_USER_AGENT "\r\n"
        "Connection: close\r\n"
        "\r\n",
        path, host, port, auth);
    netSend(sock, req, rlen, 0);

    char hdr[4096]; int htotal = 0;
    while (htotal < (int)sizeof(hdr)-1) {
        if (netRecv(sock, hdr + htotal, 1, 0) != 1) { netClose(sock); return -1; }
        htotal++;
        if (htotal >= 4 && memcmp(hdr + htotal - 4, "\r\n\r\n", 4) == 0) break;
    }
    hdr[htotal] = '\0';

    if (strncmp(hdr, "HTTP/", 5) == 0) {
        char *sp = strchr(hdr, ' ');
        if (sp && atoi(sp+1) != 200) { netClose(sock); return -1; }
    }

    s_chunked      = (strstr(hdr, "chunked") != NULL);
    s_chunk_remain = -1;
    s_chdr_n       = 0;
    s_ctrail       = 0;
    {
        char status[16] = "?";
        if (strncmp(hdr, "HTTP/", 5) == 0) {
            char *sp = strchr(hdr, ' ');
            if (sp) { strncpy(status, sp+1, 12); status[12] = '\0'; }
        }
        char buf[64];
        snprintf(buf, sizeof(buf), "stream_open: status=%s chunked=%d", status, (int)s_chunked);
        plog(buf);
    }

    // 500 ms receive timeout — caller can tighten this after connecting
    struct { u32 sec; u32 usec; } tv = { 0, 500000 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    return sock;
}

int stream_read(int sock, u8 *buf, int size) {
    int got = 0;
    while (got < size) {
        if (!s_chunked) {
            int n = netRecv(sock, buf + got, size - got, 0);
            if (n == 0) return -1;
            if (n <  0) return  0;
            got += n;
            continue;
        }

        if (s_ctrail > 0) {
            u8 c;
            int n = netRecv(sock, &c, 1, 0);
            if (n == 0) return -1;
            if (n <  0) return  0;
            s_ctrail--;
            continue;
        }

        if (s_chunk_remain <= 0) {
            u8 c;
            int n = netRecv(sock, &c, 1, 0);
            if (n == 0) return -1;
            if (n <  0) return  0;
            if (c == '\n') {
                int llen = s_chdr_n;
                while (llen > 0 && (s_chdr[llen-1] == '\r' || s_chdr[llen-1] == ' '))
                    llen--;
                s_chdr[llen] = '\0';
                s_chunk_remain = (int)strtol(s_chdr, NULL, 16);
                s_chdr_n = 0;
                if (s_chunk_remain == 0) return -1;
            } else if (s_chdr_n < (int)sizeof(s_chdr) - 1) {
                s_chdr[s_chdr_n++] = (char)c;
            }
            continue;
        }

        int want = size - got;
        if (want > s_chunk_remain) want = s_chunk_remain;
        int n = netRecv(sock, buf + got, want, 0);
        if (n == 0) return -1;
        if (n <  0) return  0;
        got            += n;
        s_chunk_remain -= n;
        if (s_chunk_remain == 0)
            s_ctrail = 2;
    }
    return 1;
}
