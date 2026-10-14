#pragma once
// Host stand-in for PSL1GHT's <ppu-types.h>, so pure app modules that only
// need the integer typedefs (vquality.cpp, stream_request.cpp) compile in
// tests/Makefile.host exactly as they do for the PPU.
#include <stdint.h>
#include <stdbool.h>
typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t   s8;
typedef int16_t  s16;
typedef int32_t  s32;
typedef int64_t  s64;
