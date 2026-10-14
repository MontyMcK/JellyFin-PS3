#pragma once

// The clock and date in the corner, written the way the XMB writes them.
// The console keeps its own time zone, summer time, 12/24 h and date order
// (system parameters); the C library's localtime() knows none of them.
// Pure C so tests/test_clock_fmt.c compiles this exact file.

#include <stdio.h>
#include <time.h>

// SYSUTIL_SYSTEMPARAM_ID_DATE_FORMAT values.
enum { CLK_DATE_YMD = 0, CLK_DATE_DMY = 1, CLK_DATE_MDY = 2 };
// SYSUTIL_SYSTEMPARAM_ID_TIME_FORMAT values.
enum { CLK_TIME_12H = 0, CLK_TIME_24H = 1 };

// utc_secs: seconds since the epoch.  utc_offset_secs: zone plus summer time.
static inline void clock_format(long long utc_secs, int utc_offset_secs, int date_fmt, int time_fmt,
                                char *t_out, size_t t_cap, char *d_out, size_t d_cap)
{
    time_t t = (time_t)(utc_secs + utc_offset_secs);
    struct tm tm;
    gmtime_r(&t, &tm);
    if (time_fmt == CLK_TIME_24H) {
        snprintf(t_out, t_cap, "%d:%02d", tm.tm_hour, tm.tm_min);
    } else {
        const int h = tm.tm_hour % 12 == 0 ? 12 : tm.tm_hour % 12;
        snprintf(t_out, t_cap, "%d:%02d %s", h, tm.tm_min, tm.tm_hour < 12 ? "AM" : "PM");
    }
    if (date_fmt == CLK_DATE_DMY)      snprintf(d_out, d_cap, "%d/%d", tm.tm_mday, tm.tm_mon + 1);
    else if (date_fmt == CLK_DATE_MDY) snprintf(d_out, d_cap, "%d/%d", tm.tm_mon + 1, tm.tm_mday);
    else                               snprintf(d_out, d_cap, "%d/%d", tm.tm_mon + 1, tm.tm_mday);
}
