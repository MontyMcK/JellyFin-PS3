#pragma once

// Persistent SPU worker pool.
//
// Deliberately small: a pool, a dispatch, a collect, and a way to give the
// SPUs back.  This is not an engine and must not become one -- the renderer
// brief is explicit about that, and every line here has to earn its place
// against a PPU path that is already fast enough for most of what the UI does
// (docs/spu-feasibility.md).
//
// The shape is set by measurements taken on the console, not by taste:
//
//   * Workers are PERSISTENT.  A thread-group start/join per frame costs
//     milliseconds of LV2 syscall; a job round trip costs 2-4 us.
//   * Workers park on SIGNAL NOTIFICATION rather than busy-polling.  The
//     syscall measured no slower than a DMA poll loop (2.16 us vs 2.01 us
//     best) and the SPU consumes no bus bandwidth while parked.
//   * Job payloads are chunked at 8-16 KB.  Below ~4 KB per transfer the DMA
//     overhead roughly doubles the job.
//   * Object state is STRUCT-OF-ARRAYS.  Array-of-structs measured 6.7x
//     slower on the SPU (1082 us vs 163 us at 10k objects) because every
//     field access needs a shuffle.
//
// cellVdec takes 3 of the 6 SPUs while video is playing (video/vdec.cpp), so
// jf_spu_start() clamps to what is actually free and jf_spu_stop() MUST be
// called before vdec_open().  That is not bookkeeping -- it is the difference
// between a decode that fits and one that does not.

#include <ppu-types.h>

#define JF_SPU_MAX_WORKERS 6
#define JF_SPU_NFIELD      8      // x y vx vy rot scale alpha life

// SoA field indices.  Must match the kernel's copy.
enum {
    JF_F_X = 0, JF_F_Y, JF_F_VX, JF_F_VY,
    JF_F_ROT, JF_F_SCALE, JF_F_ALPHA, JF_F_LIFE
};

// One unit of work.  `field` points at JF_SPU_NFIELD parallel arrays, each at
// least `count_max` floats and 128-byte aligned; the pool splits [0, count)
// across the workers itself.
typedef struct {
    float *field[JF_SPU_NFIELD];
    u32    count;          // objects to process
    u32    count_max;      // objects the arrays actually hold (bounds check)
    u32    chunk;          // objects per DMA chunk; 0 picks the measured 512
    float  dt;
    float  phase;
} JfSpuJob;

// True once the pool is up and a job may be dispatched.  Everything else is
// safe to call regardless; this is what a caller tests before choosing
// between its SPU and PPU paths.
bool jf_spu_available(void);

// Bring the pool up with at most `workers` workers, clamped to what is free.
// Returns false and leaves the app on its PPU paths if anything fails -- SPU
// acceleration is optional by construction, never the only implementation.
bool jf_spu_start(int workers);

// Tear the pool down and return the SPUs. Idempotent. Call before vdec_open().
void jf_spu_stop(void);

// How many workers the pool actually got (0 when it is down).
int jf_spu_workers(void);

// Publish `job` to every worker.  Returns false if the pool is down, in which
// case the caller runs its PPU path.  Does NOT wait.
bool jf_spu_dispatch(const JfSpuJob *job);

// Wait for the outstanding dispatch, up to `deadline_us`.  Returns false on
// timeout, having disabled the pool -- a frame must never block on an SPU, so
// a caller that gets false uses its PPU result for this frame and will find
// jf_spu_available() false from then on.
bool jf_spu_collect(u32 deadline_us);

// Per-frame cost of the last completed job, in microseconds, for the debug
// HUD.  Zero when nothing has run.
void jf_spu_last_timing(u32 *wall_us, u32 *compute_us, u32 *dma_us);

// Opt-in start-up self-test.  Does nothing unless
// /dev_hdd0/tmp/jellyfin_sputest.txt exists -- same shape as net_selftest_run().
//
// Brings the pool up, runs the same job through the SPU and the PPU paths,
// compares them, reports timings to player_log.txt, and puts the SPUs back.
// This is what proves the pool works in the real application rather than only
// in tools/spubench, and it costs nothing when the file is absent.
void jf_spu_selftest(void);

// PPU reference implementation of the same kernel, AltiVec over SoA.
// Not a fallback afterthought: at 25.4 ns/object this is 254 us for 10,000
// objects (1.5% of a 60 Hz frame) and is the right answer for most workloads.
// Exposed so callers can use it directly and so the two can be compared.
// `n` is rounded down to a multiple of 4.
void jf_spu_step_ppu(float *const field[JF_SPU_NFIELD], u32 first, u32 n,
                     float dt, float phase);
