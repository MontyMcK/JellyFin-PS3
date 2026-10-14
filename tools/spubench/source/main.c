// SPU feasibility benchmark for the Jellyfin PS3 client.
//
// Standalone homebrew so it can be run on the real console without dragging
// the whole app (and its 96 MB VDEC arena) into the measurement.  Everything
// it learns is written to /dev_hdd0/tmp/spubench.txt, flushed after each
// section, so a hang or a fault still leaves the results that came before it.
//
// Read the report next to this directory for what the numbers mean.

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <unistd.h>

#include <ppu-types.h>
#include <ppu-asm.h>
#include <sys/process.h>
#include <sys/spu.h>
#include <sys/systime.h>
#include <sysutil/sysutil.h>
#include <sysutil/video.h>
#include <rsx/rsx.h>
#include <rsx/gcm_sys.h>
#include <io/pad.h>

#include "spu_job.h"
#include "anim_kernel.h"
#include "bench_ppu.h"
#include "spu_bin.h"

SYS_PROCESS_PARAM(1001, 0x10000);

#define ptr2ea(x) ((u64)(uintptr_t)(void *)(x))

#define CB_SIZE    0x80000
#define HOST_SIZE  (8 * 1024 * 1024)

#define N_MAX      50000
#define N_ALIGNED  51200          // multiple of 4 and of every chunk size used

// 50,000 is well past anything this UI needs -- docs/wave-spec.md derives the
// reference XMB particle field at roughly 2,800 steady-state motes -- but the
// brief asks for it and it is the point where the working set (50,000 x 8 x 4
// = 1.6 MB per copy) is far outside any cache, so it is the honest test of
// whether the earlier figures extrapolate or fall off a cliff.
static const u32 N_SET[]  = { 1000, 5000, 10000, 20000, 50000 };
#define N_SET_COUNT (int)(sizeof(N_SET) / sizeof(N_SET[0]))

// ---------------------------------------------------------------- logging

static FILE *s_log = NULL;

static void L(const char *fmt, ...)
{
    va_list ap;
    char buf[512];
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (s_log) { fputs(buf, s_log); fputc('\n', s_log); fflush(s_log); }
}

// Reopen-per-section so a hard hang cannot lose the tail.
static void log_open(void)  { s_log = fopen("/dev_hdd0/tmp/spubench.txt", "w"); }
static void log_sync(void)  { if (s_log) { fclose(s_log); s_log = fopen("/dev_hdd0/tmp/spubench.txt", "a"); } }

// ---------------------------------------------------------------- timing

static u64 s_tb_hz = 79800000ULL;
static inline u64 tb(void) { return __gettime(); }
static inline double tb_us(u64 ticks) { return (double)ticks * 1000000.0 / (double)s_tb_hz; }

// ---------------------------------------------------------------- video

static gcmContextData *ctx = NULL;
static u32 *fb[2];
static u32  fb_off[2];
static u32  disp_w, disp_h, fb_pitch, curr = 0;
static u32  depth_off;
static bool video_ok = false;

static void video_init(void)
{
    void *host = memalign(1024 * 1024, HOST_SIZE);
    if (!host) return;
    rsxInit(&ctx, CB_SIZE, HOST_SIZE, host);

    videoState st;
    videoResolution r;
    if (videoGetState(0, 0, &st) != 0) return;
    if (videoGetResolution(st.displayMode.resolution, &r) != 0) return;

    videoConfiguration vc;
    memset(&vc, 0, sizeof(vc));
    vc.resolution = st.displayMode.resolution;
    vc.format     = VIDEO_BUFFER_FORMAT_XRGB;
    vc.pitch      = r.width * 4;
    videoConfigure(0, &vc, NULL, 0);
    gcmSetFlipMode(GCM_FLIP_VSYNC);

    disp_w = r.width; disp_h = r.height;
    fb_pitch = disp_w * 4;
    fb[0] = (u32 *)rsxMemalign(64, disp_h * fb_pitch);
    fb[1] = (u32 *)rsxMemalign(64, disp_h * fb_pitch);
    if (!fb[0] || !fb[1]) return;
    rsxAddressToOffset(fb[0], &fb_off[0]);
    rsxAddressToOffset(fb[1], &fb_off[1]);
    gcmSetDisplayBuffer(0, fb_off[0], fb_pitch, disp_w, disp_h);
    gcmSetDisplayBuffer(1, fb_off[1], fb_pitch, disp_w, disp_h);

    u32 *zb = (u32 *)rsxMemalign(64, disp_h * fb_pitch);
    if (zb) rsxAddressToOffset(zb, &depth_off);
    video_ok = true;
}

static void set_target(u32 i)
{
    gcmSurface sf;
    memset(&sf, 0, sizeof(sf));
    sf.colorFormat = GCM_SURFACE_X8R8G8B8;
    sf.colorTarget = GCM_SURFACE_TARGET_0;
    sf.colorLocation[0] = GCM_LOCATION_RSX;
    sf.colorOffset[0]   = fb_off[i];
    sf.colorPitch[0]    = fb_pitch;
    sf.colorLocation[1] = sf.colorLocation[2] = sf.colorLocation[3] = GCM_LOCATION_RSX;
    sf.colorPitch[1] = sf.colorPitch[2] = sf.colorPitch[3] = 64;
    sf.depthFormat   = GCM_SURFACE_ZETA_Z16;
    sf.depthLocation = GCM_LOCATION_RSX;
    sf.depthOffset   = depth_off;
    sf.depthPitch    = fb_pitch;
    sf.type      = GCM_SURFACE_TYPE_LINEAR;
    sf.antiAlias = GCM_SURFACE_CENTER_1;
    sf.width = disp_w; sf.height = disp_h;
    rsxSetSurface(ctx, &sf);
}

static void do_flip(void)
{
    if (!video_ok) return;
    gcmSetFlip(ctx, curr);
    rsxFlushBuffer(ctx);
    gcmSetWaitFlip(ctx);
    curr ^= 1;
    set_target(curr);
}

static void wait_flip(void)
{
    if (!video_ok) return;
    while (gcmGetFlipStatus() != 0) usleep(50);
    gcmResetFlipStatus();
}

// Coarse on-screen progress so the console does not look hung: a bar whose
// width is the fraction of sections finished.  Written with the CPU, which is
// also a free sanity check that framebuffer writes work at all.
static void progress(int step, int total)
{
    if (!video_ok) return;
    u32 *p = fb[curr];
    for (u32 y = 0; y < disp_h; y++)
        for (u32 x = 0; x < disp_w; x++)
            p[y * disp_w + x] = 0x00101828;
    u32 w = disp_w * (u32)step / (u32)total;
    for (u32 y = disp_h / 2 - 12; y < disp_h / 2 + 12; y++)
        for (u32 x = 0; x < w; x++)
            p[y * disp_w + x] = 0x006C5BD4;
    do_flip();
    wait_flip();
}

// ---------------------------------------------------------------- data

static float *g_field[SPUB_NFIELD];
static float *g_aos;
static float *g_ref;           // reference copy for cross-checking

static void data_init(void)
{
    for (int f = 0; f < SPUB_NFIELD; f++)
        g_field[f] = (float *)memalign(128, N_ALIGNED * sizeof(float));
    g_aos = (float *)memalign(128, (size_t)N_ALIGNED * 8 * sizeof(float));
    g_ref = (float *)memalign(128, (size_t)N_ALIGNED * SPUB_NFIELD * sizeof(float));
}

// Deterministic seed so every run and every implementation starts identical.
static u32 s_rng = 0x1234567u;
static float frand(void) { s_rng = s_rng * 1103515245u + 12345u; return (float)((s_rng >> 8) & 0xFFFF) / 65535.0f; }

static void data_seed(void)
{
    s_rng = 0x1234567u;
    for (u32 i = 0; i < N_ALIGNED; i++) {
        float x = frand() * 1280.0f, y = frand() * 720.0f;
        float vx = (frand() - 0.5f) * 60.0f, vy = (frand() - 0.5f) * 60.0f;
        float life = 0.2f + frand() * AK_LIFE0;
        g_field[F_X][i] = x;    g_field[F_Y][i] = y;
        g_field[F_VX][i] = vx;  g_field[F_VY][i] = vy;
        g_field[F_ROT][i] = frand() * 6.28f;
        g_field[F_SCALE][i] = 1.0f;
        g_field[F_ALPHA][i] = 1.0f;
        g_field[F_LIFE][i] = life;
        float *o = g_aos + (size_t)i * 8;
        o[0]=x; o[1]=y; o[2]=vx; o[3]=vy; o[4]=g_field[F_ROT][i]; o[5]=1.0f; o[6]=1.0f; o[7]=life;
    }
}

// ---------------------------------------------------------------- SPU

static sysSpuImage s_image;
static u32  s_group = 0;
static u32  s_thread[SPUB_MAX_WORKERS];
static int  s_nworkers = 0;
static bool s_image_ok = false;
static bool s_group_up = false;

static SpuCmd *s_cmd;     // SPUB_MAX_WORKERS blocks, 128-byte aligned
static SpuRes *s_res;
static u32     s_seq = 0;

static bool spu_env_init(void)
{
    s_cmd = (SpuCmd *)memalign(128, sizeof(SpuCmd) * SPUB_MAX_WORKERS);
    s_res = (SpuRes *)memalign(128, sizeof(SpuRes) * SPUB_MAX_WORKERS);
    if (!s_cmd || !s_res) return false;
    memset(s_cmd, 0, sizeof(SpuCmd) * SPUB_MAX_WORKERS);
    memset(s_res, 0, sizeof(SpuRes) * SPUB_MAX_WORKERS);

    s32 rc = sysSpuInitialize(6, 0);
    L("spu: sysSpuInitialize(6,0) rc=%d", (int)rc);
    if (rc != 0) return false;
    rc = sysSpuImageImport(&s_image, spu_bin, 0);
    L("spu: sysSpuImageImport rc=%d", (int)rc);
    if (rc != 0) return false;
    s_image_ok = true;
    return true;
}

static bool spu_group_start(int n)
{
    if (s_group_up) return false;
    if (!s_image_ok) return false;
    if (n < 1 || n > SPUB_MAX_WORKERS) return false;

    sysSpuThreadGroupAttribute gattr;
    memset(&gattr, 0, sizeof(gattr));
    sysSpuThreadGroupAttributeInitialize(gattr);
    sysSpuThreadGroupAttributeName(gattr, "spubench");

    // Priority 100 is above anything the app runs; a lower number is a higher
    // priority on LV2, and the group must not be preempted mid-measurement.
    s32 rc = sysSpuThreadGroupCreate(&s_group, (u32)n, 100, &gattr);
    if (rc != 0) { L("spu: groupCreate(%d) FAILED rc=0x%08x", n, (u32)rc); return false; }

    for (int i = 0; i < n; i++) {
        sysSpuThreadAttribute tattr;
        memset(&tattr, 0, sizeof(tattr));
        sysSpuThreadAttributeInitialize(tattr);
        sysSpuThreadAttributeName(tattr, "spuwrk");
        sysSpuThreadArgument arg;
        memset(&arg, 0, sizeof(arg));
        arg.arg0 = ptr2ea(&s_cmd[i]);
        arg.arg1 = ptr2ea(&s_res[i]);
        memset(&s_cmd[i], 0, sizeof(SpuCmd));
        memset(&s_res[i], 0, sizeof(SpuRes));
        rc = sysSpuThreadInitialize(&s_thread[i], s_group, (u32)i, &s_image, &tattr, &arg);
        if (rc != 0) {
            L("spu: threadInit(%d) FAILED rc=0x%08x", i, (u32)rc);
            sysSpuThreadGroupDestroy(s_group);
            return false;
        }
    }
    rc = sysSpuThreadGroupStart(s_group);
    if (rc != 0) {
        L("spu: groupStart FAILED rc=0x%08x", (u32)rc);
        sysSpuThreadGroupDestroy(s_group);
        return false;
    }
    s_nworkers = n;
    s_group_up = true;
    return true;
}

static void spu_group_stop(void)
{
    if (!s_group_up) return;
    for (int i = 0; i < s_nworkers; i++) {
        s_cmd[i].quit = 1;
        s_cmd[i].seq  = ++s_seq;
    }
    __asm__ __volatile__("sync" ::: "memory");
    for (int i = 0; i < s_nworkers; i++)
        sysSpuThreadWriteSignal(s_thread[i], 0, 1);   // in case one is parked

    u32 cause = 0, status = 0;
    sysSpuThreadGroupJoin(s_group, &cause, &status);
    sysSpuThreadGroupDestroy(s_group);
    s_group_up = false;
    s_nworkers = 0;
}

// Publish one job to every active worker and wait for all of them.
// Returns elapsed timebase ticks, or 0 on timeout.
static u64 spu_dispatch(const SpuCmd *tmpl, u32 total, int signal_mode)
{
    u32 per = total / (u32)s_nworkers;
    per &= ~3u;
    u32 seq = ++s_seq;

    for (int i = 0; i < s_nworkers; i++) {
        SpuCmd c = *tmpl;
        c.first = per * (u32)i;
        c.count = (i == s_nworkers - 1) ? (total - per * (u32)i) : per;
        c.count &= ~3u;
        c.sync_mode = signal_mode ? SYNC_SIGNAL : SYNC_POLL;
        // Carry the CURRENTLY PUBLISHED seq through the struct copy rather
        // than zeroing it.  The worker treats seq as the publish flag, and it
        // polls the block with a DMA that can land in the middle of this
        // assignment: a zero here reads as "different from last_seq", i.e. as
        // a new job, and the worker would run one built from a mix of this
        // job's fields and the previous job's.  A mismatched (first, count)
        // pair is not a harmless wrong answer -- it is a DMA past the end of
        // the arrays, writing back over whatever the heap put there.
        // Publishing the new seq happens below, after a sync.
        c.seq = s_cmd[i].seq;
        c.count_max = N_ALIGNED;
        s_res[i].seq = seq - 1;
        s_cmd[i] = c;
    }
    __asm__ __volatile__("sync" ::: "memory");
    u64 t0 = tb();
    for (int i = 0; i < s_nworkers; i++) s_cmd[i].seq = seq;
    __asm__ __volatile__("sync" ::: "memory");
    if (signal_mode)
        for (int i = 0; i < s_nworkers; i++) sysSpuThreadWriteSignal(s_thread[i], 0, 1);

    // Watchdog: 5 s is ~2000x the longest job here.  Never leave the user's
    // console spinning on a bug in this file.
    u64 limit = t0 + s_tb_hz * 5ULL;
    for (int i = 0; i < s_nworkers; i++) {
        while (((volatile SpuRes *)&s_res[i])->seq != seq) {
            if (tb() > limit) { L("spu: TIMEOUT waiting for worker %d", i); return 0; }
        }
    }
    u64 t1 = tb();
    return t1 - t0;
}

// ---------------------------------------------------------------- stats

typedef struct { double best, mean; } Stat;

static Stat stat_of(const u64 *s, int n)
{
    Stat r; r.best = 1e30; double acc = 0;
    for (int i = 0; i < n; i++) {
        double us = tb_us(s[i]);
        if (us < r.best) r.best = us;
        acc += us;
    }
    r.mean = acc / n;
    return r;
}

// =================================================================
// Section 1 -- PPU baselines
// =================================================================

#define REPS 32

static void sec_ppu(void)
{
    L("");
    L("== 1. PPU baseline (%d reps, best and mean us per frame) ==", REPS);
    L("%-8s %-22s %10s %10s %12s", "objects", "impl", "best_us", "mean_us", "ns/object");

    for (int k = 0; k < N_SET_COUNT; k++) {
        u32 n = N_SET[k];
        u64 t[REPS];
        float phase = 0.0f;

        data_seed();
        for (int r = 0; r < REPS; r++) {
            u64 a = tb(); ppu_step_aos(g_aos, n, 1.0f / 60.0f, phase); u64 b = tb();
            t[r] = b - a; phase += 0.01f;
        }
        Stat s1 = stat_of(t, REPS);
        L("%-8u %-22s %10.1f %10.1f %12.1f", n, "scalar AoS", s1.best, s1.mean, s1.best * 1000.0 / n);

        data_seed(); phase = 0.0f;
        for (int r = 0; r < REPS; r++) {
            u64 a = tb(); ppu_step_soa_scalar(g_field, 0, n, 1.0f / 60.0f, phase); u64 b = tb();
            t[r] = b - a; phase += 0.01f;
        }
        Stat s2 = stat_of(t, REPS);
        L("%-8u %-22s %10.1f %10.1f %12.1f", n, "scalar SoA", s2.best, s2.mean, s2.best * 1000.0 / n);

        data_seed(); phase = 0.0f;
        for (int r = 0; r < REPS; r++) {
            u64 a = tb(); ppu_step_soa_vmx(g_field, 0, n, 1.0f / 60.0f, phase); u64 b = tb();
            t[r] = b - a; phase += 0.01f;
        }
        Stat s3 = stat_of(t, REPS);
        L("%-8u %-22s %10.1f %10.1f %12.1f  (%.2fx scalar SoA)",
          n, "AltiVec SoA", s3.best, s3.mean, s3.best * 1000.0 / n, s2.best / s3.best);

        // Frame-budget context: 16.67 ms at 60 Hz, 33.33 ms at 30 Hz.
        L("%-8u %-22s %9.1f%% of a 16.67 ms frame (AltiVec)", n, "", s3.best / 166.67);
    }
}

// =================================================================
// Section 2 -- SPU scaling
// =================================================================

static void sec_spu_scaling(void)
{
    L("");
    L("== 2. SPU animation worker, SoA + double-buffered DMA, chunk=512 ==");
    L("%-8s %-8s %10s %10s %10s %10s %10s %10s",
      "objects", "workers", "wall_us", "compute", "dma_in", "dma_out", "sync_us", "ns/obj");

    for (int k = 0; k < N_SET_COUNT; k++) {
        u32 n = N_SET[k];
        for (int w = 1; w <= 5; w++) {
            if (!spu_group_start(w)) { L("  (workers=%d unavailable)", w); continue; }
            data_seed();

            SpuCmd c;
            memset(&c, 0, sizeof(c));
            for (int f = 0; f < SPUB_NFIELD; f++) c.ea_field[f] = ptr2ea(g_field[f]);
            c.ea_aos = ptr2ea(g_aos);
            c.chunk = 512;
            c.mode  = MODE_SOA_DOUBLE;
            c.dt    = 1.0f / 60.0f;

            u64 best = ~0ULL; u32 bc = 0, bi = 0, bo = 0;
            for (int r = 0; r < REPS; r++) {
                c.phase = (float)r * 0.01f;
                u64 t = spu_dispatch(&c, n, 0);
                if (t == 0) break;
                if (t < best) {
                    best = t;
                    // Slowest worker decides the frame; report that one.
                    bc = bi = bo = 0;
                    for (int i = 0; i < w; i++) {
                        if (s_res[i].t_compute > bc) bc = s_res[i].t_compute;
                        if (s_res[i].t_dma_in  > bi) bi = s_res[i].t_dma_in;
                        if (s_res[i].t_dma_out > bo) bo = s_res[i].t_dma_out;
                    }
                }
            }
            if (best != ~0ULL) {
                double wall = tb_us(best);
                double comp = tb_us(bc), din = tb_us(bi), dout = tb_us(bo);
                L("%-8u %-8d %10.1f %10.1f %10.1f %10.1f %10.1f %10.1f",
                  n, w, wall, comp, din, dout, wall - (comp + din + dout),
                  wall * 1000.0 / n);
            }
            spu_group_stop();
        }
    }
}

// =================================================================
// Section 3 -- buffering, layout, chunk size
// =================================================================

static void sec_spu_variants(void)
{
    const u32 n = 10000;
    L("");
    L("== 3. Buffering / layout / chunk size, 1 worker, %u objects ==", n);

    if (!spu_group_start(1)) { L("  (SPU unavailable)"); return; }

    SpuCmd c;
    memset(&c, 0, sizeof(c));
    for (int f = 0; f < SPUB_NFIELD; f++) c.ea_field[f] = ptr2ea(g_field[f]);
    c.ea_aos = ptr2ea(g_aos);
    c.dt = 1.0f / 60.0f;

    struct { u32 mode; u32 chunk; const char *name; } v[] = {
        { MODE_SOA_SINGLE, 512,  "SoA single-buffered" },
        { MODE_SOA_DOUBLE, 512,  "SoA double-buffered" },
        { MODE_AOS_DOUBLE, 512,  "AoS double-buffered" },
    };
    L("%-24s %10s %10s %10s %10s", "variant", "wall_us", "compute", "dma_in", "dma_out");
    for (int i = 0; i < 3; i++) {
        data_seed();
        c.mode = v[i].mode; c.chunk = v[i].chunk;
        u64 best = ~0ULL; u32 bc=0,bi=0,bo=0;
        for (int r = 0; r < REPS; r++) {
            c.phase = (float)r * 0.01f;
            u64 t = spu_dispatch(&c, n, 0);
            if (!t) break;
            if (t < best) { best = t; bc=s_res[0].t_compute; bi=s_res[0].t_dma_in; bo=s_res[0].t_dma_out; }
        }
        if (best != ~0ULL)
            L("%-24s %10.1f %10.1f %10.1f %10.1f", v[i].name,
              tb_us(best), tb_us(bc), tb_us(bi), tb_us(bo));
    }

    L("");
    L("  chunk sweep (SoA double-buffered):");
    L("  %-8s %-10s %10s %10s %10s %10s", "chunk", "in-flight", "wall_us", "compute", "dma_in", "dma_out");
    u32 chunks[] = { 32, 64, 128, 256, 512, 1024, 2048 };
    for (int i = 0; i < 7; i++) {
        data_seed();
        c.mode = MODE_SOA_DOUBLE; c.chunk = chunks[i];
        u64 best = ~0ULL; u32 bc=0,bi=0,bo=0; u32 nch=0;
        for (int r = 0; r < REPS; r++) {
            c.phase = (float)r * 0.01f;
            u64 t = spu_dispatch(&c, n, 0);
            if (!t) break;
            if (t < best) { best=t; bc=s_res[0].t_compute; bi=s_res[0].t_dma_in; bo=s_res[0].t_dma_out; nch=s_res[0].chunks; }
        }
        if (best != ~0ULL)
            L("  %-8u %-10u %10.1f %10.1f %10.1f %10.1f", chunks[i],
              chunks[i] * SPUB_NFIELD * 4, tb_us(best), tb_us(bc), tb_us(bi), tb_us(bo));
        (void)nch;
    }

    L("");
    L("  local store: static (code+data) = %u bytes, stack pointer at %u,",
      s_res[0].ls_static, s_res[0].ls_stack_lo);
    L("               free between them  = %d bytes of 262144",
      (int)s_res[0].ls_stack_lo - (int)s_res[0].ls_static);

    spu_group_stop();
}

// =================================================================
// Section 4 -- synchronisation cost
// =================================================================

static void sec_sync(void)
{
    L("");
    L("== 4. Round-trip synchronisation cost (near-empty job, 1 worker) ==");
    if (!spu_group_start(1)) { L("  (SPU unavailable)"); return; }

    SpuCmd c;
    memset(&c, 0, sizeof(c));
    for (int f = 0; f < SPUB_NFIELD; f++) c.ea_field[f] = ptr2ea(g_field[f]);
    c.chunk = 4; c.mode = MODE_SOA_DOUBLE; c.dt = 1.0f / 60.0f;

    u64 t[REPS];
    for (int r = 0; r < REPS; r++) { u64 x = spu_dispatch(&c, 4, 0); t[r] = x ? x : 0; }
    Stat p = stat_of(t, REPS);
    L("  busy-poll   : best %.2f us, mean %.2f us", p.best, p.mean);

    for (int r = 0; r < REPS; r++) { u64 x = spu_dispatch(&c, 4, 1); t[r] = x ? x : 0; }
    Stat g = stat_of(t, REPS);
    L("  signal write: best %.2f us, mean %.2f us  (includes one LV2 syscall)", g.best, g.mean);
    L("  -> per-frame sync overhead is the figure a 16667 us budget must absorb");

    spu_group_stop();
}

// =================================================================
// Section 5 -- memory bandwidth, the part that decides the UI question
// =================================================================

static void bw_line(const char *what, u64 ticks, double bytes)
{
    double us = tb_us(ticks);
    L("  %-46s %9.2f ms   %8.1f MB/s", what, us / 1000.0,
      us > 0 ? bytes / us : 0.0);
}

static void sec_ppu_bandwidth(void)
{
    L("");
    L("== 5. PPU memory bandwidth (this is what the UI actually spends) ==");

    const size_t BYTES = 4 * 1024 * 1024;
    u8 *a = (u8 *)memalign(128, BYTES);
    u8 *b = (u8 *)memalign(128, BYTES);
    if (!a || !b) { L("  (allocation failed)"); return; }
    memset(a, 0x5A, BYTES);

    u64 t0 = tb(); memcpy(b, a, BYTES); u64 t1 = tb();
    bw_line("main -> main memcpy (4 MB)", t1 - t0, (double)BYTES);

    {   // read-modify-write in main memory: the same shape as the glyph blend
        volatile u32 *p = (volatile u32 *)b;
        size_t n = BYTES / 4;
        t0 = tb();
        for (size_t i = 0; i < n; i++) { u32 v = p[i]; p[i] = v + 1; }
        t1 = tb();
        bw_line("main read-modify-write, 4 B at a time (4 MB)", t1 - t0, (double)BYTES);
    }

    if (video_ok) {
        size_t fbsz = (size_t)disp_w * disp_h * 4;
        u32 *dst = fb[curr ^ 1];       // the buffer not being displayed

        t0 = tb(); memcpy(dst, a, fbsz < BYTES ? fbsz : BYTES); t1 = tb();
        bw_line("main -> VRAM memcpy (one framebuffer)", t1 - t0,
                (double)(fbsz < BYTES ? fbsz : BYTES));

        t0 = tb();
        for (size_t i = 0; i < fbsz / 4; i++) dst[i] = 0x00102030;
        t1 = tb();
        bw_line("VRAM flat fill, 4 B stores (one framebuffer)", t1 - t0, (double)fbsz);

        t0 = tb();
        {
            volatile u32 *p = (volatile u32 *)dst;
            for (size_t i = 0; i < fbsz / 4; i++) { u32 v = p[i]; p[i] = v | 1; }
        }
        t1 = tb();
        bw_line("VRAM read-modify-write (the text/alpha path)", t1 - t0, (double)fbsz);

        t0 = tb();
        {
            volatile u32 *p = (volatile u32 *)dst;
            u32 acc = 0;
            for (size_t i = 0; i < fbsz / 4; i++) acc += p[i];
            __asm__ __volatile__("" :: "r"(acc));
        }
        t1 = tb();
        bw_line("VRAM read only", t1 - t0, (double)fbsz);

        L("");
        L("  framebuffer is %ux%u = %.2f MB; a full-screen CPU pass at the",
          disp_w, disp_h, (double)fbsz / (1024.0 * 1024.0));
        L("  read-modify-write rate above is the UI's worst case.");
    } else {
        L("  (video not up -- VRAM figures skipped)");
    }
    free(a); free(b);
}

static void sec_spu_bandwidth(void)
{
    L("");
    L("== 6. SPU MFC bandwidth, 4 transfers in flight ==");
    if (!spu_group_start(1)) { L("  (SPU unavailable)"); return; }

    u8 *scratch = (u8 *)memalign(128, 512 * 1024);
    if (!scratch) { spu_group_stop(); return; }
    memset(scratch, 0xA5, 512 * 1024);

    SpuCmd c;
    memset(&c, 0, sizeof(c));
    for (int f = 0; f < SPUB_NFIELD; f++) c.ea_field[f] = ptr2ea(g_field[f]);
    c.dt = 1.0f / 60.0f;

    u32 sizes[] = { 128, 512, 1024, 4096, 16384 };
    L("  %-10s %14s %14s", "size", "LS->main MB/s", "main->LS MB/s");
    for (int i = 0; i < 5; i++) {
        c.ea_scratch = ptr2ea(scratch);
        c.bytes_per_iter = sizes[i];
        c.iters = 4096;
        double total = (double)sizes[i] * c.iters;

        c.mode = MODE_DMA_ONLY;
        u64 tw = spu_dispatch(&c, 4, 0);
        double wus = tb_us(s_res[0].t_dma_out);

        c.mode = MODE_DMA_READ_EA;
        u64 tr = spu_dispatch(&c, 4, 0);
        double rus = tb_us(s_res[0].t_dma_in);
        (void)tw; (void)tr;

        L("  %-10u %14.1f %14.1f", sizes[i],
          wus > 0 ? total / wus : 0.0, rus > 0 ? total / rus : 0.0);
    }
    free(scratch);
    spu_group_stop();
}

// SPU DMA straight into RSX local memory.  Run last and in its own group: if
// the MFC cannot translate that mapping the thread takes a storage exception
// and the group dies, and everything above must already be on disk.
static void sec_spu_vram(void)
{
    L("");
    L("== 7. Can an SPU DMA into RSX local memory? ==");
    if (!video_ok)            { L("  (video not up -- skipped)"); return; }
    if (!spu_group_start(1))  { L("  (SPU unavailable)"); return; }

    L("  framebuffer[1] effective address = 0x%08x", (u32)(uintptr_t)fb[1]);
    log_sync();

    SpuCmd c;
    memset(&c, 0, sizeof(c));
    for (int f = 0; f < SPUB_NFIELD; f++) c.ea_field[f] = ptr2ea(g_field[f]);
    c.ea_scratch = ptr2ea(fb[curr ^ 1]);
    c.bytes_per_iter = 16384;
    c.iters = 1024;
    c.mode = MODE_DMA_VRAM;
    c.dt = 1.0f / 60.0f;

    u64 t = spu_dispatch(&c, 4, 0);
    if (t == 0) {
        L("  RESULT: no completion within the watchdog -- the MFC most likely");
        L("          faulted on the RSX mapping.  Treat SPU->VRAM as unavailable.");
    } else {
        double us = tb_us(s_res[0].t_dma_out);
        double bytes = (double)c.bytes_per_iter * c.iters;
        L("  RESULT: completed.  LS -> RSX local memory = %.1f MB/s",
          us > 0 ? bytes / us : 0.0);
        L("          (compare the PPU's main->VRAM memcpy figure in section 5)");
    }
    spu_group_stop();
}

// =================================================================
// Section 8 -- does SPU work disturb a 60 Hz flip loop?
// =================================================================

static void frame_hist(const char *label, const u64 *ft, int n)
{
    double best = 1e30, worst = 0, acc = 0;
    int over16 = 0, over20 = 0, over33 = 0;
    for (int i = 0; i < n; i++) {
        double us = tb_us(ft[i]);
        if (us < best) best = us;
        if (us > worst) worst = us;
        acc += us;
        if (us > 16700) over16++;
        if (us > 20000) over20++;
        if (us > 33400) over33++;
    }
    L("  %-28s mean %7.2f ms  best %6.2f  worst %7.2f  >16.7ms %3d/%d  >33.4ms %d",
      label, acc / n / 1000.0, best / 1000.0, worst / 1000.0, over16, n, over33);
    (void)over20;
}

#define STAB_FRAMES 300

static void sec_stability(void)
{
    L("");
    L("== 8. 60 Hz stability with and without SPU workers (%d frames) ==", STAB_FRAMES);
    if (!video_ok) { L("  (video not up -- skipped)"); return; }

    static u64 ft[STAB_FRAMES];

    // Baseline: flip loop only.
    for (int i = 0; i < STAB_FRAMES; i++) {
        u64 a = tb();
        wait_flip();
        rsxSetClearColor(ctx, 0x00101828);
        rsxClearSurface(ctx, GCM_CLEAR_R | GCM_CLEAR_G | GCM_CLEAR_B | GCM_CLEAR_A);
        do_flip();
        ft[i] = tb() - a;
    }
    frame_hist("flip only", ft, STAB_FRAMES);

    // Same loop, plus the PPU AltiVec kernel on 10000 objects every frame.
    data_seed();
    for (int i = 0; i < STAB_FRAMES; i++) {
        u64 a = tb();
        wait_flip();
        ppu_step_soa_vmx(g_field, 0, 10000, 1.0f / 60.0f, (float)i * 0.01f);
        rsxSetClearColor(ctx, 0x00102838);
        rsxClearSurface(ctx, GCM_CLEAR_R | GCM_CLEAR_G | GCM_CLEAR_B | GCM_CLEAR_A);
        do_flip();
        ft[i] = tb() - a;
    }
    frame_hist("+ PPU AltiVec 10k", ft, STAB_FRAMES);

    // Same loop, 10000 objects on 3 SPUs (3 is what is left once cellVdec has
    // taken its share during playback).
    if (spu_group_start(3)) {
        data_seed();
        SpuCmd c;
        memset(&c, 0, sizeof(c));
        for (int f = 0; f < SPUB_NFIELD; f++) c.ea_field[f] = ptr2ea(g_field[f]);
        c.chunk = 512; c.mode = MODE_SOA_DOUBLE; c.dt = 1.0f / 60.0f;
        for (int i = 0; i < STAB_FRAMES; i++) {
            u64 a = tb();
            wait_flip();
            c.phase = (float)i * 0.01f;
            spu_dispatch(&c, 10000, 0);
            rsxSetClearColor(ctx, 0x00182838);
            rsxClearSurface(ctx, GCM_CLEAR_R | GCM_CLEAR_G | GCM_CLEAR_B | GCM_CLEAR_A);
            do_flip();
            ft[i] = tb() - a;
        }
        frame_hist("+ 3 SPU workers 10k", ft, STAB_FRAMES);
        spu_group_stop();
    }

    // And with the SPU job issued BEFORE the frame's own work, collected after
    // -- the shape a real integration would use.
    if (spu_group_start(3)) {
        data_seed();
        SpuCmd c;
        memset(&c, 0, sizeof(c));
        for (int f = 0; f < SPUB_NFIELD; f++) c.ea_field[f] = ptr2ea(g_field[f]);
        c.chunk = 512; c.mode = MODE_SOA_DOUBLE; c.dt = 1.0f / 60.0f;
        u32 per = 10000 / 3; per &= ~3u;
        for (int i = 0; i < STAB_FRAMES; i++) {
            u64 a = tb();
            wait_flip();
            // issue
            u32 seq = ++s_seq;
            for (int w = 0; w < 3; w++) {
                SpuCmd cc = c;
                cc.phase = (float)i * 0.01f;
                cc.first = per * (u32)w;
                cc.count = (w == 2) ? (10000 - per * 2) : per;
                cc.count &= ~3u;
                cc.sync_mode = SYNC_POLL;
                cc.seq = s_cmd[w].seq;      // see spu_dispatch for why not 0
                cc.count_max = N_ALIGNED;
                s_res[w].seq = seq - 1;
                s_cmd[w] = cc;
            }
            __asm__ __volatile__("sync" ::: "memory");
            for (int w = 0; w < 3; w++) s_cmd[w].seq = seq;
            __asm__ __volatile__("sync" ::: "memory");
            // PPU does its own frame work meanwhile
            rsxSetClearColor(ctx, 0x00203040);
            rsxClearSurface(ctx, GCM_CLEAR_R | GCM_CLEAR_G | GCM_CLEAR_B | GCM_CLEAR_A);
            // collect
            u64 limit = tb() + s_tb_hz * 2;
            for (int w = 0; w < 3; w++)
                while (((volatile SpuRes *)&s_res[w])->seq != seq) if (tb() > limit) break;
            do_flip();
            ft[i] = tb() - a;
        }
        frame_hist("+ 3 SPU overlapped w/ RSX", ft, STAB_FRAMES);
        spu_group_stop();
    }
}

// =================================================================
// Correctness -- the SPU must produce the same simulation as the PPU
// =================================================================

static void sec_verify(void)
{
    L("");
    L("== 9. SPU / PPU agreement ==");
    const u32 n = 8192;

    data_seed();
    for (int r = 0; r < 8; r++) ppu_step_soa_vmx(g_field, 0, n, 1.0f / 60.0f, (float)r * 0.01f);
    for (int f = 0; f < SPUB_NFIELD; f++)
        memcpy(g_ref + (size_t)f * N_ALIGNED, g_field[f], n * sizeof(float));

    if (!spu_group_start(1)) { L("  (SPU unavailable)"); return; }
    data_seed();
    SpuCmd c;
    memset(&c, 0, sizeof(c));
    for (int f = 0; f < SPUB_NFIELD; f++) c.ea_field[f] = ptr2ea(g_field[f]);
    c.chunk = 512; c.mode = MODE_SOA_DOUBLE; c.dt = 1.0f / 60.0f;
    for (int r = 0; r < 8; r++) { c.phase = (float)r * 0.01f; spu_dispatch(&c, n, 0); }

    double worst = 0.0; int worst_f = -1; u32 worst_i = 0;
    for (int f = 0; f < SPUB_NFIELD; f++) {
        for (u32 i = 0; i < n; i++) {
            float a = g_ref[(size_t)f * N_ALIGNED + i], b = g_field[f][i];
            double d = a - b; if (d < 0) d = -d;
            double scale = (a < 0 ? -a : a); if (scale < 1.0) scale = 1.0;
            double rel = d / scale;
            if (rel > worst) { worst = rel; worst_f = f; worst_i = i; }
        }
    }
    static const char *fn[] = { "x","y","vx","vy","rot","scale","alpha","life" };
    L("  after 8 frames on %u objects, worst relative difference = %.3e", n, worst);
    L("  (field %s, index %u)", worst_f >= 0 ? fn[worst_f] : "-", worst_i);
    L("  %s", worst < 1e-4
        ? "PASS -- the two agree to well inside single-precision rounding."
        : "FAIL -- the SPU kernel does not match the PPU kernel.");
    spu_group_stop();
}

// ---------------------------------------------------------------- main

static u32 s_running = 1;
static void exit_cb(u64 status, u64 param, void *ud)
{
    (void)param; (void)ud;
    if (status == SYSUTIL_EXIT_GAME) s_running = 0;
}

int main(int argc, const char *argv[])
{
    (void)argc; (void)argv;

    log_open();
    u64 f = sysGetTimebaseFrequency();
    if (f >= 1000000ULL && f <= 1000000000ULL) s_tb_hz = f;

    sysUtilRegisterCallback(0, exit_cb, NULL);
    video_init();
    ioPadInit(7);

    L("Jellyfin PS3 -- SPU feasibility benchmark");
    L("REAL PS3 RESULT (this file is written by the console itself)");
    L("timebase = %llu Hz (%.3f ns/tick)", (unsigned long long)s_tb_hz,
      1e9 / (double)s_tb_hz);
    L("display  = %ux%u", disp_w, disp_h);
    L("object   = 8 floats (x y vx vy rot scale alpha life), 32 bytes");
    log_sync();

    data_init();
    int step = 0, total = 9;

    progress(++step, total); sec_ppu();            log_sync();
    bool spu = spu_env_init();
    log_sync();
    if (spu) {
        progress(++step, total); sec_verify();        log_sync();
        progress(++step, total); sec_spu_scaling();   log_sync();
        progress(++step, total); sec_spu_variants();  log_sync();
        progress(++step, total); sec_sync();          log_sync();
    } else {
        L("");
        L("SPU environment unavailable -- PPU results only.  This is exactly the");
        L("path the app must take when SPU init fails.");
        step += 4;
    }
    progress(++step, total); sec_ppu_bandwidth(); log_sync();
    if (spu) { progress(++step, total); sec_spu_bandwidth(); log_sync(); }
    progress(++step, total); sec_stability();    log_sync();
    if (spu) { progress(++step, total); sec_spu_vram(); log_sync(); }

    L("");
    L("== done ==");
    if (s_log) { fclose(s_log); s_log = NULL; }

    // Solid green: finished.  Wait for the user to quit from the XMB.
    if (video_ok) {
        for (u32 y = 0; y < disp_h; y++)
            for (u32 x = 0; x < disp_w; x++)
                fb[curr][y * disp_w + x] = 0x00206040;
        do_flip();
    }
    while (s_running) { sysUtilCheckCallback(); usleep(20000); }
    return 0;
}
