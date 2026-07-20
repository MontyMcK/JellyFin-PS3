// slog — STATE breadcrumb channel for the RPCS3 test harness (see slog.h).
// Entire translation unit is empty on real hardware.

#include "slog.h"

#if BUILD_FOR_RPCS3

#include <stdio.h>
#include <stdarg.h>

#include "timing.h"   // timing_get_us() — free-running clock, valid from boot

// STATE lines go to their own file on the PS3 VFS.  Under the RPCS3 flatpak
// this maps to a host path the test harness tails directly:
//   ~/.var/app/net.rpcs3.RPCS3/config/rpcs3/dev_hdd0/tmp/jf_state.log
#define SLOG_PATH "/dev_hdd0/tmp/jf_state.log"

// Open-write-close per line, exactly like crash_log() (main.cpp): a kept-open
// FILE*+fflush can lose its tail on a hard GPU/decoder hang, and that tail is
// precisely the breadcrumb the harness needs.  Closing every line forces it
// through the lv2 FS so the LAST STATE line is always on disk — which is what
// lets the tailer treat "no expected STATE within the timeout" as a
// hang/crash signal rather than a lost buffer.  First call truncates; every
// call after appends.
void slog_state(const char *fmt, ...) {
    static bool started = false;
    FILE *f = fopen(SLOG_PATH, started ? "a" : "w");
    if (!f) return;

    unsigned long long ms = (unsigned long long)(timing_get_us() / 1000ULL);
    fprintf(f, "STATE: [+%llums] ", ms);

    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);

    fputc('\n', f);
    fclose(f);
    started = true;
}

#endif // BUILD_FOR_RPCS3
