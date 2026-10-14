// Offline downloads -- HTTP pieces.  See dl_http.h.

#include "dl_http.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HTTP_USER_AGENT_DL "JellyfinPS3/0.1"   // same identity as http.h

bool dl_url_parse(const char *url, DlUrl *out) {
    const char *p = url;
    // The server address is stored as typed, and the rest of the app (http.cpp)
    // takes it with or without "http://".  A bare "host:port" must download
    // too, or every download from such a server is refused.
    if (strncmp(p, "http://", 7) == 0) p += 7;
    else if (strstr(p, "://")) return false;           // https: no TLS here
    const char *h = p;
    while (*p && *p != ':' && *p != '/' && *p != '?') p++;
    int hl = (int)(p - h);
    if (hl <= 0 || hl >= (int)sizeof(out->host)) return false;
    for (int i = 0; i < hl; i++) {
        const char c = h[i];
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_';
        if (!ok) return false;     // "not a url", "[::1]", ...
    }
    memcpy(out->host, h, (size_t)hl);
    out->host[hl] = '\0';
    // No port means 8096, not 80 -- the same default http.cpp and stream.cpp
    // apply to g_server, so a server entered without a port downloads from
    // the same place it browses and streams from.
    out->port = 8096;
    if (*p == ':') {
        p++;
        long port = 0;
        int digits = 0;
        while (*p >= '0' && *p <= '9') {
            port = port * 10 + (*p - '0');
            if (++digits > 5) return false;
            p++;
        }
        if (digits == 0 || port < 1 || port > 65535) return false;
        out->port = (int)port;
    }
    if (*p && *p != '/' && *p != '?') return false;
    const char *path = *p ? p : "/";
    // "?a=b" with no path still needs a leading '/'
    int need = (int)strlen(path) + (path[0] == '?' ? 1 : 0);
    if (need >= (int)sizeof(out->path)) return false;
    snprintf(out->path, sizeof(out->path), "%s%s",
             path[0] == '?' ? "/" : "", path);
    return true;
}

int dl_http_build_get(char *out, int cap, const DlUrl *u, uint64_t range_from,
                      const char *auth_header) {
    char range[48] = "";
    if (range_from > 0)
        snprintf(range, sizeof(range), "Range: bytes=%llu-\r\n",
                 (unsigned long long)range_from);
    int n = snprintf(out, (size_t)cap,
        "GET %s HTTP/1.1\r\n"
        "Host: %s:%d\r\n"
        "%s%s"
        "%s"
        "Accept: */*\r\n"
        "Accept-Encoding: identity\r\n"
        "User-Agent: " HTTP_USER_AGENT_DL "\r\n"
        "Connection: close\r\n"
        "\r\n",
        u->path, u->host, u->port,
        auth_header ? auth_header : "", auth_header ? "\r\n" : "",
        range);
    return (n < 0 || n >= cap) ? -1 : n;
}

// -------------------------------------------------------------------------
// Response head
// -------------------------------------------------------------------------

static bool ci_prefix(const char *s, int len, const char *lower) {
    int i = 0;
    for (; lower[i]; i++) {
        if (i >= len) return false;
        char c = s[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
        if (c != lower[i]) return false;
    }
    return true;
}

static bool ci_contains(const char *s, int len, const char *lower) {
    int nl = (int)strlen(lower);
    for (int i = 0; i + nl <= len; i++)
        if (ci_prefix(s + i, len - i, lower)) return true;
    return false;
}

// Strict decimal over [s, s+len), spaces around allowed.
static bool dec_u64(const char *s, int len, uint64_t *out) {
    int i = 0;
    while (i < len && (s[i] == ' ' || s[i] == '\t')) i++;
    int start = i;
    uint64_t v = 0;
    while (i < len && s[i] >= '0' && s[i] <= '9') {
        uint64_t d = (uint64_t)(s[i] - '0');
        if (v > (UINT64_MAX - d) / 10) return false;
        v = v * 10 + d;
        i++;
    }
    if (i == start) return false;
    while (i < len && (s[i] == ' ' || s[i] == '\t')) i++;
    if (i != len) return false;
    *out = v;
    return true;
}

// "bytes 100-199/1000", "bytes 100-199/*", "bytes */1000"
static bool parse_content_range(const char *v, int len, DlHttpHead *h) {
    while (len > 0 && (*v == ' ' || *v == '\t')) { v++; len--; }
    while (len > 0 && (v[len-1] == ' ' || v[len-1] == '\t')) len--;
    if (!ci_prefix(v, len, "bytes ")) return false;
    v += 6; len -= 6;
    const char *slash = (const char *)memchr(v, '/', (size_t)len);
    if (!slash) return false;
    int span_len  = (int)(slash - v);
    const char *t = slash + 1;
    int tlen      = len - span_len - 1;
    uint64_t total = 0;
    if (tlen == 1 && t[0] == '*') h->range_total = -1;
    else if (dec_u64(t, tlen, &total) && total <= (uint64_t)INT64_MAX)
        h->range_total = (int64_t)total;
    else return false;
    if (span_len == 1 && v[0] == '*') {
        if (h->range_total < 0) return false;   // "*/*" says nothing
        h->range_unsatisfied = true;
        h->has_range = true;
        return true;
    }
    const char *dash = (const char *)memchr(v, '-', (size_t)span_len);
    if (!dash) return false;
    uint64_t a, b;
    if (!dec_u64(v, (int)(dash - v), &a)) return false;
    if (!dec_u64(dash + 1, span_len - (int)(dash - v) - 1, &b)) return false;
    if (b < a) return false;
    if (h->range_total >= 0 && b >= (uint64_t)h->range_total) return false;
    h->range_start = a;
    h->range_end   = b;
    h->has_range   = true;
    return true;
}

bool dl_http_parse_head(const char *text, int len, DlHttpHead *h) {
    memset(h, 0, sizeof(*h));
    h->content_length = -1;
    h->range_total    = -1;

    // Status line: HTTP/1.x SSS reason
    if (len < 12 || strncmp(text, "HTTP/1.", 7) != 0) return false;
    const char *sp = (const char *)memchr(text, ' ', (size_t)len);
    if (!sp || sp + 4 > text + len) return false;
    int status = 0;
    for (int i = 1; i <= 3; i++) {
        char c = sp[i];
        if (c < '0' || c > '9') return false;
        status = status * 10 + (c - '0');
    }
    if (sp + 4 < text + len && sp[4] != ' ' && sp[4] != '\r' && sp[4] != '\n')
        return false;
    h->status = status;

    const char *p   = (const char *)memchr(text, '\n', (size_t)len);
    const char *end = text + len;
    if (!p) return true;   // status line only
    p++;
    while (p < end) {
        const char *ls = p;
        const char *le = (const char *)memchr(p, '\n', (size_t)(end - p));
        if (!le) le = end;
        p = (le < end) ? le + 1 : end;
        if (le > ls && le[-1] == '\r') le--;
        int ll = (int)(le - ls);
        if (ll == 0) break;   // blank line: end of head
        const char *colon = (const char *)memchr(ls, ':', (size_t)ll);
        if (!colon) continue;  // not a header; tolerate
        int nl = (int)(colon - ls);
        const char *v = colon + 1;
        int vl = ll - nl - 1;

        if (nl == 14 && ci_prefix(ls, nl, "content-length")) {
            uint64_t cl;
            if (!dec_u64(v, vl, &cl) || cl > (uint64_t)INT64_MAX) return false;
            // Two different lengths is a response that cannot be trusted
            // (request smuggling territory); refuse rather than pick one.
            if (h->content_length >= 0 && (uint64_t)h->content_length != cl)
                return false;
            h->content_length = (int64_t)cl;
        } else if (nl == 17 && ci_prefix(ls, nl, "transfer-encoding")) {
            if (ci_contains(v, vl, "chunked")) h->chunked = true;
        } else if (nl == 13 && ci_prefix(ls, nl, "accept-ranges")) {
            if (ci_contains(v, vl, "bytes")) h->accept_ranges = true;
        } else if (nl == 13 && ci_prefix(ls, nl, "content-range")) {
            if (!parse_content_range(v, vl, h)) return false;
        } else if (nl == 12 && ci_prefix(ls, nl, "content-type")) {
            while (vl > 0 && (*v == ' ' || *v == '\t')) { v++; vl--; }
            int cn = vl < (int)sizeof(h->content_type) - 1
                         ? vl : (int)sizeof(h->content_type) - 1;
            for (int i = 0; i < cn; i++) {
                char c = v[i];
                if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
                h->content_type[i] = c;
            }
            h->content_type[cn] = '\0';
        }
    }
    // RFC 7230 3.3.3: with chunked framing, Content-Length is ignored.
    if (h->chunked) h->content_length = -1;
    return true;
}

void dl_head_reader_init(DlHeadReader *r) {
    r->n = 0;
    r->done = false;
    r->buf[0] = '\0';
}

int dl_head_reader_feed(DlHeadReader *r, const uint8_t *data, int len) {
    int i = 0;
    while (i < len && !r->done) {
        if (r->n >= DL_HEAD_MAX - 1) return -1;
        r->buf[r->n++] = (char)data[i++];
        if (r->n >= 4 && memcmp(r->buf + r->n - 4, "\r\n\r\n", 4) == 0)
            r->done = true;
    }
    r->buf[r->n] = '\0';
    return i;
}

// -------------------------------------------------------------------------
// Chunked decoding
// -------------------------------------------------------------------------

enum {
    CH_SIZE,       // reading hex digits of the size line
    CH_EXT,        // skipping ";ext" up to CR/LF
    CH_SIZE_LF,    // saw CR after the size line
    CH_DATA,       // payload
    CH_DATA_CR,    // expecting CR after payload
    CH_DATA_LF,    // expecting LF after payload
    CH_TRAILER,    // after the 0 chunk: skipping trailers to the blank line
    CH_DONE,
};

void dl_chunked_init(DlChunked *c) {
    memset(c, 0, sizeof(*c));
    c->st = CH_SIZE;
}

static int hexval(uint8_t ch) {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

int dl_chunked_decode(DlChunked *c, const uint8_t *in, int len, uint8_t *out) {
    if (c->error) return -1;
    int i = 0, o = 0;
    while (i < len && c->st != CH_DONE) {
        uint8_t ch = in[i];
        switch (c->st) {
        case CH_SIZE: {
            int hv = hexval(ch);
            if (hv >= 0) {
                if (c->line_n >= 15) { c->error = true; return -1; }  // > 60 bits
                c->remain = (c->remain << 4) | (uint64_t)hv;
                c->line_n++;
                i++;
            } else if (c->line_n == 0) {
                c->error = true; return -1;        // size line with no digits
            } else if (ch == ';' || ch == ' ' || ch == '\t') {
                c->st = CH_EXT; i++;
            } else if (ch == '\r') {
                c->st = CH_SIZE_LF; i++;
            } else if (ch == '\n') {               // bare LF: tolerated
                i++;
                c->st = c->remain ? CH_DATA : CH_TRAILER;
                c->line_n = 0;
            } else {
                c->error = true; return -1;
            }
            break;
        }
        case CH_EXT:
            if (ch == '\r') c->st = CH_SIZE_LF;
            else if (ch == '\n') { c->st = c->remain ? CH_DATA : CH_TRAILER; c->line_n = 0; }
            i++;
            break;
        case CH_SIZE_LF:
            if (ch != '\n') { c->error = true; return -1; }
            i++;
            c->st = c->remain ? CH_DATA : CH_TRAILER;
            c->line_n = 0;
            break;
        case CH_DATA: {
            uint64_t avail = (uint64_t)(len - i);
            int n = (int)(c->remain < avail ? c->remain : avail);
            if (out + o != in + i) memmove(out + o, in + i, (size_t)n);
            o += n; i += n;
            c->remain -= (uint64_t)n;
            if (c->remain == 0) c->st = CH_DATA_CR;
            break;
        }
        case CH_DATA_CR:
            if (ch == '\r') { c->st = CH_DATA_LF; i++; }
            else if (ch == '\n') { c->st = CH_SIZE; i++; }   // bare LF
            else { c->error = true; return -1; }
            break;
        case CH_DATA_LF:
            if (ch != '\n') { c->error = true; return -1; }
            c->st = CH_SIZE; i++;
            break;
        case CH_TRAILER:
            // line_n counts characters on the current trailer line; an
            // empty line (only CRLF) ends the message.
            if (ch == '\n') {
                if (c->line_n == 0) { c->st = CH_DONE; c->done = true; }
                c->line_n = 0;
            } else if (ch != '\r') {
                c->line_n++;
            }
            i++;
            break;
        }
    }
    return o;
}

bool dl_http_content_type_is_error_page(const char *ct) {
    return strncmp(ct, "text/", 5) == 0 ||
           strncmp(ct, "application/json", 16) == 0 ||
           strncmp(ct, "application/problem+json", 24) == 0;
}

bool dl_url_query_get(const char *url, const char *key, char *out, int cap) {
    const char *q = strchr(url, '?');
    if (!q || cap <= 0) return false;
    const size_t kl = strlen(key);
    for (const char *p = q + 1; *p; ) {
        const char *amp = strchr(p, '&');
        const char *end = amp ? amp : p + strlen(p);
        if ((size_t)(end - p) > kl && strncmp(p, key, kl) == 0 && p[kl] == '=') {
            const char *v = p + kl + 1;
            int vl = (int)(end - v);
            if (vl >= cap) return false;
            memcpy(out, v, (size_t)vl);
            out[vl] = '\0';
            return true;
        }
        if (!amp) break;
        p = amp + 1;
    }
    return false;
}

static bool query_u64(const char *url, const char *key, uint64_t *out) {
    char v[24];
    return dl_url_query_get(url, key, v, sizeof(v)) &&
           dec_u64(v, (int)strlen(v), out);
}

bool dl_stream_is_light(const char *stream_url) {
    if (!stream_url) return false;
    uint64_t h, vb, ab;
    if (!query_u64(stream_url, "MaxHeight", &h) || h == 0 ||
        h > DL_LIGHT_MAX_HEIGHT)
        return false;
    if (!query_u64(stream_url, "VideoBitrate", &vb) || vb == 0 ||
        vb > DL_LIGHT_MAX_VIDEO_BPS)
        return false;   // no ceiling = direct play at the source's bitrate
    char copy[8];
    if (dl_url_query_get(stream_url, "AllowAudioStreamCopy", copy, sizeof(copy)) &&
        strcmp(copy, "false") != 0)
        return false;   // HD audio copied through untouched
    if (query_u64(stream_url, "AudioBitrate", &ab) && ab > DL_LIGHT_MAX_AUDIO_BPS)
        return false;
    return true;
}
