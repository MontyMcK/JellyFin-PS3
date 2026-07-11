#include "stream.h"
#include "plog.h"
#include "jellyfin_api.h"
#include "http.h"
#include "timing.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ppu-types.h>
#include <net/net.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sysutil/sysutil.h>

extern u32 running;

volatile bool g_stream_cancel = false;

// How long to wait for the server's response headers.  A burn-in request
// (SubtitleMethod=Encode) makes Jellyfin extract the subtitle track from the
// source file before the transcode produces any output, and headers are only
// sent once it does — on a big MKV that can take a minute or more.
#define STREAM_HDR_DEADLINE_US  120000000ULL

static bool s_chunked      = false;
static int  s_chunk_remain = -1;
static char s_chdr[32];
static int  s_chdr_n      = 0;
static int  s_ctrail       = 0;

// Partial-read carry: bytes already consumed from the TCP stream when a
// receive timeout interrupts a packet read.  Without this, returning 0
// mid-packet DISCARDS those bytes — the TS stream desyncs and the decoder
// conceals the resulting garbage with stale macroblocks until the next IDR.
static u8   s_carry[188];   // one TS packet — the only read size callers use
static int  s_carry_n = 0;

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

    // 500 ms receive timeout — lets the header wait below poll instead of
    // blocking forever, and remains in effect for the stream (caller can
    // tighten it after connecting).
    { struct { u32 sec; u32 usec; } tv = { 0, 500000 };
      setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)); }

    char hdr[4096]; int htotal = 0;
    u64 hdr_t0     = timing_get_us();
    u64 hdr_log_us = hdr_t0;
    while (htotal < (int)sizeof(hdr)-1) {
        int n = netRecv(sock, hdr + htotal, 1, 0);
        if (n == 1) {
            htotal++;
            if (htotal >= 4 && memcmp(hdr + htotal - 4, "\r\n\r\n", 4) == 0) break;
            continue;
        }
        if (n == 0) {
            plog("stream_open: closed before headers");
            netClose(sock); return -1;
        }
        // n < 0: receive timeout — the server hasn't started responding yet.
        // Keep the system callback pumped so quit still works, and give up
        // at the deadline instead of freezing the player.
        sysUtilCheckCallback();
        if (!running || g_stream_cancel) { netClose(sock); return -1; }
        u64 now = timing_get_us();
        if (now - hdr_t0 >= STREAM_HDR_DEADLINE_US) {
            plog("stream_open: header timeout");
            netClose(sock); return -1;
        }
        if (now - hdr_log_us >= 5000000ULL) {
            hdr_log_us = now;
            char buf[64];
            snprintf(buf, sizeof(buf), "stream_open: waiting for server (%llus)",
                     (unsigned long long)((now - hdr_t0) / 1000000ULL));
            plog(buf);
        }
    }
    hdr[htotal] = '\0';

    int status = 0;
    if (strncmp(hdr, "HTTP/", 5) == 0) {
        char *sp = strchr(hdr, ' ');
        if (sp) status = atoi(sp + 1);
    }

    s_chunked      = (strstr(hdr, "chunked") != NULL);
    s_chunk_remain = -1;
    s_chdr_n       = 0;
    s_ctrail       = 0;
    s_carry_n      = 0;
    {
        char buf[64];
        snprintf(buf, sizeof(buf), "stream_open: status=%d chunked=%d", status, (int)s_chunked);
        plog(buf);
    }
    if (status != 200) { netClose(sock); return -1; }

    return sock;
}

// Stash the partial packet so the next call resumes instead of losing bytes.
static int stream_save_carry(const u8 *buf, int got) {
    if (got > 0 && got <= (int)sizeof(s_carry)) {
        memcpy(s_carry, buf, got);
        s_carry_n = got;
        static int s_carry_log = 0;
        if (s_carry_log < 20) {
            s_carry_log++;
            char lb[48];
            snprintf(lb, sizeof(lb), "stream_carry: saved=%d", got);
            plog(lb);
        }
    }
    return 0;
}

int stream_read(int sock, u8 *buf, int size) {
    int got = 0;
    // Resume a packet interrupted by a receive timeout on a previous call.
    if (s_carry_n > 0) {
        got = s_carry_n <= size ? s_carry_n : size;
        memcpy(buf, s_carry, got);
        s_carry_n = 0;
    }
    while (got < size) {
        if (!s_chunked) {
            u64 t0 = timing_get_us();
            int n = netRecv(sock, buf + got, size - got, 0);
            u64 dt = timing_get_us() - t0;
            if (n == 0) {
                plog("net_error: rc=0 (closed)");
                return -1;
            }
            if (n < 0) return stream_save_carry(buf, got);   // timeout: resume later
            if (dt > 50000) {
                char lb[64];
                snprintf(lb, sizeof(lb), "net_stall: %llums bytes=%d",
                         (unsigned long long)(dt / 1000ULL), n);
                plog(lb);
            }
            got += n;
            continue;
        }

        if (s_ctrail > 0) {
            u8 c;
            int n = netRecv(sock, &c, 1, 0);
            if (n == 0) return -1;
            if (n <  0) return stream_save_carry(buf, got);   // timeout: resume later
            s_ctrail--;
            continue;
        }

        if (s_chunk_remain <= 0) {
            u8 c;
            int n = netRecv(sock, &c, 1, 0);
            if (n == 0) return -1;
            if (n <  0) return stream_save_carry(buf, got);   // timeout: resume later
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
        u64 t0 = timing_get_us();
        int n = netRecv(sock, buf + got, want, 0);
        u64 dt = timing_get_us() - t0;
        if (n == 0) {
            plog("net_error: rc=0 (closed)");
            return -1;
        }
        if (n < 0) return stream_save_carry(buf, got);   // timeout: resume later
        if (dt > 50000) {
            char lb[64];
            snprintf(lb, sizeof(lb), "net_stall: %llums bytes=%d",
                     (unsigned long long)(dt / 1000ULL), n);
            plog(lb);
        }
        got            += n;
        s_chunk_remain -= n;
        if (s_chunk_remain == 0)
            s_ctrail = 2;
    }
    return 1;
}
