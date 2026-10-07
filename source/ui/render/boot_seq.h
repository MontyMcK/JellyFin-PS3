/* boot_seq.h -- the cold-boot animation's timeline, as pure data.
 *
 * Everything the boot animation SHOWS is a function of this state machine,
 * and this state machine is a function of nothing but elapsed time and a few
 * readiness signals.  No RSX, no PSL1GHT, no globals: the renderer
 * (boot_anim.cpp) asks it "what does this frame look like" and draws the
 * answer.  That split is what lets tests/test_boot_seq.c drive every path --
 * early readiness, late readiness, a user skip, a login screen instead of the
 * XMB -- on the host, frame by frame, and prove none of them can wedge.
 *
 * THE SEQUENCE (docs/boot-animation.md has the long form)
 *
 *   DARK      pure black while the renderer finishes coming up
 *   EMBLEM    the Jellyfin mark fades in, centred, settling from 94% scale
 *   AWAIT     the mark holds; a soft halo breathes behind it.  Leaves only
 *             when the XMB is actually drawing frames AND the mark has been
 *             fully visible for BOOT_HOLD_MIN_MS
 *   SHINE     before anything moves, the mark catches the light: a soft
 *             glint sweeps across it and a star twinkles on its apex
 *   EMERGE    the XMB rises out of the black from the bottom edge up -- the
 *             wave first, then the shelves, the chrome last -- paced to the
 *             DOCK so the middle of the screen only clears once the mark has
 *             left it
 *   DOCK      the mark shrinks and glides into the lockup's top-left slot
 *   WORDMARK  J-E-L-L-Y-F-I-N unfolds rightward out of the docked mark
 *   DONE      the static lockup; the XMB owns every pixel again
 *
 *   DISMISS   startup is NOT going to the XMB (no server yet, no login, the
 *             network failed): the mark fades back to black and the caller's
 *             own screen takes over
 *
 * SHINE, EMERGE, DOCK and WORDMARK overlap -- they are windows on ONE clock that
 * starts when AWAIT is left, which is what makes the handoff read as a single
 * motion rather than three steps.  `phase` names the latest window that has
 * opened.
 *
 * TIMING RULES
 *
 *   - The clock advances by the real frame time, clamped to BOOT_MAX_STEP_MS.
 *     A frame that stalls on a blocking init step (http_init, a PNG decode)
 *     therefore PAUSES the animation instead of letting it jump ahead, so a
 *     fade never visibly skips.
 *   - Before the XMB is ready the minimum on-screen time is DARK + EMBLEM +
 *     HOLD_MIN (1.45 s).  Cold init measured ~1.3-1.5 s on hardware
 *     (player_log 2026-09-22), so the minimum costs nothing in practice.
 *   - After the XMB is ready the handoff is a fixed BOOT_HANDOFF_END_MS, and
 *     the XMB is live throughout it: any button press skips straight to DONE.
 *     The animation never makes the UI wait.
 *   - AWAIT has no timeout BY DESIGN: its exit is the app's own readiness,
 *     which the app blocks on regardless.  What AWAIT guarantees instead is
 *     that the screen keeps moving (the halo) and, after BOOT_STATUS_DELAY_MS,
 *     says what it is waiting for.  Every other state ends on time alone.
 *
 * Pure C (C99 and C++), header-only, like the wave_*.h kernels.  <math.h> is
 * used for one powf per frame (the geometric scale interpolation); nothing
 * here runs per pixel or per vertex.
 */
#ifndef JF_BOOT_SEQ_H
#define JF_BOOT_SEQ_H

#include <math.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- timing (milliseconds) ---------------------------------------------- */

#define BOOT_MAX_STEP_MS        50.0f   /* per-frame clock clamp (see above)  */

#define BOOT_DARK_MIN_MS       200.0f   /* black before anything appears      */
#define BOOT_EMBLEM_MS         900.0f   /* mark fade-in                       */
#define BOOT_HOLD_MIN_MS       350.0f   /* mark fully visible, at least       */
#define BOOT_STATUS_DELAY_MS  4000.0f   /* AWAIT this long -> show status     */
#define BOOT_STATUS_FADE_MS    600.0f
#define BOOT_BREATH_MS        3200.0f   /* halo breathing period              */
#define BOOT_DISMISS_MS        450.0f   /* mark fade-out when not going to XMB */

/* The handoff clock, zero when AWAIT is left.  Windows overlap on purpose.
 *
 *   0       SHINE     the mark holds still and catches the light (~1 s)
 *   300-950   glint sweeps left to right across the mark
 *   340-940   a star twinkles on the apex, peaking (580) as the glint
 *             crosses it
 *   920     EMERGE    the veil starts lifting at the bottom edge, just as
 *   950     DOCK      the mark starts to move -- only after the shine
 *   ~1330             mark ~30% along and still over the shelves: they
 *                     are still ~70% dark
 *   ~1500             mark ~75% along, clear of the shelves: every poster
 *                     row fully shown
 *   1770              veil fully gone; the mark is all but landed
 *   1780    WORDMARK
 *
 * The veil is timed to the DOCK, not to the SHINE: fitted so the shelves
 * stay dark while the mark sits over them and clear as it leaves.  (A veil
 * that started with the shine revealed the posters around a mark that had
 * barely moved -- the dock eases in slowly.)  test_veil_sync pins this.
 */
#define BOOT_SHINE_MS         1000.0f
#define BOOT_GLINT_START_MS    300.0f
#define BOOT_GLINT_MS          650.0f
#define BOOT_SPARK_START_MS    340.0f
#define BOOT_SPARK_MS          600.0f
#define BOOT_SPARK_RISE        0.40f    /* fraction of the twinkle spent rising */
#define BOOT_EMERGE_START_MS   920.0f
#define BOOT_EMERGE_MS         850.0f
#define BOOT_DOCK_START_MS     950.0f   /* after the shine: nothing moves while it plays */
#define BOOT_DOCK_MS           900.0f
#define BOOT_WORD_START_MS    1780.0f   /* just after the veil is gone        */
#define BOOT_WORD_STAGGER_MS    45.0f   /* between successive letters         */
#define BOOT_WORD_LETTER_MS    380.0f   /* one letter's own reveal            */
#define BOOT_WORD_LETTERS        8      /* J E L L Y F I N                    */
#define BOOT_HANDOFF_END_MS   (BOOT_WORD_START_MS + \
                               (BOOT_WORD_LETTERS - 1) * BOOT_WORD_STAGGER_MS + \
                               BOOT_WORD_LETTER_MS)
#define BOOT_HALO_OUT_START_MS 850.0f   /* the halo leaves as the mark moves  */
#define BOOT_HALO_OUT_MS       500.0f
#define BOOT_STATUS_OUT_MS     500.0f   /* status line goes first thing       */

/* ---- look ---------------------------------------------------------------- */

#define BOOT_EMBLEM_SCALE0      0.94f   /* EMBLEM settles from this scale     */
#define BOOT_SCALE_LEAD         1.10f   /* DOCK size finishes at 1/this of u  */
#define BOOT_ARC_BOW            0.35f   /* dock path bow toward (x0, y1)      */
#define BOOT_WORD_OVERSHOOT     1.10f   /* easeOutBack c1: a ~4% spring       */
#define BOOT_WORD_FADE_FRAC     0.60f   /* a letter is opaque by this much    */
/* How far a letter slides into place, in em.  NOT the whole distance from
 * the mark: with ~8 letters in flight at once that stacked the back half of
 * the word into a glyph soup mid-reveal.  Letters near the mark still come
 * out of it (their distance is shorter than this); the left-to-right stagger
 * does the rest of the unfolding. */
#define BOOT_WORD_SLIDE_EM      0.90f
#define BOOT_VEIL_BAND          0.60f   /* soft edge, fraction of screen H    */
#define BOOT_HALO_PEAK          0.30f   /* halo alpha at the top of a breath  */
#define BOOT_HALO_SPARK_BOOST   0.35f   /* the halo swells with the twinkle   */
/* The glint: a soft band across the mark along s = (u + K*v) / (1 + K) in
 * the mark's own box (u,v in 0..1, v down) -- K 0.45 tilts it ~24 degrees
 * off vertical -- from s = -W to s = 1 + W, so it enters and leaves clean. */
#define BOOT_GLINT_TILT         0.45f
#define BOOT_GLINT_WIDTH        0.08f   /* a narrow streak, not a wash */
#define BOOT_GLINT_GAIN         0.55f   /* added brightness at the band's core */

/* ---- state --------------------------------------------------------------- */

typedef enum {
    BOOT_DARK = 0,
    BOOT_EMBLEM,
    BOOT_AWAIT,
    BOOT_SHINE,
    BOOT_EMERGE,
    BOOT_DOCK,
    BOOT_WORDMARK,
    BOOT_DISMISS,
    BOOT_DONE
} BootPhase;

/* Readiness signals.  XMB and LEAVE are latched: once raised they stay
 * raised.  SKIP is consumed by the next step, acted on or not. */
#define BOOT_SIG_XMB     0x1u   /* the XMB loop is drawing frames            */
#define BOOT_SIG_LEAVE   0x2u   /* startup is going somewhere else: dismiss  */
#define BOOT_SIG_SKIP    0x4u   /* the user pressed a button                 */

typedef struct {
    BootPhase    phase;
    unsigned     sig;          /* latched BOOT_SIG_*                         */
    float        t;            /* ms since boot_seq_begin (clamped clock)    */
    float        t_phase;      /* ms since the current pre-handoff phase     */
    float        t_hand;       /* handoff clock; valid from EMERGE on        */
    /* What was on screen when DISMISS / the handoff began, so each fades
     * out from where it WAS instead of jumping to a nominal value first. */
    float        from_mark;
    float        from_halo;
    float        from_status;
    float        from_scale;
    int          left;         /* DONE was reached by DISMISS, not the XMB   */
    unsigned     frames;       /* boot_seq_step calls, for the log           */
} BootSeq;

/* Everything one frame shows, normalised.  The renderer turns these into
 * pixels; nothing here knows the screen size. */
typedef struct {
    BootPhase phase;
    float mark_opacity;   /* 0..1, the centred / travelling mark             */
    float mark_scale;     /* EMBLEM settle only: 0.94..1                     */
    float halo;           /* 0..1 of BOOT_HALO_PEAK                          */
    float status;         /* 0..1, the "Connecting" line under the mark      */
    float veil;           /* 0..1: 0 = whole screen black, 1 = fully lifted  */
    float dock_pos;       /* 0..1 eased progress along the dock path         */
    float dock_size;      /* 0..1 eased progress centre size -> lockup size  */
    float glint;          /* 0..1 strength of the glint band this frame      */
    float glint_pos;      /* the band's centre, in boot_glint_at()'s s units */
    float spark;          /* 0..1 the apex twinkle                           */
    float spark_rot;      /* its rotation, radians                           */
    float word_reveal[BOOT_WORD_LETTERS];  /* pen travel 0..~1.04, lands 1   */
    float word_alpha[BOOT_WORD_LETTERS];   /* 0..1                           */
    int   xmb_visible;    /* 1 once any of the XMB can be seen               */
    int   done;
} BootFrame;

/* ---- easing -------------------------------------------------------------- *
 * All map [0,1] -> [0,1], hit both ends EXACTLY (the handoff relies on the
 * last frame being the static lockup to the pixel), and clamp their input. */

static inline float boot_clamp01(float x)
{
    return x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x);
}

static inline float boot_ease_out_cubic(float t)
{
    t = boot_clamp01(t);
    float u = 1.0f - t;
    return 1.0f - u * u * u;
}

static inline float boot_ease_in_cubic(float t)
{
    t = boot_clamp01(t);
    return t * t * t;
}

static inline float boot_ease_in_out_cubic(float t)
{
    t = boot_clamp01(t);
    if (t < 0.5f) return 4.0f * t * t * t;
    float u = -2.0f * t + 2.0f;
    return 1.0f - u * u * u * 0.5f;
}

/* Quintic smootherstep: zero velocity AND acceleration at both ends.  Used
 * for the veil, where the soft edge must not appear to start with a jolt. */
static inline float boot_ease_smoother(float t)
{
    float v;
    t = boot_clamp01(t);
    if (t <= 0.0f) return 0.0f;
    if (t >= 1.0f) return 1.0f;
    v = t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
    return v > 1.0f ? 1.0f : v;       /* rounding near 1 */
}

/* easeOutBack with a caller-chosen overshoot.  c1 = 1.70158 is the classic
 * ~10%; BOOT_WORD_OVERSHOOT's 1.1 gives ~4%, which reads as a letter arriving
 * with weight rather than bouncing.  Exactly 0 at 0 and 1 at 1. */
static inline float boot_ease_out_back(float t, float c1)
{
    float c3 = c1 + 1.0f;
    float u;
    t = boot_clamp01(t);
    if (t <= 0.0f) return 0.0f;       /* 1 - c3 + c1 is 0 only on paper */
    if (t >= 1.0f) return 1.0f;
    u = t - 1.0f;
    return 1.0f + c3 * u * u * u + c1 * u * u;
}

/* A smooth 0..1..0 pulse of the given period, from polynomials (no sinf):
 * a triangle wave run through smootherstep.  Starts at 0. */
static inline float boot_breath(float t_ms, float period_ms)
{
    if (period_ms <= 0.0f) return 0.0f;
    float ph = t_ms / period_ms;
    ph -= (float)(long)ph;                     /* frac, t >= 0 */
    float tri = ph < 0.5f ? ph * 2.0f : 2.0f - ph * 2.0f;
    return boot_ease_smoother(tri);
}

/* The glint's brightness at (u,v) in the mark's box (0..1, v down) with the
 * band centred on pos: a squared parabola, 1 at the centre, 0 by
 * BOOT_GLINT_WIDTH either side.  The renderer multiplies by the mark's own
 * coverage, so it only ever lights the mark. */
static inline float boot_glint_at(float u, float v, float pos)
{
    float sc = (u + BOOT_GLINT_TILT * v) / (1.0f + BOOT_GLINT_TILT);
    float d  = (sc - pos) / BOOT_GLINT_WIDTH;
    float b  = 1.0f - d * d;
    return b > 0.0f ? b * b : 0.0f;
}

/* ---- the machine --------------------------------------------------------- */

static inline void boot_seq_begin(BootSeq *s)
{
    s->phase = BOOT_DARK;
    s->sig = 0;
    s->t = 0.0f;
    s->t_phase = 0.0f;
    s->t_hand = 0.0f;
    s->from_mark = 0.0f;
    s->from_halo = 0.0f;
    s->from_status = 0.0f;
    s->from_scale = BOOT_EMBLEM_SCALE0;
    s->left = 0;
    s->frames = 0;
}

static inline void boot_seq_signal(BootSeq *s, unsigned sig)
{
    s->sig |= sig;
}

static inline int boot_seq_done(const BootSeq *s)
{
    return s->phase == BOOT_DONE;
}

/* Mark opacity and halo in the pre-handoff phases. */
static inline float boot__emblem_opacity(const BootSeq *s)
{
    switch (s->phase) {
    case BOOT_DARK:   return 0.0f;
    case BOOT_EMBLEM: return boot_ease_in_out_cubic(s->t_phase / BOOT_EMBLEM_MS);
    default:          return 1.0f;
    }
}

static inline float boot__emblem_halo(const BootSeq *s)
{
    switch (s->phase) {
    case BOOT_EMBLEM:
        /* Arrives with the back half of the fade-in. */
        return boot_ease_in_out_cubic((s->t_phase / BOOT_EMBLEM_MS - 0.4f) / 0.6f);
    case BOOT_AWAIT:
        /* Then breathes between 60% and 100%, so it is never simply off. */
        return 1.0f - 0.4f * boot_breath(s->t_phase, BOOT_BREATH_MS);
    default:
        return 0.0f;
    }
}

static inline float boot__emblem_scale(const BootSeq *s)
{
    switch (s->phase) {
    case BOOT_DARK:   return BOOT_EMBLEM_SCALE0;
    case BOOT_EMBLEM: return BOOT_EMBLEM_SCALE0 + (1.0f - BOOT_EMBLEM_SCALE0) *
                             boot_ease_out_cubic(s->t_phase / BOOT_EMBLEM_MS);
    default:          return 1.0f;
    }
}

static inline float boot__emblem_status(const BootSeq *s)
{
    if (s->phase != BOOT_AWAIT) return 0.0f;
    return boot_ease_in_out_cubic((s->t_phase - BOOT_STATUS_DELAY_MS) /
                                  BOOT_STATUS_FADE_MS);
}

static inline void boot__enter(BootSeq *s, BootPhase p)
{
    s->phase = p;
    s->t_phase = 0.0f;
}

/* Remember what is showing now, for the next phase to fade out from. */
static inline void boot__snapshot(BootSeq *s)
{
    s->from_mark   = boot__emblem_opacity(s);
    s->from_halo   = boot__emblem_halo(s);
    s->from_status = boot__emblem_status(s);
    s->from_scale  = boot__emblem_scale(s);
}

/* Handoff windows -> phase name. */
static inline BootPhase boot__hand_phase(float th)
{
    if (th >= BOOT_HANDOFF_END_MS) return BOOT_DONE;
    if (th >= BOOT_WORD_START_MS)  return BOOT_WORDMARK;
    if (th >= BOOT_DOCK_START_MS)  return BOOT_DOCK;
    if (th >= BOOT_EMERGE_START_MS) return BOOT_EMERGE;
    return BOOT_SHINE;
}

/* What the current frame shows. */
static inline void boot_seq_frame(const BootSeq *s, BootFrame *f)
{
    int i;
    f->phase = s->phase;
    f->done  = (s->phase == BOOT_DONE);
    f->mark_opacity = 0.0f;
    f->mark_scale   = 1.0f;
    f->halo = 0.0f;
    f->status = 0.0f;
    f->veil = 0.0f;
    f->dock_pos = 0.0f;
    f->dock_size = 0.0f;
    f->glint = 0.0f;
    f->glint_pos = -BOOT_GLINT_WIDTH;   /* parked at its start */
    f->spark = 0.0f;
    f->spark_rot = 0.0f;
    f->xmb_visible = 0;
    for (i = 0; i < BOOT_WORD_LETTERS; i++) {
        f->word_reveal[i] = 0.0f;
        f->word_alpha[i]  = 0.0f;
    }

    switch (s->phase) {
    case BOOT_DARK:
        /* Invisible, but already at the scale EMBLEM starts from. */
        f->mark_scale = BOOT_EMBLEM_SCALE0;
        return;

    case BOOT_EMBLEM:
    case BOOT_AWAIT:
        f->mark_opacity = boot__emblem_opacity(s);
        f->mark_scale   = boot__emblem_scale(s);
        f->halo   = boot__emblem_halo(s);
        f->status = boot__emblem_status(s);
        return;

    case BOOT_DISMISS: {
        /* Smootherstep, NOT ease-in: an ease-in ends at full speed, so its
         * last frame snapped the mark from ~11% to black. */
        float k = 1.0f - boot_ease_smoother(s->t_phase / BOOT_DISMISS_MS);
        f->mark_opacity = s->from_mark * k;
        f->mark_scale   = s->from_scale;   /* fades where it stood */
        f->halo         = s->from_halo * k;
        f->status       = s->from_status * k;
        return;
    }

    case BOOT_SHINE:
    case BOOT_EMERGE:
    case BOOT_DOCK:
    case BOOT_WORDMARK:
    case BOOT_DONE: {
        float th;
        /* A boot that was dismissed ends on black: whatever screen took over
         * draws itself, and nothing here may claim the XMB is showing. */
        if (s->phase == BOOT_DONE && s->left) {
            f->mark_scale = s->from_scale;
            return;
        }
        th  = (s->phase == BOOT_DONE) ? BOOT_HANDOFF_END_MS : s->t_hand;
        float out = 1.0f - boot_ease_smoother((th - BOOT_HALO_OUT_START_MS) /
                                               BOOT_HALO_OUT_MS);
        f->xmb_visible  = 1;
        f->mark_opacity = 1.0f;
        /* The glint: a smootherstep sweep, brightest mid-crossing. */
        {
            float u = boot_clamp01((th - BOOT_GLINT_START_MS) / BOOT_GLINT_MS);
            float tri = u < 0.5f ? 2.0f * u : 2.0f - 2.0f * u;
            f->glint     = boot_ease_smoother(tri);
            /* Parked at its ends outside the sweep, never reset to 0. */
            f->glint_pos = -BOOT_GLINT_WIDTH +
                           (1.0f + 2.0f * BOOT_GLINT_WIDTH) * boot_ease_smoother(u);
        }
        /* The twinkle: rises, peaks as the glint reaches the apex, falls;
         * turning slowly the whole time. */
        {
            float v = boot_clamp01((th - BOOT_SPARK_START_MS) / BOOT_SPARK_MS);
            f->spark = v < BOOT_SPARK_RISE
                ? boot_ease_smoother(v / BOOT_SPARK_RISE)
                : 1.0f - boot_ease_smoother((v - BOOT_SPARK_RISE) /
                                            (1.0f - BOOT_SPARK_RISE));
            f->spark_rot = 0.5f * v;
        }
        f->halo = s->from_halo * (1.0f + BOOT_HALO_SPARK_BOOST * f->spark) * out;
        if (f->halo > 1.0f) f->halo = 1.0f;
        f->status = s->from_status *
                    (1.0f - boot_ease_smoother(th / BOOT_STATUS_OUT_MS));
        f->veil   = boot_ease_smoother((th - BOOT_EMERGE_START_MS) / BOOT_EMERGE_MS);
        {
            float u = (th - BOOT_DOCK_START_MS) / BOOT_DOCK_MS;
            f->dock_pos  = boot_ease_in_out_cubic(u);
            f->dock_size = boot_ease_in_out_cubic(u * BOOT_SCALE_LEAD);
        }
        for (i = 0; i < BOOT_WORD_LETTERS; i++) {
            float lt = (th - BOOT_WORD_START_MS - (float)i * BOOT_WORD_STAGGER_MS)
                     / BOOT_WORD_LETTER_MS;
            f->word_reveal[i] = boot_ease_out_back(lt, BOOT_WORD_OVERSHOOT);
            f->word_alpha[i]  = boot_ease_smoother(lt / BOOT_WORD_FADE_FRAC);
        }
        return;
    }
    }
}

/* Advance by one frame of dt_ms real time. */
static inline void boot_seq_step(BootSeq *s, float dt_ms)
{
    if (dt_ms != dt_ms || dt_ms < 0.0f) dt_ms = 0.0f;     /* NaN, negative */
    if (dt_ms > BOOT_MAX_STEP_MS) dt_ms = BOOT_MAX_STEP_MS;
    s->frames++;
    if (s->phase == BOOT_DONE) return;
    s->t += dt_ms;

    /* A skip once the XMB is up ends everything, from any phase: the user can
     * see (or is about to see) a live UI and wants it now.  Before the XMB is
     * up there is nothing to skip TO, so the press is DISCARDED -- not
     * latched, or it would fire the instant the XMB arrived and throw the
     * whole reveal away on a press made seconds earlier. */
    if (s->sig & BOOT_SIG_SKIP) {
        s->sig &= ~BOOT_SIG_SKIP;
        if ((s->sig & BOOT_SIG_XMB) && s->phase != BOOT_DISMISS) {
            s->t_hand = BOOT_HANDOFF_END_MS;
            boot__enter(s, BOOT_DONE);
            return;
        }
    }

    /* Leaving for another screen beats everything that has not yet started
     * the handoff.  From SHINE on the XMB is running underneath and the
     * signal is meaningless, so it is ignored there. */
    if ((s->sig & BOOT_SIG_LEAVE) &&
        (s->phase == BOOT_DARK || s->phase == BOOT_EMBLEM ||
         s->phase == BOOT_AWAIT)) {
        boot__snapshot(s);
        s->left = 1;
        boot__enter(s, s->from_mark > 0.0f ? BOOT_DISMISS : BOOT_DONE);
        return;
    }

    s->t_phase += dt_ms;

    switch (s->phase) {
    case BOOT_DARK:
        if (s->t_phase >= BOOT_DARK_MIN_MS) boot__enter(s, BOOT_EMBLEM);
        break;
    case BOOT_EMBLEM:
        if (s->t_phase >= BOOT_EMBLEM_MS) boot__enter(s, BOOT_AWAIT);
        break;
    case BOOT_AWAIT:
        if ((s->sig & BOOT_SIG_XMB) && s->t_phase >= BOOT_HOLD_MIN_MS) {
            boot__snapshot(s);
            s->t_hand = 0.0f;
            boot__enter(s, BOOT_SHINE);
        }
        break;
    case BOOT_SHINE:
    case BOOT_EMERGE:
    case BOOT_DOCK:
    case BOOT_WORDMARK:
        s->t_hand += dt_ms;
        s->phase = boot__hand_phase(s->t_hand);
        break;
    case BOOT_DISMISS:
        if (s->t_phase >= BOOT_DISMISS_MS) boot__enter(s, BOOT_DONE);
        break;
    case BOOT_DONE:
        break;
    }
}

/* True when a button press right now would be acted on as a skip, i.e. when
 * the caller should swallow it rather than pass it to the XMB underneath. */
static inline int boot_seq_skip_effective(const BootSeq *s)
{
    return (s->sig & BOOT_SIG_XMB) && s->phase != BOOT_DISMISS &&
           s->phase != BOOT_DONE;
}

/* ---- geometry helpers (pixels in, pixels out) ---------------------------- */

/* Mark size along the dock: GEOMETRIC, not linear, so the shrink reads as a
 * constant rate -- a linear lerp from 180px to 30px spends most of its time
 * looking nearly finished.  Exactly `from` at 0 and `to` at 1. */
static inline float boot_dock_size_px(float from, float to, float s)
{
    s = boot_clamp01(s);
    if (s <= 0.0f) return from;
    if (s >= 1.0f) return to;
    if (from <= 0.0f || to <= 0.0f) return from + (to - from) * s;
    return from * powf(to / from, s);
}

/* Point along the dock path at eased progress p: a quadratic Bezier from
 * (x0,y0) to (x1,y1) whose control point is bowed BOOT_ARC_BOW of the way
 * from the chord's midpoint toward (x0,y1).  So the mark lifts first and
 * glides LEFT into the lockup -- landing along the same horizontal the
 * wordmark then unfolds on.  Exact at both ends. */
static inline void boot_dock_point(float x0, float y0, float x1, float y1,
                                   float p, float *x, float *y)
{
    float mx = 0.5f * (x0 + x1), my = 0.5f * (y0 + y1);
    float cx = mx + BOOT_ARC_BOW * (x0 - mx);
    float cy = my + BOOT_ARC_BOW * (y1 - my);
    float u;
    p = boot_clamp01(p);
    if (p <= 0.0f) { *x = x0; *y = y0; return; }
    if (p >= 1.0f) { *x = x1; *y = y1; return; }
    u = 1.0f - p;
    *x = u * u * x0 + 2.0f * u * p * cx + p * p * x1;
    *y = u * u * y0 + 2.0f * u * p * cy + p * p * y1;
}

/* The veil as horizontal rows of (y, alpha), top to bottom, clipped to the
 * screen, for a GPU quad strip to interpolate between.  Above the soft band
 * it is solid black; the band ramps to clear along smootherstep, sampled at
 * five rows (linear between them is within 3% of the curve); below it there
 * is nothing to draw.  v = BootFrame.veil.
 *
 * Returns the row count (0 = nothing to draw, the veil has fully lifted).
 * ys[] are pixel rows, as[] alpha 0..255.  Room for BOOT_VEIL_ROWS_MAX. */
#define BOOT_VEIL_ROWS_MAX 8

static inline int boot_veil_rows(float v, float screen_h,
                                 float *ys, unsigned char *as)
{
    static const float k[5] = { 0.0f, 0.25f, 0.5f, 0.75f, 1.0f };
    float band = BOOT_VEIL_BAND * screen_h;
    float top  = screen_h - boot_clamp01(v) * (screen_h + band);  /* band top */
    float py[7];
    float pa[7];
    int   n = 0, i, out = 0;

    if (screen_h <= 0.0f) return 0;
    if (top + band <= 0.0f) return 0;          /* lifted clear off the top */

    /* The unclipped polyline: solid from far above, then the band. */
    py[n] = -1.0e9f; pa[n] = 255.0f; n++;
    for (i = 0; i < 5; i++) {
        py[n] = top + k[i] * band;
        pa[n] = 255.0f * (1.0f - boot_ease_smoother(k[i]));
        n++;
    }

    /* Clip to [0, screen_h], interpolating alpha at the cut points. */
    for (i = 0; i + 1 < n; i++) {
        float ya = py[i], yb = py[i + 1], aa = pa[i], ab = pa[i + 1];
        float lo, hi;
        if (yb <= 0.0f || ya >= screen_h) continue;
        lo = ya < 0.0f ? 0.0f : ya;
        hi = yb > screen_h ? screen_h : yb;
        if (out == 0) {
            float f0 = (yb > ya) ? (lo - ya) / (yb - ya) : 0.0f;
            ys[out] = lo;
            as[out] = (unsigned char)(aa + (ab - aa) * f0 + 0.5f);
            out++;
        }
        {
            float f1 = (yb > ya) ? (hi - ya) / (yb - ya) : 1.0f;
            ys[out] = hi;
            as[out] = (unsigned char)(aa + (ab - aa) * f1 + 0.5f);
            out++;
        }
        if (hi >= screen_h) break;
    }
    return out >= 2 ? out : 0;
}

#ifdef __cplusplus
}
#endif

#endif /* JF_BOOT_SEQ_H */
