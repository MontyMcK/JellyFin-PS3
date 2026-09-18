// Parallel-socket receive test — see net_selftest.h for why this exists.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ppu-types.h>
#include <net/net.h>
#include <net/socket.h>
#include <net/poll.h>
#include <netinet/in.h>

#include "net_selftest.h"
#include "jf_paths.h"
#include "plog.h"
#include "timing.h"

#define NETTEST_FILE  "jellyfin_nettest.txt"
#define MAX_SOCKETS   8
#define SCRATCH       (64 * 1024)

// Each socket starts 64 MB further into the file than the last, so the server
// is doing genuinely independent reads rather than serving one hot range to
// everybody.
#define RANGE_STRIDE  (64ull * 1024 * 1024)

static u8 s_scratch[SCRATCH];

// Minimal URL split — this only ever sees a URL a human put in a config file,
// and a wrong one costs a log line, so it stays small on purpose.
static bool split_url(const char *url, char *host, int hsz, int *port, char *path, int psz)
{
    const char *p = url;
    if (strncmp(p, "http://", 7) == 0) p += 7;
    const char *h = p;
    while (*p && *p != ':' && *p != '/') p++;
    int hl = (int)(p - h);
    if (hl <= 0 || hl >= hsz) return false;
    memcpy(host, h, hl); host[hl] = '\0';
    *port = 80;
    if (*p == ':') { p++; *port = atoi(p); while (*p && *p != '/') p++; }
    snprintf(path, psz, "%s", *p ? p : "/");
    return true;
}

static u32 resolve(const char *host)
{
    unsigned a = 0, b = 0, c = 0, d = 0;
    if (sscanf(host, "%u.%u.%u.%u", &a, &b, &c, &d) == 4)
        return htonl((a << 24) | (b << 16) | (c << 8) | d);
    struct net_hostent *he = netGetHostByName(host);
    if (he) {
        u32 *list = (u32 *)(u64)he->h_addr_list;
        if (list && list[0]) return *(u32 *)(u64)list[0];
    }
    return 0;
}

// Connect, send a ranged GET, and leave the socket positioned at the body.
// Returns the socket or -1.  Header bytes are read one at a time so the read
// cannot run past the body and lose bytes we are about to count.
static int open_ranged(u32 ip, int port, const char *host, const char *path, u64 start)
{
    int s = netSocket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s < 0) return -1;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons((u16)port);
    addr.sin_addr.s_addr = ip;

    // Same receive buffer the player ships with, so the test measures the
    // path the player actually uses rather than a differently-tuned one.
    int rb = 512 * 1024;
    setsockopt(s, SOL_SOCKET, SO_RCVBUF, &rb, sizeof(rb));

    if (netConnect(s, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        netClose(s); return -1;
    }

    char req[640];
    int n = snprintf(req, sizeof(req),
                     "GET %s HTTP/1.1\r\nHost: %s\r\n"
                     "Range: bytes=%llu-\r\nConnection: close\r\n\r\n",
                     path, host, (unsigned long long)start);
    if (netSend(s, req, n, 0) != n) { netClose(s); return -1; }

    struct { u32 sec; u32 usec; } tv = { 5, 0 };
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    char hdr[2048]; int t = 0;
    while (t < (int)sizeof(hdr) - 1) {
        int r = netRecv(s, hdr + t, 1, 0);
        if (r <= 0) { netClose(s); return -1; }
        t += r; hdr[t] = '\0';
        if (t >= 4 && memcmp(hdr + t - 4, "\r\n\r\n", 4) == 0) break;
    }
    // 206 is what a ranged request should get; 200 means the server ignored
    // Range and is sending the whole file to every socket, which would make
    // the aggregate meaningless.  Say so rather than reporting a fake win.
    if (!strstr(hdr, " 206") && !strstr(hdr, " 200")) { netClose(s); return -1; }
    if (!strstr(hdr, " 206")) plog("nettest: WARNING server ignored Range (200, not 206)");
    return s;
}

void net_selftest_run(void)
{
    FILE *f = fopen(jf_data_path(NETTEST_FILE), "r");
    if (!f) return;                       // not configured: do nothing at all

    int nsock = 0, secs = 0;
    char url[512] = {0};
    if (fscanf(f, "%d %d", &nsock, &secs) != 2) { fclose(f); return; }
    if (fscanf(f, "%511s", url) != 1)           { fclose(f); return; }
    fclose(f);

    if (nsock < 1) nsock = 1;
    if (nsock > MAX_SOCKETS) nsock = MAX_SOCKETS;
    if (secs  < 1) secs = 1;
    if (secs  > 60) secs = 60;

    char host[256], path[256]; int port = 80;
    if (!split_url(url, host, sizeof(host), &port, path, sizeof(path))) {
        plog("nettest: bad url"); return;
    }
    u32 ip = resolve(host);
    if (!ip) { plog("nettest: cannot resolve host"); return; }

    char b[160];
    snprintf(b, sizeof(b), "nettest: %d socket(s), %ds, %s:%d%s",
             nsock, secs, host, port, path);
    plog(b);

    int  sk[MAX_SOCKETS];
    u64  got[MAX_SOCKETS];
    int  live = 0;
    for (int i = 0; i < nsock; i++) {
        sk[i]  = open_ranged(ip, port, host, path, (u64)i * RANGE_STRIDE);
        got[i] = 0;
        if (sk[i] >= 0) live++;
    }
    if (!live) { plog("nettest: no sockets opened"); return; }

    const u64 t0  = timing_get_us();
    const u64 end = t0 + (u64)secs * 1000000ull;
    while (timing_get_us() < end) {
        struct pollfd pfd[MAX_SOCKETS];
        int map[MAX_SOCKETS], np = 0;
        for (int i = 0; i < nsock; i++) {
            if (sk[i] < 0) continue;
            pfd[np].fd = sk[i]; pfd[np].events = POLLIN; pfd[np].revents = 0;
            map[np] = i; np++;
        }
        if (!np) break;
        if (netPoll(pfd, np, 200) <= 0) continue;
        for (int k = 0; k < np; k++) {
            if (!(pfd[k].revents & POLLIN)) continue;
            int i = map[k];
            int r = netRecv(sk[i], s_scratch, SCRATCH, 0);
            if (r > 0)      got[i] += (u64)r;
            else if (r == 0) { netClose(sk[i]); sk[i] = -1; }
        }
    }
    const u64 span = timing_get_us() - t0;

    u64 total = 0;
    for (int i = 0; i < nsock; i++) {
        if (sk[i] >= 0) netClose(sk[i]);
        if (got[i] == 0 && sk[i] < 0) continue;
        total += got[i];
        snprintf(b, sizeof(b), "nettest:   socket %d -> %.1f Mbps (%llu KB)",
                 i, (double)got[i] * 8.0 / (double)span,
                 (unsigned long long)(got[i] / 1024));
        plog(b);
    }
    snprintf(b, sizeof(b),
             "nettest: AGGREGATE %.1f Mbps over %d socket(s) in %.1fs",
             (double)total * 8.0 / (double)span, live, (double)span / 1e6);
    plog(b);
}
