// Background GitHub release check — fetches the latest release tag once at
// startup and compares it against APP_VERSION.
//
// The GitHub API is HTTPS-only, which net/http.cpp (raw sockets) can't speak,
// so this module uses the firmware HTTPS stack (libhttp/libssl — already
// linked for the app).  The whole exchange runs on a low-priority thread with
// 2-second connect/send/recv timeouts; any failure just leaves the "no update"
// default in place, so the UI never sees an error from this path.
//
// Every step is traced to UPDATE_LOG_PATH (independent of the Debug Logging
// setting) so a silent failure on real hardware can be located after the
// fact — including a dump of whatever the API returned.

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <strings.h>

#include <ppu-types.h>
#include <sys/thread.h>
#include <sysmodule/sysmodule.h>
#include <http/https.h>
#include <http/util.h>
#include <ssl/ssl.h>

#include "update_check.h"
#include "http.h"    // HTTP_USER_AGENT (GitHub rejects requests without one)
#include "timing.h"
#include "plog.h"

#define UPDATE_URL "https://api.github.com/repos/Tuggs-Bunny/JellyFin-PS3/releases/latest"
#define UPDATE_TIMEOUT_US (2 * 1000 * 1000)

#define UPDATE_LOG_PATH "/dev_hdd0/tmp/update_detection.txt"

static sys_ppu_thread_t s_thread  = 0;
static bool             s_started = false;

// Results — written by the worker, then s_done is set last (after a barrier)
// so readers polling update_check_result() only ever see a complete result.
static volatile bool s_done  = false;
static bool          s_newer = false;
static char          s_latest[64];

// The tag_name field sits near the front of the response; 16K is plenty even
// with long release notes before it.
static char s_resp[16 * 1024];

// -------------------------------------------------------
// Trace log — always on, unlike plog.  Same open/write/close-per-line
// pattern as crash_log() so the last line survives a hang; first call
// truncates, every call after that appends.  Lines carry the ms elapsed
// since the check started, which makes timeout stalls obvious.
// -------------------------------------------------------

static bool s_log_started = false;
static u64  s_log_t0      = 0;

static void ulog(const char *fmt, ...) {
    FILE *f = fopen(UPDATE_LOG_PATH, s_log_started ? "a" : "w");
    if (!f) return;
    fprintf(f, "[+%5llums] ", (unsigned long long)((timing_get_us() - s_log_t0) / 1000));
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
    s_log_started = true;
}

// Raw dump (no printf formatting — the response body contains '%').
static void ulog_raw(const char *buf, u32 len) {
    FILE *f = fopen(UPDATE_LOG_PATH, s_log_started ? "a" : "w");
    if (!f) return;
    fwrite(buf, 1, len, f);
    fputc('\n', f);
    fclose(f);
    s_log_started = true;
}

// -------------------------------------------------------
// Version comparison
// -------------------------------------------------------

// Split "v2.0-beta" into up to three numeric fields (missing = 0) and the
// trailing suffix ("-beta", or "" for a plain release).
static void ver_split(const char *s, int num[3], const char **suffix) {
    if (*s == 'v' || *s == 'V') s++;
    num[0] = num[1] = num[2] = 0;
    int i = 0;
    while (i < 3 && *s >= '0' && *s <= '9') {
        int v = 0;
        while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0');
        num[i++] = v;
        if (*s != '.') break;
        s++;
    }
    *suffix = s;
}

// > 0 when a is newer than b.  Numeric fields decide first; on a tie a plain
// release outranks a pre-release ("1.0" > "1.0-beta"), and two pre-release
// suffixes fall back to name order ("-beta" > "-alpha").  A tag with no
// number at all ("Testing") parses as 0.0.0, i.e. never newer.
static int ver_cmp(const char *a, const char *b) {
    int na[3], nb[3];
    const char *sa, *sb;
    ver_split(a, na, &sa);
    ver_split(b, nb, &sb);
    for (int i = 0; i < 3; i++)
        if (na[i] != nb[i]) return na[i] - nb[i];
    if ((*sa == '\0') != (*sb == '\0')) return (*sa == '\0') ? 1 : -1;
    return strcasecmp(sa, sb);
}

// -------------------------------------------------------
// Fetch
// -------------------------------------------------------

// Extract a "key":"value" string field from a JSON buffer.  Enough for
// tag_name — release tags never contain escapes.
static bool json_str_field(const char *json, const char *key,
                           char *out, int out_size) {
    char pat[32];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(json, pat);
    if (!p) return false;
    p += strlen(pat);
    while (*p == ' ' || *p == ':') p++;
    if (*p != '"') return false;
    p++;
    int i = 0;
    while (*p && *p != '"' && *p != '\\' && i < out_size - 1)
        out[i++] = *p++;
    out[i] = '\0';
    return i > 0;
}

// The firmware CA bundle predates GitHub's current root certificates, so
// strict verification can fail even when the connection itself is fine.
// Nothing sensitive is sent and the result is display-only, so accept the
// certificate; transport errors still fail the check quietly.  Being called
// at all proves the TLS handshake itself got as far as the server's
// certificate — worth a trace line.
static int ssl_accept_cb(s32 verErr, sslCert const certs[], int certNum,
                         const char *host, httpSslId id, void *arg) {
    (void)certs; (void)id; (void)arg;
    ulog("ssl cb: verErr=0x%08x certs=%d host=%s -> accepting",
         (u32)verErr, certNum, host ? host : "(null)");
    return 0;
}

// One GET of the latest-release JSON.  Mirrors the canonical PSL1GHT https
// flow (init pools -> firmware certs -> client -> transaction), with every
// step bailing to cleanup on failure.  netInitialize() has already run
// (http_init() in main), so only the HTTP/SSL layers are set up here.
static void do_check(void) {
    void *http_pool = NULL, *ssl_pool = NULL, *cert_buf = NULL, *uri_pool = NULL;
    bool  mod_http = false, mod_util = false, mod_https = false, mod_ssl = false;
    bool  init_http = false, init_ssl = false, init_https = false;
    httpClientId client = 0;
    httpTransId  trans  = 0;
    s32 rc;

    ulog("update check: app=%s", APP_VERSION);
    ulog("url: %s", UPDATE_URL);

    rc = sysModuleLoad(SYSMODULE_HTTP);
    if (rc < 0) { ulog("FAIL sysModuleLoad(HTTP): 0x%08x", (u32)rc); goto cleanup; }
    mod_http = true;
    rc = sysModuleLoad(SYSMODULE_HTTP_UTIL);
    if (rc < 0) { ulog("FAIL sysModuleLoad(HTTP_UTIL): 0x%08x", (u32)rc); goto cleanup; }
    mod_util = true;
    rc = sysModuleLoad(SYSMODULE_HTTPS);
    if (rc < 0) { ulog("FAIL sysModuleLoad(HTTPS): 0x%08x", (u32)rc); goto cleanup; }
    mod_https = true;
    rc = sysModuleLoad(SYSMODULE_SSL);
    if (rc < 0) { ulog("FAIL sysModuleLoad(SSL): 0x%08x", (u32)rc); goto cleanup; }
    mod_ssl = true;
    ulog("modules loaded");

    http_pool = malloc(0x10000);
    if (!http_pool) { ulog("FAIL http pool alloc"); goto cleanup; }
    rc = httpInit(http_pool, 0x10000);
    if (rc < 0) { ulog("FAIL httpInit: 0x%08x", (u32)rc); goto cleanup; }
    init_http = true;

    ssl_pool = malloc(0x40000);
    if (!ssl_pool) { ulog("FAIL ssl pool alloc"); goto cleanup; }
    rc = sslInit(ssl_pool, 0x40000);
    if (rc < 0) { ulog("FAIL sslInit: 0x%08x", (u32)rc); goto cleanup; }
    init_ssl = true;

    {
        u32 cert_size = 0;
        rc = sslCertificateLoader(SSL_LOAD_CERT_ALL, NULL, 0, &cert_size);
        if (rc < 0) { ulog("FAIL cert size query: 0x%08x", (u32)rc); goto cleanup; }
        cert_buf = malloc(cert_size);
        if (!cert_buf) { ulog("FAIL cert buf alloc (%u bytes)", cert_size); goto cleanup; }
        rc = sslCertificateLoader(SSL_LOAD_CERT_ALL, (char*)cert_buf,
                                  cert_size, NULL);
        if (rc < 0) { ulog("FAIL cert load: 0x%08x", (u32)rc); goto cleanup; }
        ulog("firmware certs loaded (%u bytes)", cert_size);

        httpsData ca;
        ca.ptr  = (char*)cert_buf;
        ca.size = cert_size;
        rc = httpsInit(1, &ca);
        if (rc < 0) { ulog("FAIL httpsInit: 0x%08x", (u32)rc); goto cleanup; }
        init_https = true;
    }

    rc = httpCreateClient(&client);
    if (rc < 0) { ulog("FAIL httpCreateClient: 0x%08x", (u32)rc); client = 0; goto cleanup; }
    httpClientSetConnTimeout(client, UPDATE_TIMEOUT_US);
    httpClientSetSendTimeout(client, UPDATE_TIMEOUT_US);
    httpClientSetRecvTimeout(client, UPDATE_TIMEOUT_US);
    httpClientSetUserAgent(client, HTTP_USER_AGENT);
    httpClientSetAutoRedirect(client, 1);   // repo renames answer with a 301
    httpClientSetSslCallback(client, ssl_accept_cb, NULL);
    ulog("client ready (timeouts %dms)", UPDATE_TIMEOUT_US / 1000);

    {
        httpUri uri;
        u32 pool_size = 0;
        rc = httpUtilParseUri(&uri, UPDATE_URL, NULL, 0, &pool_size);
        if (rc < 0) { ulog("FAIL uri size query: 0x%08x", (u32)rc); goto cleanup; }
        uri_pool = malloc(pool_size);
        if (!uri_pool) { ulog("FAIL uri pool alloc"); goto cleanup; }
        rc = httpUtilParseUri(&uri, UPDATE_URL, uri_pool, pool_size, NULL);
        if (rc < 0) { ulog("FAIL uri parse: 0x%08x", (u32)rc); goto cleanup; }
        rc = httpCreateTransaction(&trans, client, HTTP_METHOD_GET, &uri);
        if (rc < 0) { ulog("FAIL httpCreateTransaction: 0x%08x", (u32)rc); trans = 0; goto cleanup; }
    }

    // DNS, TCP connect, and the TLS handshake all happen inside this call —
    // if the console's SSL stack can't talk to GitHub, this is where it dies.
    rc = httpSendRequest(trans, NULL, 0, NULL);
    if (rc < 0) { ulog("FAIL httpSendRequest (dns/tcp/tls): 0x%08x", (u32)rc); goto cleanup; }
    ulog("request sent");

    {
        s32 code = 0;
        rc = httpResponseGetStatusCode(trans, &code);
        if (rc < 0) { ulog("FAIL get status code: 0x%08x", (u32)rc); goto cleanup; }
        ulog("http status: %d", (int)code);
        if (code != 200) {
            // Still capture the body — GitHub explains 403/404/redirect
            // failures in JSON, and that explanation is the diagnosis.
            u32 got = 0, n = 0;
            while (got < sizeof(s_resp) - 1 &&
                   httpRecvResponse(trans, s_resp + got,
                                    sizeof(s_resp) - 1 - got, &n) == 0 && n > 0)
                got += n;
            s_resp[got] = '\0';
            ulog("---- response body (%u bytes) ----", got);
            if (got) ulog_raw(s_resp, got);
            ulog("---- end ----");
            goto cleanup;
        }
    }

    {
        u32 got = 0;
        while (got < sizeof(s_resp) - 1) {
            u32 n = 0;
            rc = httpRecvResponse(trans, s_resp + got,
                                  sizeof(s_resp) - 1 - got, &n);
            if (rc != 0) { ulog("recv stopped: 0x%08x after %u bytes", (u32)rc, got); break; }
            if (n == 0) break;
            got += n;
        }
        s_resp[got] = '\0';
        ulog("---- response body (%u bytes) ----", got);
        if (got) ulog_raw(s_resp, got);
        ulog("---- end ----");
    }

    {
        char tag[64];
        if (!json_str_field(s_resp, "tag_name", tag, sizeof(tag))) {
            ulog("FAIL no tag_name in response");
            goto cleanup;
        }
        int cmp = ver_cmp(tag, APP_VERSION);
        ulog("tag=%s app=%s cmp=%d -> %s", tag, APP_VERSION, cmp,
             cmp > 0 ? "UPDATE AVAILABLE" : "up to date");
        if (cmp > 0) {
            snprintf(s_latest, sizeof(s_latest), "%s", tag);
            s_newer = true;
        }
        char msg[96];
        snprintf(msg, sizeof(msg), "update: latest=%s newer=%d",
                 s_newer ? s_latest : "(none)", (int)s_newer);
        plog(msg);
    }

cleanup:
    if (trans)      httpDestroyTransaction(trans);
    if (client)     httpDestroyClient(client);
    if (init_https) httpsEnd();
    if (init_ssl)   sslEnd();
    if (init_http)  httpEnd();
    if (mod_ssl)    sysModuleUnload(SYSMODULE_SSL);
    if (mod_https)  sysModuleUnload(SYSMODULE_HTTPS);
    if (mod_util)   sysModuleUnload(SYSMODULE_HTTP_UTIL);
    if (mod_http)   sysModuleUnload(SYSMODULE_HTTP);
    if (uri_pool)   free(uri_pool);
    if (cert_buf)   free(cert_buf);
    if (ssl_pool)   free(ssl_pool);
    if (http_pool)  free(http_pool);
    ulog("done (newer=%d)", (int)s_newer);
}

static void check_thread_fn(void *arg) {
    (void)arg;
    do_check();
    __sync_synchronize();   // result fields land before the done flag
    s_done = true;
    sysThreadExit(0);
}

void update_check_start(void) {
    if (s_started) return;
    s_latest[0] = '\0';
    s_log_t0 = timing_get_us();
    s32 rc = sysThreadCreate(&s_thread, check_thread_fn, NULL,
                             1500, 65536, 0, "upd_check");
    if (rc == 0)
        s_started = true;
    else
        ulog("FAIL sysThreadCreate: 0x%08x", (u32)rc);
}

void update_check_shutdown(void) {
    if (!s_started) return;
    u64 tret;
    sysThreadJoin(s_thread, &tret);
    s_started = false;
    s_thread  = 0;
}

bool update_check_result(char *out, int out_size) {
    if (!s_done || !s_newer) return false;
    snprintf(out, out_size, "%s", s_latest);
    return true;
}
