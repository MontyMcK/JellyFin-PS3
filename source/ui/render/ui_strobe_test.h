#pragma once

#include <stdbool.h>

void strobe_test_enter_xmb(void);
void strobe_test_leave_xmb(void);
void strobe_test_tick(void);

int         strobe_test_profile(void);
const char *strobe_test_profile_name(void);

bool strobe_test_disable_inactive_labels(void);
bool strobe_test_disable_category_labels(void);
bool strobe_test_disable_tracked_text(void);
bool strobe_test_disable_cpu_divider(void);
bool strobe_test_disable_card_cpu_fallback(void);
bool strobe_test_disable_gpu_glow(void);
bool strobe_test_disable_jellywave(void);
bool strobe_test_disable_jellywave_body(void);
bool strobe_test_disable_jellywave_rim(void);
