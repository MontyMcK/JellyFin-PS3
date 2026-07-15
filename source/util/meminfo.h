#pragma once
#include <ppu-types.h>

// LV2's view of the process's main-memory budget (syscall 352).  This is the
// pool malloc grows into, so it is the number that decides whether a decode
// can get its transient buffer.  PSL1GHT has no wrapper for it.
//
// RPCS3 hands the process a far more generous container than a retail console,
// which is exactly why heap exhaustion only ever showed up on hardware.
bool meminfo_get(u32 *total_bytes, u32 *avail_bytes);

// Free main memory in KB, or 0 if the syscall fails.  Cheap enough to call
// from a log line.
u32  meminfo_avail_kb(void);
