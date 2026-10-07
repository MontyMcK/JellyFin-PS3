#pragma once
#include "bg_gradient.h"

/*
 * The theme background on the console's local clock: the theme itself by
 * night, a brighter day look by day, easing between them on the XMB's own
 * schedule (bg_day_blend) with a brief glow at dawn and dusk.
 *
 * Callers may refresh the background on every wave_draw(), several times a
 * frame, and from more than one screen.  So the clock is sampled at most every
 * ten seconds into one cached value, on the render thread, and every call in
 * between returns the same quad -- a ramp moves 1/720 of the way per sample,
 * far below a visible step.
 *
 * Local time is the console's UTC clock plus its timezone and summer-time
 * system params (the C library's localtime() knows neither).
 *
 * Settings > Day / Night Palette, persisted in jellyfin_daynight.txt:
 * 0 off, 1 automatic (the default), 2 always night, 3 always day.  2 and 3
 * are FTP-only, to check the look without waiting for 06:00; the Settings row
 * shows them as On and turns them Off.
 */
void    month_bg_load(void);
bg_quad month_bg_current(u32 top, u32 bot, u32 accent);

// The cached day amount (0 night .. 1 day) and the dawn / dusk glow (0..1)
// behind the last month_bg_current(); the wave tints its ribbons with them.
float   month_bg_day(void);
float   month_bg_dusk(void);

bool    daynight_enabled(void);
void    daynight_set_enabled(bool on);   // set + persist immediately
