#pragma once

// =========================================================================
//  slog — STATE breadcrumb channel for the RPCS3 automated-test harness
// =========================================================================
//  Emits machine-readable "STATE: ... TAG k=v" lines at meaningful UI/player
//  transitions so the host-side test harness can drive the app by asserting
//  on log output instead of screenshots.  See the skill
//  jellyfin-ps3-rpcs3-test (references/state_vocab.md) for the line grammar.
//
//  EMULATOR-ONLY.  This reuses the project's single build switch
//  BUILD_FOR_RPCS3 (build_config.h) — do NOT add a second switch.  When
//  BUILD_FOR_RPCS3 == 0 (real-hardware / release build) slog_state() is an
//  empty inline whose (side-effect-free) arguments the optimizer discards at
//  -O2, so the console binary is byte-for-byte unaffected.
// =========================================================================

#include "../build_config.h"   // relative: source/ is not on the -I path

#if BUILD_FOR_RPCS3
void slog_state(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
#else
static inline void slog_state(const char *fmt, ...) { (void)fmt; }
#endif
