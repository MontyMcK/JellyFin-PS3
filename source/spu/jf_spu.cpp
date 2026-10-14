// Persistent SPU worker pool.  See jf_spu.h for why it is shaped this way;
// every design choice here traces back to a figure in
// tools/spubench/RESULTS-real-ps3.txt.

#include <stdio.h>
#include <string.h>
#include <malloc.h>
#include <altivec.h>

#include <ppu-types.h>
#include <ppu-asm.h>
#include <sys/spu.h>
#include <sys/systime.h>

#include "jf_spu.h"
#include "jf_spu_proto.h"
#include "jf_anim_kernel.h"
#include "plog.h"

// Synchronous breadcrumbs (main.cpp): open/write/close per call, so the last
// one survives even a hard hang.  The group join below is the one call here
// that could in principle block forever if a worker wedged mid-job, and a
// silent hang at start-up would be far worse to diagnose than a slow one.
extern void crash_log(const char *msg);

// Embedded SPU kernel image (data/jf_spu_kernel.bin, via the Makefile's
// bin2o rule).  Built by source/spu/kernel/.
extern const unsigned char jf_spu_kernel_bin[];

#define ptr2ea(x) ((u64)(uintptr_t)(void *)(x))

// Measured crossover: 8-16 KB per transfer.  512 objects x 8 fields x 4 B is
// 16 KB and sat at the flat part of the curve (162 us vs 161 us at 1024, but
// 187 us at 128 and 293 us at 32).
#define JFS_DEFAULT_CHUNK 512

static bool           s_up        = false;
static bool           s_image_ok  = false;
static int            s_workers   = 0;
static u32            s_group     = 0;
static u32            s_thread[JF_SPU_MAX_WORKERS];
static sysSpuImage    s_image;
static JfsCmd        *s_cmd       = NULL;
static JfsRes        *s_res       = NULL;
static u32            s_seq       = 0;
static bool           s_pending   = false;
static u64            s_tb_hz     = 79800000ULL;
static u32            s_last_wall, s_last_compute, s_last_dma;

static inline u64 tb(void) { return __gettime(); }
static inline u32 tb_us(u64 t) { return (u32)(t * 1000000ULL / s_tb_hz); }

bool jf_spu_available(void) { return s_up; }
int  jf_spu_workers(void)   { return s_workers; }

void jf_spu_last_timing(u32 *wall, u32 *comp, u32 *dma)
{
    if (wall) *wall = s_last_wall;
    if (comp) *comp = s_last_compute;
    if (dma)  *dma  = s_last_dma;
}

// ---------------------------------------------------------------- lifetime

bool jf_spu_start(int workers)
{
    if (s_up) return true;
    if (workers < 1) return false;
    if (workers > JF_SPU_MAX_WORKERS) workers = JF_SPU_MAX_WORKERS;

    u64 f = sysGetTimebaseFrequency();
    if (f >= 1000000ULL && f <= 1000000000ULL) s_tb_hz = f;

    if (!s_cmd) s_cmd = (JfsCmd *)memalign(128, sizeof(JfsCmd) * JF_SPU_MAX_WORKERS);
    if (!s_res) s_res = (JfsRes *)memalign(128, sizeof(JfsRes) * JF_SPU_MAX_WORKERS);
    if (!s_cmd || !s_res) { plog("spu: control block alloc FAILED"); return false; }
    memset(s_cmd, 0, sizeof(JfsCmd) * JF_SPU_MAX_WORKERS);
    memset(s_res, 0, sizeof(JfsRes) * JF_SPU_MAX_WORKERS);

    if (!s_image_ok) {
        s32 rc = sysSpuInitialize(6, 0);
        if (rc != 0) { char b[64]; snprintf(b,sizeof(b),"spu: sysSpuInitialize rc=0x%08x", (u32)rc); plog(b); return false; }
        rc = sysSpuImageImport(&s_image, jf_spu_kernel_bin, 0);
        if (rc != 0) { char b[64]; snprintf(b,sizeof(b),"spu: imageImport rc=0x%08x", (u32)rc); plog(b); return false; }
        s_image_ok = true;
    }

    sysSpuThreadGroupAttribute gattr;
    memset(&gattr, 0, sizeof(gattr));
    sysSpuThreadGroupAttributeInitialize(gattr);
    sysSpuThreadGroupAttributeName(gattr, "jf_spu");

    // Priority 100: above every PPU thread the app runs, so a dispatch is not
    // preempted mid-frame.  It does not compete with cellVdec's own SPUs --
    // those are a different group entirely, and the reason this pool must be
    // stopped before vdec_open() rather than merely deprioritised.
    s32 rc = sysSpuThreadGroupCreate(&s_group, (u32)workers, 100, &gattr);
    if (rc != 0) {
        char b[64]; snprintf(b, sizeof(b), "spu: groupCreate(%d) rc=0x%08x", workers, (u32)rc);
        plog(b);
        return false;
    }

    for (int i = 0; i < workers; i++) {
        sysSpuThreadAttribute tattr;
        memset(&tattr, 0, sizeof(tattr));
        sysSpuThreadAttributeInitialize(tattr);
        sysSpuThreadAttributeName(tattr, "jf_wrk");
        sysSpuThreadArgument arg;
        memset(&arg, 0, sizeof(arg));
        arg.arg0 = ptr2ea(&s_cmd[i]);
        arg.arg1 = ptr2ea(&s_res[i]);
        rc = sysSpuThreadInitialize(&s_thread[i], s_group, (u32)i, &s_image, &tattr, &arg);
        if (rc != 0) {
            char b[64]; snprintf(b, sizeof(b), "spu: threadInit(%d) rc=0x%08x", i, (u32)rc);
            plog(b);
            sysSpuThreadGroupDestroy(s_group);
            return false;
        }
    }

    rc = sysSpuThreadGroupStart(s_group);
    if (rc != 0) {
        char b[64]; snprintf(b, sizeof(b), "spu: groupStart rc=0x%08x", (u32)rc);
        plog(b);
        sysSpuThreadGroupDestroy(s_group);
        return false;
    }

    s_workers = workers;
    s_up = true;
    s_pending = false;
    { char b[48]; snprintf(b, sizeof(b), "spu: pool up, %d worker(s)", workers); plog(b); }
    return true;
}

void jf_spu_stop(void)
{
    if (!s_up) return;

    // Publish quit, then signal -- a parked worker is blocked in
    // spu_read_signal1() and will not see the command block until poked.
    for (int i = 0; i < s_workers; i++) s_cmd[i].quit = 1;
    __asm__ __volatile__("sync" ::: "memory");
    u32 seq = ++s_seq;
    for (int i = 0; i < s_workers; i++) s_cmd[i].seq = seq;
    __asm__ __volatile__("sync" ::: "memory");
    for (int i = 0; i < s_workers; i++) sysSpuThreadWriteSignal(s_thread[i], 0, 1);

    crash_log("spu: joining thread group");
    u32 cause = 0, status = 0;
    sysSpuThreadGroupJoin(s_group, &cause, &status);
    crash_log("spu: joined");
    sysSpuThreadGroupDestroy(s_group);

    s_up = false;
    s_workers = 0;
    s_pending = false;
    plog("spu: pool down, SPUs released");
}

// ---------------------------------------------------------------- dispatch

bool jf_spu_dispatch(const JfSpuJob *job)
{
    if (!s_up || !job) return false;
    if (job->count == 0) return false;

    u32 chunk = job->chunk ? job->chunk : JFS_DEFAULT_CHUNK;
    u32 total = job->count & ~3u;          // the vector kernel needs a multiple of 4
    if (total == 0) return false;

    u32 per = (total / (u32)s_workers) & ~3u;
    u32 seq = ++s_seq;

    for (int i = 0; i < s_workers; i++) {
        JfsCmd c;
        memset(&c, 0, sizeof(c));
        for (int f = 0; f < JFS_NFIELD; f++) c.ea_field[f] = ptr2ea(job->field[f]);
        c.first     = per * (u32)i;
        c.count     = (i == s_workers - 1) ? (total - per * (u32)i) : per;
        c.count     &= ~3u;
        c.count_max = job->count_max;
        c.chunk     = chunk;
        c.dt        = job->dt;
        c.phase     = job->phase;
        c.quit      = 0;
        c.seq       = s_cmd[i].seq;        // carry the live seq; see jf_spu_proto.h
        s_res[i].seq = seq - 1;
        s_cmd[i] = c;
    }
    __asm__ __volatile__("sync" ::: "memory");
    for (int i = 0; i < s_workers; i++) s_cmd[i].seq = seq;
    __asm__ __volatile__("sync" ::: "memory");
    for (int i = 0; i < s_workers; i++) sysSpuThreadWriteSignal(s_thread[i], 0, 1);

    s_last_wall = 0;
    s_pending   = true;
    return true;
}

bool jf_spu_collect(u32 deadline_us)
{
    if (!s_up || !s_pending) return false;

    u64 t0 = tb();
    u64 limit = t0 + (u64)deadline_us * s_tb_hz / 1000000ULL;
    u32 seq = s_seq;

    for (int i = 0; i < s_workers; i++) {
        while (((volatile JfsRes *)&s_res[i])->seq != seq) {
            if (tb() > limit) {
                // A frame must never block on an SPU.  Give up, hand the
                // caller back to its PPU path, and stay there: something is
                // wrong with the pool and retrying every frame would turn a
                // stall into a stutter.
                //
                // Do NOT call jf_spu_stop() here.  It publishes `quit` and
                // then JOINS, which waits for every worker to leave its loop
                // voluntarily -- and a worker that missed its deadline is
                // exactly the one that may never do so.  That would make the
                // watchdog, whose entire job is to not block, block forever.
                // Terminate first so the join is guaranteed to return.
                plog("spu: dispatch missed its deadline -- terminating pool");
                s_pending = false;
                s_up      = false;
                crash_log("spu: terminating wedged group");
                sysSpuThreadGroupTerminate(s_group, 0);
                u32 cause = 0, status = 0;
                sysSpuThreadGroupJoin(s_group, &cause, &status);
                sysSpuThreadGroupDestroy(s_group);
                crash_log("spu: wedged group terminated");
                s_workers = 0;
                return false;
            }
        }
    }
    u64 t1 = tb();
    s_pending = false;

    u32 comp = 0, dma = 0, rejected = 0;
    for (int i = 0; i < s_workers; i++) {
        if (s_res[i].t_compute > comp) comp = s_res[i].t_compute;
        if (s_res[i].t_dma     > dma)  dma  = s_res[i].t_dma;
        rejected |= s_res[i].rejected;
    }
    s_last_wall    = tb_us(t1 - t0);
    s_last_compute = (u32)((u64)comp * 1000000ULL / s_tb_hz);
    s_last_dma     = (u32)((u64)dma  * 1000000ULL / s_tb_hz);

    if (rejected) {
        // The worker refused a torn or out-of-range command block.  That is
        // the guard doing its job, but it means this frame's state is stale,
        // so say so loudly rather than letting it pass as a good result.
        plog("spu: worker REJECTED a job block (bounds check) -- pool disabled");
        jf_spu_stop();
        return false;
    }
    return true;
}

// ---------------------------------------------------------------- PPU path

// AltiVec over SoA.  Measured 25.4 ns/object -- 13.3x the scalar version and
// 1.5% of a 60 Hz frame at 10,000 objects.  For most UI workloads this is the
// right answer and the SPU pool is not needed at all; see docs/wave-spec.md,
// where the reference particle field works out at roughly 2,800 objects.

static inline vector float vsin_ppu(vector float x)
{
    const vector float inv2pi = vec_splats(AK_INV_TWO_PI);
    const vector float two_pi = vec_splats(AK_TWO_PI);
    const vector float half   = vec_splats(0.5f);
    const vector float mhalf  = vec_splats(-0.5f);
    const vector float zero   = vec_splats(0.0f);

    vector float k = vec_madd(x, inv2pi, zero);
    vector bool int neg = vec_cmpgt(zero, k);
    vector float bias = vec_sel(half, mhalf, neg);
    vector float r = vec_ctf(vec_cts(vec_add(k, bias), 0), 0);
    x = vec_sub(x, vec_madd(r, two_pi, zero));

    vector float x2 = vec_madd(x, x, zero);
    vector float p = vec_madd(x2, vec_splats(-0.00019841f), vec_splats(0.00833333f));
    p = vec_madd(x2, p, vec_splats(-0.16666667f));
    p = vec_madd(x2, p, vec_splats(1.0f));
    return vec_madd(x, p, zero);
}

void jf_spu_step_ppu(float *const field[JF_SPU_NFIELD], u32 first, u32 n,
                     float dt_s, float phase_s)
{
    n &= ~3u;
    if (n == 0) return;

    vector float *X  = (vector float *)(field[JF_F_X]     + first);
    vector float *Y  = (vector float *)(field[JF_F_Y]     + first);
    vector float *VX = (vector float *)(field[JF_F_VX]    + first);
    vector float *VY = (vector float *)(field[JF_F_VY]    + first);
    vector float *RO = (vector float *)(field[JF_F_ROT]   + first);
    vector float *SC = (vector float *)(field[JF_F_SCALE] + first);
    vector float *AL = (vector float *)(field[JF_F_ALPHA] + first);
    vector float *LI = (vector float *)(field[JF_F_LIFE]  + first);

    const vector float dt      = vec_splats(dt_s);
    const vector float phase   = vec_splats(phase_s);
    const vector float cx      = vec_splats(AK_CX);
    const vector float cy      = vec_splats(AK_CY);
    const vector float attract = vec_splats(AK_ATTRACT);
    const vector float damp    = vec_splats(AK_DAMP);
    const vector float tamp    = vec_splats(AK_TURB_AMP);
    const vector float tfx     = vec_splats(AK_TURB_FX);
    const vector float tfy     = vec_splats(AK_TURB_FY);
    const vector float rotk    = vec_splats(AK_ROT_K);
    const vector float life0   = vec_splats(AK_LIFE0);
    const vector float invlife = vec_splats(1.0f / AK_LIFE0);
    const vector float one     = vec_splats(1.0f);
    const vector float zero    = vec_splats(0.0f);
    const vector float three   = vec_splats(3.0f);
    const vector float p2      = vec_splats(0.2f);
    const vector float eps     = vec_splats(0.0000001f);

    u32 nv = n >> 2;
    for (u32 i = 0; i < nv; i++) {
        vector float x = X[i], y = Y[i], vx = VX[i], vy = VY[i];
        vector float life = LI[i];

        vector float ax = vec_madd(vec_sub(cx, x), attract, zero);
        vector float ay = vec_madd(vec_sub(cy, y), attract, zero);

        vector float tx = vec_madd(tamp, vsin_ppu(vec_madd(y, tfy, phase)), zero);
        vector float ty = vec_madd(tamp, vsin_ppu(vec_sub(vec_madd(x, tfx, zero), phase)), zero);

        vx = vec_madd(vx, damp, vec_madd(vec_add(ax, tx), dt, zero));
        vy = vec_madd(vy, damp, vec_madd(vec_add(ay, ty), dt, zero));

        x = vec_madd(vx, dt, x);
        y = vec_madd(vy, dt, y);

        vector float rot = vec_madd(vec_madd(vec_add(vx, vy), rotk, zero), dt, RO[i]);

        life = vec_sub(life, dt);
        vector bool int dmask = vec_cmpgt(eps, life);
        vector float dead = vec_sel(zero, one, dmask);
        life = vec_madd(dead, life0, life);
        vector float keep = vec_sub(one, dead);
        x  = vec_madd(keep, x, vec_madd(dead, cx, zero));
        y  = vec_madd(keep, y, vec_madd(dead, cy, zero));
        vx = vec_madd(keep, vx, zero);
        vy = vec_madd(keep, vy, zero);

        vector float a = vec_madd(life, invlife, zero);
        a = vec_sel(a, one,  vec_cmpgt(a, one));
        a = vec_sel(a, zero, vec_cmpgt(zero, a));

        vector float sc = vec_madd(p2, vsin_ppu(vec_madd(life, three, phase)), one);

        X[i] = x; Y[i] = y; VX[i] = vx; VY[i] = vy;
        RO[i] = rot; SC[i] = sc; AL[i] = a; LI[i] = life;
    }
}

// ---------------------------------------------------------------- selftest

#include "jf_paths.h"

// Sizes chosen from docs/wave-spec.md: the reference XMB particle field works
// out at roughly 2,800 steady-state motes, so that is the number this client
// actually has to be able to animate.  4,096 gives headroom and keeps the
// chunking tidy.
#define JFS_TEST_N 4096

void jf_spu_selftest(void)
{
    {
        FILE *f = fopen(jf_data_path("jellyfin_sputest.txt"), "r");
        if (!f) return;               // not enabled: cost nothing
        fclose(f);
    }
    crash_log("sputest: enabled, starting");
    plog("sputest: start");

    float *field[JF_SPU_NFIELD] = { 0 };
    float *ref[JF_SPU_NFIELD]   = { 0 };
    bool ok = true;
    for (int f = 0; f < JF_SPU_NFIELD; f++) {
        field[f] = (float *)memalign(128, JFS_TEST_N * sizeof(float));
        ref[f]   = (float *)memalign(128, JFS_TEST_N * sizeof(float));
        if (!field[f] || !ref[f]) ok = false;
    }
    if (!ok) { plog("sputest: alloc FAILED"); goto done; }

    // Deterministic seed, so a difference between the two paths is the
    // decoder's and not the data's.
    {
        u32 rng = 0x1234567u;
        for (u32 i = 0; i < JFS_TEST_N; i++) {
            #define FR() (rng = rng * 1103515245u + 12345u, (float)((rng >> 8) & 0xFFFF) / 65535.0f)
            float x = FR() * 1280.0f, y = FR() * 720.0f;
            float vx = (FR() - 0.5f) * 60.0f, vy = (FR() - 0.5f) * 60.0f;
            float rot = FR() * 6.28f, life = 0.2f + FR() * AK_LIFE0;
            #undef FR
            field[JF_F_X][i] = ref[JF_F_X][i] = x;
            field[JF_F_Y][i] = ref[JF_F_Y][i] = y;
            field[JF_F_VX][i] = ref[JF_F_VX][i] = vx;
            field[JF_F_VY][i] = ref[JF_F_VY][i] = vy;
            field[JF_F_ROT][i] = ref[JF_F_ROT][i] = rot;
            field[JF_F_SCALE][i] = ref[JF_F_SCALE][i] = 1.0f;
            field[JF_F_ALPHA][i] = ref[JF_F_ALPHA][i] = 1.0f;
            field[JF_F_LIFE][i] = ref[JF_F_LIFE][i] = life;
        }
    }

    // PPU reference: 8 frames.
    {
        u64 a = tb();
        for (int r = 0; r < 8; r++)
            jf_spu_step_ppu(ref, 0, JFS_TEST_N, 1.0f / 60.0f, (float)r * 0.01f);
        u64 b = tb();
        char s[96];
        snprintf(s, sizeof(s), "sputest: PPU AltiVec %u objects, %u us/frame",
                 (unsigned)JFS_TEST_N, (unsigned)(tb_us(b - a) / 8));
        plog(s);
    }

    // SPU: same 8 frames. Three workers, because three is what is free once
    // cellVdec has taken its share -- testing with six would measure a
    // configuration the app can never actually have during playback.
    if (!jf_spu_start(3)) {
        plog("sputest: pool would not start -- PPU path is the implementation, so this is survivable");
        goto done;
    }
    {
        JfSpuJob job;
        memset(&job, 0, sizeof(job));
        for (int f = 0; f < JF_SPU_NFIELD; f++) job.field[f] = field[f];
        job.count = JFS_TEST_N;
        job.count_max = JFS_TEST_N;
        job.dt = 1.0f / 60.0f;

        u64 a = tb();
        bool good = true;
        for (int r = 0; r < 8; r++) {
            job.phase = (float)r * 0.01f;
            if (!jf_spu_dispatch(&job)) { good = false; break; }
            if (!jf_spu_collect(16000)) { good = false; break; }   // one frame
        }
        u64 b = tb();

        if (!good) {
            plog("sputest: dispatch/collect FAILED -- falling back to PPU");
        } else {
            u32 wall = 0, comp = 0, dma = 0;
            jf_spu_last_timing(&wall, &comp, &dma);
            char s[128];
            snprintf(s, sizeof(s),
                     "sputest: SPU x3 %u objects, %u us/frame (wall %u, compute %u, dma %u)",
                     (unsigned)JFS_TEST_N, (unsigned)(tb_us(b - a) / 8), wall, comp, dma);
            plog(s);

            // Agreement. Not bit-equality: AltiVec's vmaddfp and the SPU's
            // fma round differently from a scalar multiply-then-add, and 8
            // frames of feedback through velocity shows that in the fifth
            // decimal. Anything near 1e-4 means the same simulation.
            double worst = 0.0;
            for (int f = 0; f < JF_SPU_NFIELD; f++) {
                for (u32 i = 0; i < JFS_TEST_N; i++) {
                    double x = ref[f][i], y = field[f][i];
                    double d = x - y; if (d < 0) d = -d;
                    double sc = (x < 0 ? -x : x); if (sc < 1.0) sc = 1.0;
                    if (d / sc > worst) worst = d / sc;
                }
            }
            snprintf(s, sizeof(s), "sputest: worst relative difference %.3e -- %s",
                     worst, worst < 1e-3 ? "PASS" : "FAIL");
            plog(s);
        }
    }
    jf_spu_stop();

done:
    for (int f = 0; f < JF_SPU_NFIELD; f++) { free(field[f]); free(ref[f]); }
    crash_log("sputest: done");
    plog("sputest: done");
}
