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
#include "ui_visuals.h"
#include "http.h"
#include "update_check.h"
#include "jellyfin_api.h"
#include "thumbnail_cache.h"
#include "img_arena.h"
#include "meminfo.h"
#include "plog.h"
#include "video.h"
#include "player_hud.h"
#include "slog.h"

SYS_PROCESS_PARAM(1001, 0x8000000);

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

// Each breadcrumb is opened, written, and closed immediately. A kept-open
// FILE* + fflush can lose its tail on a hard GPU hang (the kind that needs a
// power-cycle); closing every line forces it through the lv2 FS so the last
// breadcrumb is on disk no matter how the next step dies. First call
// truncates the file; every call after that appends.
void crash_log(const char *msg) {
    static bool started = false;
    FILE *f = fopen("/dev_hdd0/tmp/crash_log.txt", started ? "a" : "w");
    if (f) {
        fprintf(f, "%s\n", msg);
        fclose(f);
        started = true;
    }
}

// -------------------------------------------------------
// Entry point
// -------------------------------------------------------

int main(int argc, const char *argv[]) {
    (void)argc; (void)argv;

    {
        FILE *f = fopen("/dev_hdd0/tmp/launch_test.txt", "w");
        if (f) { fprintf(f, "main() reached\n"); fclose(f); }
    }

    crash_log("1 memalign");
    void *host_addr = memalign(1024*1024, HOST_SIZE);
    crash_log(host_addr ? "1b host_addr ok" : "1b host_addr NULL");

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
    plog_load_setting();   // starts logging only if the user enabled it

    crash_log("7 splash drawHeader");
    drawHeader();
    crash_log("7b splash drawTTF");
    drawTTF(40, 96, "Starting...", 16, 0x0099A0BC);
    crash_log("7c splash flip");
    flip();
    crash_log("7d splash done");

    // Reserve the player's big buffers NOW, while the heap is pristine:
    // HUD overlay (~8MB x2), VDEC arena (96MB), jitter buffer (16 slots).
    // They are cached for the app's lifetime — allocated per-session they
    // raced thumbnails/UI for a heap with only a few MB of slack, and a
    // movie could fail to start depending on what the UI had allocated
    // (jbuf_alloc FAILED even on the first play after some boots).
    crash_log("7e media reserve");
    hud_overlay_alloc();
    vdec_reserve_mem();
    {
        u32 rw = display_width  < 1280 ? display_width  : 1280;
        u32 rh = display_height < 720  ? display_height : 720;
        if (!jbuf_reserve(rw, rh)) crash_log("7e jbuf_reserve FAILED");
    }
    // The image decoder gets a reserved home too.  Thumbnail SLOTS were already
    // reserved, but the decode that fills them still called malloc per image
    // (~455KB output + working set) — and thumb_cache_init() below allocates
    // slots until the heap runs dry, so on hardware there was never anything
    // left and every decode failed with outofmem.  Measured stb peak for the
    // largest card (450x253) is 657KB baseline / 1005KB progressive JPEG, so
    // 4MB is ~4x the worst case; the HB line logs the real high-water mark as
    // arenaPeak — retune from that rather than from guesswork.
    img_arena_reserve(4 * 1024 * 1024);
    // Big one-shot transient (~9MB+), so do it here rather than on first draw.
    ps_sprites_preload();
    {
        u32 total = 0, avail = 0;
        char buf[96];
        if (meminfo_get(&total, &avail))
            snprintf(buf, sizeof(buf), "7f reserve done: free=%uKB of %uKB",
                     avail / 1024, total / 1024);
        else
            snprintf(buf, sizeof(buf), "7f reserve done (meminfo unavailable)");
        crash_log(buf);
        plog(buf);
    }

    crash_log("8 http_init");
    if (http_init() != HTTP_SUCCESS) {
        crash_log("8 FAILED");
        drawHeader();
        drawTTF(40, 96, "Network initialisation failed.", 16, 0x0099A0BC);
        flip();
        while (running) sysUtilCheckCallback();
        return 1;
    }

    crash_log("9 running=1");
    running = 1;

    // One-shot GitHub release check on its own thread; fails quietly.
    update_check_start();

    thumb_cache_init();

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
            slog_state("SERVER_URL_SCREEN");
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
            slog_state("LOGIN_OK userid=%s", g_userid);
        }

        crash_log("13 show_main_menu");
        slog_state("MAIN_MENU_ENTER");
        show_main_menu();
        // If the menu returned with no token, the user logged out. Keep the
        // server URL (jellyfin_logout preserves it) so the loop goes straight
        // back to the login screen rather than asking for the server again.
    }

    crash_log("14 done");
    update_check_shutdown();
    thumb_cache_shutdown();
    http_end();
    ui_cleanup();
    plog_stop();
    return 0;
}
