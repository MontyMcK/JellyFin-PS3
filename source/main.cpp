#include "audio_bitstream.h"
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
#include "avconf_capture.h"
#include "display_24p.h"
#include "ui.h"
#include "ui_visuals.h"
#include "ui_sfx.h"
#include "ui_wave.h"   // wave_init() -- called here, not from ui_init()
#include "http.h"
#include "update_check.h"
#include <unistd.h>   // usleep
#include "jellyfin_api.h"
#include "thumbnail_cache.h"
#include "img_arena.h"
#include "meminfo.h"
#include "plog.h"
#include "audio/audio_out.h"   // audio_out_log_capabilities()
#include "overscan.h"
#include "ui/ui_scale.h"
#include "ui/render/ui_spine.h"
#include "hd1080.h"
#include "vquality.h"
#include "surround.h"
#include "centermix.h"
#include "net_selftest.h"
#include "subfont.h"
#include "subcolor.h"
#include "statsovl.h"
#include "menusnow.h"
#include "autoskip.h"
#include "audio.h"
#include "video.h"
#include "player_hud.h"
#include "slog.h"
#include "lclog.h"
#include "jf_spu.h"
#include "ui_card_gpu.h"
#include "ui_text_gpu.h"
#include "boot_anim.h"

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
    (void)usrdata;
    // Every event, not just EXIT_GAME: an XMB open/close or menu event during
    // the 24p confirmation window must be visible in the lifecycle trace.
    lc_logf("sysutil: event status=0x%llx param=0x%llx",
            (unsigned long long)status, (unsigned long long)param);
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
    input_init();

    crash_log("4 atexit");
    atexit(program_exit_callback);

    crash_log("5 sysutil_cb");
    sysUtilRegisterCallback(0, sysutil_exit_callback, NULL);

    crash_log("6 ui_init");
    ui_init();
    plog_load_setting();   // starts logging only if the user enabled it
    // Theme, first of the restores and deliberately right after the logger:
    // g_theme is already built-in 0 via its static initialiser (so the boot
    // frames above were themed), and this applies the user's saved choice.
    // It logs which theme is live and why, which is exactly the kind of line
    // ui_init() would have thrown away.
    crash_log("6.3 theme_load_setting");
    theme_load_setting();
    // AFTER plog is up, deliberately: this decides whether card images go
    // through the RSX or the CPU blit, and which one is live has to be
    // visible in the log.  Called from ui_init() it ran before the logger
    // existed and said nothing at all.  Needs only RSX, which init_screen()
    // brought up at step 2.
    crash_log("6.4b card_gpu_init");
    ui_card_gpu_init();
    // Same reasoning, same stage: needs RSX and wants its "which path is live"
    // line in the log, so it cannot run from ui_init() either.
    crash_log("6.4c text_gpu_init");
    ui_text_gpu_init();
    // Third one for the same reason.  wave_init() reads jellyfin_gpuwave.txt
    // and reports which of the three submission paths is live; from ui_init()
    // that line went nowhere, which cost a measurement session.  Nothing draws
    // between ui_init() and here, and wave_draw() no-ops until this runs.
    crash_log("6.4d wave_init");
    wave_init();
    ui_scale_load();       // how 1280x720 authored numbers map to this screen
    spine_load();          // spine vs tab strip (jellyfin_spine.txt); logs which
    overscan_load();       // restore the user's CRT overscan calibration
    hd1080_load();         // restore the 1080p playback (Alpha) toggle
    vquality_load();       // restore the video quality choice (info screen)
    surround_load();       // restore the surround 5.1 (Alpha) toggle
    centermix_load();      // restore the dialogue / centre-channel mode
    subfont_load();        // restore the subtitle typeface
    subcolor_load();       // restore the subtitle colour
    menusnow_load();       // restore the Menu Particles toggle
    autoskip_load();       // restore the Auto Skip toggle
    statsovl_load();       // restore the player stats overlay toggle
    audio_volume_load();   // restore the saved master volume
    audio_bitstream_recover();   // undo an output change a crashed session left behind
    ui_sfx_init();         // XMB menu sounds, read from the console flash
    video_log_capabilities();   // what refresh rates does this panel offer?
    audio_out_log_capabilities();  // ...and will this chain take a bitstream?
    avconf_capture_run();       // opt-in 24p research capture; see its header
    d24_boot_check();           // did the last run die with the output in 24p?

    // Cold-boot animation: black, then the Jellyfin mark, while the rest of
    // startup runs underneath it; the XMB then rises out of the black and the
    // mark docks into the lockup (docs/boot-animation.md).  It needs the
    // theme, the UI scale and wave_init(), all loaded above.  When it is off
    // -- jellyfin_bootanim.txt = 0, or the emulator build -- this is the old
    // "Starting..." splash, unchanged, and every boot_anim_* below no-ops.
    crash_log("7 boot_anim_begin");
    if (!boot_anim_begin()) {
        crash_log("7 splash drawHeader");
        drawHeader();
        crash_log("7b splash drawTTF");
        drawTTF(40, 96, "Starting...", 16, 0x0099A0BC);
        crash_log("7c splash flip");
        flip();
    }
    crash_log("7d splash done");
    boot_anim_pump();

    // Bring the network up and run the one-shot update check BEFORE reserving
    // the big buffers below.  The check has to load the HTTPS + SSL PRX modules;
    // once vdec_reserve_mem() (96MB) + the jitter buffer have claimed the heap
    // there is not enough left, and on the real console sysModuleLoad(HTTPS)
    // failed with 0x80010004 (ENOMEM) so the "New version" popup never appeared
    // (RPCS3 has RAM to spare, which hid it).  Run it here while the heap is
    // still free and WAIT for it to finish: it unloads those modules and frees
    // its pools before we reserve, so the reservations still land on a clean
    // heap.  http_init() brings up SYSMODULE_NET once, up front (a single
    // allocation, not the alloc/free churn that fragments a big contiguous
    // reservation), and the app keeps it for the Jellyfin API.
    crash_log("8 http_init");
    if (http_init() != HTTP_SUCCESS) {
        crash_log("8 FAILED");
        boot_anim_leave();
        drawHeader();
        drawTTF(40, 96, "Network initialisation failed.", 16, 0x0099A0BC);
        flip();
        while (running) sysUtilCheckCallback();
        return 1;
    }
    // Opt-in diagnostic, before anything else touches the network.  Does
    // nothing unless jellyfin_nettest.txt exists -- see net_selftest.h.
    net_selftest_run();
    // Opt-in SPU pool check (jellyfin_sputest.txt).  Runs here, while the heap
    // is still pristine and before vdec_reserve_mem() takes its 96MB -- and
    // crucially before anything could want the SPUs cellVdec will claim.
    jf_spu_selftest();
    boot_anim_pump();

    crash_log("9 running=1");
    running = 1;
    crash_log("8b update check");
    update_check_start();
    // Wait (responsively) until it finishes so its HTTPS/SSL memory is released
    // before we reserve VDEC.  Bounded by the check's own 2s network timeouts;
    // the boot animation runs meanwhile (or the "Starting..." splash stays up).
    while (!update_check_done() && running) {
        sysUtilCheckCallback();
        if (!boot_anim_pump()) usleep(16000);
    }
    crash_log("8c update check done");

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
        // The jitter-buffer slots must hold a whole DECODED planar-YUV frame,
        // whose size is the transcode size we request (player.cpp), NOT the
        // display mode.  Reserved up front, before the UI fragments the heap:
        // 24 x 1.32MB at 720p, 16 x 3.13MB on the 1080p (Alpha) path.  If the
        // toggle changes at runtime, jbuf_reserve() re-grabs on next play.
        // Resolved through the same call the player uses, so the reservation
        // matches the frame size that will actually be requested — including
        // when the info screen's quality row overrides the 1080p toggle.
        u32 rw, rh;
        vquality_params(vquality_get(), hd1080_enabled(),
                        display_width, display_height, &rw, &rh,
                        NULL, NULL, NULL);
        if (!jbuf_reserve(rw, rh)) crash_log("7e jbuf_reserve FAILED");
    }
    boot_anim_pump();
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
    boot_anim_pump();
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

    thumb_cache_init();
    boot_anim_pump();

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
            boot_anim_leave();   // first run: fade the mark out, then ask
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
            boot_anim_leave();
            crash_log("12 do_login");
            if (!do_login()) { g_server[0] = '\0'; continue; }
            slog_state("LOGIN_OK userid=%s", g_userid);
        }

        crash_log("13 show_main_menu");
        // Cold boot only: the library list and Home rows load on a worker
        // while the mark holds, so the XMB's first frame has its data.  A
        // no-op call-through when the animation is off or already over.
        if (boot_anim_active())
            boot_anim_run(xmb_prepare, "library + Home prefetch");
        slog_state("MAIN_MENU_ENTER");
        show_main_menu();
        // Normally long finished; this covers the XMB returning mid-reveal
        // (a revoked token on the first request) and frees the mark's VRAM.
        boot_anim_finish();
        // If the menu returned with no token, the user logged out. Keep the
        // server URL (jellyfin_logout preserves it) so the loop goes straight
        // back to the login screen rather than asking for the server again.
        //
        // It can also return because the server revoked the saved token
        // mid-session, which looks like an empty library rather than an
        // error; jellyfin_session_expired() explains that and clears the
        // saved login so the loop below asks for credentials again.
        if (g_auth_expired) {
            crash_log("13z session expired");
            slog_state("SESSION_EXPIRED");
            jellyfin_session_expired();
        }
    }

    crash_log("14 done");
    update_check_shutdown();
    thumb_cache_shutdown();
    http_end();
    ui_cleanup();
    plog_stop();
    return 0;
}
