#pragma once
// Playback-lifecycle trace for the 24p confirmation/revert investigation.
//
// Every line is "lc t=<ms> <what>", with t in milliseconds of the timebase
// clock, so one `grep "lc t="` over player_log.txt gives a single ordered
// timeline across the player, the stream, the decode thread, the 24p switch
// and sysutil -- plog's own stamp is whole seconds, too coarse to order
// events inside a 15 s confirmation window.
#include <stdarg.h>
#include <stdio.h>
#include "plog.h"
#include "timing.h"

static inline void lc_logf(const char *fmt, ...)
{
	char msg[200], line[240];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(msg, sizeof(msg), fmt, ap);
	va_end(ap);
	snprintf(line, sizeof(line), "lc t=%llu %s",
	         (unsigned long long)(timing_get_us() / 1000ULL), msg);
	plog(line);
}
