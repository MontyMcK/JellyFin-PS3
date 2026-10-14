// SPU worker for the Jellyfin animation pool.
//
// Persistent: started once, then parks on signal notification waiting for
// jobs.  A thread-group start/join per frame costs milliseconds of LV2
// syscall; parking and being signalled costs ~2 us and consumes no bus
// bandwidth in between.
//
// Timing uses the SPU decrementer, which counts DOWN at the Cell timebase
// rate (79.8 MHz, 12.53 ns/tick), so an interval is (before - after).  It is
// loaded with 0xFFFFFFFF at entry, giving ~53 s of headroom.

#include <spu_intrinsics.h>
#include <spu_mfcio.h>
#include <sys/spu_thread.h>
#include <stdint.h>

#include "jf_spu_proto.h"
#include "jf_anim_kernel.h"

#define TAG_CMD 0
#define TAG_RES 1
#define TAG_A   2
#define TAG_B   3

// Working set, in objects.  Two buffers x 8 fields x 4 B x CHUNK_MAX.
// 1024 objects is 64 KB of the 256 KB local store, which leaves ample room
// for code and stack and still sits on the flat part of the measured
// chunk-size curve (161 us at 1024 vs 162 us at 512).
//
// The buffers MUST alias one arena.  Declaring them separately is how this
// hits "ld: .bss exceeds local store range" -- the local store is the real
// constraint on any SPU job, not main memory.
#define CHUNK_MAX 1024
#define ARENA_FLOATS (2 * JFS_NFIELD * CHUNK_MAX)

static volatile float g_arena[ARENA_FLOATS] __attribute__((aligned(128)));
static volatile float *const g_buf[2] = {
    g_arena, g_arena + JFS_NFIELD * CHUNK_MAX
};

typedef struct { uint32_t size; uint32_t eal; } dma_elem;
static volatile dma_elem g_list[2][JFS_NFIELD] __attribute__((aligned(16)));

static JfsCmd g_cmd __attribute__((aligned(128)));
static JfsRes g_res __attribute__((aligned(128)));

static inline uint32_t dec(void) { return spu_read_decrementer(); }

static inline void wait_tag(uint32_t tag)
{
    mfc_write_tag_mask(1u << tag);
    mfc_read_tag_status_all();
}

// --- vector kernel -------------------------------------------------------
// Four objects at a time, kept line-for-line parallel to ak_step_scalar() in
// jf_anim_kernel.h so the two can be checked against each other.

static inline vector float vsin(vector float x)
{
    const vector float inv2pi = spu_splats(AK_INV_TWO_PI);
    const vector float two_pi = spu_splats(AK_TWO_PI);
    const vector float half   = spu_splats(0.5f);
    const vector float zero   = spu_splats(0.0f);

    vector float k = spu_mul(x, inv2pi);
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

static void step_vec(volatile float *base, uint32_t stride, uint32_t n,
                     float dt_s, float phase_s)
{
    volatile vector float *X  = (volatile vector float *)(base + JFS_F_X     * stride);
    volatile vector float *Y  = (volatile vector float *)(base + JFS_F_Y     * stride);
    volatile vector float *VX = (volatile vector float *)(base + JFS_F_VX    * stride);
    volatile vector float *VY = (volatile vector float *)(base + JFS_F_VY    * stride);
    volatile vector float *RO = (volatile vector float *)(base + JFS_F_ROT   * stride);
    volatile vector float *SC = (volatile vector float *)(base + JFS_F_SCALE * stride);
    volatile vector float *AL = (volatile vector float *)(base + JFS_F_ALPHA * stride);
    volatile vector float *LI = (volatile vector float *)(base + JFS_F_LIFE  * stride);

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

// --- DMA -----------------------------------------------------------------

static void build_list(volatile dma_elem *l, const JfsCmd *c,
                       uint32_t first, uint32_t n)
{
    uint32_t bytes = n * 4;
    for (int f = 0; f < JFS_NFIELD; f++) {
        l[f].size = bytes;
        l[f].eal  = (uint32_t)(c->ea_field[f] + first * 4);
    }
}

// Software pipeline: fetch chunk i+1 while computing chunk i.  Measured to
// hide 93% of the input transfer (26.1 us -> 1.9 us).  The output side does
// NOT hide -- the final put must be waited on before the job can report
// completion -- so do not expect it to.
static void run_job(const JfsCmd *c)
{
    uint32_t chunk = c->chunk;
    if (chunk > CHUNK_MAX) chunk = CHUNK_MAX;
    chunk &= ~3u;
    if (chunk == 0) chunk = 4;

    uint64_t eah = c->ea_field[0] & 0xFFFFFFFF00000000ULL;
    uint32_t t_dma = 0, t_cmp = 0;
    uint32_t b = 0, off = 0;

    uint32_t n0 = c->count < chunk ? c->count : chunk;
    build_list(g_list[0], c, c->first, (n0 + 3) & ~3u);
    mfc_getl((void *)g_buf[0], eah, (void *)g_list[0],
             JFS_NFIELD * sizeof(dma_elem), TAG_A, 0, 0);

    while (off < c->count) {
        uint32_t n = c->count - off;
        if (n > chunk) n = chunk;
        uint32_t nal = (n + 3) & ~3u;
        uint32_t nx_off = off + n;
        uint32_t nx = c->count - nx_off;
        if (nx > chunk) nx = chunk;

        if (nx) {
            build_list(g_list[b ^ 1], c, c->first + nx_off, (nx + 3) & ~3u);
            mfc_getl((void *)g_buf[b ^ 1], eah, (void *)g_list[b ^ 1],
                     JFS_NFIELD * sizeof(dma_elem), (b ^ 1) ? TAG_B : TAG_A, 0, 0);
        }

        uint32_t t0 = dec();
        wait_tag(b ? TAG_B : TAG_A);
        uint32_t t1 = dec();
        step_vec(g_buf[b], nal, nal, c->dt, c->phase);
        uint32_t t2 = dec();
        build_list(g_list[b], c, c->first + off, nal);
        mfc_putl((void *)g_buf[b], eah, (void *)g_list[b],
                 JFS_NFIELD * sizeof(dma_elem), b ? TAG_B : TAG_A, 0, 0);
        wait_tag(b ? TAG_B : TAG_A);
        uint32_t t3 = dec();

        t_dma += (t0 - t1) + (t2 - t3);
        t_cmp += t1 - t2;
        off = nx_off;
        b ^= 1;
    }

    g_res.t_dma     = t_dma;
    g_res.t_compute = t_cmp;
    g_res.objects   = c->count;
}

int main(uint64_t ea_cmd, uint64_t ea_res, uint64_t a3, uint64_t a4)
{
    (void)a3; (void)a4;

    spu_write_decrementer(0xFFFFFFFFu);
    spu_write_event_mask(0);

    uint32_t last_seq = 0;

    for (;;) {
        // Park until the PPU pokes us.  The command block is then fetched
        // fresh, so a signal that arrives during a publish still reads a
        // consistent block -- seq is written last, and a mismatch just means
        // we loop and wait for the next signal.
        (void)spu_read_signal1();
        mfc_get(&g_cmd, ea_cmd, sizeof(JfsCmd), TAG_CMD, 0, 0);
        wait_tag(TAG_CMD);

        if (g_cmd.seq == last_seq) continue;   // spurious wake
        last_seq = g_cmd.seq;
        if (g_cmd.quit) break;

        g_res.rejected = 0;
        g_res.objects  = 0;
        g_res.t_dma = g_res.t_compute = 0;

        // Refuse a torn or out-of-range block rather than executing it.  On
        // Cell the MFC writes wherever the effective address points, so a
        // bad (first, count) is heap corruption, not a wrong answer.
        if (g_cmd.count_max == 0 ||
            g_cmd.first > g_cmd.count_max ||
            g_cmd.count > g_cmd.count_max ||
            g_cmd.first + g_cmd.count > g_cmd.count_max) {
            g_res.rejected = 1;
        } else if (g_cmd.count) {
            uint32_t tt0 = dec();
            run_job(&g_cmd);
            uint32_t tt1 = dec();
            g_res.t_total = tt0 - tt1;
        }

        // seq last: the PPU treats it as the completion flag, so every other
        // field must be in memory first.  One 128-byte put lands atomically
        // at the destination; the barrier stops the compiler sinking stores
        // past it.
        __asm__ __volatile__("" ::: "memory");
        g_res.seq = last_seq;
        mfc_put(&g_res, ea_res, sizeof(JfsRes), TAG_RES, 0, 0);
        wait_tag(TAG_RES);
    }

    spu_thread_exit(0);
    return 0;
}
