#pragma once
#include "bg_gradient.h"

/*
 * Month background selection is intentionally clock-independent in the
 * shipping JellyWave path for now. The important contract is that callers may
 * refresh the background on every wave_draw() without observing alternating
 * palette data from another thread.
 *
 * The extension point remains here so a future calendar-driven palette can be
 * added behind a cached, single-threaded clock sample.
 */
void month_bg_load(void);
bg_quad month_bg_current(u32 top, u32 bot);
