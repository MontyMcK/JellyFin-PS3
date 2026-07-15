#include "meminfo.h"
#include <ppu-lv2.h>
#include <stdint.h>

// sys_memory_get_user_memory_size(sys_memory_info_t *)
//   struct { u32 total_user_memory; u32 available_user_memory; }
// Not exposed by PSL1GHT's sys/memory.h, so the syscall is issued directly.
struct lv2_mem_info { u32 total; u32 avail; };

LV2_SYSCALL sys_memory_get_user_memory_size(struct lv2_mem_info *info)
{
    lv2syscall1(352, (u64)(uintptr_t)info);
    return_to_user_prog(s32);
}

bool meminfo_get(u32 *total_bytes, u32 *avail_bytes)
{
    struct lv2_mem_info info = { 0, 0 };
    if (sys_memory_get_user_memory_size(&info) != 0) return false;
    if (total_bytes) *total_bytes = info.total;
    if (avail_bytes) *avail_bytes = info.avail;
    return true;
}

u32 meminfo_avail_kb(void)
{
    u32 avail = 0;
    if (!meminfo_get(NULL, &avail)) return 0;
    return avail / 1024;
}
