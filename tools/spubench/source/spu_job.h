// Shared PPU/SPU job protocol for the animation benchmark.
//
// One cache line (128 B) per worker for the command block and one for the
// result block, so the PPU polling a result never shares a line with a
// command the SPU is reading -- false sharing across the EIB costs more than
// the DMA itself.
//
// Every field is a fixed-width type laid out identically for ppu-gcc and
// spu-gcc (both big-endian, both 4-byte float), so the same struct can be
// DMA'd byte-for-byte with no marshalling.

#ifndef SPU_JOB_H
#define SPU_JOB_H

#ifdef __SPU__
#include <stdint.h>
typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int32_t  s32;
#else
#include <ppu-types.h>
#endif

#define SPUB_MAX_WORKERS   6
#define SPUB_NFIELD        8     // x y vx vy rot scale alpha life

// Field indices into the SoA array-pointer table.
enum {
    F_X = 0, F_Y, F_VX, F_VY, F_ROT, F_SCALE, F_ALPHA, F_LIFE
};

// ---- modes ---------------------------------------------------------------
#define MODE_SOA_DOUBLE   0   // SoA, double-buffered DMA (the real design)
#define MODE_SOA_SINGLE   1   // SoA, single-buffered (DMA never overlaps)
#define MODE_AOS_DOUBLE   2   // AoS 32B/object, double-buffered
#define MODE_DMA_ONLY     3   // DMA in + DMA out, no compute (bandwidth floor)
#define MODE_COMPUTE_ONLY 4   // compute on resident LS data, no DMA (ALU ceiling)
#define MODE_DMA_VRAM     5   // LS -> arbitrary EA put, sized by bytes_per_iter
#define MODE_DMA_READ_EA  6   // arbitrary EA -> LS get, sized by bytes_per_iter

// ---- synchronisation -----------------------------------------------------
#define SYNC_POLL         0   // SPU busy-polls the command block over DMA
#define SYNC_SIGNAL       1   // SPU blocks on signal notification 1

// Command block: PPU writes, SPU reads.  128 bytes.
typedef struct {
    u64 ea_field[SPUB_NFIELD];  //  0..63  SoA base EA of each field array
    u64 ea_aos;                 // 64..71  AoS base EA
    u64 ea_scratch;             // 72..79  target EA for MODE_DMA_VRAM / READ_EA
    u32 first;                  // 80      first object index for this worker
    u32 count;                  // 84      objects for this worker
    u32 chunk;                  // 88      objects per DMA chunk
    u32 mode;                   // 92
    float dt;                   // 96
    float phase;                // 100     animation phase (drives turbulence)
    u32 bytes_per_iter;         // 104     MODE_DMA_* transfer size
    u32 iters;                  // 108     MODE_DMA_*/COMPUTE_ONLY repeat count
    u32 seq;                    // 112     bumped by PPU to publish a new job
    u32 quit;                   // 116     non-zero: leave the worker loop
    u32 sync_mode;              // 120
    u32 count_max;              // 124     objects the arrays actually hold
} __attribute__((aligned(128))) SpuCmd;

// Result block: SPU writes, PPU reads.  128 bytes.
// All tick counts are raw SPU decrementer ticks (timebase rate, 79.8 MHz
// nominal -> 12.53 ns/tick); the PPU converts using the value it reads from
// sysGetTimebaseFrequency() so the two clocks agree.
typedef struct {
    u32 seq;           //  0   echoes SpuCmd.seq -- the completion flag
    u32 t_total;       //  4   whole job, entry to exit
    u32 t_compute;     //  8   time inside the update kernel only
    u32 t_dma_in;      // 12   time blocked waiting for an input DMA
    u32 t_dma_out;     // 16   time blocked waiting for an output DMA
    u32 t_wait;        // 20   time blocked waiting for the job to arrive
    u32 objects;       // 24   objects actually processed (sanity check)
    u32 chunks;        // 28
    u32 ls_static;     // 32   &_end - LS base: code + data, from the linker
    u32 ls_stack_lo;   // 36   lowest stack pointer observed
    u32 dec_freq_hint; // 40   0 (PPU supplies the real timebase frequency)
    float checksum;    // 44   sum of x+y over the slice -- proves work happened
    u32 _pad[20];
} __attribute__((aligned(128))) SpuRes;

#endif
