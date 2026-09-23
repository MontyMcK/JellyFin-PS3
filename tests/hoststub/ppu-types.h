#pragma once
// Host stand-in for PSL1GHT's <ppu-types.h>, so ui_text.cpp can be compiled
// and exercised on the build machine.  Types only -- nothing here changes
// behaviour, and the real header is used for every target build.
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

typedef uint8_t  u8;   typedef int8_t  s8;
typedef uint16_t u16;  typedef int16_t s16;
typedef uint32_t u32;  typedef int32_t s32;
typedef uint64_t u64;  typedef int64_t s64;
typedef volatile uint32_t vu32;
typedef float    f32;  typedef double  f64;
