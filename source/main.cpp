#include <stdio.h>
#include <stdlib.h>
#include <malloc.h>
#include <string.h>

#include <ppu-types.h>
#include <sys/process.h>
#include <sysutil/sysutil.h>
#include <rsx/rsx.h>
#include <io/pad.h>

#include "rsxutil.h"
#include "ui.h"
#include "http.h"
#include "jellyfin_api.h"

SYS_PROCESS_PARAM(1001, 0x100000);

// Defined here; declared extern in ui.h so all other modules can read it.
u32 running = 0;

// -------------------------------------------------------
// System callbacks
// -------------------------------------------------------

extern "C" {
static void program_exit_callback(void) {
    sysUtilUnregisterCallback(SYSUTIL_EVENT_SLOT0);
    gcmSetWaitFlip(context);
    rsxFinish(context, 1);
}
static void sysutil_exit_callback(u64 status, u64 param, void *usrdata) {
    (void)param; (void)usrdata;
    if (status == SYSUTIL_EXIT_GAME) running = 0;
}
}

// -------------------------------------------------------
// Crash log (survives a crash, written before each step)
// -------------------------------------------------------

static FILE *g_clog = NULL;
static void crash_log(const char *msg) {
    if (!g_clog) g_clog = fopen("/dev_hdd0/tmp/crash_log.txt", "w");
    if  (g_clog) { fprintf(g_clog, "%s\n", msg); fflush(g_clog); }
}

// -------------------------------------------------------
// Entry point
// -------------------------------------------------------

int main(int argc, const char *argv[]) {
    (void)argc; (void)argv;

    crash_log("1 memalign");
    void *host_addr = memalign(1024*1024, HOST_SIZE);

    crash_log("2 init_screen");
    init_screen(host_addr, HOST_SIZE);

    crash_log("3 ioPadInit");
    ioPadInit(7);

    crash_log("4 atexit");
    atexit(program_exit_callback);

    crash_log("5 sysutil_cb");
    sysUtilRegisterCallback(0, sysutil_exit_callback, NULL);

    crash_log("6 ui_init");
    ui_init();

    crash_log("7 splash");
    drawHeader();
    drawText(40, 100, "Starting...");
    flip();

    crash_log("8 http_init");
    if (http_init() != HTTP_SUCCESS) {
        crash_log("8 FAILED");
        drawHeader();
        drawText(40, 100, "HTTP init failed!");
        flip();
        while (running) sysUtilCheckCallback();
        return 1;
    }

    crash_log("9 running=1");
    running = 1;

    crash_log("10 load_config");
    load_config();
    {
        char buf[128];
        snprintf(buf, sizeof(buf), "10 server=%s token_len=%d userid=%s",
                 g_server, (int)strlen(g_token), g_userid);
        crash_log(buf);
    }

    while (running) {
        if (!g_server[0]) {
            crash_log("11 get_server");
            char new_server[256] = "http://";
            if (get_input(new_server, sizeof(new_server),
                          "Server URL (e.g. http://192.168.1.2:8096)", false) != 1)
                break;
            strncpy(g_server, new_server, sizeof(g_server)-1);
            g_server[sizeof(g_server)-1] = '\0';
            { int sl = strlen(g_server); if (sl > 0 && g_server[sl-1] == '/') g_server[sl-1] = '\0'; }
            g_token[0] = '\0';
        }

        if (!g_token[0]) {
            crash_log("12 do_login");
            if (!do_login()) { g_server[0] = '\0'; continue; }
        }

        crash_log("13 show_main_menu");
        show_main_menu();
        if (!g_token[0]) g_server[0] = '\0';
    }

    crash_log("14 done");
    http_end();
    ui_cleanup();
    return 0;
}
