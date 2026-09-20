#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <malloc.h>   // memalign -- libnet pool alignment

#include "plog.h"   // truncated-body warning

#include <ppu-types.h>
#include <net/net.h>
#include <net/socket.h>
#include <net/poll.h>
#include <netinet/in.h>
#include <sys/time.h>
#include <sysmodule/sysmodule.h>
#include <sys/mutex.h>

#include "http.h"
#include "jf_paths.h"

// Jellyfin-layer hooks (api_auth.cpp): the per-install device id sent in the
// MediaBrowser identity, and the expired-session flag raised on any
// authenticated 401 so the UI can bounce back to the login screen.
extern const char *jf_device_id(void);
extern volatile bool g_auth_expired;

static char s_raw_buf[RESPONSE_SIZE + 4096];

// Serializes http_request() — it shares s_raw_buf, and the playback progress
// reporter calls it from its own thread while the UI/seek paths use it too.
static sys_mutex_t s_http_mtx;
static bool        s_http_mtx_ok = false;

// Bounds so a slow/unreachable/keep-alive server can never hang us forever.
#define HTTP_CONNECT_TIMEOUT_MS 5000
#define HTTP_IO_TIMEOUT_SEC        8

// SO_RCVTIMEO is an IDLE timeout, but on the FIRST read it is really a
// time-to-first-byte limit, and those are very different budgets.
//
// Measured against this user's server (a debrid/AIOStreams-backed library):
// a browse/tab request answers in 35 ms, but a `searchTerm` query takes
// 2.5-8.4 s to emit its first byte, varying run to run for the same term.
// Eight seconds sits right on top of that spread, so search timed out
// constantly while everything else was nowhere near the limit -- and no
// rearrangement of the query helps, because Limit, SortBy, Fields and
// EnableTotalRecordCount were each measured and none of them is what costs
// the time.
//
// So let the FIRST byte wait longer, and go straight back to failing fast on
// an idle socket the moment any data has arrived. A server that is simply
// down still fails at connect() in 5 s; this only extends the case where the
// server accepted the connection and is thinking about it.
#define HTTP_FIRST_BYTE_TRIES      3   // x HTTP_IO_TIMEOUT_SEC = 24 s

static void url_parse(const char *url, char *host, int hsz,
                      int *port, char *path, int psz) {
    const char *p = url;
    bool https = false;
    if      (strncmp(p, "http://",  7) == 0) p += 7;
    else if (strncmp(p, "https://", 8) == 0) { p += 8; https = true; }
    const char *h = p;
    while (*p && *p != ':' && *p != '/') p++;
    int hl = p - h;
    if (hl >= hsz) hl = hsz - 1;
    if (hl < 0) hl = 0;
    memcpy(host, h, hl);
    host[hl] = '\0';
    *port = https ? 443 : 8096;
    if (*p == ':') { p++; *port = atoi(p); while (*p && *p != '/') p++; }
    strncpy(path, *p ? p : "/", psz - 1); path[psz - 1] = '\0';
}

// -------------------------------------------------------
// Host resolution — accepts dotted-quad IPs *and* hostnames (DNS), so
// remote / reverse-proxied servers entered by domain name work.
// Returns the address in network byte order, or 0 on failure.
// -------------------------------------------------------
static bool host_is_ipv4(const char *h) {
    int dots = 0;
    for (const char *p = h; *p; p++) {
        if (*p == '.') dots++;
        else if (*p < '0' || *p > '9') return false;
    }
    return dots == 3;
}

static u32 resolve_host(const char *host) {
    if (host_is_ipv4(host)) {
        unsigned a=0, b=0, c=0, d=0;
        sscanf(host, "%u.%u.%u.%u", &a, &b, &c, &d);
        return htonl((a<<24)|(b<<16)|(c<<8)|d);
    }
    struct net_hostent *he = netGetHostByName(host);
    if (he) {
        u32 *addr_list = (u32*)(u64)he->h_addr_list;
        if (addr_list && addr_list[0]) {
            u32 *in = (u32*)(u64)addr_list[0];
            return *in;   // already network byte order
        }
    }
    return 0;
}

// -------------------------------------------------------
// Connect with a bounded timeout, then arm idle read/write timeouts.
// Returns a connected blocking socket, or -1.
// -------------------------------------------------------
static int http_connect(const char *host, int port) {
    u32 ip = resolve_host(host);
    if (ip == 0) return -1;

    int sock = netSocket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock < 0) return -1;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons((u16)port);
    addr.sin_addr.s_addr = ip;

    // Non-blocking connect so we can bound the wait.
    int nb = 1;
    netSetSockOpt(sock, SOL_SOCKET, SO_NBIO, &nb, sizeof(nb));

    int cr = netConnect(sock, (struct sockaddr*)&addr, sizeof(addr));
    if (cr < 0) {
        struct pollfd pfd;
        pfd.fd = sock; pfd.events = POLLOUT; pfd.revents = 0;
        int pr = netPoll(&pfd, 1, HTTP_CONNECT_TIMEOUT_MS);
        if (pr <= 0 || !(pfd.revents & POLLOUT)) { netClose(sock); return -1; }
        int soerr = 0; socklen_t sl = sizeof(soerr);
        if (netGetSockOpt(sock, SOL_SOCKET, SO_ERROR, &soerr, &sl) < 0 || soerr != 0) {
            netClose(sock); return -1;
        }
    }

    // Back to blocking, with idle timeouts on both directions.
    nb = 0;
    netSetSockOpt(sock, SOL_SOCKET, SO_NBIO, &nb, sizeof(nb));
    struct timeval tv; tv.tv_sec = HTTP_IO_TIMEOUT_SEC; tv.tv_usec = 0;
    netSetSockOpt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    netSetSockOpt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    return sock;
}

static void send_all(int sock, const char *buf, int len) {
    int sent = 0;
    while (sent < len) {
        int n = netSend(sock, buf + sent, len - sent, 0);
        if (n <= 0) break;
        sent += n;
    }
}

// Case-insensitive substring search over a fixed-length region (needle is lowercase).
static const char *ci_find(const char *hay, int hlen, const char *needle) {
    int nlen = (int)strlen(needle);
    for (int i = 0; i + nlen <= hlen; i++) {
        int j = 0;
        for (; j < nlen; j++) {
            char a = hay[i + j];
            if (a >= 'A' && a <= 'Z') a += 32;
            if (a != needle[j]) break;
        }
        if (j == nlen) return hay + i;
    }
    return NULL;
}

// Decode a Transfer-Encoding: chunked body in place. Returns decoded length.
static int dechunk(char *body, int len) {
    int ri = 0, wi = 0;
    while (ri < len) {
        int sz = 0; bool any = false;
        while (ri < len && isxdigit((unsigned char)body[ri])) {
            int c = tolower((unsigned char)body[ri]);
            sz = sz * 16 + ((c <= '9') ? c - '0' : c - 'a' + 10);
            ri++; any = true;
        }
        if (!any) break;
        while (ri + 1 < len && !(body[ri] == '\r' && body[ri+1] == '\n')) ri++;
        ri += 2;                                  // skip CRLF after size
        if (sz == 0) break;                       // terminating chunk
        if (ri + sz > len) sz = len - ri;         // truncation guard
        memmove(body + wi, body + ri, sz);
        wi += sz; ri += sz;
        if (ri + 1 < len && body[ri] == '\r' && body[ri+1] == '\n') ri += 2;
    }
    return wi;
}

// Read a full HTTP response. Returns status code (or -1) and, via out params,
// the byte offset and length of the (decoded) body within buf. Bounded by the
// socket's idle timeout, and stops early on Content-Length so keep-alive
// connections don't stall waiting for a close.
// Why a read ended the way it did.  Filled in unconditionally (it is a handful
// of stores), but only ever LOGGED when the result is anomalous -- see the
// "200 but EMPTY body" case in http_request().  Thumbnail fetches run this
// path constantly, so a log line per request is not acceptable; a log line per
// impossible-looking result is.
struct RespDiag {
    int  total;         // bytes read from the socket in all
    int  header_end;    // offset of the body, -1 if headers never completed
    int  reads;         // successful netRecv calls
    int  last_n;        // what the final netRecv returned (0/-1 = close/timeout)
    long content_len;   // -1 when the server framed it chunked instead
    bool chunked;
    bool cap_hit;       // ran out of buffer rather than out of response
};

static int read_response(int sock, char *buf, int cap,
                         int *body_off, int *body_len, RespDiag *dg) {
    int  total = 0, header_end = -1;
    long content_len = -1;
    bool chunked = false, parsed = false;
    int  reads = 0, last_n = 0, first_byte_tries = 0;
    *body_off = 0; *body_len = 0;

    while (total < cap - 1) {
        if (parsed && !chunked && content_len >= 0 &&
            (long)(total - header_end) >= content_len)
            break;

        int n = netRecv(sock, buf + total, cap - 1 - total, 0);
        last_n = n;
        if (n <= 0) {
            // Close, error, or timeout.  While NOTHING has arrived yet this is
            // a time-to-first-byte timeout rather than an idle one, so spend
            // the larger budget before giving up -- see HTTP_FIRST_BYTE_TRIES.
            if (total == 0 && ++first_byte_tries < HTTP_FIRST_BYTE_TRIES)
                continue;
            break;
        }
        reads++;
        total += n;
        buf[total] = '\0';

        if (!parsed) {
            char *he = strstr(buf, "\r\n\r\n");   // headers are ASCII
            if (he) {
                header_end = (int)(he - buf) + 4;
                const char *cl = ci_find(buf, header_end, "content-length:");
                if (cl) content_len = atol(cl + strlen("content-length:"));
                chunked = ci_find(buf, header_end, "transfer-encoding:") &&
                          ci_find(buf, header_end, "chunked");
                parsed = true;
            }
        }
    }

    if (dg) {
        dg->total       = total;
        dg->header_end  = header_end;
        dg->reads       = reads;
        dg->last_n      = last_n;
        dg->content_len = content_len;
        dg->chunked     = chunked;
        dg->cap_hit     = (total >= cap - 1);
    }

    // `total` guards the status parse, and it is not paranoia -- it is the bug
    // that hid the search failure for a whole session.
    //
    // `buf` is s_raw_buf, a STATIC buffer reused by every request. When a read
    // fails outright (total == 0) nothing is written into it, so it still holds
    // the PREVIOUS response -- which began "HTTP/1.1 200 OK". The parse below
    // happily read that and returned 200 for a request that received zero
    // bytes, so a hard timeout was reported to callers as a successful empty
    // result. The search screen then showed "No results" instead of an error,
    // and the log said `status=200 count=0`, which is why this looked for a
    // long time like the server returning nothing.
    int status = -1;
    if (total >= 12 && strncmp(buf, "HTTP/", 5) == 0) {
        char *sp = strchr(buf, ' ');
        if (sp) status = atoi(sp + 1);
    }
    if (header_end < 0) return status;

    int blen = total - header_end;
    if (chunked)                                   blen = dechunk(buf + header_end, blen);
    else if (content_len >= 0 && content_len < blen) blen = (int)content_len;

    *body_off = header_end;
    *body_len = blen;
    return status;
}

static int build_headers(char *req, int cap, const char *method,
                         const char *path, const char *host, int port,
                         const char *token, const char *accept, int blen) {
    // Always send the full client identity (Client/Device/DeviceId/Version),
    // appending the access token when we have one.  Sending only the token —
    // without DeviceId — means Jellyfin can't bind the request to a device
    // session: the PS3 never appears in the dashboard, and the PlaySessionId
    // used for seeking isn't tied to a tracked session, so StartTimeTicks is
    // ignored and seeks fall back to offset 0.  stream.cpp already does this.
    char auth[400];
    if (token && token[0])
        snprintf(auth, sizeof(auth),
            "MediaBrowser Client=\"PS3\", Device=\"PS3\","
            " DeviceId=\"%s\", Version=\"0.1\", Token=\"%s\"",
            jf_device_id(), token);
    else
        snprintf(auth, sizeof(auth),
            "MediaBrowser Client=\"PS3\", Device=\"PS3\","
            " DeviceId=\"%s\", Version=\"0.1\"", jf_device_id());

    int rlen = 0;
    rlen += snprintf(req+rlen, cap-rlen,
        "%s %s HTTP/1.1\r\n"
        "Host: %s:%d\r\n"
        "Authorization: %s\r\n"
        "Accept: %s\r\n"
        "Accept-Encoding: identity\r\n"
        "User-Agent: " HTTP_USER_AGENT "\r\n",
        method, path, host, port, auth, accept);
    if (blen > 0)
        rlen += snprintf(req+rlen, cap-rlen,
            "Content-Type: application/json\r\n"
            "Content-Length: %d\r\n", blen);
    rlen += snprintf(req+rlen, cap-rlen, "Connection: close\r\n\r\n");
    return rlen;
}

int http_init(void) {
    int ret;
    ret = sysModuleLoad(SYSMODULE_NET); if (ret < 0) return ret;

    // How libnet's MEMORY POOL is sized, and why this is a switch rather than
    // a decision.
    //
    // netInitialize() hands libnet a hardcoded 128 KB pool (LIBNET_MEMORY_SIZE
    // in ppu/sprx/libnet/init.c) shared by every socket in the process, and
    // socket receive buffers are charged against it.  The theory was that this
    // was the real 25 Mbps receive ceiling: SO_RCVBUF=512KB was accepted and
    // read back as 524288, but nothing said there was 512 KB of pool to back
    // it.  netInitializeNetworkEx() is exported, so a bigger pool is one call
    // away.
    //
    // MEASURED ON HARDWARE 2026-09-18, AND THE THEORY IS WRONG.  With a 4 MB
    // pool, against the identical build and movie at 1080p 25 Mbps:
    //
    //            net= median   net= peak   heartbeats with ring empty
    //   stock      25.0 Mbps    62.2 Mbps             8%
    //   4 MB       12.0 Mbps    18.7 Mbps            55%
    //
    // Throughput HALVED, and getsockopt(SO_RCVBUF) -- the same call, on the
    // same line -- went from returning 524288 to failing outright, as did
    // netGetSockInfo.  Both are libnet telling us it is in a worse state than
    // the stock init leaves it in, not a coincidence of load.  A pool that is
    // merely too big would not break getsockopt.
    //
    // So the default is stock, exactly as v1.0 shipped.  The pool stays
    // reachable because the experiment is worth finishing -- 128 here would
    // isolate whether the damage is the SIZE or the Ex path itself -- but it
    // is opt-in, set at runtime, and never on unless someone asks for it.
    //
    // Size in KB in jellyfin_netpool.txt; 0 or absent means stock.
    {
        u32 kb = 0;
        FILE *f = fopen(jf_data_path("jellyfin_netpool.txt"), "r");
        if (f) {
            unsigned v = 0;
            if (fscanf(f, "%u", &v) != 1) v = 0;
            fclose(f);
            if (v > 8192) v = 8192;     // 8 MB is already absurd; cap it
            kb = v;
        }

        bool up = false;
        if (kb) {
            // 64 KB alignment rather than whatever malloc happens to give.
            // Alignment is the other candidate explanation for the damage
            // above, and PSL1GHT's 128 KB malloc may simply land aligned by
            // luck where a 4 MB one does not.
            void *mem = memalign(64 * 1024, (size_t)kb << 10);
            if (mem) {
                netInitParam pp;
                memset(&pp, 0, sizeof(pp));
                pp.memory      = (u32)(u64)mem;
                pp.memory_size = (u32)kb << 10;
                pp.flags       = 0;
                const s32 rc = netInitializeNetworkEx(&pp);
                char b[112];
                snprintf(b, sizeof(b), "net: libnet pool %u KB rc=%d addr=0x%08x",
                         (unsigned)kb, (int)rc, (unsigned)(u64)mem);
                plog(b);
                if (rc == 0) up = true;
                else         free(mem);
            } else {
                plog("net: pool allocation refused, using stock netInitialize()");
            }
        }
        if (!up) {
            ret = netInitialize();
            if (ret < 0) { sysModuleUnload(SYSMODULE_NET); return ret; }
            plog("net: libnet stock (PSL1GHT 128 KB pool)");
        }
    }

    // Prove the socket layer is healthy BEFORE any playback depends on it.
    // This is the exact pair of calls that regressed under the 4 MB pool, so
    // running them on a throwaway socket at startup turns "is libnet in a good
    // state?" into one line at the top of the log instead of something only
    // visible once a stream is already stuttering.
    {
        int s = netSocket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s < 0) {
            plog("net: probe socket failed");
        } else {
            int want = 512 * 1024, got = 0;
            socklen_t gl = sizeof(got);
            const int src = setsockopt(s, SOL_SOCKET, SO_RCVBUF, &want, sizeof(want));
            const int grc = getsockopt(s, SOL_SOCKET, SO_RCVBUF, &got, &gl);
            char b[112];
            snprintf(b, sizeof(b), "net: probe set=%d get=%d rcvbuf=%d",
                     src, grc, (grc == 0) ? got : -1);
            plog(b);
            netClose(s);
        }
    }
    sys_mutex_attr_t attr;
    memset(&attr, 0, sizeof(attr));
    attr.attr_protocol  = SYS_MUTEX_PROTOCOL_FIFO;
    attr.attr_recursive = SYS_MUTEX_ATTR_NOT_RECURSIVE;
    s_http_mtx_ok = (sysMutexCreate(&s_http_mtx, &attr) == 0);
    return HTTP_SUCCESS;
}

void http_end(void) {
    netDeinitialize();
    sysModuleUnload(SYSMODULE_NET);
}

int http_request(int method, const char *url, const char *body,
                 const char *token, char *out, int out_size) {
    if (s_http_mtx_ok) sysMutexLock(s_http_mtx, 0);
    char host[256]; int port; char path[512];
    url_parse(url, host, sizeof(host), &port, path, sizeof(path));

    int sock = http_connect(host, port);
    if (sock < 0) {
        if (s_http_mtx_ok) sysMutexUnlock(s_http_mtx);
        return -1;
    }

    const char *verb = (method == HTTP_POST)   ? "POST"   :
                       (method == HTTP_DELETE) ? "DELETE" : "GET";
    int  blen = (method == HTTP_POST && body) ? (int)strlen(body) : 0;
    char req[2048];
    int  rlen = build_headers(req, sizeof(req), verb,
                              path, host, port, token, "application/json", blen);
    send_all(sock, req, rlen);
    if (blen > 0) send_all(sock, body, blen);

    int body_off, body_len;
    RespDiag dg; memset(&dg, 0, sizeof(dg));
    int status = read_response(sock, s_raw_buf, sizeof(s_raw_buf),
                               &body_off, &body_len, &dg);
    netClose(sock);

    // A 200 with nothing in it is not a thing a server does, so when it
    // happens the interesting facts are on THIS side of the socket and they
    // are gone the moment this function returns.  The search screen hit
    // exactly this -- "search status: 200 count: 0" with a provably non-empty
    // response on the wire (24 items, 57 KB, confirmed against the server) --
    // and there was no way to tell whether the body never arrived, arrived and
    // failed to dechunk, or arrived and overflowed.  Now there is.
    if (status == 200 && body_len <= 0) {
        char b[224];
        snprintf(b, sizeof(b),
                 "http: 200 EMPTY body -- total=%d hdr_end=%d reads=%d "
                 "last_n=%d chunked=%d clen=%ld cap_hit=%d raw=%.28s | %.60s",
                 dg.total, dg.header_end, dg.reads, dg.last_n,
                 dg.chunked ? 1 : 0, dg.content_len, dg.cap_hit ? 1 : 0,
                 dg.header_end > 0 ? s_raw_buf + dg.header_end : "(no body)",
                 path);
        plog(b);
    }

    // 401 on a request that carried a token: the saved session is dead
    // (revoked server-side).  Login itself sends no token, so a wrong
    // password can't trip this.
    if (status == 401 && token && token[0]) g_auth_expired = true;

    memset(out, 0, out_size);
    if (body_len > out_size - 1) {
        // Truncating JSON produces a parse that "works" and quietly returns
        // fewer items — the version list losing everything past the cut is
        // exactly that bug.  Say so.
        char b[96];
        snprintf(b, sizeof(b),
                 "http: body TRUNCATED %d -> %d bytes (raise RESPONSE_SIZE)",
                 body_len, out_size - 1);
        plog(b);
        body_len = out_size - 1;
    }
    if (body_len > 0) memcpy(out, s_raw_buf + body_off, body_len);
    out[body_len > 0 ? body_len : 0] = '\0';

    if (s_http_mtx_ok) sysMutexUnlock(s_http_mtx);
    return status;
}

int http_fetch_binary(const char *url, const char *token,
                      uint8_t *out, int out_size) {
    // MUST hold the same lock as http_request().  This runs on the thumbnail
    // fetch thread; the UI/seek/progress paths run http_request() on other
    // threads.  The PS3 sys/net resolver + connection setup are not reentrant,
    // so two concurrent HTTP calls corrupt shared net-stack state — observed
    // as a thumbnail fetch that hangs PAST the socket timeouts (the socket
    // timeouts can't bound a wedged netGetHostByName / connect), which with a
    // single fetch thread kills all poster loading after a tab switch.  Also
    // Bounded by the connect/IO timeouts in http_connect,
    // so the UI can never wait more than one in-flight fetch.
    if (s_http_mtx_ok) sysMutexLock(s_http_mtx, 0);

    char host[256]; int port; char path[512];
    url_parse(url, host, sizeof(host), &port, path, sizeof(path));

    int sock = http_connect(host, port);
    if (sock < 0) {
        if (s_http_mtx_ok) sysMutexUnlock(s_http_mtx);
        return -1;
    }

    char req[2048];
    int  rlen = build_headers(req, sizeof(req), "GET", path, host, port,
                              token, "image/jpeg,image/*", 0);
    send_all(sock, req, rlen);

    // Read the response directly into the caller's buffer.  The old 64KB
    // staging buffer silently TRUNCATED anything bigger, which stbi would
    // then reject with "outofdata" — a real trap for full-size art, and 64KB
    // of BSS besides.  (It was NOT the blank-poster bug, despite the earlier
    // hunch: those failures were all reason=outofmem on 12-25KB images, i.e.
    // heap exhaustion — see img_arena.h.  Keeping this anyway: it's correct.)
    int body_off, body_len;
    int status = read_response(sock, (char*)out, out_size,
                               &body_off, &body_len, NULL);
    netClose(sock);

    if (status != 200 || body_len <= 0) {
        if (s_http_mtx_ok) sysMutexUnlock(s_http_mtx);
        return -1;
    }
    memmove(out, out + body_off, body_len);

    if (s_http_mtx_ok) sysMutexUnlock(s_http_mtx);
    return body_len;
}
