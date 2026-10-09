// HTTPS for the Jellyfin API.  See https_client.h.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <ppu-types.h>
#include <sys/mutex.h>
#include <sysmodule/sysmodule.h>
#include <http/https.h>
#include <http/util.h>
#include <ssl/ssl.h>

#include "https_client.h"
#include "http.h"
#include "plog.h"
#include "jf_paths.h"

#define HTTPS_PIN_FILE "jellyfin_tls_pin.txt"

// --- runtime -----------------------------------------------------------------
static sys_mutex_t s_rt_mtx;
static bool        s_rt_mtx_ok = false;
static int         s_rt_refs   = 0;
static void       *s_http_pool = NULL, *s_ssl_pool = NULL, *s_cert_buf = NULL;
static bool        s_mod_http, s_mod_util, s_mod_https, s_mod_ssl;
static bool        s_init_http, s_init_ssl, s_init_https;

void https_rt_init(void) {
    sys_mutex_attr_t attr;
    memset(&attr, 0, sizeof attr);
    attr.attr_protocol  = SYS_MUTEX_PROTOCOL_FIFO;
    attr.attr_recursive = SYS_MUTEX_ATTR_NOT_RECURSIVE;
    s_rt_mtx_ok = (sysMutexCreate(&s_rt_mtx, &attr) == 0);
}

static void rt_teardown(void) {
    if (s_init_https) { httpsEnd(); s_init_https = false; }
    if (s_init_ssl)   { sslEnd();   s_init_ssl   = false; }
    if (s_init_http)  { httpEnd();  s_init_http  = false; }
    if (s_mod_ssl)    { sysModuleUnload(SYSMODULE_SSL);       s_mod_ssl   = false; }
    if (s_mod_https)  { sysModuleUnload(SYSMODULE_HTTPS);     s_mod_https = false; }
    if (s_mod_util)   { sysModuleUnload(SYSMODULE_HTTP_UTIL); s_mod_util  = false; }
    if (s_mod_http)   { sysModuleUnload(SYSMODULE_HTTP);      s_mod_http  = false; }
    free(s_cert_buf);  s_cert_buf  = NULL;
    free(s_ssl_pool);  s_ssl_pool  = NULL;
    free(s_http_pool); s_http_pool = NULL;
}

static void rt_fail(const char *what, s32 rc) {
    char b[96];
    snprintf(b, sizeof b, "https: FAIL %s: 0x%08x", what, (u32)rc);
    plog(b);
    rt_teardown();
}

static bool rt_bring_up(void) {
    s32 rc;
    if ((rc = sysModuleLoad(SYSMODULE_HTTP)) < 0)      { rt_fail("load HTTP", rc); return false; }
    s_mod_http = true;
    if ((rc = sysModuleLoad(SYSMODULE_HTTP_UTIL)) < 0) { rt_fail("load HTTP_UTIL", rc); return false; }
    s_mod_util = true;
    if ((rc = sysModuleLoad(SYSMODULE_HTTPS)) < 0)     { rt_fail("load HTTPS", rc); return false; }
    s_mod_https = true;
    if ((rc = sysModuleLoad(SYSMODULE_SSL)) < 0)       { rt_fail("load SSL", rc); return false; }
    s_mod_ssl = true;

    s_http_pool = malloc(0x10000);
    if (!s_http_pool)                                  { rt_fail("http pool", -1); return false; }
    if ((rc = httpInit(s_http_pool, 0x10000)) < 0)     { rt_fail("httpInit", rc); return false; }
    s_init_http = true;

    s_ssl_pool = malloc(0x40000);
    if (!s_ssl_pool)                                   { rt_fail("ssl pool", -1); return false; }
    if ((rc = sslInit(s_ssl_pool, 0x40000)) < 0)       { rt_fail("sslInit", rc); return false; }
    s_init_ssl = true;

    u32 cert_size = 0;
    if ((rc = sslCertificateLoader(SSL_LOAD_CERT_ALL, NULL, 0, &cert_size)) < 0) { rt_fail("cert size", rc); return false; }
    s_cert_buf = malloc(cert_size);
    if (!s_cert_buf)                                   { rt_fail("cert buffer", -1); return false; }
    if ((rc = sslCertificateLoader(SSL_LOAD_CERT_ALL, (char *)s_cert_buf, cert_size, NULL)) < 0) { rt_fail("cert load", rc); return false; }
    httpsData ca;
    ca.ptr  = (char *)s_cert_buf;
    ca.size = cert_size;
    if ((rc = httpsInit(1, &ca)) < 0)                  { rt_fail("httpsInit", rc); return false; }
    s_init_https = true;
    return true;
}

bool https_rt_acquire(void) {
    if (!s_rt_mtx_ok) return false;
    sysMutexLock(s_rt_mtx, 0);
    bool ok = true;
    if (s_rt_refs == 0) ok = rt_bring_up();
    if (ok) s_rt_refs++;
    sysMutexUnlock(s_rt_mtx);
    return ok;
}

void https_rt_release(void) {
    if (!s_rt_mtx_ok) return;
    sysMutexLock(s_rt_mtx, 0);
    if (s_rt_refs > 0 && --s_rt_refs == 0) rt_teardown();
    sysMutexUnlock(s_rt_mtx);
}

// --- certificate trust -------------------------------------------------------
static void hex16(const uint8_t *m, char *o) {
    for (int i = 0; i < 16; i++) snprintf(o + 2 * i, 3, "%02x", m[i]);
}

// verErr != 0 means the firmware could not verify the chain (an old CA store,
// or a self-signed certificate).  Pin the leaf on first use; refuse a change.
static int ssl_trust_cb(s32 verErr, sslCert const certs[], int certNum,
                        const char *host, httpSslId id, void *arg) {
    (void)id; (void)arg;
    char b[160];
    if (verErr == 0) return 0;
    if (certNum < 1 || !certs || !host) return -1;

    uint8_t md5[16];
    uint32_t ml = sizeof md5;
    if (sslCertGetMd5Fingerprint(certs[0], md5, &ml) < 0 || ml != 16) {
        plog("https: cannot fingerprint the server certificate -- refused");
        return -1;
    }
    char fp[33];
    hex16(md5, fp);

    char found[40] = "";
    FILE *f = fopen(jf_data_path(HTTPS_PIN_FILE), "r");
    if (f) {
        char h[256], p[40];
        while (fscanf(f, "%255s %39s", h, p) == 2)
            if (strcasecmp(h, host) == 0) { snprintf(found, sizeof found, "%s", p); break; }
        fclose(f);
    }
    if (found[0]) {
        if (strcasecmp(found, fp) == 0) return 0;
        snprintf(b, sizeof b, "https: certificate for %.60s CHANGED -- refused (delete %s to re-pin)",
                 host, HTTPS_PIN_FILE);
        plog(b);
        return -1;
    }
    f = fopen(jf_data_path(HTTPS_PIN_FILE), "a");
    if (!f) { plog("https: cannot write the pin file -- refused"); return -1; }
    fprintf(f, "%s %s\n", host, fp);
    fclose(f);
    snprintf(b, sizeof b, "https: unverified certificate (0x%08x) for %.60s pinned on first use",
             (u32)verErr, host);
    plog(b);
    return 0;
}

// --- client ------------------------------------------------------------------
static httpClientId s_client  = 0;
static bool         s_have_rt = false;     // one runtime reference, held for the session

static bool client_ready(int timeout_secs) {
    if (!s_have_rt) {
        if (!https_rt_acquire()) return false;
        s_have_rt = true;
    }
    if (!s_client) {
        if (httpCreateClient(&s_client) < 0) {
            s_client = 0;
            plog("https: httpCreateClient failed");
            return false;
        }
        httpClientSetUserAgent(s_client, HTTP_USER_AGENT);
        httpClientSetAutoRedirect(s_client, 1);
        httpClientSetSslCallback(s_client, ssl_trust_cb, NULL);
    }
    httpClientSetConnTimeout(s_client, 8 * 1000000);
    httpClientSetSendTimeout(s_client, 8 * 1000000);
    httpClientSetRecvTimeout(s_client, (s64)timeout_secs * 1000000);
    return true;
}

bool https_url(const char *url) { return url && strncmp(url, "https://", 8) == 0; }

int https_request(const char *method, const char *url, const char *body,
                  const char *ctype, const char *auth, const char *accept,
                  char *out, int out_size, int *body_len, int timeout_secs) {
    if (body_len) *body_len = 0;
    if (out && out_size > 0) out[0] = '\0';
    if (!client_ready(timeout_secs)) return -1;

    void       *uri_pool = NULL;
    httpTransId trans    = 0;
    int         status   = -1;
    httpUri     uri;
    u32         pool_size = 0;

    if (httpUtilParseUri(&uri, url, NULL, 0, &pool_size) < 0) goto done;
    uri_pool = malloc(pool_size);
    if (!uri_pool || httpUtilParseUri(&uri, url, uri_pool, pool_size, NULL) < 0) goto done;
    if (httpCreateTransaction(&trans, s_client, method, &uri) < 0) { trans = 0; goto done; }

    {
        httpHeader h;
        h.name = "Authorization";   h.value = auth;       httpRequestAddHeader(trans, &h);
        h.name = "Accept";          h.value = accept;     httpRequestAddHeader(trans, &h);
        h.name = "Accept-Encoding"; h.value = "identity"; httpRequestAddHeader(trans, &h);
        if (body) {
            h.name = "Content-Type"; h.value = ctype ? ctype : "application/json";
            httpRequestAddHeader(trans, &h);
        }
        const u32 blen = body ? (u32)strlen(body) : 0;
        if (blen) httpRequestSetContentLength(trans, blen);
        const s32 rc = httpSendRequest(trans, body, blen, NULL);
        if (rc < 0) {
            char b[160];
            snprintf(b, sizeof b, "https: request failed (dns/tcp/tls) 0x%08x %.80s", (u32)rc, url);
            plog(b);
            goto done;
        }
    }
    {
        s32 code = 0;
        if (httpResponseGetStatusCode(trans, &code) < 0) goto done;
        status = (int)code;
        int got = 0;
        for (;;) {
            u32  n = 0;
            char tmp[2048];
            const bool room_in_out = out && got < out_size - 1;
            char *dst  = room_in_out ? out + got : tmp;
            u32   room = room_in_out ? (u32)(out_size - 1 - got) : (u32)sizeof tmp;
            if (httpRecvResponse(trans, dst, room, &n) != 0 || n == 0) break;
            got += (int)n;
        }
        if (out && out_size > 0) out[got < out_size - 1 ? got : out_size - 1] = '\0';
        if (body_len) *body_len = got;
    }
done:
    if (trans)    httpDestroyTransaction(trans);
    if (uri_pool) free(uri_pool);
    return status;
}
