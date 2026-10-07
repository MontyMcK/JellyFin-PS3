// Stage B of the audio-reactive wave: features in, wave parameters out.
//
//   wave_audio.h   -> features
//   wave_motion.h  -> smoothed, slew-limited wave parameters   (this file)
//   wave_layers.h  -> per-layer geometry and colour
//
// Three of this stage's outputs go straight into the EXISTING solver through
// arguments it already takes -- wk_step(c, dt * timescale, perturb, drive) --
// and the rest are consumed after the spline by stage C.  wave_kernel.h,
// wave_spline.h and wave_ribbon.h are not modified by any of this.
//
// Written to docs/wave-audio-spec.md sections 3 and 4.
//
// THE ONE RULE THIS FILE EXISTS TO ENFORCE
//
//   Nothing that comes out of the analyser touches geometry.  Every driver is
//   an envelope, and every geometry parameter passes a SLEW LIMITER on its way
//   out.
//
// The slew limiter is the load-bearing half.  Envelope constants are tuning,
// and tuning can be wrong; a slew limit is a bound.  Whatever stage A produces
// -- a corrupt buffer, a codec glitch, a 0 dBFS square wave, a NaN -- a
// geometry parameter physically cannot move faster than its declared rate per
// second.  The wave's calm is a property of the code, not a property of having
// picked good numbers, and test_wave_motion.c asserts it directly by
// differencing consecutive frames under deliberately hostile input.
//
// House rules (.clinerules rule 7): header-only, pure C, no libm, no PS3
// headers, deterministic, caller-owned state, no globals.

#ifndef WAVE_MOTION_H
#define WAVE_MOTION_H

#include <stdint.h>
#include <string.h>

#include "wave_audio.h"

#define WM_LAYERS       4       // Swell, Body, Filament, Sheen -- spec 1.2
#define WM_PULSES       4       // concurrent travelling pulses -- spec 3.4

#define WM_TWO_PI       6.28318530718f

// --- the idle set ---------------------------------------------------------
// Silence is a state with its own animation, not the absence of one, and its
// state is NOT zero.  The spring chain's own perturbation term means these
// values still drift indefinitely and never repeat, which is why the idle
// case needs no separate code path -- that behaviour was designed into
// wave_kernel.h from the start (see WK_DAMPING's comment).
#define WM_IDLE_DRIVE   0.35f
#define WM_IDLE_PERTURB 0.020f
#define WM_IDLE_TIME    0.80f
#define WM_IDLE_AMP     0.34f
#define WM_IDLE_BRIGHT  0.55f
#define WM_IDLE_LIFT    0.00f

// --- driven ranges --------------------------------------------------------
// WM_MAX_DRIVE IS 1.10 BECAUSE OF A MEASUREMENT, not because it looked right.
// The chain's peak amplitude is linear in drive at about 0.672 per unit, so
// drive 1.30 reaches 0.840 -- past WK_KNEE (0.80), which means the solver
// spends part of its time inside its own soft clip.  A solver sitting in its
// own clip has stopped being a solver: the clip is monotone but compressive,
// so loud passages would stop growing and the whole top of the dynamic range
// would flatten out.  1.10 peaks at 0.739, which is 8% clear of the knee.
// See wave_layers.h's WL_BASE_MAX for the measurement and the harness.
//
// WM_MAX_PERTURB stays under the reference's observed PERTURBATION ceiling of
// 0.1 (wave-spec.md section 1), and well under it, because WK_NOISE_SCALE was
// calibrated against that ceiling as a liveliness term rather than a primary
// motion source.  The same measurement confirms that directly: 0.030 and
// 0.085 both give a peak of 0.840 at drive 1.30, to three decimals.  This
// knob buys texture, not amplitude, which is exactly what it is used for here.
#define WM_MAX_DRIVE    1.10f
#define WM_MAX_PERTURB  0.085f
#define WM_MIN_TIME     0.70f
#define WM_MAX_TIME     1.60f
#define WM_BEAT_REF     2.00f   // Hz; 120 BPM reads as timescale 1.0

// --- slew limits, in units per second -------------------------------------
// Read these as "the fastest this parameter may cross its whole range":
// drive 0.9/s over a ~0.95 range is just over a second.  Hue is the slowest
// thing on screen by a factor of three, because colour that moves is the
// single fastest way to make a UI look cheap.
//
// Glow is the one asymmetric pair.  It is a transient highlight, so it is
// allowed to arrive in ~125 ms; it leaves over ~400 ms, which is what makes it
// read as a decay rather than as a blink.
#define WM_SLEW_DRIVE   0.90f
#define WM_SLEW_PERTURB 0.25f
#define WM_SLEW_TIME    0.60f
#define WM_SLEW_AMP     1.10f
#define WM_SLEW_DETAIL  1.60f
#define WM_SLEW_LIFT    0.70f
#define WM_SLEW_BRIGHT  1.20f
#define WM_SLEW_HUE     0.28f
#define WM_SLEW_GLOW_UP 8.00f
#define WM_SLEW_GLOW_DN 2.50f

// The fastest any parameter may move, used by the test as a single bound and
// by the assert below as a sanity guard on the table above.
#define WM_SLEW_MAX     WM_SLEW_GLOW_UP

// --- hue ------------------------------------------------------------------
// The centroid is remapped into the MIDDLE of the theme's violet->cyan line
// rather than spanning it.  Those two colours mean something specific in this
// UI -- handoff section 2.1, "purple for what the user is pointing at, blue
// for what the file is" -- and a background that reaches either endpoint
// competes with a focus ring.
#define WM_HUE_LO       0.20f
#define WM_HUE_SPAN     0.60f

// --- per-layer drift rates, radians per second at timescale 1 -------------
// Rising with depth, because the nearest layer must move fastest: that is the
// parallax half of the aerial-perspective cue in spec 1.2.  The WAVENUMBERS
// that pair with these live in wave_layers.h, with the rest of the
// appearance constants; the rates live here because the phases they advance
// are integrated state and integrated state belongs to the motion stage.
//
// The ratios are deliberately not simple fractions, so the crossing pattern
// the layers make does not repeat on any short cycle.
static const float WM_PHASE_RATE[WM_LAYERS] = {
    0.093f, 0.171f, 0.311f, 0.482f
};

// How much of each driver reaches each layer.  Row = layer, and the four
// columns are the four mixes computed in wm_update: bass, low-mid, mid, high.
// This is the "frequency is depth" principle as a matrix: the mass sits on the
// diagonal running from bass at the back to high at the front.
static const float WM_LAYER_MIX[WM_LAYERS][4] = {
    /* Swell    */ { 0.86f, 0.14f, 0.00f, 0.00f },
    /* Body     */ { 0.46f, 0.40f, 0.14f, 0.00f },
    /* Filament */ { 0.10f, 0.28f, 0.50f, 0.12f },
    /* Sheen    */ { 0.00f, 0.08f, 0.34f, 0.58f }
};

// Fine-ripple weight per layer.  Zero at the back: a distant object does not
// show fine detail, which is the whole reason the depth reading works.
static const float WM_LAYER_DETAIL[WM_LAYERS] = {
    0.00f, 0.55f, 0.85f, 1.00f
};

// --- pulses ---------------------------------------------------------------
// An onset does not set a parameter, it spawns an object.  A beat that
// brightens the screen is a strobe; a beat that launches something which then
// travels for two beats and fades is a gesture, and the eye reads the second
// as the wave having been disturbed by the music rather than redrawn from it.
#define WM_PULSE_SPAN   1.14f   // entry to exit in u, a little past both ends
#define WM_PULSE_BEATS  2.0f    // beats to cross the screen
#define WM_PULSE_PERIOD 0.60f   // fallback beat period when nothing is locked
#define WM_PULSE_ATTACK 0.14f   // fraction of life spent rising
#define WM_PULSE_W_MIN  0.075f  // a hard onset is tight
#define WM_PULSE_W_MAX  0.150f  // a soft one is broad
#define WM_PULSE_AMP_LO 0.35f   // floor, so a weak onset still shows
#define WM_PULSE_LIFE_K 2.4f    // life in beat periods

typedef struct {
    float x;        // position along the ribbon in normalised u
    float v;        // u per second, signed
    float amp;      // current amplitude, 0..1
    float amp0;     // spawn amplitude
    float width;    // half-width of support in u
    float age;      // seconds
    float life;     // seconds
    int   live;
} wm_pulse;

// Everything stage C needs.  Every field is finite and inside its stated range
// for every input wm_update accepts.
typedef struct {
    float drive;                // -> wk_step drive,   WM_IDLE_DRIVE..WM_MAX_DRIVE
    float perturb;              // -> wk_step perturb, 0..WM_MAX_PERTURB
    float timescale;            // -> dt multiplier,   WM_MIN_TIME..WM_MAX_TIME
    float amp[WM_LAYERS];       // 0..1 per-layer amplitude
    float detail[WM_LAYERS];    // 0..1 per-layer fine-ripple amplitude
    float phase[WM_LAYERS];     // radians, wrapped to [0, 2pi)
    float lift;                 // 0..1 vertical breath of the whole band
    float bright;               // 0..1 crest luminance
    float hue;                  // 0..1, WM_HUE_LO..WM_HUE_LO+WM_HUE_SPAN
    float glow;                 // 0..1 transient sheen
    wm_pulse pulse[WM_PULSES];
} wm_params;

typedef struct {
    wm_params p;
    int       dir;              // +1 / -1, alternates every spawn
    int       slot;             // round-robin replacement
    int       ready;
} wm_state;

static inline float wm_clamp(float v, float lo, float hi)
{
    if (v != v) return lo;                  // NaN in, defined out
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static inline float wm_lerp(float a, float b, float t)
{
    return a + (b - a) * t;
}

// Smoothstep, used on the drivers rather than a straight linear map.  Its
// fixed point is 0.5, which is exactly where wa_compress puts a band sitting
// at its own typical level -- so "normal" music lands at the middle of the
// visual range by construction, and the S only softens the extremes.
static inline float wm_smooth01(float t)
{
    t = wm_clamp(t, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

// Move cur toward target by at most rate*dt.  THIS IS THE ELEGANCE GUARANTEE;
// see the header comment.  A NaN target holds the current value rather than
// propagating, because a held parameter is a visible pause and a NaN parameter
// is a wedged GPU.
static inline float wm_slew(float cur, float target, float rate, float dt)
{
    float d, step;
    if (cur != cur) return 0.0f;
    if (target != target) return cur;
    if (!(rate > 0.0f) || !(dt > 0.0f)) return cur;
    step = rate * dt;
    d = target - cur;
    if (d >  step) d =  step;
    if (d < -step) d = -step;
    return cur + d;
}

// Wrap a phase into [0, 2pi).  Keeps full float precision in the sine argument
// over a session of any length instead of losing a mantissa bit every time the
// accumulated angle doubles -- the same reason wk_wrap2pi exists.
static inline float wm_wrap2pi(float a)
{
    float q;
    if (!(a > -1.0e6f && a < 1.0e6f)) return 0.0f;
    q = (float)(int)(a * 0.15915494309f);
    a -= q * WM_TWO_PI;
    if (a < 0.0f)        a += WM_TWO_PI;
    if (a >= WM_TWO_PI)  a -= WM_TWO_PI;    // guards the rounding edge
    return a;
}

// Pulse envelope over its normalised age s in [0,1]: smoothstep UP over the
// first WM_PULSE_ATTACK, smoothstep DOWN over the rest.
//
// Both halves are smoothstep, and that is not laziness -- it is the only way
// to get zero slope at all three of the points that matter.  The obvious
// decay, (1-u)^2, looks like it starts flat and does not: its derivative at
// u = 0 is -2, so scaled over the remaining 0.86 of the life it arrives at the
// peak with a slope of -2.33 against the attack's 0.  That is a corner, and a
// corner in an envelope that is being added to geometry is a visible crease
// travelling along the ribbon.  test_wave_motion.c measures the two one-sided
// slopes at the join and caught exactly that.
//
// With smoothstep on both sides the envelope is C1 at s = 0, at the join and
// at s = 1, so a pulse fades in, peaks and fades out with nothing to see at
// any of the three.
static inline float wm_pulse_env(float s)
{
    float u;
    if (!(s > 0.0f)) return 0.0f;
    if (s >= 1.0f)   return 0.0f;
    if (s < WM_PULSE_ATTACK) {
        u = s * (1.0f / WM_PULSE_ATTACK);
        return u * u * (3.0f - 2.0f * u);
    }
    u = (s - WM_PULSE_ATTACK) * (1.0f / (1.0f - WM_PULSE_ATTACK));
    return 1.0f - u * u * (3.0f - 2.0f * u);
}

// Reset to the idle state -- NOT to zero.  A client that never feeds audio
// gets a calm, drifting wave from this alone, which is the required behaviour
// for the XMB with nothing playing.
static inline int wm_init(wm_state *s)
{
    int i;
    if (!s) return 0;
    memset(s, 0, sizeof *s);
    s->p.drive     = WM_IDLE_DRIVE;
    s->p.perturb   = WM_IDLE_PERTURB;
    s->p.timescale = WM_IDLE_TIME;
    s->p.bright    = WM_IDLE_BRIGHT;
    s->p.lift      = WM_IDLE_LIFT;
    s->p.hue       = WM_HUE_LO + 0.5f * WM_HUE_SPAN;
    s->p.glow      = 0.0f;
    for (i = 0; i < WM_LAYERS; i++) {
        s->p.amp[i]    = WM_IDLE_AMP;
        s->p.detail[i] = 0.0f;
        // Phases are spread rather than started together, so the very first
        // frame already shows the layers crossing instead of stacked.
        s->p.phase[i]  = (float)i * (WM_TWO_PI * 0.237f);
        s->p.phase[i]  = wm_wrap2pi(s->p.phase[i]);
    }
    s->dir   = 1;
    s->slot  = 0;
    s->ready = 1;
    return 1;
}

// Spawn a travelling pulse.  Direction alternates on every call: a fixed
// direction develops into a visible conveyor belt within about ten seconds.
static inline void wm_spawn_pulse(wm_state *s, float strength, float period,
                                  float scale)
{
    wm_pulse *q;
    float speed;

    if (!s || !s->ready) return;
    strength = wm_clamp(strength, 0.0f, 1.0f);
    scale    = wm_clamp(scale,    0.0f, 1.0f);
    if (!(period >= WA_BEAT_MIN && period <= WA_BEAT_MAX)) period = WM_PULSE_PERIOD;

    q = &s->p.pulse[s->slot];
    s->slot = (s->slot + 1) % WM_PULSES;

    // Cross the screen in WM_PULSE_BEATS beats, so the field's visual rhythm
    // relates to the music's metre without anything being drawn on the beat.
    speed = WM_PULSE_SPAN / (WM_PULSE_BEATS * period);

    q->x     = (s->dir > 0) ? -0.07f : 1.07f;
    q->v     = (float)s->dir * speed;
    q->amp0  = (WM_PULSE_AMP_LO + (1.0f - WM_PULSE_AMP_LO) * strength) * scale;
    q->amp   = 0.0f;
    // A hard onset is tight and a soft one is broad, which is what makes a
    // snare read differently from a swell without either being drawn.
    q->width = wm_lerp(WM_PULSE_W_MAX, WM_PULSE_W_MIN, strength);
    q->age   = 0.0f;
    q->life  = WM_PULSE_LIFE_K * period;
    q->live  = 1;
    s->dir   = -s->dir;
}

// Advance every pulse and retire the ones that are done.
static inline void wm_step_pulses(wm_state *s, float dt)
{
    int i;
    for (i = 0; i < WM_PULSES; i++) {
        wm_pulse *q = &s->p.pulse[i];
        if (!q->live) { q->amp = 0.0f; continue; }
        q->age += dt;
        q->x   += q->v * dt;
        if (q->age >= q->life || q->x < -0.35f || q->x > 1.35f) {
            q->live = 0;
            q->amp  = 0.0f;
            continue;
        }
        q->amp = q->amp0 * wm_pulse_env(q->age / q->life);
    }
}

// Advance the whole parameter set by dt seconds against this frame's features.
//
// f may be NULL, which is treated as full silence -- a caller with no audio
// source at all gets the idle wave by passing NULL every frame and needs no
// other code path.
//
// Non-finite or non-positive dt leaves the state untouched.
static inline void wm_update(wm_state *s, const wa_features *f, float dt)
{
    wa_features quiet;
    float bass, low, mid, high, hifast;
    float energy, live, sil;
    float t_drive, t_pert, t_time, t_lift, t_bright, t_hue, t_glow;
    float period;
    int   i;

    if (!s || !s->ready) return;
    if (!(dt > 0.0f) || !(dt < 1.0e30f)) return;
    if (dt > WA_DT_MAX) dt = WA_DT_MAX;

    if (!f) {
        memset(&quiet, 0, sizeof quiet);
        quiet.silence  = 1.0f;
        quiet.centroid = 0.5f;
        f = &quiet;
    }

    sil  = wm_clamp(f->silence, 0.0f, 1.0f);
    live = 1.0f - sil;

    // --- the four mixes ---------------------------------------------------
    // Pairs, not single bands: SUB and BASS disagree about where a kick sits
    // depending on the mastering, and HIGH and AIR disagree about a cymbal.
    // Averaging the pair makes the driver depend on the music rather than on
    // which side of a filter edge the engineer put it.
    bass   = wm_clamp(0.60f * f->band[WA_SUB]  + 0.40f * f->band[WA_BASS],   0.0f, 1.0f);
    low    = wm_clamp(f->band[WA_LOWMID], 0.0f, 1.0f);
    mid    = wm_clamp(f->band[WA_MID],    0.0f, 1.0f);
    high   = wm_clamp(0.55f * f->band[WA_HIGH] + 0.45f * f->band[WA_AIR],    0.0f, 1.0f);
    hifast = wm_clamp(0.55f * f->band_fast[WA_HIGH]
                    + 0.45f * f->band_fast[WA_AIR], 0.0f, 1.0f);
    energy = wm_smooth01(wm_clamp(f->rms, 0.0f, 1.0f));

    // --- targets ----------------------------------------------------------
    t_drive = wm_lerp(WM_IDLE_DRIVE, WM_MAX_DRIVE, wm_smooth01(bass));
    t_drive = wm_lerp(WM_IDLE_DRIVE, t_drive, live);

    // Perturbation is the one driver taken from the FAST envelope rather than
    // the medium one.  It is broadband forcing on the chain and it produces
    // fine spatial detail, so it is the only place where a band maps to
    // something that looks like its own frequency -- and fine detail that lags
    // by a quarter second reads as smearing rather than as texture.
    t_pert  = wm_lerp(WM_IDLE_PERTURB, WM_MAX_PERTURB, hifast);
    t_pert  = wm_lerp(WM_IDLE_PERTURB, t_pert, live);

    // Drift rate from the beat estimate, blended in by confidence so a track
    // with no detectable pulse falls back to a gentle loudness term rather
    // than holding the last track's tempo.  See spec 2.4: this is the thing
    // that makes it feel like listening rather than reacting.
    {
        float ts_beat = WM_IDLE_TIME;
        float conf    = wm_clamp(f->beat_conf, 0.0f, 1.0f);
        if (f->beat_hz > 0.0f)
            ts_beat = wm_clamp(f->beat_hz / WM_BEAT_REF, WM_MIN_TIME, WM_MAX_TIME);
        t_time = wm_lerp(0.85f + 0.35f * energy, ts_beat, conf);
        t_time = wm_clamp(wm_lerp(WM_IDLE_TIME, t_time, live),
                          WM_MIN_TIME, WM_MAX_TIME);
    }

    t_lift   = wm_lerp(WM_IDLE_LIFT,   energy,                      live);
    t_bright = wm_lerp(WM_IDLE_BRIGHT, 0.35f + 0.65f * energy,      live);

    // Hue holds through silence rather than drifting back to the middle: a
    // track that ends should leave the room the colour it made it.
    t_hue    = WM_HUE_LO + WM_HUE_SPAN * wm_clamp(f->centroid, 0.0f, 1.0f);
    t_hue    = wm_lerp(s->p.hue, t_hue, live);

    t_glow   = wm_clamp(f->onset_strength, 0.0f, 1.0f) * live;

    // --- slew everything out ---------------------------------------------
    s->p.drive     = wm_slew(s->p.drive,     t_drive,   WM_SLEW_DRIVE,   dt);
    s->p.perturb   = wm_slew(s->p.perturb,   t_pert,    WM_SLEW_PERTURB, dt);
    s->p.timescale = wm_slew(s->p.timescale, t_time,    WM_SLEW_TIME,    dt);
    s->p.lift      = wm_slew(s->p.lift,      t_lift,    WM_SLEW_LIFT,    dt);
    s->p.bright    = wm_slew(s->p.bright,    t_bright,  WM_SLEW_BRIGHT,  dt);
    s->p.hue       = wm_slew(s->p.hue,       t_hue,     WM_SLEW_HUE,     dt);
    s->p.glow      = wm_slew(s->p.glow,      t_glow,
                             (t_glow > s->p.glow) ? WM_SLEW_GLOW_UP
                                                  : WM_SLEW_GLOW_DN, dt);

    for (i = 0; i < WM_LAYERS; i++) {
        const float *w = WM_LAYER_MIX[i];
        float drv = w[0] * bass + w[1] * low + w[2] * mid + w[3] * high;
        float t_amp = wm_lerp(WM_IDLE_AMP, wm_smooth01(drv), live);
        float t_det = hifast * WM_LAYER_DETAIL[i] * live;

        s->p.amp[i]    = wm_slew(s->p.amp[i],    t_amp, WM_SLEW_AMP,    dt);
        s->p.detail[i] = wm_slew(s->p.detail[i], t_det, WM_SLEW_DETAIL, dt);

        // Phases advance at timescale, so the whole field speeds up and slows
        // down together with the music's pace.  Using the SLEWED timescale
        // rather than the target is what keeps that change gradual.
        s->p.phase[i] = wm_wrap2pi(s->p.phase[i]
                                 + WM_PHASE_RATE[i] * s->p.timescale * dt);
    }

    // --- pulses -----------------------------------------------------------
    period = (f->beat_hz > 0.0f) ? 1.0f / f->beat_hz : WM_PULSE_PERIOD;
    wm_step_pulses(s, dt);
    if (f->onset > 0.5f)
        wm_spawn_pulse(s, f->onset_strength, period, live);

    // --- final bounds -----------------------------------------------------
    // The slew limiter constrains the STEP, not the destination, and every
    // target above is already inside range -- so this clamp should never bite.
    // It is here for the same reason wk_softclip is: a bound that is asserted
    // rather than assumed, and a clip that ever engages is a bug report.
    s->p.drive     = wm_clamp(s->p.drive,     WM_IDLE_DRIVE, WM_MAX_DRIVE);
    s->p.perturb   = wm_clamp(s->p.perturb,   0.0f,          WM_MAX_PERTURB);
    s->p.timescale = wm_clamp(s->p.timescale, WM_MIN_TIME,   WM_MAX_TIME);
    s->p.lift      = wm_clamp(s->p.lift,      0.0f, 1.0f);
    s->p.bright    = wm_clamp(s->p.bright,    0.0f, 1.0f);
    s->p.hue       = wm_clamp(s->p.hue, WM_HUE_LO, WM_HUE_LO + WM_HUE_SPAN);
    s->p.glow      = wm_clamp(s->p.glow,      0.0f, 1.0f);
    for (i = 0; i < WM_LAYERS; i++) {
        s->p.amp[i]    = wm_clamp(s->p.amp[i],    0.0f, 1.0f);
        s->p.detail[i] = wm_clamp(s->p.detail[i], 0.0f, 1.0f);
    }
}

#endif // WAVE_MOTION_H
