#include <stdio.h>

#include "ui_strobe_test.h"
#include "plog.h"

enum {
    STROBE_PROFILE_LAST = 6,
    STROBE_PROFILE_FRAMES = 300
};

static bool s_active;
static int  s_profile;
static int  s_frames;

static const char *const s_names[] = {
    "baseline",
    "gpu-glow-off",
    "jellywave-body-off",
    "jellywave-rim-off",
    "jellywave-standard-alpha",
    "jellywave-off",
    "baseline-confirmation"
};

int strobe_test_profile(void)
{
    return s_profile;
}

const char *strobe_test_profile_name(void)
{
    return s_names[s_profile];
}

bool strobe_test_disable_inactive_labels(void)
{
    return false;
}

bool strobe_test_disable_category_labels(void)
{
    return false;
}

bool strobe_test_disable_tracked_text(void)
{
    return false;
}

bool strobe_test_disable_cpu_divider(void)
{
    return false;
}

bool strobe_test_disable_card_cpu_fallback(void)
{
    return false;
}

bool strobe_test_disable_gpu_glow(void)
{
    return s_profile == 1;
}

bool strobe_test_disable_jellywave(void)
{
    return s_profile == 5;
}

bool strobe_test_disable_jellywave_body(void)
{
    return s_profile == 2;
}

bool strobe_test_disable_jellywave_rim(void)
{
    return s_profile == 3;
}

static void strobe_test_log(const char *event)
{
    char line[224];
    snprintf(line, sizeof(line),
             "strobe_test: %s profile=%d name=%s frames=%d "
             "jellywave=%s body=%s rim=%s tracked_text=%s "
             "cpu_divider=%s card_cpu_fallback=%s gpu_glow=%s",
             event, s_profile, strobe_test_profile_name(), s_frames,
             strobe_test_disable_jellywave() ? "off" : "on",
             strobe_test_disable_jellywave_body() ? "off" : "on",
             strobe_test_disable_jellywave_rim() ? "off" : "on",
             strobe_test_disable_tracked_text() ? "off" : "on",
             strobe_test_disable_cpu_divider() ? "off" : "on",
             strobe_test_disable_card_cpu_fallback() ? "off" : "on",
             strobe_test_disable_gpu_glow() ? "off" : "on");
    plog(line);
}

void strobe_test_enter_xmb(void)
{
    s_active = true;
    s_profile = 0;
    s_frames = 0;
    strobe_test_log("start");
}

void strobe_test_leave_xmb(void)
{
    if (!s_active) return;
    strobe_test_log("stop");
    s_active = false;
}

void strobe_test_tick(void)
{
    if (!s_active || s_profile == STROBE_PROFILE_LAST) return;
    if (++s_frames < STROBE_PROFILE_FRAMES) return;

    strobe_test_log("complete");
    s_profile++;
    s_frames = 0;
    strobe_test_log("start");
}
