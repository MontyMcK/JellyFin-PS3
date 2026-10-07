#include "month_bg.h"

#include <stdio.h>
#include <sys/systime.h>
#include <sysutil/sysutil.h>

#include "jf_paths.h"
#include "plog.h"

#define DAYNIGHT_FILE  "jellyfin_daynight.txt"
#define DN_OFF   0
#define DN_AUTO  1
#define DN_NIGHT 2
#define DN_DAY   3
#define DN_SAMPLE_SECS 10

static int   s_mode = DN_AUTO;
static int   s_utc_offset_secs = 0;   // timezone + summer time, read once
static u64   s_sampled_at = 0;        // UTC seconds of the last clock sample
static float s_day = 0.0f;
static bool  s_logged = false;

void month_bg_load(void)
{
    s_mode = DN_AUTO;
    FILE *f = fopen(jf_data_path(DAYNIGHT_FILE), "r");
    if (f) {
        int v;
        if (fscanf(f, "%d", &v) == 1 && v >= DN_OFF && v <= DN_DAY) s_mode = v;
        fclose(f);
    }
    s32 tz = 0, summer = 0;
    if (sysUtilGetSystemParamInt(SYSUTIL_SYSTEMPARAM_ID_TIMEZONE, &tz) != 0) tz = 0;
    if (sysUtilGetSystemParamInt(SYSUTIL_SYSTEMPARAM_ID_SUMMERTIME, &summer) != 0) summer = 0;
    s_utc_offset_secs = (int)tz * 60 + (summer ? 3600 : 0);
    s_sampled_at = 0;
}

static float day_now(void)
{
    if (s_mode == DN_OFF || s_mode == DN_NIGHT) return 0.0f;
    if (s_mode == DN_DAY) return 1.0f;
    u64 sec = 0, nsec = 0;
    sysGetCurrentTime(&sec, &nsec);
    if (s_sampled_at == 0 || sec >= s_sampled_at + DN_SAMPLE_SECS || sec < s_sampled_at) {
        s_sampled_at = sec;
        const s64 local = (s64)sec + s_utc_offset_secs;
        const int minute = (int)((local / 60) % 1440);
        s_day = bg_day_blend(minute);
        // Once, on the first frame: month_bg_load() runs inside ui_init(),
        // before the logger is loaded, so a line there would be discarded.
        if (!s_logged) {
            s_logged = true;
            char b[128];
            snprintf(b, sizeof(b), "daynight: mode=%d utc_offset=%ds local=%02d:%02d day=%d%%",
                     s_mode, s_utc_offset_secs, minute / 60, minute % 60,
                     (int)(s_day * 100.0f + 0.5f));
            plog(b);
        }
    }
    return s_day;
}

bg_quad month_bg_current(u32 top, u32 bot, u32 accent)
{
    const float d = day_now();
    s_day = d;
    return bg_apply_day(bg_from_two(top, bot), d, accent);
}

float month_bg_day(void)  { return s_mode == DN_OFF ? 0.0f : s_day; }
float month_bg_dusk(void) { return s_mode == DN_OFF ? 0.0f : bg_dusk_glow(s_day); }

bool daynight_enabled(void) { return s_mode != DN_OFF; }

void daynight_set_enabled(bool on)
{
    s_mode = on ? DN_AUTO : DN_OFF;
    s_sampled_at = 0;                  // resample on the next frame
    if (!on) s_day = 0.0f;
    FILE *f = fopen(jf_data_path(DAYNIGHT_FILE), "w");
    if (!f) return;
    fprintf(f, "%d\n", s_mode);
    fclose(f);
}
