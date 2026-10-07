// Host test for source/ui/render/boot_seq.h -- the cold-boot animation's
// timeline.
//
// What this guards, in order of how badly it would hurt on a TV:
//
//   1. NOTHING CAN WEDGE.  A boot animation that never finishes is a console
//      that never reaches its UI.  Every scenario here -- XMB early, XMB late,
//      a login screen instead, a skip, stalled frames, garbage frame times,
//      a few thousand random interleavings -- must reach DONE within a bound
//      measured from the moment the app became ready.
//   2. NO POPS.  The whole point is that nothing ever appears or jumps in one
//      frame.  Every visible quantity is checked frame-to-frame at 60 Hz,
//      including across every phase boundary.
//   3. THE LAST FRAME IS THE STATIC LOCKUP.  The handoff to the XMB is only
//      seamless if DONE's values are exact (1.0, not 0.99997), so the easing
//      endpoints are checked with ==, not with a tolerance.
//   4. DETERMINISM.  Same inputs, same frames, bit for bit, across a reset.

#include <stdio.h>
#include <string.h>
#include <math.h>
#include <stdlib.h>
#include "boot_seq.h"

static int fails = 0;

static void ck(int cond, const char *what)
{
    if (!cond) { printf("  FAIL: %s\n", what); fails++; }
}

#define FRAME_MS (1000.0f / 60.0f)

// ---------------------------------------------------------------------------
// 1. easing
// ---------------------------------------------------------------------------
static void test_easing(void)
{
    typedef float (*E)(float);
    static const E fns[] = { boot_ease_out_cubic, boot_ease_in_cubic,
                             boot_ease_in_out_cubic, boot_ease_smoother };
    static const char *names[] = { "out_cubic", "in_cubic", "in_out_cubic",
                                   "smoother" };
    char b[96];
    for (int k = 0; k < 4; k++) {
        snprintf(b, sizeof b, "%s(0) == 0", names[k]);   ck(fns[k](0.0f) == 0.0f, b);
        snprintf(b, sizeof b, "%s(1) == 1", names[k]);   ck(fns[k](1.0f) == 1.0f, b);
        snprintf(b, sizeof b, "%s clamps below", names[k]); ck(fns[k](-3.0f) == 0.0f, b);
        snprintf(b, sizeof b, "%s clamps above", names[k]); ck(fns[k](7.0f) == 1.0f, b);
        float prev = 0.0f; int mono = 1;
        for (int i = 0; i <= 1000; i++) {
            float v = fns[k](i / 1000.0f);
            if (v < prev - 1e-6f) mono = 0;
            prev = v;
        }
        snprintf(b, sizeof b, "%s monotonic", names[k]); ck(mono, b);
    }

    // easeOutBack: exact ends, and the overshoot is the subtle ~4% intended,
    // not the classic 10% that reads as cartoon bounce.
    ck(boot_ease_out_back(0.0f, BOOT_WORD_OVERSHOOT) == 0.0f, "out_back(0) == 0");
    ck(boot_ease_out_back(1.0f, BOOT_WORD_OVERSHOOT) == 1.0f, "out_back(1) == 1");
    float peak = 0.0f;
    for (int i = 0; i <= 1000; i++) {
        float v = boot_ease_out_back(i / 1000.0f, BOOT_WORD_OVERSHOOT);
        if (v > peak) peak = v;
    }
    printf("  wordmark spring overshoot: %.2f%%\n", (peak - 1.0f) * 100.0f);
    ck(peak > 1.01f && peak < 1.06f, "word spring overshoot between 1% and 6%");

    // Breath: stays in [0,1], starts at 0, is periodic, peaks mid-period.
    ck(boot_breath(0.0f, BOOT_BREATH_MS) == 0.0f, "breath starts at 0");
    ck(fabsf(boot_breath(BOOT_BREATH_MS * 0.5f, BOOT_BREATH_MS) - 1.0f) < 1e-5f,
       "breath peaks mid-period");
    int in_range = 1;
    for (int i = 0; i < 20000; i++) {
        float v = boot_breath((float)i * 1.7f, BOOT_BREATH_MS);
        if (!(v >= 0.0f && v <= 1.0f)) in_range = 0;
    }
    ck(in_range, "breath within [0,1]");
    ck(fabsf(boot_breath(123.0f, BOOT_BREATH_MS) -
             boot_breath(123.0f + 3 * BOOT_BREATH_MS, BOOT_BREATH_MS)) < 1e-3f,
       "breath periodic");
    ck(boot_breath(100.0f, 0.0f) == 0.0f, "breath with zero period is 0");
}

// ---------------------------------------------------------------------------
// 2. the timeline's own invariants
// ---------------------------------------------------------------------------
static void test_constants(void)
{
    // The wordmark is drawn by the lockup in the XMB's text phase, which runs
    // BEFORE the veil is drawn -- so the veil must be gone before any letter
    // shows or the letters would be drawn under it.
    ck(BOOT_WORD_START_MS >= BOOT_EMERGE_START_MS + BOOT_EMERGE_MS,
       "wordmark starts only after the veil has fully lifted");
    // One motion, not three steps: DOCK opens while EMERGE is still running,
    // and WORDMARK while DOCK is.
    ck(BOOT_DOCK_START_MS < BOOT_EMERGE_START_MS + BOOT_EMERGE_MS,
       "dock overlaps emerge");
    ck(BOOT_WORD_START_MS < BOOT_DOCK_START_MS + BOOT_DOCK_MS,
       "wordmark overlaps dock");
    ck(BOOT_HANDOFF_END_MS >= BOOT_DOCK_START_MS + BOOT_DOCK_MS,
       "handoff does not end before the dock lands");
    ck(BOOT_HANDOFF_END_MS < 2500.0f, "handoff under 2.5 s");
    ck(BOOT_DARK_MIN_MS + BOOT_EMBLEM_MS + BOOT_HOLD_MIN_MS <= 1600.0f,
       "pre-ready minimum stays within measured cold-init time");
    printf("  pre-ready minimum %.0f ms, handoff %.0f ms\n",
           (double)(BOOT_DARK_MIN_MS + BOOT_EMBLEM_MS + BOOT_HOLD_MIN_MS),
           (double)BOOT_HANDOFF_END_MS);
}

// ---------------------------------------------------------------------------
// Scenario runner
// ---------------------------------------------------------------------------
typedef struct {
    float xmb_at;     // ms of real time when the XMB starts drawing, <0 never
    float leave_at;   // ms when startup goes elsewhere instead, <0 never
    float skip_at;    // ms of a button press, <0 never
    float stall_at;   // one long frame at this time ...
    float stall_ms;   // ... of this length
} Scenario;

typedef struct {
    int   reached_done;
    float done_real_ms;      // real time of DONE
    float trigger_real_ms;   // real time the app became ready / left
    float emerge_real_ms;    // real time EMERGE began, <0 never
    int   pops;              // frame-to-frame jumps over the limit
    int   xmb_before_ready;  // xmb_visible while the XMB was not drawing
    int   mark_in_dark;      // mark visible in DARK
    int   bad_values;        // anything outside [0,1] (reveal: [0,1.1])
    BootFrame last;
    int   frames;
} Result;

static int bad01(float v) { return !(v >= 0.0f && v <= 1.0f); }

// The standard: nothing visible moves more than ~10% of its full range in one
// 60 Hz frame -- the rate of a 170 ms linear fade, which reads as motion, not
// as a cut.  The curves' actual peaks, for reference: in_out_cubic peaks at
// slope 3 (NOT 1.5), so the 900 ms mark fade is 5.6%/frame, the 540 ms halo
// rise 9.3% (of a 30%-alpha halo: under 3% alpha), the 600 ms status fade
// 8.3%, the 850 ms dock 6.5% with its size lead; smootherstep peaks at 1.875,
// so the 1100 ms veil is 2.8%.  A real pop -- a value appearing -- is 100%.
#define MAXD_OPACITY 0.10f
#define MAXD_HALO    0.10f
#define MAXD_STATUS  0.10f
#define MAXD_VEIL    0.05f
#define MAXD_DOCK    0.10f
#define MAXD_REVEAL  0.20f   // pen travel: easeOutBack leaves fast, by design
#define MAXD_WORD    0.15f

static int verbose_pops = 0;
static void pop(Result *r, const char *what, float t, float a, float b)
{
    r->pops++;
    if (verbose_pops && r->pops <= 4)
        printf("    pop: %s %.4f -> %.4f at %.0f ms\n", what, a, b, t);
}

static void run(const Scenario *sc, float max_real_ms, Result *r)
{
    BootSeq s;
    BootFrame prev, f;
    memset(&prev, 0, sizeof prev);
    memset(&f, 0, sizeof f);
    float t = 0.0f;
    int have_prev = 0, stalled = 0;
    memset(r, 0, sizeof *r);
    r->emerge_real_ms = -1.0f;
    r->trigger_real_ms = -1.0f;
    boot_seq_begin(&s);

    while (t < max_real_ms) {
        float dt = FRAME_MS;
        if (!stalled && sc->stall_at >= 0.0f && t >= sc->stall_at) {
            dt = sc->stall_ms; stalled = 1;
        }
        t += dt;
        if (sc->xmb_at >= 0.0f && t >= sc->xmb_at && !(s.sig & BOOT_SIG_XMB)) {
            boot_seq_signal(&s, BOOT_SIG_XMB);
            if (r->trigger_real_ms < 0.0f) r->trigger_real_ms = t;
        }
        if (sc->leave_at >= 0.0f && t >= sc->leave_at && !(s.sig & BOOT_SIG_LEAVE)) {
            boot_seq_signal(&s, BOOT_SIG_LEAVE);
            if (r->trigger_real_ms < 0.0f) r->trigger_real_ms = t;
        }
        if (sc->skip_at >= 0.0f && t >= sc->skip_at && t - dt < sc->skip_at)
            boot_seq_signal(&s, BOOT_SIG_SKIP);

        boot_seq_step(&s, dt);
        boot_seq_frame(&s, &f);
        r->frames++;

        // The handoff starts with SHINE now; EMERGE is a window inside it.
        if (f.phase == BOOT_SHINE && r->emerge_real_ms < 0.0f) r->emerge_real_ms = t;
        if (f.xmb_visible && !(s.sig & BOOT_SIG_XMB)) r->xmb_before_ready++;
        if (f.phase == BOOT_DARK && f.mark_opacity > 0.0f) r->mark_in_dark++;
        if (bad01(f.mark_opacity) || bad01(f.halo) || bad01(f.status) ||
            bad01(f.veil) || bad01(f.dock_pos) || bad01(f.dock_size))
            r->bad_values++;
        for (int i = 0; i < BOOT_WORD_LETTERS; i++)
            if (!(f.word_reveal[i] >= 0.0f && f.word_reveal[i] <= 1.1f) ||
                bad01(f.word_alpha[i]))
                r->bad_values++;

        // Continuity -- except across a deliberate skip, which is a cut the
        // user asked for, and across the one stalled frame (clamped, but still
        // up to BOOT_MAX_STEP_MS of motion).
        int skip_frame = (sc->skip_at >= 0.0f && t >= sc->skip_at && t - dt < sc->skip_at + FRAME_MS);
        if (have_prev && !skip_frame && dt <= FRAME_MS + 0.01f) {
#define POPCK(field, lim) \
            if (fabsf(f.field - prev.field) > (lim)) \
                pop(r, #field, t, prev.field, f.field)
            POPCK(mark_opacity, MAXD_OPACITY);
            POPCK(halo, MAXD_HALO);
            POPCK(status, MAXD_STATUS);
            POPCK(veil, MAXD_VEIL);
            POPCK(dock_pos, MAXD_DOCK);
            POPCK(dock_size, MAXD_DOCK);
            POPCK(mark_scale, 0.01f);
            POPCK(glint, 0.12f);
            POPCK(glint_pos, 0.10f);
            POPCK(spark, 0.15f);      // a twinkle: quick, but never a cut
            POPCK(spark_rot, 0.02f);
            for (int i = 0; i < BOOT_WORD_LETTERS; i++) {
                POPCK(word_reveal[i], MAXD_REVEAL);
                POPCK(word_alpha[i], MAXD_WORD);
            }
#undef POPCK
        }
        prev = f; have_prev = 1;

        if (f.done) { r->reached_done = 1; r->done_real_ms = t; break; }
    }
    r->last = f;
}

static void report_pops(const char *name, const Result *r)
{
    char b[128];
    snprintf(b, sizeof b, "%s: no frame-to-frame pops (%d)", name, r->pops);
    ck(r->pops == 0, b);
    snprintf(b, sizeof b, "%s: no XMB visible before it is ready", name);
    ck(r->xmb_before_ready == 0, b);
    snprintf(b, sizeof b, "%s: nothing drawn in DARK", name);
    ck(r->mark_in_dark == 0, b);
    snprintf(b, sizeof b, "%s: every value in range", name);
    ck(r->bad_values == 0, b);
}

// ---------------------------------------------------------------------------
// 3. early readiness: the XMB is up before the mark has even appeared
// ---------------------------------------------------------------------------
static void test_early_ready(void)
{
    Scenario sc = { 10.0f, -1.0f, -1.0f, -1.0f, 0.0f };
    Result r;
    run(&sc, 60000.0f, &r);
    report_pops("early", &r);
    ck(r.reached_done, "early: reaches DONE");
    // EMERGE waits for the minimum -- and not a frame more.
    float min_pre = BOOT_DARK_MIN_MS + BOOT_EMBLEM_MS + BOOT_HOLD_MIN_MS;
    printf("  early: handoff at %.0f ms (minimum %.0f), DONE at %.0f ms\n",
           r.emerge_real_ms, min_pre, r.done_real_ms);
    ck(r.emerge_real_ms >= min_pre - 0.5f, "early: mark gets its minimum time");
    ck(r.emerge_real_ms <= min_pre + 4 * FRAME_MS, "early: no extra delay");
    ck(r.done_real_ms <= r.emerge_real_ms + BOOT_HANDOFF_END_MS + 2 * FRAME_MS,
       "early: handoff takes exactly its length");
}

// ---------------------------------------------------------------------------
// 4. delayed readiness: a slow network keeps the mark up for 12 s
// ---------------------------------------------------------------------------
static void test_late_ready(void)
{
    Scenario sc = { 12000.0f, -1.0f, -1.0f, -1.0f, 0.0f };
    Result r;
    run(&sc, 60000.0f, &r);
    report_pops("late", &r);
    ck(r.reached_done, "late: reaches DONE");
    ck(r.emerge_real_ms >= 12000.0f && r.emerge_real_ms <= 12000.0f + 2 * FRAME_MS,
       "late: handoff the frame after the XMB arrives");
    ck(r.done_real_ms - r.trigger_real_ms <= BOOT_HANDOFF_END_MS + 3 * FRAME_MS,
       "late: DONE within one handoff of readiness");

    // While waiting: the halo keeps breathing (never frozen, never off) and
    // the status line appears only after its delay.
    BootSeq s; BootFrame f;
    boot_seq_begin(&s);
    float t = 0.0f, hmin = 2.0f, hmax = -1.0f;
    int status_early = 0, status_late = 0;
    while (t < 11000.0f) {
        boot_seq_step(&s, FRAME_MS); t += FRAME_MS;
        boot_seq_frame(&s, &f);
        if (f.phase == BOOT_AWAIT) {
            if (f.halo < hmin) hmin = f.halo;
            if (f.halo > hmax) hmax = f.halo;
            if (s.t_phase < BOOT_STATUS_DELAY_MS && f.status > 0.0f) status_early++;
            if (s.t_phase > BOOT_STATUS_DELAY_MS + BOOT_STATUS_FADE_MS &&
                f.status < 1.0f) status_late++;
        }
    }
    ck(f.phase == BOOT_AWAIT, "late: still AWAITing with no XMB");
    ck(hmin >= 0.59f && hmax <= 1.0f && hmax - hmin > 0.3f,
       "late: halo breathes between 60% and 100%");
    ck(status_early == 0, "late: no status line before its delay");
    ck(status_late == 0, "late: status fully shown after its fade");
}

// ---------------------------------------------------------------------------
// 5. a login screen instead of the XMB (and a network failure)
// ---------------------------------------------------------------------------
static void test_leave(void)
{
    // Mid fade-in: fades OUT from where it was, never brightening first.
    {
        BootSeq s; BootFrame f;
        boot_seq_begin(&s);
        for (int i = 0; i < 40; i++) boot_seq_step(&s, FRAME_MS);   // ~667 ms
        boot_seq_frame(&s, &f);
        float before = f.mark_opacity;
        ck(f.phase == BOOT_EMBLEM && before > 0.0f && before < 1.0f,
           "leave: test point is mid fade-in");
        boot_seq_signal(&s, BOOT_SIG_LEAVE);
        boot_seq_step(&s, FRAME_MS);
        boot_seq_frame(&s, &f);
        ck(f.phase == BOOT_DISMISS, "leave: DISMISS");
        ck(f.mark_opacity <= before + 1e-6f, "leave: never brightens first");
        ck(fabsf(f.mark_opacity - before) < MAXD_OPACITY, "leave: no pop");
        int n = 0;
        while (!boot_seq_done(&s) && n < 1000) { boot_seq_step(&s, FRAME_MS); n++; }
        ck(boot_seq_done(&s), "leave: DONE");
        ck(n * FRAME_MS <= BOOT_DISMISS_MS + 2 * FRAME_MS, "leave: within DISMISS_MS");
        boot_seq_frame(&s, &f);
        ck(f.mark_opacity == 0.0f && f.halo == 0.0f && !f.xmb_visible,
           "leave: ends on black");
    }
    // In DARK nothing is showing, so there is nothing to fade: straight to DONE.
    {
        BootSeq s;
        boot_seq_begin(&s);
        boot_seq_step(&s, FRAME_MS);
        boot_seq_signal(&s, BOOT_SIG_LEAVE);
        boot_seq_step(&s, FRAME_MS);
        ck(boot_seq_done(&s), "leave in DARK: DONE immediately");
    }
    // Once the XMB is being revealed, LEAVE means nothing and is ignored.
    {
        BootSeq s; BootFrame f;
        boot_seq_begin(&s);
        boot_seq_signal(&s, BOOT_SIG_XMB);
        while (s.phase != BOOT_SHINE) boot_seq_step(&s, FRAME_MS);
        boot_seq_signal(&s, BOOT_SIG_LEAVE);
        boot_seq_step(&s, FRAME_MS);
        boot_seq_frame(&s, &f);
        ck(f.phase != BOOT_DISMISS && !s.left && f.xmb_visible,
           "leave once the handoff has started is ignored");
    }
    // A dismiss with the status line up fades that too.
    {
        Scenario sc = { -1.0f, 7000.0f, -1.0f, -1.0f, 0.0f };
        Result r;
        run(&sc, 20000.0f, &r);
        report_pops("leave-late", &r);
        ck(r.reached_done, "leave-late: DONE");
        ck(r.last.status == 0.0f, "leave-late: status gone");
    }
}

// ---------------------------------------------------------------------------
// 6. skip
// ---------------------------------------------------------------------------
static void test_skip(void)
{
    // A press before the XMB exists is DISCARDED, not latched: it must not
    // throw away the reveal the moment the XMB turns up seconds later.
    {
        Scenario sc = { 3000.0f, -1.0f, 500.0f, -1.0f, 0.0f };
        Result r;
        run(&sc, 60000.0f, &r);
        ck(r.reached_done, "early skip: DONE");
        ck(r.emerge_real_ms > 0.0f, "early skip: the reveal still plays");
        ck(r.done_real_ms >= r.emerge_real_ms + BOOT_HANDOFF_END_MS - 2 * FRAME_MS,
           "early skip: the full handoff still plays");
    }
    // After the XMB is up, a press ends it at once -- from every phase.
    static const BootPhase at[] = { BOOT_DARK, BOOT_EMBLEM, BOOT_AWAIT,
                                    BOOT_SHINE, BOOT_EMERGE, BOOT_DOCK, BOOT_WORDMARK };
    for (unsigned k = 0; k < sizeof at / sizeof at[0]; k++) {
        BootSeq s; BootFrame f;
        char b[80];
        boot_seq_begin(&s);
        boot_seq_signal(&s, BOOT_SIG_XMB);
        int guard = 0;
        while (s.phase != at[k] && guard++ < 10000) boot_seq_step(&s, FRAME_MS);
        ck(boot_seq_skip_effective(&s), "skip: effective once the XMB is up");
        boot_seq_signal(&s, BOOT_SIG_SKIP);
        boot_seq_step(&s, FRAME_MS);
        boot_seq_frame(&s, &f);
        snprintf(b, sizeof b, "skip from phase %d: DONE next frame", (int)at[k]);
        ck(f.done, b);
        ck(f.veil == 1.0f && f.dock_pos == 1.0f && f.dock_size == 1.0f,
           "skip: lands on the static lockup");
    }
    // Not effective before the XMB, or once done.
    {
        BootSeq s;
        boot_seq_begin(&s);
        ck(!boot_seq_skip_effective(&s), "skip: not effective before the XMB");
        boot_seq_signal(&s, BOOT_SIG_LEAVE);
        boot_seq_step(&s, FRAME_MS);
        ck(!boot_seq_skip_effective(&s), "skip: not effective once done");
    }
}

// ---------------------------------------------------------------------------
// 7. stalls and garbage frame times
// ---------------------------------------------------------------------------
static void test_stalls(void)
{
    // One 700 ms frame (a PNG decode, http_init) moves the clock at most
    // BOOT_MAX_STEP_MS: the animation pauses rather than skipping ahead.
    BootSeq s;
    boot_seq_begin(&s);
    boot_seq_step(&s, 700.0f);
    ck(s.t == BOOT_MAX_STEP_MS, "stall: clock clamped");
    boot_seq_step(&s, -40.0f);
    ck(s.t == BOOT_MAX_STEP_MS, "negative dt: no motion");
    boot_seq_step(&s, NAN);
    ck(s.t == BOOT_MAX_STEP_MS, "NaN dt: no motion");

    Scenario sc = { 2500.0f, -1.0f, -1.0f, 1500.0f, 900.0f };
    Result r;
    run(&sc, 60000.0f, &r);
    report_pops("stall", &r);
    ck(r.reached_done, "stall: DONE");
}

// ---------------------------------------------------------------------------
// 8. the last frame, exactly
// ---------------------------------------------------------------------------
static void test_final_frame(void)
{
    Scenario sc = { 0.0f, -1.0f, -1.0f, -1.0f, 0.0f };
    Result r;
    run(&sc, 60000.0f, &r);
    const BootFrame *f = &r.last;
    ck(f->done && f->phase == BOOT_DONE, "final: DONE");
    ck(f->veil == 1.0f, "final: veil exactly lifted");
    ck(f->dock_pos == 1.0f && f->dock_size == 1.0f, "final: mark exactly docked");
    ck(f->halo == 0.0f && f->status == 0.0f, "final: halo and status gone");
    int ok = 1;
    for (int i = 0; i < BOOT_WORD_LETTERS; i++)
        if (f->word_reveal[i] != 1.0f || f->word_alpha[i] != 1.0f) ok = 0;
    ck(ok, "final: every letter exactly home and opaque");

    // And DONE is sticky: stepping it further changes nothing.
    BootSeq s; BootFrame a, b;
    boot_seq_begin(&s);
    boot_seq_signal(&s, BOOT_SIG_XMB);
    while (!boot_seq_done(&s)) boot_seq_step(&s, FRAME_MS);
    boot_seq_frame(&s, &a);
    for (int i = 0; i < 100; i++) boot_seq_step(&s, FRAME_MS);
    boot_seq_frame(&s, &b);
    ck(memcmp(&a, &b, sizeof a) == 0, "final: DONE is sticky");

    // The DOCK stays on the GPU until the veil is gone: at the moment the
    // veil clears, the mark must already be close to its final size, so the
    // switch to the lockup's CPU path happens late in the motion.
    BootSeq q; BootFrame g;
    boot_seq_begin(&q);
    boot_seq_signal(&q, BOOT_SIG_XMB);
    while (q.phase != BOOT_EMERGE) boot_seq_step(&q, FRAME_MS);
    while (q.t_hand < BOOT_EMERGE_START_MS + BOOT_EMERGE_MS) boot_seq_step(&q, FRAME_MS);
    boot_seq_frame(&q, &g);
    ck(g.dock_size > 0.9f, "veil clears when the mark is >90% docked in size");
}

// ---------------------------------------------------------------------------
// 9. determinism across a reset
// ---------------------------------------------------------------------------
static void test_determinism(void)
{
    BootFrame a[400], b[400];
    for (int pass = 0; pass < 2; pass++) {
        BootSeq s;
        BootFrame *out = pass ? b : a;
        boot_seq_begin(&s);
        for (int i = 0; i < 400; i++) {
            if (i == 90) boot_seq_signal(&s, BOOT_SIG_XMB);
            boot_seq_step(&s, (i % 7 == 0) ? 33.3f : FRAME_MS);
            boot_seq_frame(&s, &out[i]);
        }
    }
    ck(memcmp(a, b, sizeof a) == 0, "determinism: identical frames after reset");

    // A second full boot on the same struct (re-entering after a logout would
    // do this) starts from DARK with no residue.
    BootSeq s; BootFrame f;
    boot_seq_begin(&s);
    boot_seq_signal(&s, BOOT_SIG_XMB | BOOT_SIG_LEAVE);
    for (int i = 0; i < 500; i++) boot_seq_step(&s, FRAME_MS);
    boot_seq_begin(&s);
    boot_seq_frame(&s, &f);
    ck(s.phase == BOOT_DARK && s.sig == 0 && s.t == 0.0f && f.mark_opacity == 0.0f,
       "reset: back to DARK, no signals");
}

// ---------------------------------------------------------------------------
// 10. nothing wedges: random interleavings
// ---------------------------------------------------------------------------
static unsigned rng = 12345u;
static float frand(float lo, float hi)
{
    rng = rng * 1103515245u + 12345u;
    return lo + (hi - lo) * (float)((rng >> 8) & 0xFFFF) / 65535.0f;
}

static void test_random(void)
{
    verbose_pops = 0;
    int stuck = 0, late = 0, pops = 0, badv = 0;
    float worst = 0.0f;
    for (int n = 0; n < 3000; n++) {
        Scenario sc;
        int kind = n % 3;
        sc.xmb_at   = (kind != 1) ? frand(0.0f, 20000.0f) : -1.0f;
        sc.leave_at = (kind != 0) ? frand(0.0f, 20000.0f) : -1.0f;
        sc.skip_at  = (n % 5 == 0) ? frand(0.0f, 25000.0f) : -1.0f;
        sc.stall_at = (n % 4 == 0) ? frand(0.0f, 20000.0f) : -1.0f;
        sc.stall_ms = frand(20.0f, 3000.0f);
        Result r;
        run(&sc, 120000.0f, &r);
        if (!r.reached_done) { stuck++; continue; }
        float after = r.done_real_ms - r.trigger_real_ms;
        // From the first readiness signal the worst case is: finish the
        // mark's minimum time, then a full handoff (or a dismiss), plus one
        // stalled frame's worth of clamped time.
        float bound = BOOT_DARK_MIN_MS + BOOT_EMBLEM_MS + BOOT_HOLD_MIN_MS +
                      BOOT_HANDOFF_END_MS + BOOT_MAX_STEP_MS + 4 * FRAME_MS;
        // A stalled frame costs its REAL length while moving the clock only
        // BOOT_MAX_STEP_MS -- that pause is the point of the clamp.
        if (sc.stall_at >= 0.0f) bound += sc.stall_ms;
        if (after > worst) worst = after;
        if (after > bound) late++;
        if (r.pops && !pops) {
            // Replay the first offender loudly, so a failure names itself.
            Result q;
            printf("  first popping run: xmb=%.0f leave=%.0f skip=%.0f "
                   "stall=%.0f/%.0f\n", sc.xmb_at, sc.leave_at, sc.skip_at,
                   sc.stall_at, sc.stall_ms);
            verbose_pops = 1;
            run(&sc, 120000.0f, &q);
            verbose_pops = 0;
        }
        pops += r.pops;
        badv += r.bad_values + r.xmb_before_ready + r.mark_in_dark;
    }
    printf("  random: 3000 runs, worst DONE %.0f ms after readiness\n", worst);
    ck(stuck == 0, "random: every run reaches DONE");
    ck(late == 0, "random: every run finishes within the bound");
    ck(pops == 0, "random: no pops");
    ck(badv == 0, "random: no out-of-range or premature values");
}

// ---------------------------------------------------------------------------
// 11. geometry
// ---------------------------------------------------------------------------
static void test_veil_rows(void)
{
    float ys[BOOT_VEIL_ROWS_MAX];
    unsigned char as[BOOT_VEIL_ROWS_MAX];
    const float H = 1080.0f;

    int n = boot_veil_rows(0.0f, H, ys, as);
    ck(n == 2 && ys[0] == 0.0f && ys[1] == H && as[0] == 255 && as[1] == 255,
       "veil 0: one solid black quad over the whole screen");
    ck(boot_veil_rows(1.0f, H, ys, as) == 0, "veil 1: nothing to draw");
    ck(boot_veil_rows(0.5f, 0.0f, ys, as) == 0, "veil: zero height draws nothing");

    int ok = 1, first_black = 1;
    for (int i = 0; i <= 200; i++) {
        float v = i / 200.0f;
        n = boot_veil_rows(v, H, ys, as);
        if (n > BOOT_VEIL_ROWS_MAX) ok = 0;
        for (int j = 0; j < n; j++) {
            if (ys[j] < 0.0f || ys[j] > H) ok = 0;
            if (j && ys[j] < ys[j - 1]) ok = 0;          // top to bottom
            if (j && as[j] > as[j - 1]) ok = 0;          // clears downward
        }
        // Until the band's top edge passes the top of the screen, the top row
        // stays fully black: the chrome is the LAST thing revealed.
        float band_top = H - v * (H + BOOT_VEIL_BAND * H);
        if (n && band_top > 0.0f && as[0] != 255) first_black = 0;
    }
    ck(ok, "veil rows: clipped, ordered, alpha non-increasing downward");
    ck(first_black, "veil: top of screen is last to clear");

    // Bottom clears first: early in EMERGE the bottom row is (nearly) clear
    // while the top is still solid -- the wave shows before the chrome.
    n = boot_veil_rows(0.3f, H, ys, as);
    ck(n >= 2 && as[0] == 255 && as[n - 1] < 64 && ys[n - 1] == H,
       "veil 0.3: bottom already clearing, top still black");
}

static void test_dock(void)
{
    float x, y;
    boot_dock_point(960.0f, 508.0f, 70.0f, 47.0f, 0.0f, &x, &y);
    ck(x == 960.0f && y == 508.0f, "dock path: exact start");
    boot_dock_point(960.0f, 508.0f, 70.0f, 47.0f, 1.0f, &x, &y);
    ck(x == 70.0f && y == 47.0f, "dock path: exact end");

    // Lifts first: the opening move is more up than left ...
    float x1, y1, x2, y2;
    boot_dock_point(960.0f, 508.0f, 70.0f, 47.0f, 0.02f, &x1, &y1);
    ck((508.0f - y1) > 0.0f && (960.0f - x1) > 0.0f, "dock path: starts up and left");
    // ... and glides in LEFT: the landing move is more left than up, along
    // the line the wordmark then unfolds on.
    boot_dock_point(960.0f, 508.0f, 70.0f, 47.0f, 0.98f, &x2, &y2);
    ck((x2 - 70.0f) > (y2 - 47.0f), "dock path: lands moving mostly left");

    // Stays inside the box spanned by its ends (no swing off-screen).
    int inside = 1;
    for (int i = 0; i <= 100; i++) {
        boot_dock_point(960.0f, 508.0f, 70.0f, 47.0f, i / 100.0f, &x, &y);
        if (x < 70.0f - 0.01f || x > 960.0f + 0.01f ||
            y < 47.0f - 0.01f || y > 508.0f + 0.01f) inside = 0;
    }
    ck(inside, "dock path: within the bounding box");

    // Size: exact ends, monotonic, geometric (the midpoint is the geometric
    // mean, not the arithmetic one).
    ck(boot_dock_size_px(184.0f, 31.0f, 0.0f) == 184.0f, "dock size: exact start");
    ck(boot_dock_size_px(184.0f, 31.0f, 1.0f) == 31.0f, "dock size: exact end");
    ck(fabsf(boot_dock_size_px(184.0f, 31.0f, 0.5f) - sqrtf(184.0f * 31.0f)) < 0.01f,
       "dock size: geometric midpoint");
    float prev = 1e9f; int mono = 1;
    for (int i = 0; i <= 100; i++) {
        float v = boot_dock_size_px(184.0f, 31.0f, i / 100.0f);
        if (v > prev + 1e-4f) mono = 0;
        prev = v;
    }
    ck(mono, "dock size: shrinks monotonically");
}

// ---------------------------------------------------------------------------
// 12. the shine: the mark catches the light, THEN moves
// ---------------------------------------------------------------------------
static void test_shine(void)
{
    // The glint profile: full at its centre, gone past its width.
    ck(boot_glint_at(0.5f, 0.0f, (0.5f) / (1.0f + BOOT_GLINT_TILT)) == 1.0f,
       "glint: 1 at the band centre");
    ck(boot_glint_at(0.0f, 0.0f, 0.5f) == 0.0f, "glint: 0 outside the band");

    // Where the apex is in s units: the bell's top, centred -- measured from
    // the 256px raster at (127.5, 23) of 256.
    const float s_apex = (0.5f + BOOT_GLINT_TILT * (23.0f / 256.0f)) /
                         (1.0f + BOOT_GLINT_TILT);

    BootSeq s; BootFrame f;
    boot_seq_begin(&s);
    boot_seq_signal(&s, BOOT_SIG_XMB);
    while (s.phase != BOOT_SHINE) boot_seq_step(&s, FRAME_MS);

    int moved_early = 0, glint_backwards = 0, glint_seen = 0;
    float gpeak = 0.0f, prev_pos = -1e9f, spark_peak = 0.0f, pos_at_spark = 0.0f;
    float glint_left_at = -1.0f, spark_at_dock = 0.0f;
    while (!boot_seq_done(&s)) {
        boot_seq_step(&s, FRAME_MS);
        boot_seq_frame(&s, &f);
        const float th = s.t_hand;
        if (th < BOOT_DOCK_START_MS && f.dock_pos != 0.0f) moved_early++;
        if (f.glint > 0.0f) {
            glint_seen = 1;
            if (f.glint_pos < prev_pos) glint_backwards++;
            prev_pos = f.glint_pos;
            if (f.glint > gpeak) gpeak = f.glint;
            glint_left_at = th;
        }
        if (f.spark > spark_peak) { spark_peak = f.spark; pos_at_spark = f.glint_pos; }
        if (th >= BOOT_DOCK_START_MS && spark_at_dock == 0.0f) spark_at_dock = f.spark + 1e-9f;
        if (f.phase == BOOT_SHINE && f.xmb_visible && f.veil != 0.0f) moved_early++;
    }
    printf("  shine: glint peak %.2f, twinkle peak %.2f with the glint at s=%.2f "
           "(apex s=%.2f), twinkle %.2f when the mark starts to move\n",
           gpeak, spark_peak, pos_at_spark, s_apex, spark_at_dock);
    ck(moved_early == 0, "shine: the mark holds still and the veil stays shut during SHINE");
    ck(glint_seen && gpeak > 0.98f, "shine: the glint reaches full strength");
    ck(glint_backwards == 0, "shine: the glint only ever sweeps forward");
    ck(glint_left_at <= BOOT_DOCK_START_MS, "shine: the glint is gone before the mark moves");
    ck(spark_peak > 0.98f, "shine: the twinkle reaches full strength");
    ck(fabsf(pos_at_spark - s_apex) < 0.25f,
       "shine: the twinkle peaks as the glint passes the apex");
    ck(spark_at_dock < 0.15f, "shine: the twinkle has all but faded when the mark moves");
    ck(BOOT_GLINT_START_MS + BOOT_GLINT_MS <= BOOT_DOCK_START_MS &&
       BOOT_SHINE_MS >= 900.0f, "shine: about a second, all before the move");
}

// ---------------------------------------------------------------------------
// 13. the reveal is paced to the dock: the shelves stay dark while the mark
//     sits over them and are fully shown once it has moved away
// ---------------------------------------------------------------------------
static float veil_alpha_at(float v, float H, float y)
{
    float ys[BOOT_VEIL_ROWS_MAX];
    unsigned char as[BOOT_VEIL_ROWS_MAX];
    int n = boot_veil_rows(v, H, ys, as);
    if (n == 0) return 0.0f;
    if (y <= ys[0]) return as[0];
    for (int i = 0; i + 1 < n; i++)
        if (y <= ys[i + 1]) {
            float k = (ys[i + 1] > ys[i]) ? (y - ys[i]) / (ys[i + 1] - ys[i]) : 0.0f;
            return as[i] + (as[i + 1] - as[i]) * k;
        }
    return as[n - 1];
}

static void test_veil_sync(void)
{
    const float H = 1080.0f;
    const float shelves[3] = { 0.30f * H, 0.47f * H, 0.70f * H };   // posters
    BootSeq s; BootFrame f;
    boot_seq_begin(&s);
    boot_seq_signal(&s, BOOT_SIG_XMB);
    int dark_fail = 0, clear_fail = 0;
    float pos_when_clear = -1.0f;
    while (!boot_seq_done(&s)) {
        boot_seq_step(&s, FRAME_MS);
        boot_seq_frame(&s, &f);
        if (!f.xmb_visible) continue;
        float mean = 0.0f, least = 0.0f;
        for (int i = 0; i < 3; i++) {
            float a = veil_alpha_at(f.veil, H, shelves[i]);
            mean += a / 3.0f;
            if (a > least) least = a;
        }
        // While the mark is still over the shelves (the first ~20% of its
        // path, measured on the 1080p arc) they stay at least ~70% dark.
        if (f.dock_pos <= 0.2f && mean < 180.0f) dark_fail++;
        // Once it is clear of them (~75%), every poster row is fully shown.
        if (f.dock_pos >= 0.75f && least > 8.0f) clear_fail++;
        if (pos_when_clear < 0.0f && least <= 8.0f) pos_when_clear = f.dock_pos;
    }
    printf("  reveal: the shelves are fully shown when the mark is %.0f%% of the "
           "way to the lockup\n", pos_when_clear * 100.0f);
    ck(dark_fail == 0, "reveal: the shelves stay >=70% dark while the mark is over them");
    ck(clear_fail == 0, "reveal: the shelves are fully shown once the mark is 75% docked");
    ck(pos_when_clear >= 0.5f, "reveal: the shelves do not clear before the mark has left");
}

int main(void)
{
    printf("test_boot_seq\n");
    verbose_pops = 1;
    test_easing();
    test_constants();
    test_early_ready();
    test_late_ready();
    test_leave();
    test_skip();
    test_stalls();
    test_final_frame();
    test_determinism();
    test_random();
    test_veil_rows();
    test_dock();
    test_shine();
    test_veil_sync();
    if (fails) { printf("test_boot_seq: %d FAILED\n", fails); return 1; }
    printf("test_boot_seq: all passed\n");
    return 0;
}
