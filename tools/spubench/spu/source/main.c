// SPU-side animation worker for the benchmark.
//
// Runs as a persistent thread: it is started once and then sits in a loop
// taking jobs, because a per-frame thread-group start/join costs milliseconds
// of LV2 syscall and would swamp the very thing being measured.
//
// Timing uses the SPU decrementer, which ticks at the Cell timebase rate
// (79.8 MHz nominal -> 12.53 ns).  It counts DOWN, so an elapsed interval is
// (before - after).  The decrementer is 32-bit and is loaded with 0xFFFFFFFF
// at entry, giving ~53 s of headroom -- far more than any single job.

#include <spu_intrinsics.h>
#include <spu_mfcio.h>
#include <sys/spu_thread.h>
#include <stdint.h>
#include <string.h>

#include "../../source/spu_job.h"
#include "../../source/anim_kernel.h"

#define TAG_CMD   0
#define TAG_RES   1
#define TAG_A     2
#define TAG_B     3

// Largest chunk the local store can hold, in objects.  Two buffers, eight
// 4-byte fields, input and output sharing the same storage because the kernel
// updates in place:  2 * 8 * 4 * CHUNK_MAX bytes = 64 * CHUNK_MAX.
//
// 2048 objects is 128 KB of the 256 KB local store, and that is the entire
// working-set budget: the SoA buffers, the AoS staging area and the bandwidth
// scratch all alias ONE arena rather than each reserving its own.  Declared
// separately they overflowed outright -- "ld: .bss exceeds local store range"
// -- which is the constraint this whole exercise is about.  A worker that
// wants more than 256 KB of working set does not exist; it has to stream.
#define CHUNK_MAX 2048
#define ARENA_FLOATS (2 * SPUB_NFIELD * CHUNK_MAX)

static volatile float g_arena[ARENA_FLOATS] __attribute__((aligned(128)));

// SoA double buffer: the two halves of the arena, each eight contiguous field
// blocks -- exactly the layout one DMA list writes.
static volatile float *const g_buf[2] = {
    g_arena, g_arena + SPUB_NFIELD * CHUNK_MAX
};

// AoS staging (32 bytes per object) and the raw-bandwidth scratch reuse the
// same arena; no mode needs two of them at once.
static volatile float *const g_aos[2] = {
    g_arena, g_arena + 8 * CHUNK_MAX
};

#define SCRATCH_BYTES (32 * 1024)
#define g_scratch ((volatile unsigned char *)g_arena)

typedef struct { uint32_t size; uint32_t eal; } dma_elem;
static volatile dma_elem g_list[2][SPUB_NFIELD] __attribute__((aligned(16)));

static SpuCmd g_cmd __attribute__((aligned(128)));
static SpuRes g_res __attribute__((aligned(128)));

extern char _end;   // linker: first byte past static data

static inline uint32_t dec(void) { return spu_read_decrementer(); }

static inline void wait_tag(uint32_t tag)
{
    mfc_write_tag_mask(1u << tag);
    mfc_read_tag_status_all();
}

// --- vector kernel -------------------------------------------------------
// The scalar kernel in anim_kernel.h, four objects at a time.  Kept
// line-for-line parallel to it so a reader can check they match.

static inline vector float vsin(vector float x)
{
    const vector float inv2pi = spu_splats(AK_INV_TWO_PI);
    const vector float two_pi = spu_splats(AK_TWO_PI);
    const vector float half   = spu_splats(0.5f);
    const vector float zero   = spu_splats(0.0f);

    vector float k = spu_mul(x, inv2pi);
    // (k >= 0) ? k + 0.5 : k - 0.5, then truncate -- matches ak_sinf exactly.
    vector unsigned int neg = spu_cmpgt(zero, k);
    vector float bias = spu_sel(half, spu_splats(-0.5f), neg);
    vector signed int ki = spu_convts(spu_add(k, bias), 0);
    vector float r = spu_convtf(ki, 0);
    x = spu_sub(x, spu_mul(r, two_pi));

    vector float x2 = spu_mul(x, x);
    vector float p = spu_madd(x2, spu_splats(-0.00019841f), spu_splats(0.00833333f));
    p = spu_madd(x2, p, spu_splats(-0.16666667f));
    p = spu_madd(x2, p, spu_splats(1.0f));
    return spu_mul(x, p);
}

// n must be a multiple of 4.  Operates in place on eight field blocks that
// are `stride` floats apart.
static void ak_step_vec(volatile float *base, uint32_t stride, uint32_t n,
                        float dt_s, float phase_s)
{
    volatile vector float *X  = (volatile vector float *)(base + F_X     * stride);
    volatile vector float *Y  = (volatile vector float *)(base + F_Y     * stride);
    volatile vector float *VX = (volatile vector float *)(base + F_VX    * stride);
    volatile vector float *VY = (volatile vector float *)(base + F_VY    * stride);
    volatile vector float *RO = (volatile vector float *)(base + F_ROT   * stride);
    volatile vector float *SC = (volatile vector float *)(base + F_SCALE * stride);
    volatile vector float *AL = (volatile vector float *)(base + F_ALPHA * stride);
    volatile vector float *LI = (volatile vector float *)(base + F_LIFE  * stride);

    const vector float dt      = spu_splats(dt_s);
    const vector float phase   = spu_splats(phase_s);
    const vector float cx      = spu_splats(AK_CX);
    const vector float cy      = spu_splats(AK_CY);
    const vector float attract = spu_splats(AK_ATTRACT);
    const vector float damp    = spu_splats(AK_DAMP);
    const vector float tamp    = spu_splats(AK_TURB_AMP);
    const vector float tfx     = spu_splats(AK_TURB_FX);
    const vector float tfy     = spu_splats(AK_TURB_FY);
    const vector float rotk    = spu_splats(AK_ROT_K);
    const vector float life0   = spu_splats(AK_LIFE0);
    const vector float invlife = spu_splats(1.0f / AK_LIFE0);
    const vector float one     = spu_splats(1.0f);
    const vector float zero    = spu_splats(0.0f);
    const vector float three   = spu_splats(3.0f);
    const vector float p2      = spu_splats(0.2f);

    uint32_t nv = n >> 2;
    for (uint32_t i = 0; i < nv; i++) {
        vector float x = X[i], y = Y[i], vx = VX[i], vy = VY[i];
        vector float life = LI[i];

        vector float ax = spu_mul(spu_sub(cx, x), attract);
        vector float ay = spu_mul(spu_sub(cy, y), attract);

        vector float tx = spu_mul(tamp, vsin(spu_madd(y, tfy, phase)));
        vector float ty = spu_mul(tamp, vsin(spu_sub(spu_mul(x, tfx), phase)));

        vx = spu_madd(vx, damp, spu_mul(spu_add(ax, tx), dt));
        vy = spu_madd(vy, damp, spu_mul(spu_add(ay, ty), dt));

        x = spu_madd(vx, dt, x);
        y = spu_madd(vy, dt, y);

        vector float rot = spu_madd(spu_mul(spu_add(vx, vy), rotk), dt, RO[i]);

        life = spu_sub(life, dt);
        // dead = (life <= 0) ? 1 : 0
        vector unsigned int dmask = spu_cmpgt(spu_splats(0.0000001f), life);
        vector float dead = spu_sel(zero, one, dmask);
        life = spu_madd(dead, life0, life);
        vector float keep = spu_sub(one, dead);
        x  = spu_madd(keep, x, spu_mul(dead, cx));
        y  = spu_madd(keep, y, spu_mul(dead, cy));
        vx = spu_mul(keep, vx);
        vy = spu_mul(keep, vy);

        vector float a = spu_mul(life, invlife);
        a = spu_sel(a, one,  spu_cmpgt(a, one));
        a = spu_sel(a, zero, spu_cmpgt(zero, a));

        vector float sc = spu_madd(p2, vsin(spu_madd(life, three, phase)), one);

        X[i] = x; Y[i] = y; VX[i] = vx; VY[i] = vy;
        RO[i] = rot; SC[i] = sc; AL[i] = a; LI[i] = life;
    }
}

// AoS variant: 8 floats per object, so each vector load straddles object
// boundaries and every field needs a shuffle.  Included because the layout
// question is the interesting one, not because anyone should ship it.
static void ak_step_aos(volatile float *p, uint32_t n, float dt, float phase)
{
    for (uint32_t i = 0; i < n; i++) {
        volatile float *o = p + i * 8;
        float x = o[0], y = o[1], vx = o[2], vy = o[3];
        float rot = o[4], life = o[7];

        float ax = (AK_CX - x) * AK_ATTRACT;
        float ay = (AK_CY - y) * AK_ATTRACT;
        float tx = AK_TURB_AMP * ak_sinf(y * AK_TURB_FY + phase);
        float ty = AK_TURB_AMP * ak_sinf(x * AK_TURB_FX - phase);
        vx = vx * AK_DAMP + (ax + tx) * dt;
        vy = vy * AK_DAMP + (ay + ty) * dt;
        x += vx * dt;
        y += vy * dt;
        rot += (vx + vy) * AK_ROT_K * dt;
        life -= dt;
        float dead = (life <= 0.0f) ? 1.0f : 0.0f;
        life += dead * AK_LIFE0;
        float keep = 1.0f - dead;
        x = keep * x + dead * AK_CX;
        y = keep * y + dead * AK_CY;
        vx *= keep; vy *= keep;
        float a = life * (1.0f / AK_LIFE0);
        if (a > 1.0f) a = 1.0f;
        if (a < 0.0f) a = 0.0f;
        float sc = 1.0f + 0.2f * ak_sinf(life * 3.0f + phase);

        o[0] = x; o[1] = y; o[2] = vx; o[3] = vy;
        o[4] = rot; o[5] = sc; o[6] = a; o[7] = life;
    }
}

// --- DMA helpers ---------------------------------------------------------

// Build the eight-element scatter list for objects [first, first+n).
static void build_list(volatile dma_elem *l, const SpuCmd *c,
                       uint32_t first, uint32_t n)
{
    uint32_t bytes = n * 4;
    for (int f = 0; f < SPUB_NFIELD; f++) {
        l[f].size = bytes;
        l[f].eal  = (uint32_t)(c->ea_field[f] + first * 4);
    }
}

static inline uint64_t eah_of(uint64_t ea) { return ea & 0xFFFFFFFF00000000ULL; }

// --- job modes -----------------------------------------------------------

static float sum_xy(volatile float *base, uint32_t stride, uint32_t n)
{
    float s = 0.0f;
    for (uint32_t i = 0; i < n; i++)
        s += base[F_X * stride + i] + base[F_Y * stride + i];
    return s;
}

static void run_soa(const SpuCmd *c, int double_buffered)
{
    uint32_t chunk = c->chunk;
    if (chunk > CHUNK_MAX) chunk = CHUNK_MAX;
    chunk &= ~3u;                       // the vector kernel needs a multiple of 4
    if (chunk == 0) chunk = 4;

    uint64_t eah = eah_of(c->ea_field[0]);
    uint32_t done = 0, nchunk = 0;
    uint32_t t_in = 0, t_out = 0, t_cmp = 0;
    float chk = 0.0f;

    if (!double_buffered) {
        while (done < c->count) {
            uint32_t n = c->count - done;
            if (n > chunk) n = chunk;
            uint32_t nal = (n + 3) & ~3u;

            build_list(g_list[0], c, c->first + done, nal);
            uint32_t t0 = dec();
            mfc_getl((void *)g_buf[0], eah, (void *)g_list[0],
                     SPUB_NFIELD * sizeof(dma_elem), TAG_A, 0, 0);
            wait_tag(TAG_A);
            uint32_t t1 = dec();
            ak_step_vec(g_buf[0], nal, nal, c->dt, c->phase);
            uint32_t t2 = dec();
            chk += sum_xy(g_buf[0], nal, n);
            mfc_putl((void *)g_buf[0], eah, (void *)g_list[0],
                     SPUB_NFIELD * sizeof(dma_elem), TAG_A, 0, 0);
            wait_tag(TAG_A);
            uint32_t t3 = dec();

            t_in  += t0 - t1;
            t_cmp += t1 - t2;
            t_out += t2 - t3;
            done += n; nchunk++;
        }
    } else {
        // Software pipeline: fetch chunk i+1 while computing chunk i and
        // writing back chunk i-1.  The whole point of Part 8 -- if the SPU
        // stalls on every transfer the measurement says nothing useful.
        uint32_t b = 0;
        uint32_t n0 = c->count < chunk ? c->count : chunk;
        uint32_t n0al = (n0 + 3) & ~3u;
        build_list(g_list[0], c, c->first, n0al);
        uint32_t ta = dec();
        mfc_getl((void *)g_buf[0], eah, (void *)g_list[0],
                 SPUB_NFIELD * sizeof(dma_elem), TAG_A, 0, 0);

        uint32_t off = 0;
        while (off < c->count) {
            uint32_t n = c->count - off;
            if (n > chunk) n = chunk;
            uint32_t nal = (n + 3) & ~3u;
            uint32_t nx_off = off + n;
            uint32_t nx = c->count - nx_off;
            if (nx > chunk) nx = chunk;
            uint32_t nxal = (nx + 3) & ~3u;

            // Start the next fetch into the other buffer before waiting.
            if (nx) {
                build_list(g_list[b ^ 1], c, c->first + nx_off, nxal);
                mfc_getl((void *)g_buf[b ^ 1], eah, (void *)g_list[b ^ 1],
                         SPUB_NFIELD * sizeof(dma_elem),
                         (b ^ 1) ? TAG_B : TAG_A, 0, 0);
            }

            uint32_t t0 = dec();
            wait_tag(b ? TAG_B : TAG_A);
            uint32_t t1 = dec();
            ak_step_vec(g_buf[b], nal, nal, c->dt, c->phase);
            uint32_t t2 = dec();
            chk += sum_xy(g_buf[b], nal, n);
            // The list for this buffer still describes this chunk when nx==0;
            // when nx!=0 it was overwritten, so rebuild before the put.
            build_list(g_list[b], c, c->first + off, nal);
            mfc_putl((void *)g_buf[b], eah, (void *)g_list[b],
                     SPUB_NFIELD * sizeof(dma_elem), b ? TAG_B : TAG_A, 0, 0);
            wait_tag(b ? TAG_B : TAG_A);
            uint32_t t3 = dec();

            t_in  += t0 - t1;
            t_cmp += t1 - t2;
            t_out += t2 - t3;
            off = nx_off; nchunk++;
            b ^= 1;
        }
        (void)ta;
    }

    g_res.t_dma_in  = t_in;
    g_res.t_dma_out = t_out;
    g_res.t_compute = t_cmp;
    g_res.chunks    = nchunk;
    g_res.objects   = c->count;
    g_res.checksum  = chk;
}

static void run_aos(const SpuCmd *c)
{
    uint32_t chunk = c->chunk;
    if (chunk > CHUNK_MAX) chunk = CHUNK_MAX;
    if (chunk == 0) chunk = 4;

    uint32_t done = 0, nchunk = 0;
    uint32_t t_in = 0, t_out = 0, t_cmp = 0;
    float chk = 0.0f;
    uint32_t b = 0;

    while (done < c->count) {
        uint32_t n = c->count - done;
        if (n > chunk) n = chunk;
        uint64_t ea = c->ea_aos + (uint64_t)(c->first + done) * 32;

        uint32_t t0 = dec();
        mfc_get((void *)g_aos[b], ea, n * 32, b ? TAG_B : TAG_A, 0, 0);
        wait_tag(b ? TAG_B : TAG_A);
        uint32_t t1 = dec();
        ak_step_aos(g_aos[b], n, c->dt, c->phase);
        uint32_t t2 = dec();
        for (uint32_t i = 0; i < n; i++) chk += g_aos[b][i * 8] + g_aos[b][i * 8 + 1];
        mfc_put((void *)g_aos[b], ea, n * 32, b ? TAG_B : TAG_A, 0, 0);
        wait_tag(b ? TAG_B : TAG_A);
        uint32_t t3 = dec();

        t_in += t0 - t1; t_cmp += t1 - t2; t_out += t2 - t3;
        done += n; nchunk++; b ^= 1;
    }

    g_res.t_dma_in  = t_in;
    g_res.t_dma_out = t_out;
    g_res.t_compute = t_cmp;
    g_res.chunks    = nchunk;
    g_res.objects   = c->count;
    g_res.checksum  = chk;
}

// Raw transfer bandwidth to or from an arbitrary EA.  ea_scratch may point at
// main memory or at RSX local memory -- that second case is the one worth
// knowing, because if an SPU can push pixels into the framebuffer faster than
// the PPU can, the whole UI architecture changes.
static void run_dma_bw(const SpuCmd *c, int write)
{
    uint32_t sz = c->bytes_per_iter;
    if (sz > SCRATCH_BYTES) sz = SCRATCH_BYTES;
    sz &= ~127u;
    if (sz == 0) sz = 128;

    // Four transfers in flight: a single blocking transfer measures latency,
    // not bandwidth.
    uint32_t t0 = dec();
    for (uint32_t i = 0; i < c->iters; i += 4) {
        uint64_t ea = c->ea_scratch + (uint64_t)(i & 7) * sz;
        for (uint32_t j = 0; j < 4; j++) {
            if (write) mfc_put((void *)g_scratch, ea + j * sz, sz, TAG_A, 0, 0);
            else       mfc_get((void *)g_scratch, ea + j * sz, sz, TAG_A, 0, 0);
        }
        wait_tag(TAG_A);
    }
    uint32_t t1 = dec();

    g_res.t_dma_in   = write ? 0 : (t0 - t1);
    g_res.t_dma_out  = write ? (t0 - t1) : 0;
    g_res.t_compute  = 0;
    g_res.chunks     = c->iters;
    g_res.objects    = 0;
    g_res.checksum   = 0.0f;
}

// Compute with no DMA at all: the ALU ceiling, for working out whether a job
// is transfer-bound or arithmetic-bound.
static void run_compute_only(const SpuCmd *c)
{
    uint32_t n = c->chunk;
    if (n > CHUNK_MAX) n = CHUNK_MAX;
    n &= ~3u;
    if (n == 0) n = 4;

    uint32_t t0 = dec();
    for (uint32_t i = 0; i < c->iters; i++)
        ak_step_vec(g_buf[0], n, n, c->dt, c->phase + (float)i * 0.001f);
    uint32_t t1 = dec();

    g_res.t_compute = t0 - t1;
    g_res.t_dma_in = g_res.t_dma_out = 0;
    g_res.chunks  = c->iters;
    g_res.objects = n * c->iters;
    g_res.checksum = sum_xy(g_buf[0], n, n);
}

// --- worker loop ---------------------------------------------------------

int main(uint64_t ea_cmd, uint64_t ea_res, uint64_t arg3, uint64_t arg4)
{
    (void)arg3; (void)arg4;

    spu_write_decrementer(0xFFFFFFFFu);
    spu_write_event_mask(0);

    // Seed the compute-only buffer so it is not full of denormals, which some
    // FPUs handle at a different rate and would skew the ALU ceiling.
    for (uint32_t f = 0; f < SPUB_NFIELD; f++)
        for (uint32_t i = 0; i < CHUNK_MAX; i++)
            g_arena[f * CHUNK_MAX + i] = 1.0f + (float)i * 0.01f;

    uint32_t last_seq = 0;

    for (;;) {
        uint32_t tw0 = dec();

        if (g_cmd.sync_mode == SYNC_SIGNAL && last_seq != 0) {
            // Blocking read of signal notification 1 -- the SPU is parked and
            // consumes no EIB bandwidth until the PPU pokes it.
            (void)spu_read_signal1();
            mfc_get(&g_cmd, ea_cmd, sizeof(SpuCmd), TAG_CMD, 0, 0);
            wait_tag(TAG_CMD);
        } else {
            // Busy-poll the command block.  Lowest possible wake latency, at
            // the cost of one 128-byte DMA per poll.
            do {
                mfc_get(&g_cmd, ea_cmd, sizeof(SpuCmd), TAG_CMD, 0, 0);
                wait_tag(TAG_CMD);
                if (g_cmd.seq != last_seq) break;
                // Back off ~1 us so polling does not saturate the bus.
                uint32_t d0 = dec();
                while ((d0 - dec()) < 80) { }
            } while (1);
        }

        uint32_t tw1 = dec();
        if (g_cmd.quit) break;
        last_seq = g_cmd.seq;

        // Belt and braces against a torn command block.  The PPU publishes
        // seq last so this should never fire, but a job whose (first, count)
        // ran off the end of the arrays would DMA over the heap rather than
        // merely compute the wrong answer, and that is not a failure mode
        // worth leaving to a single ordering argument.
        if (g_cmd.count_max &&
            (g_cmd.first > g_cmd.count_max ||
             g_cmd.count > g_cmd.count_max ||
             g_cmd.first + g_cmd.count > g_cmd.count_max)) {
            g_res.objects = 0;
            g_res.chunks  = 0xFFFFFFFFu;    // visible in the log as "rejected"
            g_res.seq = last_seq;
            mfc_put(&g_res, ea_res, sizeof(SpuRes), TAG_RES, 0, 0);
            wait_tag(TAG_RES);
            continue;
        }

        uint32_t tt0 = dec();
        switch (g_cmd.mode) {
        case MODE_SOA_DOUBLE:   run_soa(&g_cmd, 1); break;
        case MODE_SOA_SINGLE:   run_soa(&g_cmd, 0); break;
        case MODE_AOS_DOUBLE:   run_aos(&g_cmd);    break;
        case MODE_DMA_ONLY:     run_dma_bw(&g_cmd, 1); break;
        case MODE_COMPUTE_ONLY: run_compute_only(&g_cmd); break;
        case MODE_DMA_VRAM:     run_dma_bw(&g_cmd, 1); break;
        case MODE_DMA_READ_EA:  run_dma_bw(&g_cmd, 0); break;
        default: break;
        }
        uint32_t tt1 = dec();

        g_res.t_total     = tt0 - tt1;
        g_res.t_wait      = tw0 - tw1;
        g_res.ls_static   = (uint32_t)(uintptr_t)&_end;
        g_res.ls_stack_lo = (uint32_t)(uintptr_t)__builtin_frame_address(0);
        g_res.dec_freq_hint = 0;

        // seq last: the PPU treats it as the completion flag, so every other
        // field must already be in memory when it lands.  A single 128-byte
        // DMA is atomic at the destination, so one put is enough -- but the
        // compiler must not sink the stores past it.
        __asm__ __volatile__("" ::: "memory");
        g_res.seq = last_seq;
        mfc_put(&g_res, ea_res, sizeof(SpuRes), TAG_RES, 0, 0);
        wait_tag(TAG_RES);
    }

    spu_thread_exit(0);
    return 0;
}
