// App-data directory helper — see jf_paths.h.

#include "jf_paths.h"
#include "../build_config.h"

#include <stdio.h>
#if BUILD_FOR_RPCS3
#include <sys/file.h>   // sysLv2FsMkdir (inline LV2 syscall, no PRX module needed)
#endif

// Where config / device id / settings live.  This differs by target:
//
//  - Real PS3 (BUILD_FOR_RPCS3 0): /dev_hdd0/tmp.  The installed game USRDIR is
//    NOT reliably writable while the title runs on hardware, so saving the login
//    there silently failed and user info was never persisted.  /dev_hdd0/tmp is
//    writable at runtime.  It is scratch, so a console that wipes it on boot
//    drops the saved login (re-login next boot) — the accepted trade for it
//    saving at all.
//  - RPCS3 (BUILD_FOR_RPCS3 1): the game USRDIR, unchanged.  RPCS3's USRDIR is
//    writable and persists, so the emulator keeps its saved login across runs.
#if BUILD_FOR_RPCS3
#define JF_DATA_DIR "/dev_hdd0/game/JFPS30000/USRDIR"
#else
#define JF_DATA_DIR "/dev_hdd0/tmp"
#endif

const char *jf_data_path(const char *name) {
#if BUILD_FOR_RPCS3
    static bool made = false;
    if (!made) {
        // Create the tree if missing (bare .self boot under RPCS3).
        sysLv2FsMkdir("/dev_hdd0/game/JFPS30000", 0755);
        sysLv2FsMkdir(JF_DATA_DIR, 0755);
        made = true;
    }
#endif
    static char buf[128];
    snprintf(buf, sizeof(buf), "%s/%s", JF_DATA_DIR, name);
    return buf;
}
