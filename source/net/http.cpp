#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include <ppu-types.h>
#include <net/net.h>
#include <net/socket.h>
#include <net/poll.h>
#include <netinet/in.h>
#include <sys/time.h>
#include <sysmodule/sysmodule.h>
#include <sys/mutex.h>

#include "http.h"

static char s_raw_buf[RESPONSE_SIZE + 4096];

// Serializes http_request() — it shares s_raw_buf, and the playback progress
// reporter calls it from its own thread while the UI/seek paths use it too.
static sys_mutex_t s_http_mtx;
static bool        s_http_mtx_ok = false;
static char s_bin_buf[64*1024];

// Bounds so a slow/unreachable/keep-alive server can never hang us forever.
#define HTTP_CONNECT_TIMEOUT_MS 5000
#define HTTP_IO_TIMEOUT_SEC        8

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
static int read_response(int sock, char *buf, int cap,
                         int *body_off, int *body_len) {
    int  total = 0, header_end = -1;
    long content_len = -1;
    bool chunked = false, parsed = false;
    *body_off = 0; *body_len = 0;

    while (total < cap - 1) {
        if (parsed && !chunked && content_len >= 0 &&
            (long)(total - header_end) >= content_len)
            break;

        int n = netRecv(sock, buf + total, cap - 1 - total, 0);
        if (n <= 0) break;        // close, error, or idle timeout
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

    int status = -1;
    if (strncmp(buf, "HTTP/", 5) == 0) {
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
            " DeviceId=\"ps3\", Version=\"0.1\", Token=\"%s\"", token);
    else
        snprintf(auth, sizeof(auth),
            "MediaBrowser Client=\"PS3\", Device=\"PS3\","
            " DeviceId=\"ps3\", Version=\"0.1\"");

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
    ret = netInitialize();              if (ret < 0) { sysModuleUnload(SYSMODULE_NET); return ret; }
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
    int status = read_response(sock, s_raw_buf, sizeof(s_raw_buf),
                               &body_off, &body_len);
    netClose(sock);

    memset(out, 0, out_size);
    if (body_len > out_size - 1) body_len = out_size - 1;
    if (body_len > 0) memcpy(out, s_raw_buf + body_off, body_len);
    out[body_len > 0 ? body_len : 0] = '\0';

    if (s_http_mtx_ok) sysMutexUnlock(s_http_mtx);
    return status;
}

int http_fetch_binary(const char *url, const char *token,
                      uint8_t *out, int out_size) {
    char host[256]; int port; char path[512];
    url_parse(url, host, sizeof(host), &port, path, sizeof(path));

    int sock = http_connect(host, port);
    if (sock < 0) return -1;

    char req[2048];
    int  rlen = build_headers(req, sizeof(req), "GET", path, host, port,
                              token, "image/jpeg,image/*", 0);
    send_all(sock, req, rlen);

    int body_off, body_len;
    int status = read_response(sock, s_bin_buf, sizeof(s_bin_buf),
                               &body_off, &body_len);
    netClose(sock);

    if (status != 200 || body_len <= 0) return -1;
    if (body_len > out_size) body_len = out_size;
    memcpy(out, s_bin_buf + body_off, body_len);
    return body_len;
}
