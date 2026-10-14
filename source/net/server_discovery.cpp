// The PS3 half of server_discovery.h: the broadcast, and the login check.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <ppu-types.h>
#include <net/net.h>
#include <net/socket.h>
#include <net/poll.h>
#include <netinet/in.h>

#include "server_discovery.h"
#include "http.h"
#include "plog.h"
#include "timing.h"

static int sd_discover_locked(SdServer *out, int max, int wait_ms);

// The socket work holds the net lock, so it cannot race a poster or a search
// opening their own connections.
int sd_discover(SdServer *out, int max, int wait_ms) {
    http_net_lock();
    const int n = sd_discover_locked(out, max, wait_ms);
    http_net_unlock();
    return n;
}

static int sd_discover_locked(SdServer *out, int max, int wait_ms) {
    const int s = netSocket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s < 0) { plog("discovery: no UDP socket"); return 0; }
    int on = 1;
    netSetSockOpt(s, SOL_SOCKET, SO_BROADCAST, &on, sizeof on);

    struct sockaddr_in to;
    memset(&to, 0, sizeof to);
    to.sin_family      = AF_INET;
    to.sin_port        = htons(SD_PORT);
    to.sin_addr.s_addr = 0xFFFFFFFFu;   // 255.255.255.255 in either byte order
    // Twice: a lone datagram on a busy Wi-Fi link is easily lost.
    int sent = 0;
    for (int i = 0; i < 2; i++)
        if (netSendTo(s, SD_QUERY, strlen(SD_QUERY), 0,
                      (struct sockaddr *)&to, sizeof to) > 0) sent++;
    if (!sent) {
        plog("discovery: broadcast failed (network not up?)");
        netClose(s);
        return 0;
    }

    int n = 0;
    const u64 deadline = timing_get_us() + (u64)wait_ms * 1000ULL;
    char buf[1024];
    while (n < max) {
        const u64 now = timing_get_us();
        if (now >= deadline) break;
        struct pollfd pfd;
        pfd.fd = s; pfd.events = POLLIN; pfd.revents = 0;
        const int left_ms = (int)((deadline - now) / 1000ULL) + 1;
        if (netPoll(&pfd, 1, left_ms) <= 0 || !(pfd.revents & POLLIN)) break;

        struct sockaddr_in from;
        socklen_t fl = sizeof from;
        memset(&from, 0, sizeof from);
        const int got = (int)netRecvFrom(s, buf, sizeof buf - 1, 0,
                                         (struct sockaddr *)&from, &fl);
        if (got <= 0) break;
        buf[got] = '\0';

        const u8 *a = (const u8 *)&from.sin_addr.s_addr;   // network order = memory order
        char ip[20];
        snprintf(ip, sizeof ip, "%u.%u.%u.%u", a[0], a[1], a[2], a[3]);
        SdServer sv;
        if (!sd_parse_reply(buf, got, ip, &sv)) continue;
        bool dup = false;
        for (int i = 0; i < n; i++) if (strcmp(out[i].id, sv.id) == 0) dup = true;
        if (dup) continue;
        out[n++] = sv;
        char b[256];
        snprintf(b, sizeof b, "discovery: '%.40s' at %.80s (from %s)", sv.name, sv.address, ip);
        plog(b);
    }
    netClose(s);
    return n;
}

bool sd_find_moved(const char *old_server, const char *userid, const char *token,
                   char *out, int cap) {
    if (out && cap > 0) out[0] = '\0';
    if (!old_server || !userid || !userid[0] || !token || !token[0] || !out) return false;
    if (strncasecmp(old_server, "https://", 8) == 0) {
        plog("discovery: the saved server is https -- not looking for it by broadcast");
        return false;
    }

    SdServer sv[SD_MAX];
    const int n = sd_discover(sv, SD_MAX, 1500);
    char cand[SD_MAX * 2][160];
    const int k = sd_candidates(sv, n, old_server, cand, SD_MAX * 2);
    { char b[96]; snprintf(b, sizeof b, "discovery: %d server(s), %d new address(es) to try", n, k); plog(b); }

    const int cap_buf = 16 * 1024;
    char *buf = (char *)malloc(cap_buf);
    if (!buf) return false;
    bool found = false;
    for (int i = 0; i < k && !found; i++) {
        char url[256];
        snprintf(url, sizeof url, "%s/Users/%s", cand[i], userid);
        const int st = http_probe_get(url, token, buf, cap_buf);
        char b[224];
        snprintf(b, sizeof b, "discovery: %.150s -> http=%d", cand[i], st);
        plog(b);
        if (st == 200) {
            snprintf(out, cap, "%s", cand[i]);
            found = true;
        }
    }
    free(buf);
    return found;
}
