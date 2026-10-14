#pragma once

// Job protocol shared by the PPU pool (jf_spu.cpp) and the SPU worker
// (kernel/source/main.c).  Both compilers are big-endian with 4-byte float,
// so the blocks DMA byte-for-byte with no marshalling.
//
// One 128-byte cache line per block per worker.  The command and result
// blocks are separate lines on purpose: the PPU spins on a result while the
// SPU reads a command, and sharing a line would cost more in coherency
// traffic than the transfer itself.

#ifdef __SPU__
#include <stdint.h>
typedef uint32_t u32;
typedef uint64_t u64;
#else
#include <ppu-types.h>
#endif

#define JFS_NFIELD      8
#define JFS_MAX_WORKERS 6

enum { JFS_F_X = 0, JFS_F_Y, JFS_F_VX, JFS_F_VY,
       JFS_F_ROT, JFS_F_SCALE, JFS_F_ALPHA, JFS_F_LIFE };

// Command: PPU writes, SPU reads.
//
// `seq` is the publish flag and MUST be written last, after the other fields
// and a sync.  The PPU also has to carry the CURRENT seq through any struct
// copy rather than zeroing it: a worker polling mid-copy would otherwise see
// a changed seq and run a job assembled from two different commands, and a
// mismatched (first, count) pair is not a wrong answer -- it is a DMA past
// the end of the arrays, over whatever the heap put there.  `count_max`
// exists so the worker can refuse such a block instead of executing it.
typedef struct {
    u64 ea_field[JFS_NFIELD];   //  0..63
    u32 first;                  // 64
    u32 count;                  // 68
    u32 count_max;              // 72
    u32 chunk;                  // 76
    float dt;                   // 80
    float phase;                // 84
    u32 seq;                    // 88   publish flag, written last
    u32 quit;                   // 92
    u32 _pad[8];                // 96..127
} __attribute__((aligned(128))) JfsCmd;

// Result: SPU writes, PPU reads.  Tick counts are raw SPU decrementer ticks
// at the timebase rate (79.8 MHz nominal, 12.53 ns); the PPU converts using
// sysGetTimebaseFrequency() so both clocks agree.
typedef struct {
    u32 seq;            //  0   echoes JfsCmd.seq -- the completion flag
    u32 t_total;        //  4
    u32 t_compute;      //  8
    u32 t_dma;          // 12   blocked on transfers
    u32 objects;        // 16
    u32 rejected;       // 20   non-zero: the block failed its bounds check
    u32 _pad[26];
} __attribute__((aligned(128))) JfsRes;
