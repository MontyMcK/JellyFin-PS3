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

// -------------------------------------------------------------------------
//  Bounded waits
// -------------------------------------------------------------------------
//  This runs on the boot path, between http_init() and running = 1, so a
//  blocking socket call in here is a black screen with no way out: on
//  2026-09-19 a gate file left behind from an earlier session pointed at a
//  server that was no longer listening, netConnect never returned, and the
//  app never reached the XMB (crash_log stuck at "8 http_init").  Nothing
//  below may wait without a bound.
//
//  A gate file pointing at a live server never comes near any of these — a
//  LAN connect and header exchange are milliseconds — so the throughput
//  figures stay comparable to previous runs.
#define CONNECT_TIMEOUT_MS   5000   // per-socket, non-blocking connect + poll
#define IO_TIMEOUT_SEC          5   // per-call recv/send idle timeout
#define DEADLINE_MARGIN_SEC    15   // wall-clock slack on top of the gate file's seconds

static u8 s_scratch[SCRATCH];

// Milliseconds left before the overall deadline, clamped to cap_ms.  Zero
// means the deadline has passed and the caller should give up now.
static int remaining_ms(u64 deadline_us, int cap_ms)
{
    const u64 now = timing_get_us();
    if (now >= deadline_us) return 0;
    const u64 ms = (deadline_us - now) / 1000ull;
    return ms > (u64)cap_ms ? cap_ms : (int)ms;
}

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
    // netGetHostByName is the one call in here with no timeout knob, so it is
    // also the one place that can still stall the boot path.  Log before it so
    // the log says where we stopped; put a literal IP in the gate file and
    // this branch never runs.
    plog("nettest: resolving hostname (no timeout available — prefer a literal IP)");
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
// deadline_us is the overall wall clock for the whole test; every wait in here
// is clamped to whatever is left of it.
static int open_ranged(u32 ip, int port, const char *host, const char *path,
                       u64 start, u64 deadline_us)
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

    // Non-blocking connect bounded by poll — same shape as http_connect() in
    // http.cpp.  A blocking netConnect to a host that answers ARP but has
    // nothing on the port (server stopped, firewall dropping SYN) never
    // returns, which is exactly how this wedged startup.
    int nb = 1;
    netSetSockOpt(s, SOL_SOCKET, SO_NBIO, &nb, sizeof(nb));
    if (netConnect(s, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        struct pollfd pfd;
        pfd.fd = s; pfd.events = POLLOUT; pfd.revents = 0;
        const int budget = remaining_ms(deadline_us, CONNECT_TIMEOUT_MS);
        if (budget <= 0 || netPoll(&pfd, 1, budget) <= 0 || !(pfd.revents & POLLOUT)) {
            netClose(s); return -1;
        }
        int soerr = 0; socklen_t sl = sizeof(soerr);
        if (netGetSockOpt(s, SOL_SOCKET, SO_ERROR, &soerr, &sl) < 0 || soerr != 0) {
            netClose(s); return -1;
        }
    }
    nb = 0;
    netSetSockOpt(s, SOL_SOCKET, SO_NBIO, &nb, sizeof(nb));

    // Idle timeouts on BOTH directions, armed before the request goes out so
    // the send cannot park forever on a window that never opens.  The receive
    // value is the same 5 s the test has always used and stays in effect for
    // the measurement loop below, so nothing about the measurement changes.
    struct { u32 sec; u32 usec; } tv = { IO_TIMEOUT_SEC, 0 };
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    char req[640];
    int n = snprintf(req, sizeof(req),
                     "GET %s HTTP/1.1\r\nHost: %s\r\n"
                     "Range: bytes=%llu-\r\nConnection: close\r\n\r\n",
                     path, host, (unsigned long long)start);
    if (netSend(s, req, n, 0) != n) { netClose(s); return -1; }

    char hdr[2048]; int t = 0;
    while (t < (int)sizeof(hdr) - 1) {
        // One byte per recv with a 5 s idle timeout is up to 2048 * 5 s if a
        // server dribbles the header, so the overall deadline is rechecked
        // every byte.  timing_get_us() is a timebase register read.
        if (timing_get_us() >= deadline_us) { netClose(s); return -1; }
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
    // Overall wall clock for everything below.  secs is the measurement
    // window; the margin covers connects and header exchange, which cost
    // milliseconds against a reachable server.  Once it passes, the test
    // abandons whatever it is doing and returns.
    const int total_budget_secs = secs + DEADLINE_MARGIN_SEC;
    const u64 t_start  = timing_get_us();
    const u64 deadline = t_start + (u64)total_budget_secs * 1000000ull;

    u32 ip = resolve(host);
    if (!ip) { plog("nettest: cannot resolve host"); return; }

    char b[160];
    snprintf(b, sizeof(b), "nettest: %d socket(s), %ds, %s:%d%s",
             nsock, secs, host, port, path);
    plog(b);

    int  sk[MAX_SOCKETS];
    u64  got[MAX_SOCKETS];
    int  live = 0;
    bool abandoned = false;
    for (int i = 0; i < nsock; i++) { sk[i] = -1; got[i] = 0; }

    for (int i = 0; i < nsock; i++) {
        if (timing_get_us() >= deadline) {
            snprintf(b, sizeof(b),
                     "nettest: ABANDONED - %ds budget gone while opening socket %d of %d",
                     total_budget_secs, i, nsock);
            plog(b);
            abandoned = true;
            break;
        }
        sk[i] = open_ranged(ip, port, host, path, (u64)i * RANGE_STRIDE, deadline);
        if (sk[i] >= 0) live++;
    }
    if (abandoned || !live) {
        // Nothing was measured, so there is nothing to report — just make sure
        // no socket is left behind before the UI comes up.
        int closed = 0;
        for (int i = 0; i < nsock; i++)
            if (sk[i] >= 0) { netClose(sk[i]); sk[i] = -1; closed++; }
        if (!abandoned)
            plog("nettest: ABANDONED - no sockets opened (server not listening?)");
        snprintf(b, sizeof(b), "nettest: gave up after %.1fs, %d socket(s) closed",
                 (double)(timing_get_us() - t_start) / 1e6, closed);
        plog(b);
        return;
    }

    const u64 t0  = timing_get_us();
    const u64 end = t0 + (u64)secs * 1000000ull;
    for (;;) {
        const u64 now = timing_get_us();
        if (now >= end) break;
        if (now >= deadline) {
            // Only reachable if opening the sockets ate the whole margin; the
            // figures below are still bytes over real elapsed time, but the
            // window was cut short, so flag it rather than let the numbers be
            // compared against a full run.
            snprintf(b, sizeof(b),
                     "nettest: ABANDONED - %ds budget gone during measurement, "
                     "window cut short", total_budget_secs);
            plog(b);
            break;
        }
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
            // POLLIN says this recv returns immediately, and SO_RCVTIMEO caps
            // it at 5 s if it does not; stopping here keeps the worst case at
            // one stalled recv rather than one per socket.
            if (timing_get_us() >= deadline) break;
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
