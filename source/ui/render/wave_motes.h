// JellyWave stage 9: the motes.
//
//   wave_cam.h    world space -> clip space
//   wave_light.h  the one shared light + its LUTs
//   wave_gel.h    the lofted translucent body
//   wave_motes.h  the drifting glints around it                 (this file)
//
// NOT WIRED IN.  Nothing in ui_wave.cpp includes this yet, on purpose: the
// JellyWave frame is over budget on the RSX (see the jellywave notes), and a
// particle field is more RSX work at exactly the point that is short.  This
// file is the simulation and the shading, finished and host-tested, so wiring
// it is a renderer task and not a design task.  What the renderer will need to
// add is described at the bottom.
//
// WHAT IT IS
//
// A cloud of small flakes hanging in the air around the band.  They are
// neither falling nor fired: they sit near neutral buoyancy, drift slowly
// along the band on a light wind, jostle under random forcing, and are held
// loosely around the band by a pull toward its CENTRELINE -- a line, not a
// point.  A point attractor collapses any such cloud into a ball; a line only
// constrains the two directions across the band and leaves the motes free
// along it, so the cloud takes the band's shape.  Each flake tumbles slowly,
// and glints when its face turns to reflect the key light at the camera.
//
// WHY THE LIGHT IS SHARED
//
// What makes motes look like they belong to the ribbon is not how many there
// are -- it is that they are lit by the SAME light.  wave_light.h exists so
// there is one key direction, one specular lobe and one definition of "lit" in
// the scene; this file calls jw_key_lit() and indexes JW_SPEC_LUT at
// JW_SPEC_I rather than carrying copies, so the flakes glint from the same
// direction and with the same lobe shape as the gel's wet highlight.
//
// ONE DELIBERATE DIFFERENCE from jw_specular(): the half vector is computed
// ONCE per shade call, from the eye vector at the cloud's centre, instead of
// per mote.  The gel needs the per-vertex eye vector for its grazing response
// along a rolled edge; a flake has no edge, and the eye vector's swing across
// the cloud only changes WHICH flakes glint, not what a glint looks like.  It
// saves two reciprocal square roots a mote.
//
// THE AUDIO, through wave_motion.h's stage B only -- no second analyser:
//
//   amp[0]     (bass)          the cloud breathes: stronger random forcing,
//                              so it spreads, over about a second
//   detail[3]  (highs, fast)   flakes spin faster, so cymbals sparkle
//   glow       (onsets)        glints brighten
//   bright     (loudness)      the whole cloud brightens
//   hue        (centroid)      the flakes' own colour, violet -> blue
//   lift                       the cloud rises
//   timescale  (tempo)         everything runs faster
//   pulse[]    (beats)         a travelling pulse LIFTS and lights the motes
//                              it passes under -- a ripple running through
//                              the cloud, which is the "gesture, not strobe"
//                              stage B's pulses were designed for
//
// Every one of those is exactly neutral at stage B's idle values, so a client
// with no music gets the same motes as one with the analyser off.
//
// STROBE SAFETY.  A particle field can strobe in a way a ribbon cannot: many
// small sources changing together.  Four things stop it, and
// test_wave_motes.c measures the result rather than trusting them:
//   - lifetimes start staggered, so births and deaths are spread over time
//     instead of arriving in waves;
//   - the spin rate is capped so a bright glint lasts at least eight frames
//     at the fastest the audio can make a flake spin;
//   - every way a mote can die other than old age happens at zero alpha,
//     because the shading fades it out before each boundary;
//   - every audio input arrives already slewed by stage B.
// The test bound is on the TOTAL light the field emits from one frame to the
// next -- individual flakes twinkling is the point; the whole field pulsing
// is the bug.
//
// ORIGINAL WORK (.clinerules rule 10).  The model comes from a prose
// description -- near-neutral buoyancy, wind, random forcing, an attractor
// that does not collapse the cloud, depth of field -- and every constant
// below is derived from THIS scene: the band's geometry in wave_gel.h, the
// camera in wave_cam.h, and the spread and speed wanted on screen.  None is
// taken from captured data.
//
// House rules (.clinerules rule 7): header-only, pure C, no libm, no PS3
// headers, struct-of-arrays, caller-owned state, no globals.  The per-mote
// loops are branchless apart from the ternary selects, which compile to
// fsel/isel on the PPU.

#ifndef WAVE_MOTES_H
#define WAVE_MOTES_H

#include <stdint.h>
#include <string.h>

#include "wave_kernel.h"
#include "wave_light.h"     /* pulls wave_cam.h */
#include "wave_motion.h"

// --- population -----------------------------------------------------------
//
// WMO_MAX is the storage; WMO_COUNT_DEF is where to start on a TV.  1024 at
// four vertices a mote is 4,096 vertices, under half of what the gel already
// emits, which is the right size of experiment for a frame that is short on
// RSX time.  The state is struct-of-arrays at 19 words a mote, 156 KB at
// WMO_MAX, owned by the caller.
#define WMO_MAX         2048
#define WMO_COUNT_DEF   1024

// --- where the cloud lives, in wave_cam.h's world space -------------------
//
// The CENTRELINE is wave_gel.h's spine for the MID layer with the undulation
// taken out -- straight, because the motes should follow the band's course,
// not its every wiggle:
//
//   u = x / JW_LEN + 0.5
//   y = JW_TILT * u + JW_YBASE + mid.y_off  =  -0.955  - 0.07308 * x
//   z = JW_Z0 + JW_ZRAMP * u  + mid.z_off   =  -3.550  - 0.22308 * x
//
// The mid layer because it is the middle of the three: the near and far bands
// sit 3.1 world units in front and behind it, and the cloud's depth spread is
// sized to reach both.  test_wave_motes.c re-derives these from wave_gel.h so
// a change to the band's course cannot leave the cloud behind.
#define WMO_YC0        (-0.955f)
#define WMO_YC_K       (-0.0730769f)
#define WMO_ZC0        (-3.550f)
#define WMO_ZC_K       (-0.2230769f)

// The cloud's extent along x.  ASYMMETRIC, because the camera looks down and
// to the right: measured at 16:9, the centreline leaves the screen at
// x = -12.6 on the left but +10.2 on the right, and the cloud's far side
// (2 sd behind) needs about three units more on each side than that.  So a
// mote is born and dies off screen at both ends, and WMO_X_FADE dims it out
// before the boundary as well, belt and braces.  test_wave_motes.c projects
// both ends.
#define WMO_X_LO      (-18.0f)
#define WMO_X_HI       14.0f
#define WMO_X_FADE      1.5f

// Stage B's pulses run over u in [0, 1] across the ribbon, a little past both
// ends.  They are laid along the stretch of centreline that is ON SCREEN at
// 16:9, so a pulse enters at the left edge and leaves at the right, as it
// would on the ribbon.  Measured; test_wave_motes.c re-projects both ends.
#define WMO_PULSE_X0  (-12.6f)
#define WMO_PULSE_X1   10.2f

// --- the drift model ------------------------------------------------------
//
// Per axis ACROSS the band this is a damped spring with random forcing:
//
//   v' = -K (y - yc) - GAMMA v + SIGMA xi(t)
//
// whose steady spread is known in closed form, sd = SIGMA / sqrt(2 GAMMA K).
// That is what makes the constants choosable rather than tunable by eye: pick
// the spread wanted, pick how floaty (GAMMA) and how slow (K), and SIGMA
// follows.  test_wave_motes.c measures the spread against the formula.
//
//   GAMMA 0.6/s   low drag: a mote given a nudge coasts for about 1.7 s, which
//                 is what reads as "floating" rather than "swimming"
//   K     0.09    a natural period of 21 s -- the cloud re-forms slowly
//   sd_y  0.60    covers the band's thickness plus the layers' vertical offset
//   sd_z  1.80    reaches the near and far layers at 1.7 sd
//
// ALONG the band there is no spring, only the wind and a gentler forcing, so
// the motes travel its length instead of clumping at the attractor.
#define WMO_GAMMA       0.60f
#define WMO_K           0.090f
#define WMO_SD_Y        0.60f
#define WMO_SD_Z        1.80f
#define WMO_SIGMA_Y     (WMO_SD_Y * 0.3286335f)    /* sqrt(2 GAMMA K) */
#define WMO_SIGMA_Z     (WMO_SD_Z * 0.3286335f)
#define WMO_SIGMA_X     0.15f
#define WMO_WIND        0.15f      // world units/s along +x: 15 px/s at 1080p

// A mote that strays past four standard deviations, or leaves along x, or
// comes back non-finite, is respawned rather than chased.  Four sd of the
// WIDEST cloud the audio can make (bass scales the forcing by up to
// WMO_NOISE_MAX): a box sized for the idle spread truncated the breathing
// cloud and turned the bass response into a stream of boundary deaths.  Even
// the near corner of this box is over six units in front of the camera.  The
// speed clamp is a guard against a hostile dt, not something normal forcing
// ever reaches.
#define WMO_BOX_Y       (4.0f * WMO_SD_Y * WMO_NOISE_MAX)
#define WMO_BOX_Z       (4.0f * WMO_SD_Z * WMO_NOISE_MAX)
#define WMO_VMAX        3.0f

// Lifetimes are staggered from the start (see wmo_init) and a mote fades in
// and out over WMO_FADE, so nothing ever pops.
#define WMO_LIFE_MIN    7.0f
#define WMO_LIFE_MAX   14.0f
#define WMO_FADE        1.2f

// A stall is not a frame -- same cap as stage A's WA_DT_MAX, for the same
// reason.
#define WMO_DT_MAX      0.10f

// --- flakes ---------------------------------------------------------------
//
// Each mote is a flake spinning about its own fixed axis.  THE SPIN CAP IS A
// STROBE BOUND: the glint core in JW_SPEC_LUT (the h^48 term) is about 19
// degrees wide, and a flake whose spin plane only just reaches the core
// crosses it in less than that.  Measured at the fastest the audio can spin a
// flake (x WMO_SPIN_GAIN x WMO_TS_MAX), the shortest bright glint is 8 frames
// with a 1.0 rad/s cap and 10 frames with this one.  test_wave_motes.c tracks
// every bright glint and asserts an 8-frame floor.
#define WMO_SPIN_MIN    0.30f      // rad/s
#define WMO_SPIN_MAX    0.80f

// World radius.  At the focus depth that is 1.5 - 4.5 px across at 1080p:
// pinpoints, not blobs.
#define WMO_RAD_MIN     0.010f
#define WMO_RAD_MAX     0.030f

// --- depth of field -------------------------------------------------------
//
// The blur disc of a thin lens grows with |1/z - 1/focus|, so that is the
// term, focused at the cloud's own centre: the view depth of the centreline
// at x = 0, which test_wave_motes.c re-projects.  Blurring SPREADS a mote's
// light rather than adding to it, so alpha falls with the square of the
// growth -- sharp motes at the band are bright points, near ones are large,
// soft and faint, and far ones shrink.  That is the whole depth cue and it
// costs one extra divide a mote.
#define WMO_FOCUS_Z     15.77f
#define WMO_COC_K       0.22f      // clip units of blur per unit of |1/z - 1/f|
#define WMO_MIN_S       0.0028f    // smallest drawn half-size: 1.5 px at 1080p

// No mote is visible above the card grid.  The fade runs over the band below
// the line, so a mote rising toward it dims out instead of being cut off.
// This is enforced in the shading, not left to the physics.
#define WMO_CLIP_TOP   (-0.02f)
#define WMO_CLIP_FADE   0.12f

// --- shading --------------------------------------------------------------
//
// A mote's colour is its own tint -- the fill light's violet toward the rim
// light's blue, both from wave_light.h, placed by stage B's hue -- plus the
// key light's diffuse and its glint in the key's colour.  The glint term is
// the only one that saturates, which is what makes a glint read as a glint.
#define WMO_BASE_I      0.55f
#define WMO_DIFF_I      0.35f
#define WMO_GLINT_I     1.60f
#define WMO_ALPHA       0.85f

// --- audio ----------------------------------------------------------------
//
// Gains on stage B's parameters, each written as 1 + gain * (p - idle) so that
// idle is EXACTLY neutral, not merely close.
#define WMO_NOISE_GAIN  0.75f      // bass amp -> random forcing, 0.75 .. 1.49
#define WMO_NOISE_MAX   (1.0f + WMO_NOISE_GAIN * (1.0f - WM_IDLE_AMP))
#define WMO_SPIN_GAIN   0.60f      // highs    -> spin rate, 1.0 .. 1.6
#define WMO_GLOW_GAIN   1.20f      // onsets   -> glint, 1.0 .. 2.2
#define WMO_EMIT_GAIN   0.70f      // loudness -> brightness, 0.86 .. 1.32 over stage B's range
#define WMO_LIFT_ACC    0.032f     // lift     -> rise of lift*ACC/K = 0.36 at full
#define WMO_TS_GAIN     0.50f      // tempo    -> dt, 0.95 .. 1.40
#define WMO_TS_MIN      0.90f
#define WMO_TS_MAX      1.40f

// Pulses, placed by WMO_PULSE_X0/X1 -- the ON-SCREEN stretch of the
// centreline.  The ribbon (wave_render_map.h, wrm_accent) places the same
// pulses on the solver u of each layer, which runs a little past both screen
// edges; when the motes are wired, align the two or the ripple in the cloud
// will lead the one in the band near the edges.
#define WMO_PULSE_KICK  0.80f      // upward accel under a full pulse, units/s^2
#define WMO_EXC_TAU     0.35f      // how long a lit mote stays lit, s
#define WMO_EXC_GAIN    0.90f      // brightness added at full excitation

// --- per-frame controls ---------------------------------------------------

typedef struct {
    float ts;                  // dt multiplier
    float noise;               // random forcing gain
    float spin;                // spin-rate gain
    float lift;                // upward accel, world units/s^2
    float emit;                // brightness gain
    float glint;               // glint gain
    float tint;                // 0 = violet .. 1 = blue
    float px[WM_PULSES];       // pulse centre, world x
    float pinv[WM_PULSES];     // 1 / pulse half-width, world
    float pa[WM_PULSES];       // pulse amplitude, 0 when not live
} wmo_ctl;

static inline float wmo_clamp(float v, float lo, float hi)
{
    if (v != v) return lo;              // NaN in, defined out
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

// Stage B's parameters -> this frame's controls.  p may be NULL, which gives
// the idle set -- the same thing a freshly initialised wm_state gives, to the
// bit, which test_wave_motes.c checks.  Every field is clamped, so a NaN in
// the parameters lands on a defined value here and never reaches a mote.
static inline void wmo_controls(const wm_params *p, wmo_ctl *c)
{
    const float hue_mid = WM_HUE_LO + 0.5f * WM_HUE_SPAN;
    int i;

    if (!c) return;
    if (!p) {
        c->ts = c->noise = c->spin = c->emit = c->glint = 1.0f;
        c->lift = 0.0f;
        c->tint = 0.5f;
        for (i = 0; i < WM_PULSES; i++) {
            c->px[i] = 0.0f; c->pinv[i] = 1.0f; c->pa[i] = 0.0f;
        }
        return;
    }
    c->ts    = wmo_clamp(1.0f + (p->timescale - WM_IDLE_TIME) * WMO_TS_GAIN,
                         WMO_TS_MIN, WMO_TS_MAX);
    c->noise = wmo_clamp(1.0f + (p->amp[0] - WM_IDLE_AMP) * WMO_NOISE_GAIN,
                         1.0f - WMO_NOISE_GAIN * WM_IDLE_AMP,
                         1.0f + WMO_NOISE_GAIN * (1.0f - WM_IDLE_AMP));
    c->spin  = wmo_clamp(1.0f + p->detail[3] * WMO_SPIN_GAIN,
                         1.0f, 1.0f + WMO_SPIN_GAIN);
    c->lift  = wmo_clamp(p->lift, 0.0f, 1.0f) * WMO_LIFT_ACC;
    c->emit  = wmo_clamp(1.0f + (p->bright - WM_IDLE_BRIGHT) * WMO_EMIT_GAIN,
                         1.0f - WMO_EMIT_GAIN * WM_IDLE_BRIGHT,
                         1.0f + WMO_EMIT_GAIN * (1.0f - WM_IDLE_BRIGHT));
    c->glint = wmo_clamp(1.0f + p->glow * WMO_GLOW_GAIN,
                         1.0f, 1.0f + WMO_GLOW_GAIN);
    c->tint  = wmo_clamp(0.5f + (p->hue - hue_mid) * (1.0f / WM_HUE_SPAN),
                         0.0f, 1.0f);
    for (i = 0; i < WM_PULSES; i++) {
        const wm_pulse *q = &p->pulse[i];
        float x  = wmo_clamp(q->x, -0.5f, 1.5f);
        float hw = (WMO_PULSE_X1 - WMO_PULSE_X0) * wmo_clamp(q->width, 0.02f, 0.5f);
        c->px[i]   = WMO_PULSE_X0 + (WMO_PULSE_X1 - WMO_PULSE_X0) * x;
        c->pinv[i] = 1.0f / hw;
        c->pa[i]   = q->live ? wmo_clamp(q->amp, 0.0f, 1.0f) : 0.0f;
    }
}

// --- state ----------------------------------------------------------------

typedef struct {
    int      n;                                // live count, 0..WMO_MAX
    int      ready;
    float    x[WMO_MAX],  y[WMO_MAX],  z[WMO_MAX];
    float    vx[WMO_MAX], vy[WMO_MAX], vz[WMO_MAX];
    float    age[WMO_MAX], life[WMO_MAX];
    float    ang[WMO_MAX], spin[WMO_MAX];      // tumble angle, rad/s signed
    float    e1x[WMO_MAX], e1y[WMO_MAX], e1z[WMO_MAX];   // spin plane basis
    float    e2x[WMO_MAX], e2y[WMO_MAX], e2z[WMO_MAX];
    float    rad[WMO_MAX];                     // world radius
    float    exc[WMO_MAX];                     // pulse excitation, 0..1
    uint32_t rng[WMO_MAX];
} wmo_state;

// One step of a per-mote LCG and its top 24 bits as [0,1).  Numerical Recipes'
// constants; a mote's stream is independent of every other's because each
// starts from its own hashed seed.
static inline float wmo_rand(uint32_t *s)
{
    *s = *s * 1664525u + 1013904223u;
    return (float)(*s >> 8) * (1.0f / 16777216.0f);
}

static inline float wmo_yc(float x) { return WMO_YC0 + WMO_YC_K * x; }
static inline float wmo_zc(float x) { return WMO_ZC0 + WMO_ZC_K * x; }

// Fill `count` motes (clamped to 0..WMO_MAX) from `seed`.  The cloud starts
// ALREADY SETTLED -- positions drawn from the steady spread, ages spread over
// each lifetime -- so there is no formation transient on the first frames and
// no wave of simultaneous births later.
static inline int wmo_init(wmo_state *s, int count, uint32_t seed)
{
    int i;
    if (!s) return 0;
    memset(s, 0, sizeof *s);
    if (count < 0) count = 0;
    if (count > WMO_MAX) count = WMO_MAX;
    s->n = count;

    for (i = 0; i < count; i++) {
        uint32_t r = (seed ^ 0x9e3779b9u) + (uint32_t)i * 0x85ebca6bu;
        jw_vec3  a, b, e1, e2;
        float    g1, g2, spin;
        int      k;

        // Scramble the seed so neighbouring motes' streams do not start
        // correlated -- a raw i*const start shows up as diagonal banding in
        // the first few hundred frames.
        r ^= r >> 16; r *= 0x7feb352du; r ^= r >> 15; r *= 0x846ca68bu; r ^= r >> 16;
        for (k = 0; k < 4; k++) (void)wmo_rand(&r);

        s->x[i] = WMO_X_LO + (WMO_X_HI - WMO_X_LO) * wmo_rand(&r);
        g1 = (wmo_rand(&r) + wmo_rand(&r) - 1.0f) * 2.4494897f;
        g2 = (wmo_rand(&r) + wmo_rand(&r) - 1.0f) * 2.4494897f;
        s->y[i] = wmo_yc(s->x[i]) + WMO_SD_Y * g1;
        s->z[i] = wmo_zc(s->x[i]) + WMO_SD_Z * g2;

        s->life[i] = WMO_LIFE_MIN + (WMO_LIFE_MAX - WMO_LIFE_MIN) * wmo_rand(&r);
        s->age[i]  = s->life[i] * wmo_rand(&r);

        spin = WMO_SPIN_MIN + (WMO_SPIN_MAX - WMO_SPIN_MIN) * wmo_rand(&r);
        s->spin[i] = (wmo_rand(&r) < 0.5f) ? -spin : spin;
        s->ang[i]  = WK_TWO_PI * wmo_rand(&r);
        s->rad[i]  = WMO_RAD_MIN + (WMO_RAD_MAX - WMO_RAD_MIN) * wmo_rand(&r);

        // A random spin plane: e1 any unit vector, e2 perpendicular to it.
        // A degenerate draw falls back to a fixed plane rather than a NaN.
        a  = jw_v3(wmo_rand(&r) - 0.5f, wmo_rand(&r) - 0.5f, wmo_rand(&r) - 0.5f);
        b  = jw_v3(wmo_rand(&r) - 0.5f, wmo_rand(&r) - 0.5f, wmo_rand(&r) - 0.5f);
        e1 = jw_norm(a);
        e2 = jw_norm(jw_cross(e1, b));
        if (!(jw_dot(e1, e1) > 0.5f) || !(jw_dot(e2, e2) > 0.5f)) {
            e1 = jw_v3(1.0f, 0.0f, 0.0f);
            e2 = jw_v3(0.0f, 1.0f, 0.0f);
        }
        s->e1x[i] = e1.x; s->e1y[i] = e1.y; s->e1z[i] = e1.z;
        s->e2x[i] = e2.x; s->e2y[i] = e2.y; s->e2z[i] = e2.z;
        s->rng[i] = r;
    }
    s->ready = 1;
    return 1;
}

// --- the step ---------------------------------------------------------------
//
// Advance every mote by dt seconds under this frame's controls.  c may be
// NULL (idle).  A non-finite or non-positive dt leaves the state untouched.
//
// Semi-implicit Euler with the drag applied implicitly, which is
// unconditionally stable for any dt this can be handed after the WMO_DT_MAX
// cap.  The random forcing is scaled by sqrt(dt) so the spread does not
// depend on the frame rate.
//
// RESPAWN IS A SELECT, NOT A BLEND.  jf_anim_kernel.h respawns with
// keep*x + dead*x0, which is fine for it; here one of the reasons to respawn
// is that a value went non-finite, and 0 * NaN is NaN -- the blend would carry
// the fault into the new mote.  A ternary picks one side and never multiplies
// the other.
//
// The respawn candidate reuses this frame's random draws.  They are
// independent of the mote's current state, so the candidate is correctly
// distributed, and on a respawn frame the forcing they also fed is thrown
// away with the old velocity -- six draws a mote a frame instead of ten.
static inline void wmo_step(wmo_state *s, const wmo_ctl *c, float dt)
{
    wmo_ctl idle;
    float   h, sq, damp, dec;
    int     i, j;

    if (!s || !s->ready) return;
    if (!(dt > 0.0f) || !(dt < 1.0e30f)) return;
    if (dt > WMO_DT_MAX) dt = WMO_DT_MAX;
    if (!c) { wmo_controls(NULL, &idle); c = &idle; }

    h    = dt * c->ts;
    sq   = h * jw_rsqrt(h);                  // sqrt(h), no libm
    damp = 1.0f / (1.0f + WMO_GAMMA * h);
    dec  = 1.0f / (1.0f + h * (1.0f / WMO_EXC_TAU));

    for (i = 0; i < s->n; i++) {
        float x = s->x[i], y = s->y[i], z = s->z[i];
        float vx = s->vx[i], vy = s->vy[i], vz = s->vz[i];
        float u1, u2, u3, u4, u5, u6, gx, gy, gz;
        float yc, zc, ax, ay, az, w, age, ex;
        float nx, ny, nz, nlife;
        int   dead;

        u1 = wmo_rand(&s->rng[i]); u2 = wmo_rand(&s->rng[i]);
        u3 = wmo_rand(&s->rng[i]); u4 = wmo_rand(&s->rng[i]);
        u5 = wmo_rand(&s->rng[i]); u6 = wmo_rand(&s->rng[i]);
        // Sums of two uniforms scaled to unit variance: a triangular stand-in
        // for a gaussian, which a spread built from thousands of steps
        // cannot tell apart.
        gx = (u5 + u6 - 1.0f) * 2.4494897f;
        gy = (u1 + u2 - 1.0f) * 2.4494897f;
        gz = (u3 + u4 - 1.0f) * 2.4494897f;

        yc = wmo_yc(x);
        zc = wmo_zc(x);

        // Pulses: a smooth compact bump, (1 - q^2)^2, around each.
        w = 0.0f;
        for (j = 0; j < WM_PULSES; j++) {
            float q = (x - c->px[j]) * c->pinv[j];
            float t = 1.0f - q * q;
            t  = (t > 0.0f) ? t : 0.0f;
            w += c->pa[j] * t * t;
        }
        w = (w < 1.0f) ? w : 1.0f;

        ax = WMO_WIND;
        ay = -WMO_K * (y - yc) + c->lift + WMO_PULSE_KICK * w;
        az = -WMO_K * (z - zc);

        vx = (vx + ax * h + WMO_SIGMA_X * c->noise * sq * gx) * damp;
        vy = (vy + ay * h + WMO_SIGMA_Y * c->noise * sq * gy) * damp;
        vz = (vz + az * h + WMO_SIGMA_Z * c->noise * sq * gz) * damp;
        vx = (vx > WMO_VMAX) ? WMO_VMAX : ((vx < -WMO_VMAX) ? -WMO_VMAX : vx);
        vy = (vy > WMO_VMAX) ? WMO_VMAX : ((vy < -WMO_VMAX) ? -WMO_VMAX : vy);
        vz = (vz > WMO_VMAX) ? WMO_VMAX : ((vz < -WMO_VMAX) ? -WMO_VMAX : vz);

        x += vx * h;
        y += vy * h;
        z += vz * h;

        ex  = s->exc[i] * dec;
        ex  = (w > ex) ? w : ex;
        age = s->age[i] + h;

        // Bitwise OR, not ||: every test is evaluated, so there is no branch.
        // NaN fails every ordered compare, so `x != x` is the only test that
        // catches it; an infinity is caught by the range tests.
        yc = wmo_yc(x);
        zc = wmo_zc(x);
        dead = (age >= s->life[i])
             | (x > WMO_X_HI) | (x < WMO_X_LO)
             | (y - yc > WMO_BOX_Y) | (y - yc < -WMO_BOX_Y)
             | (z - zc > WMO_BOX_Z) | (z - zc < -WMO_BOX_Z)
             | (x != x) | (y != y) | (z != z)
             | (vx != vx) | (vy != vy) | (vz != vz);

        // Respawn into the cloud as it is NOW -- lift included (the spring
        // settles lift/K above the centreline) and at the current spread.
        // Refilling at the idle shape instead held a lifted cloud short of
        // its lift and a breathing one short of its breath.
        nx    = WMO_X_LO + (WMO_X_HI - WMO_X_LO) * u5;
        ny    = wmo_yc(nx) + c->lift * (1.0f / WMO_K) + WMO_SD_Y * c->noise * gy;
        nz    = wmo_zc(nx) + WMO_SD_Z * c->noise * gz;
        nlife = WMO_LIFE_MIN + (WMO_LIFE_MAX - WMO_LIFE_MIN) * u6;

        s->x[i]    = dead ? nx    : x;
        s->y[i]    = dead ? ny    : y;
        s->z[i]    = dead ? nz    : z;
        s->vx[i]   = dead ? 0.0f  : vx;
        s->vy[i]   = dead ? 0.0f  : vy;
        s->vz[i]   = dead ? 0.0f  : vz;
        s->age[i]  = dead ? 0.0f  : age;
        s->life[i] = dead ? nlife : s->life[i];
        s->exc[i]  = dead ? 0.0f  : ex;
        s->ang[i]  = wk_wrap2pi(s->ang[i] + s->spin[i] * c->spin * h);
    }
}

// --- shading --------------------------------------------------------------

// One finished mote, for the renderer.  Like wave_gel.h's jw_vert this is NOT
// the RSX vertex layout and must never be written to video memory as-is: the
// renderer expands it into its own quads.  a == 0 means "do not draw".
typedef struct {
    float         x, y;        // clip-space centre, +y up
    float         s;           // clip-space half-size, in y units
    float         z;           // view depth -- behind or in front of the gel
    unsigned char r, g, b, a;
} wmo_sprite;

// The key light's half vector as seen from the cloud's centre.  Computed per
// shade call rather than per mote -- see the file comment.
static inline jw_vec3 wmo_half_vector(void)
{
    jw_vec3 ev = jw_eyevec(jw_v3(0.0f, WMO_YC0, WMO_ZC0));
    jw_vec3 k  = jw_key_dir();
    return jw_norm(jw_v3(k.x + ev.x, k.y + ev.y, k.z + ev.z));
}

// Mote i's flake normal, turned to face the half vector -- a flake has two
// faces and either can glint.  Returns n . h (>= 0) through *d.
static inline jw_vec3 wmo_normal(const wmo_state *s, int i, jw_vec3 hv,
                                 float *d)
{
    float   sa = wk_sinf(s->ang[i]);
    float   ca = wk_sinf(s->ang[i] + WK_HALF_PI);
    jw_vec3 n  = jw_v3(ca * s->e1x[i] + sa * s->e2x[i],
                       ca * s->e1y[i] + sa * s->e2y[i],
                       ca * s->e1z[i] + sa * s->e2z[i]);
    float   dd = jw_dot(n, hv);
    float   f  = (dd < 0.0f) ? -1.0f : 1.0f;
    if (d) *d = dd * f;
    return jw_v3(n.x * f, n.y * f, n.z * f);
}

// Shade and project up to `cap` motes into `out`, one sprite per mote in
// state order, and return how many are visible (a > 0).  c may be NULL
// (idle).  `aspect` is display_width / display_height, as for jw_project.
//
// Costs two divides a mote (the projection's, and 1/z for size and blur),
// two polynomial sines for the flake, and one LUT read.
static inline int wmo_shade(const wmo_state *s, const wmo_ctl *c, float aspect,
                            wmo_sprite *out, int cap)
{
    wmo_ctl idle;
    jw_vec3 hv;
    float   cr, cg, cb, invf;
    int     i, n, vis = 0;

    if (!s || !s->ready || !out || cap <= 0) return 0;
    if (!c) { wmo_controls(NULL, &idle); c = &idle; }
    n = (s->n < cap) ? s->n : cap;

    hv   = wmo_half_vector();
    invf = 1.0f / WMO_FOCUS_Z;
    // The mote's own colour, once per call: fill violet toward rim blue.
    cr = JW_FILL_R + (JW_RIM_R - JW_FILL_R) * c->tint;
    cg = JW_FILL_G + (JW_RIM_G - JW_FILL_G) * c->tint;
    cb = JW_FILL_B + (JW_RIM_B - JW_FILL_B) * c->tint;

    for (i = 0; i < n; i++) {
        jw_vec3 p = jw_v3(s->x[i], s->y[i], s->z[i]);
        jw_proj pr = jw_project(p, aspect);
        float   zs = pr.ok ? pr.z : 1.0f;
        float   iz = 1.0f / zs;
        float   sg = JW_FOCAL * s->rad[i] * iz;
        float   bl = invf - iz;
        float   sd, fall, t, env, top, cf, mg, m2, alpha, d, spec, lit, em, gl;
        jw_vec3 nrm;
        int     show;

        bl = (bl < 0.0f) ? -bl : bl;
        sd = sg + WMO_COC_K * bl;
        sd = (sd > WMO_MIN_S) ? sd : WMO_MIN_S;
        fall = sg / sd;

        // Fade in and out over WMO_FADE at each end of the life, smoothstep.
        t = s->age[i];
        t = (s->life[i] - t < t) ? s->life[i] - t : t;
        t = t * (1.0f / WMO_FADE);
        t = (t < 0.0f) ? 0.0f : ((t > 1.0f) ? 1.0f : t);
        env = t * t * (3.0f - 2.0f * t);

        top = pr.y + sd;
        cf  = (WMO_CLIP_TOP - top) * (1.0f / WMO_CLIP_FADE);
        cf  = (cf < 0.0f) ? 0.0f : ((cf > 1.0f) ? 1.0f : cf);

        // Margin fade: dim to nothing before any boundary that respawns a
        // mote, so the ONLY deaths that happen at non-zero alpha are none.
        // Lifetime deaths are already covered by env.
        mg = (s->x[i] - WMO_X_LO < WMO_X_HI - s->x[i]) ? s->x[i] - WMO_X_LO
                                                       : WMO_X_HI - s->x[i];
        mg = mg * (1.0f / WMO_X_FADE);
        t  = s->y[i] - wmo_yc(s->x[i]);
        t  = (t < 0.0f) ? -t : t;
        m2 = (WMO_BOX_Y - t) * (1.0f / WMO_SD_Y);
        mg = (m2 < mg) ? m2 : mg;
        t  = s->z[i] - wmo_zc(s->x[i]);
        t  = (t < 0.0f) ? -t : t;
        m2 = (WMO_BOX_Z - t) * (1.0f / WMO_SD_Z);
        mg = (m2 < mg) ? m2 : mg;
        mg = (mg < 0.0f) ? 0.0f : ((mg > 1.0f) ? 1.0f : mg);

        alpha = jw_clamp01(WMO_ALPHA * env * cf * mg * fall * fall);

        nrm  = wmo_normal(s, i, hv, &d);
        spec = jw_lut(JW_SPEC_LUT, d) * JW_SPEC_I;
        lit  = jw_key_lit(nrm);

        em = c->emit * (1.0f + WMO_EXC_GAIN * s->exc[i]);
        gl = WMO_GLINT_I * c->glint * spec;

        show = pr.ok & (pr.x - sd < 1.0f) & (pr.x + sd > -1.0f)
             & (pr.y - sd < 1.0f) & (pr.y + sd > -1.0f);

        out[i].x = pr.x;
        out[i].y = pr.y;
        out[i].s = sd;
        out[i].z = pr.z;
        out[i].r = jw_u8(em * (WMO_BASE_I * cr + (WMO_DIFF_I * lit + gl) * JW_KEY_R));
        out[i].g = jw_u8(em * (WMO_BASE_I * cg + (WMO_DIFF_I * lit + gl) * JW_KEY_G));
        out[i].b = jw_u8(em * (WMO_BASE_I * cb + (WMO_DIFF_I * lit + gl) * JW_KEY_B));
        out[i].a = show ? jw_u8(alpha) : 0;
        vis += (out[i].a != 0);
    }
    return vis;
}

// --- WIRING IT IN, LATER --------------------------------------------------
//
// What ui_wave.cpp will need, none of which exists yet:
//
//   - a wmo_state (static, 156 KB) and wmo_init(&s, WMO_COUNT_DEF, seed) at
//     wave_init();
//   - wmo_controls(&stage_b_params, &c) beside wave_audio_look(), which means
//     ui_wave_audio.cpp exposing stage B's wm_params -- it does not today;
//   - wmo_step every wave_draw() call, like wf_step, and wmo_shade at the
//     JellyWave rebuild cadence;
//   - each visible sprite as a quad into the staged vertex buffer through
//     jw_upload, under ADDITIVE blend, which makes draw order irrelevant
//     within the cloud; motes with z beyond the band's depth drawn before the
//     gel and the rest after, which is what `z` is for;
//   - its own gate file, off by default, like every other RSX-facing path.
//
// And first, a frame budget that can take it.

#endif // WAVE_MOTES_H
