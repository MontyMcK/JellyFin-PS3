#include <http/https.h>
#include <ssl/ssl.h>
#include <net/net.h>
#include <sysmodule/sysmodule.h>
#include <sysutil/sysutil.h>
#include <rsx/rsx.h>
#include <rsx/gcm_sys.h>
#include <ppu-lv2.h>
#include <io/pad.h>
#include <debugcons.h>

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

// ---- CONFIGURATION ----
#define JELLYFIN_SERVER  "http://192.168.1.2:8096"
#define JELLYFIN_TOKEN   "[REVOKED_JELLYFIN_TOKEN]"
#define HTTP_USER_AGENT  "JellyfinPS3/0.1"
#define BUFFER_SIZE      (128*1024)
// -----------------------

#define HTTP_YES      1
#define HTTP_NO       0
#define HTTP_SUCCESS  1
#define HTTP_FAILED   0

#define HOST_SIZE  0x100000
#define CB_SIZE    0x100

typedef struct {
    void*       http_pool;
    void*       ssl_pool;
    httpsData*  caList;
    void*       cert_buffer;
} t_http_pools;

static t_http_pools http_pools;
static char responseBuffer[BUFFER_SIZE];

// RSX context
static gcmContextData *context;
static void *host_addr = NULL;

// -------------------------------------------------------
// Screen setup
// -------------------------------------------------------

void init_screen()
{
    host_addr = memalign(1024*1024, HOST_SIZE);
    context = initScreen(host_addr, HOST_SIZE);
    setResolution(context, 1280, 720);
}

void flip()
{
    gcmSetFlip(context, 0);
    rsxFlushBuffer(context);
    gcmSetWaitFlip(context);
}

// -------------------------------------------------------
// http_init / http_end
// -------------------------------------------------------

int http_init(void)
{
    int ret;
    u32 cert_size = 0;
    u8 module_https_loaded = HTTP_NO, module_http_loaded = HTTP_NO;
    u8 module_net_loaded = HTTP_NO,  module_ssl_loaded = HTTP_NO;
    u8 https_init = HTTP_NO, http_init_done = HTTP_NO;
    u8 net_init = HTTP_NO,   ssl_init = HTTP_NO;

    ret = sysModuleLoad(SYSMODULE_NET);
    if (ret < 0) { debugPrintf("sysModuleLoad NET failed (%x)\n", ret); ret = HTTP_FAILED; goto end; }
    module_net_loaded = HTTP_YES;

    ret = netInitialize();
    if (ret < 0) { debugPrintf("netInitialize failed (%x)\n", ret); ret = HTTP_FAILED; goto end; }
    net_init = HTTP_YES;

    ret = sysModuleLoad(SYSMODULE_HTTP);
    if (ret < 0) { debugPrintf("sysModuleLoad HTTP failed (%x)\n", ret); ret = HTTP_FAILED; goto end; }
    module_http_loaded = HTTP_YES;

    http_pools.http_pool = malloc(0x10000);
    if (!http_pools.http_pool) { debugPrintf("OOM: http_pool\n"); ret = HTTP_FAILED; goto end; }

    ret = httpInit(http_pools.http_pool, 0x10000);
    if (ret < 0) { debugPrintf("httpInit failed (%x)\n", ret); ret = HTTP_FAILED; goto end; }
    http_init_done = HTTP_YES;

    ret = sysModuleLoad(SYSMODULE_HTTPS);
    if (ret < 0) { debugPrintf("sysModuleLoad HTTPS failed (%x)\n", ret); ret = HTTP_FAILED; goto end; }
    module_https_loaded = HTTP_YES;

    ret = sysModuleLoad(SYSMODULE_SSL);
    if (ret < 0) { debugPrintf("sysModuleLoad SSL failed (%x)\n", ret); ret = HTTP_FAILED; goto end; }
    module_ssl_loaded = HTTP_YES;

    http_pools.ssl_pool = malloc(0x40000);
    if (!http_pools.ssl_pool) { debugPrintf("OOM: ssl_pool\n"); ret = HTTP_FAILED; goto end; }

    ret = sslInit(http_pools.ssl_pool, 0x40000);
    if (ret < 0) { debugPrintf("sslInit failed (%x)\n", ret); ret = HTTP_FAILED; goto end; }
    ssl_init = HTTP_YES;

    http_pools.caList = (httpsData*)malloc(sizeof(httpsData));
    ret = sslCertificateLoader(SSL_LOAD_CERT_ALL, NULL, 0, &cert_size);
    if (ret < 0) { debugPrintf("sslCertificateLoader failed (%x)\n", ret); ret = HTTP_FAILED; goto end; }

    http_pools.cert_buffer = malloc(cert_size);
    if (!http_pools.cert_buffer) { debugPrintf("OOM: cert_buffer\n"); ret = HTTP_FAILED; goto end; }

    ret = sslCertificateLoader(SSL_LOAD_CERT_ALL, http_pools.cert_buffer, cert_size, NULL);
    if (ret < 0) { debugPrintf("sslCertificateLoader(2) failed (%x)\n", ret); ret = HTTP_FAILED; goto end; }

    http_pools.caList[0].ptr  = http_pools.cert_buffer;
    http_pools.caList[0].size = cert_size;

    ret = httpsInit(1, http_pools.caList);
    if (ret < 0) { debugPrintf("httpsInit failed (%x)\n", ret); ret = HTTP_FAILED; goto end; }
    https_init = HTTP_YES;

    return HTTP_SUCCESS;

end:
    if (http_pools.caList)    free(http_pools.caList);
    if (https_init)           httpsEnd();
    if (ssl_init)             sslEnd();
    if (http_init_done)       httpEnd();
    if (net_init)             netDeinitialize();
    if (module_http_loaded)   sysModuleUnload(SYSMODULE_HTTP);
    if (module_https_loaded)  sysModuleUnload(SYSMODULE_HTTPS);
    if (module_ssl_loaded)    sysModuleUnload(SYSMODULE_SSL);
    if (module_net_loaded)    sysModuleUnload(SYSMODULE_NET);
    if (http_pools.http_pool) free(http_pools.http_pool);
    if (http_pools.ssl_pool)  free(http_pools.ssl_pool);
    if (http_pools.cert_buffer) free(http_pools.cert_buffer);
    return ret;
}

void http_end(void)
{
    if (http_pools.caList) free(http_pools.caList);
    httpsEnd(); sslEnd(); httpEnd(); netDeinitialize();
    sysModuleUnload(SYSMODULE_HTTP);
    sysModuleUnload(SYSMODULE_HTTPS);
    sysModuleUnload(SYSMODULE_SSL);
    sysModuleUnload(SYSMODULE_NET);
    if (http_pools.http_pool)   free(http_pools.http_pool);
    if (http_pools.ssl_pool)    free(http_pools.ssl_pool);
    if (http_pools.cert_buffer) free(http_pools.cert_buffer);
}

// -------------------------------------------------------
// jellyfin_get()
// -------------------------------------------------------

int jellyfin_get(const char* path, char* out, int out_size)
{
    int ret = 0, status_code = -1;
    httpClientId  client = 0;
    httpTransId   trans  = 0;
    httpUri       uri;
    void*         uri_pool = NULL;
    u32           uri_size = 0;
    u32           nRecv = 1;
    int           written = 0;

    char url[512];
    snprintf(url, sizeof(url), "%s%s", JELLYFIN_SERVER, path);

    char auth_header[256];
    snprintf(auth_header, sizeof(auth_header),
        "MediaBrowser Token=\"%s\", Client=\"PS3\", Device=\"PS3\", DeviceId=\"ps3-jellyfin\", Version=\"0.1\"",
        JELLYFIN_TOKEN);

    ret = httpCreateClient(&client);
    if (ret < 0) { debugPrintf("httpCreateClient failed (%x)\n", ret); goto end; }

    httpClientSetConnTimeout(client, 10 * 1000 * 1000);
    httpClientSetUserAgent(client, HTTP_USER_AGENT);
    httpClientSetAutoRedirect(client, 1);

    ret = httpUtilParseUri(&uri, url, NULL, 0, &uri_size);
    if (ret < 0) { debugPrintf("httpUtilParseUri(1) failed (%x)\n", ret); goto end; }

    uri_pool = malloc(uri_size);
    if (!uri_pool) { debugPrintf("OOM: uri_pool\n"); goto end; }

    ret = httpUtilParseUri(&uri, url, uri_pool, uri_size, 0);
    if (ret < 0) { debugPrintf("httpUtilParseUri(2) failed (%x)\n", ret); goto end; }

    ret = httpCreateTransaction(&trans, client, HTTP_METHOD_GET, &uri);
    if (ret < 0) { debugPrintf("httpCreateTransaction failed (%x)\n", ret); goto end; }

    httpHeader authHdr;
    authHdr.name = "X-Emby-Authorization";
    authHdr.value = auth_header;
    ret = httpRequestAddHeader(trans, &authHdr);
    if (ret < 0) { debugPrintf("addRequestHeader auth failed (%x)\n", ret); goto end; }

    httpHeader acceptHdr;
    acceptHdr.name = "Accept";
    acceptHdr.value = "application/json";
    ret = httpRequestAddHeader(trans, &acceptHdr);
    if (ret < 0) { debugPrintf("addRequestHeader accept failed (%x)\n", ret); goto end; }

    ret = httpSendRequest(trans, NULL, 0, NULL);
    if (ret < 0) { debugPrintf("httpSendRequest failed (%x)\n", ret); goto end; }

    ret = httpResponseGetStatusCode(trans, &status_code);
    if (ret < 0) { debugPrintf("getStatusCode failed (%x)\n", ret); status_code = -1; goto end; }

    debugPrintf("HTTP %d for %s\n", status_code, path);

    if (status_code >= 400) goto end;

    memset(out, 0, out_size);
    while (nRecv != 0 && written < out_size - 1) {
        char chunk[4096];
        if (httpRecvResponse(trans, chunk, sizeof(chunk) - 1, &nRecv) > 0) break;
        if (nRecv == 0) break;
        if (written + (int)nRecv >= out_size) nRecv = out_size - written - 1;
        memcpy(out + written, chunk, nRecv);
        written += nRecv;
    }
    out[written] = '\0';

end:
    if (trans)    httpDestroyTransaction(trans);
    if (client)   httpDestroyClient(client);
    if (uri_pool) free(uri_pool);
    return status_code;
}

// -------------------------------------------------------
// main
// -------------------------------------------------------

s32 main(s32 argc, const char* argv[])
{
    // Init screen and debug console
    init_screen();
    initDebugConsole(context, 1280, 720);

    debugPrintf("Jellyfin PS3 Client v0.1\n");
    debugPrintf("========================\n\n");
    debugPrintf("Connecting to %s...\n", JELLYFIN_SERVER);
    flip();

    if (http_init() != HTTP_SUCCESS) {
        debugPrintf("ERROR: HTTP init failed!\n");
        flip();
        // Wait for X button before exiting
        padInfo padinfo;
        padData paddata;
        ioPadInit(7);
        while(1) {
            ioPadGetInfo(&padinfo);
            if(padinfo.status[0]) {
                ioPadGetData(0, &paddata);
                if(paddata.BTN_CROSS) break;
            }
        }
        return 1;
    }

    debugPrintf("HTTP init OK\n");
    debugPrintf("Requesting /System/Info/Public...\n");
    flip();

    int status = jellyfin_get("/System/Info/Public", responseBuffer, BUFFER_SIZE);

    if (status == 200) {
        debugPrintf("SUCCESS! HTTP 200\n\n");
        // Print first 400 chars of response
        responseBuffer[400] = '\0';
        debugPrintf("%s\n", responseBuffer);
    } else {
        debugPrintf("FAILED! Status: %d\n", status);
    }

    flip();
    http_end();

    // Wait for X to exit
    debugPrintf("\n\nPress X to exit.\n");
    flip();

    padInfo padinfo;
    padData paddata;
    ioPadInit(7);
    while(1) {
        ioPadGetInfo(&padinfo);
        if(padinfo.status[0]) {
            ioPadGetData(0, &paddata);
            if(paddata.BTN_CROSS) break;
        }
    }

    return 0;
}
