// JellyDrop: the three JellyWave ribbons, each closed into a Jellyfin bell.
//
//   wave_gel.h       the lofted translucent body (JellyWave)
//   jd_bell_geom.h   the bell's ring, generated from the lockup path
//   wave_drop.h      the same body lofted round the bell        (this file)
//
// The approved concept is design-import-jellydrop/ (Claude Design project
// b738fc7b, "JellyDrop", 2026-09-26): one mesh, two lofts.  morph = 0 is the
// shipping JellyWave loft, vertex for vertex; morph = 1 is the SAME vertices
// (station i, section point j) lofted round the bell.  Nothing is
// cross-faded -- every vertex travels, so the ribbons visibly retract, stand
// up and curl into the mark.  The front layer settles as the mark itself; the
// other two become thinner shells tipped into orbit round it, and the hole
// lights up as a core.
//
// Deliberate departures from the design, at the user's request (2026-09-26):
//
//   * It is not a music-visualiser preset.  L1 + R1 together toggle it
//     anywhere the wave is drawn (ui_input.cpp), and the choice is kept.
//   * It floats BEHIND everything: centred on the screen, behind the cards,
//     posters and text, never over them (wave_draw_front stands down).
//   * Depth comes from particles orbiting it (jd_motes below): the ones on
//     the far side of the orbit are drawn behind the gel, the near ones in
//     front, larger and brighter.
//   * It rocks (+/-0.35 rad on the design clock) instead of turning a full
//     circle every ~40 s: an upside-down mark behind the menus reads as a
//     fault, not a flourish.
//
// What is ported from the design verbatim: the per-layer table (JD_LAYER),
// the spring constants of the soft-body motion (jd_motion_step), the ring
// deformations (life, the travelling bump, the three-lobe ripple, the
// five-lobe section ripple, stereo spread), the transition's staggers and
// the point-closing of the strands mid-flight, the orbit frames, and the
// core's three nested additive shells.  What is not: the treble glints (the
// rim pass has no per-vertex sparkle here), the reflection pass (a second
// draw of every mesh through a mirror -- not worth its fill behind the XMB),
// per-vertex Fresnel ALPHA (the emitter carries one alpha per layer; the
// layers' alphas move toward the design's instead), and the five PROPOSED
// wa_features fields, which the analyser does not produce.
//
// House rules (.clinerules rule 7): header-only, pure C, no libm, no PS3
// headers, deterministic, caller-owned state, no globals.

#ifndef WAVE_DROP_H
#define WAVE_DROP_H

#include "wave_gel.h"
#include "jd_bell_geom.h"

#if JD_RING_N != JW_STATIONS
#error "jd_bell_geom.h must be generated with JW_STATIONS stations"
#endif

// --- small maths, no libm ----------------------------------------------------
static inline float jd_clamp(float v, float lo, float hi)
{
    if (v != v) return lo;
    return v < lo ? lo : (v > hi ? hi : v);
}

static inline float jd_floor(float x)
{
    float t = (float)(int)x;
    return (t > x) ? t - 1.0f : t;
}

static inline float jd_frac(float x) { return x - jd_floor(x); }

// exp(-x) for x >= 0, as 2^-(x / ln 2): the whole part goes straight into the
// float's exponent, the fraction through a degree-5 Taylor series of
// exp(-f ln 2) on [0, 1) -- under 3e-4 relative error, which test_wave_drop
// checks against libm.
static inline float jd_expn(float x)
{
    float y, f, p, s;
    int   i;
    uint32_t bits;
    if (!(x > 0.0f)) return 1.0f;
    if (x > 80.0f) return 0.0f;
    y = x * 1.44269504f;
    i = (int)y;
    f = (y - (float)i) * 0.69314718f;
    p = 1.0f - f * (1.0f - f * (0.5f - f * (0.16666667f - f * (0.04166667f - f * 0.00833333f))));
    bits = (uint32_t)(127 - i) << 23;          // 2^-i, i <= 115 here
    memcpy(&s, &bits, sizeof s);
    return p * s;
}

// c + (t - c) * (1 - exp(-dt / tau)): the design's one-pole approach.
static inline float jd_approach(float c, float t, float dt, float tau)
{
    return c + (t - c) * (1.0f - jd_expn(dt / tau));
}

static inline float jd_sqrt(float x) { return x > 0.0f ? x * jw_rsqrt(x) : 0.0f; }

static inline float jd_back_out(float a)
{
    const float c1 = 1.25f, c3 = c1 + 1.0f, x = a - 1.0f;
    return 1.0f + c3 * x * x * x + c1 * x * x;
}

// --- the layers ------------------------------------------------------------------
// The design's LAYERS table, bell half only.  Layer 0 is the mark, facing the
// viewer, in half-set gel; 1 and 2 are thinner shells orbiting it on tilted
// axes -- 1 almost opaque, 2 glassy.  `bright` is the design's settled
// brightness, `alpha` what the layer's single alpha moves to (the design
// modulates it per vertex by Fresnel; see the header).
typedef struct {
    float ls, thin, life, tend, tilt, off, bright;
    float alpha;
} jd_layer;

static const jd_layer JD_LAYER[JW_LAYERS] = {
    /* mark   */ { 1.00f, 1.00f, 0.70f, 0.0f,  0.00f, 0.0f, 0.78f, 150.0f },
    /* opaque */ { 1.10f, 0.50f, 1.10f, 1.0f,  0.16f, 0.0f, 0.62f, 190.0f },
    /* glassy */ { 1.20f, 0.42f, 1.30f, 1.0f, -0.14f, 2.1f, 0.62f,  80.0f },
};

// --- the motion ------------------------------------------------------------------
// The design's JD.Motion, stage B for the preset: springs where it asks for
// inertia (bass, beat, drop), one-pole slews elsewhere; every target is
// neutral at rest so silence still moves.
typedef struct {
    float sub, bass, lowmid, mid, high, air;   // 0..1 bands
    float rms;                                 // 0..1 loudness
    float silence;                             // 0..1
    float onset, onset_strength;               // a beat this frame
    float beat_hz, beat_conf;                  // 0 = unknown
    float balance, width, pitch;               // -1..1, 0..1, -1..1 (0 = none)
    float drop;                                // 1 on the frame a drop lands
} jd_in;

typedef struct {
    float t, ts, spin, spin_w;
    float bounce, bounce_v, jig, jig_v, rest, last_sil;
    float wx, wy, wvx, wvy, wt, w_amp, calm, wax;
    float shear, shear_v, sub_prev, swell, swell_v, sq, sq_v;
    float drop, drop_v, yaw, yaw_v, orbit, roll;
    float lean, shift, tilt, spread, rip_a, rip_b, ph1, ph2, ph3;
    float glow, bright, kick, wph;
    float pace, turn, turn_w;   // 2026-09-27: smoothed clock rate; the continuous spin
    float cam_t;                // the camera's own slow clock (real seconds)
} jd_motion;

static inline void jd_motion_init(jd_motion *m)
{
    if (!m) return;
    memset(m, 0, sizeof *m);
    m->ts = 1.0f; m->spin_w = 0.3f; m->w_amp = 0.4f;
    m->rip_a = 0.02f; m->rip_b = 0.01f; m->glow = 0.6f; m->bright = 0.95f;
    m->pace = 1.7f; m->turn_w = 0.55f;
}

static inline void jd_motion_step(jd_motion *m, const jd_in *F, float dt)
{
    float live, ts_t, sub, d_sub, b_a, j_f, en, reach, tx, ty, ax, ay;
    if (!m || !F) return;
    if (!(dt > 0.0f)) return;
    if (dt > 0.05f) dt = 0.05f;
    live = 1.0f - F->silence;
    // Pace (2026-09-27): the design's clocks drifted; on a TV it read as
    // half-asleep next to JellyWave.  Every PHASE clock runs 1.7x, up to ~3x
    // when it is loud.  The springs keep real time -- they are physics.
    // ... SMOOTHED over ~0.6 s: taken straight from the frame's loudness the
    // clock rate itself jittered, which read as jerky (hardware, 09-27).
    // (1.9 + 1.5 x loudness was far too fast with music playing: user, 09-27.)
    m->pace = jd_approach(m->pace, 1.25f + 0.6f * jd_clamp(F->rms * live, 0.0f, 1.0f), dt, 1.2f);
    const float dtp = dt * m->pace;
    m->t += dtp;

    ts_t = F->beat_hz > 0.0f ? jd_clamp(F->beat_hz * 0.5f, 0.7f, 1.6f) : 1.0f;
    m->ts = jd_approach(m->ts, 1.0f + (ts_t - 1.0f) * F->beat_conf, dt, 1.2f);

    // Soft body.  SUB drives a slow, heavy swell whose changes also excite the
    // jiggle mode; hits throw the mass up on a loose spring and every landing
    // feeds the jiggle and a lagging shear, so it wobbles like set gel.
    sub = F->sub * live;
    m->swell_v += (26.0f * (sub - m->swell) - 3.4f * m->swell_v) * dt;
    m->swell   += m->swell_v * dt;
    d_sub = jd_clamp((sub - m->sub_prev) / dt, -8.0f, 8.0f);
    m->sub_prev = sub;
    if (F->onset > 0.0f) m->bounce_v += 1.5f * F->onset_strength * (0.35f + F->bass * live);
    b_a = -9.0f * m->bounce - 2.4f * m->bounce_v;
    m->bounce_v += b_a * dt; m->bounce += m->bounce_v * dt;
    j_f = -0.05f * b_a + 0.02f * d_sub + 0.0014f * jd_clamp(m->wax < 0 ? -m->wax : m->wax, 0.0f, 160.0f);
    m->jig_v += (-30.0f * m->jig - 3.2f * m->jig_v + 6.0f * j_f) * dt;
    m->jig   += m->jig_v * dt;
    m->shear_v += (-22.0f * m->shear - 2.6f * m->shear_v + 0.4f * m->bounce_v + 0.15f * m->yaw_v
                   - 0.0009f * jd_clamp(m->wax, -200.0f, 200.0f)) * dt;
    m->shear += m->shear_v * dt;
    if (F->onset > 0.0f) {
        m->sq_v += 1.2f * F->onset_strength;
        m->yaw_v += 0.22f * F->onset_strength;
        m->turn_w += 0.3f * F->onset_strength;      // a beat nudges the spin
        m->kick += 0.30f * F->onset_strength;
    }
    m->sq_v += (-36.0f * m->sq - 4.2f * m->sq_v) * dt; m->sq += m->sq_v * dt;
    if (F->drop > 0.0f) m->drop_v += 6.5f;
    m->drop_v += (-5.5f * m->drop - 1.15f * m->drop_v) * dt; m->drop += m->drop_v * dt;
    m->yaw_v += (-9.0f * m->yaw - 3.0f * m->yaw_v) * dt; m->yaw += m->yaw_v * dt;
    // The spin: continuous, never a sine back and forth.  It coasts back to
    // a steady turn after each beat's throw.
    // ... at the SONG's tempo: one full turn every 16 beats (four bars), so
    // at 120 BPM one turn in 8 s.  An unsure tempo leans to 0.55 rad/s.
    {
        const float bpm_w = F->beat_hz > 0.5f ? WK_TWO_PI * F->beat_hz / 16.0f : 0.55f;
        const float tw_t  = 0.55f + (bpm_w - 0.55f) * jd_clamp(F->beat_conf, 0.0f, 1.0f);
        m->turn_w = jd_approach(m->turn_w, tw_t, dt, 1.4f);
    }
    m->cam_t += dt;
    if (m->cam_t > 3600.0f) m->cam_t -= 3600.0f;
    m->turn  += m->turn_w * dt;
    if (m->turn > 1000.0f) m->turn -= 159.0f * WK_TWO_PI;
    m->orbit += dtp * 0.34f * m->ts;
    // The design spins it a full turn every ~40 s; behind the XMB an upside-down
    // mark reads as a fault, so here the same clock ROCKS it, +/-0.35 rad.
    m->roll  += dtp * 0.16f * m->ts;
    if (m->roll > 1000.0f) m->roll -= 159.0f * WK_TWO_PI;       // whole turns

    // Wander, in 1280x720 stage px from rest.  The reach follows loudness; a
    // drop pulls it to centre and releases over seconds; a soft wall keeps it
    // inside the box.
    // The whole screen (2026-09-27): a free body drifting at a steady speed,
    // faster with the music, bouncing off the screen's edges -- a squash, a
    // jiggle and a nudge of spin on every hit -- instead of wandering a
    // small box round the middle.  Stage px: 1280 x 720, (0,0) the centre.
    en = jw_smooth(0.04f, 0.55f, F->rms * live);
    m->w_amp = jd_approach(m->w_amp, 0.18f + 0.82f * en, dt, 1.2f);
    (void)reach; (void)tx; (void)ty; (void)ax; (void)ay;
    {
        if (m->wvx == 0.0f && m->wvy == 0.0f) { m->wvx = 150.0f; m->wvy = 92.0f; }
        // Slow and floaty (user, 09-27: far too fast), and SOFT edges: a
        // spring pushes back from a band inside each edge, so it curves away
        // from the wall instead of snapping round rigidly on contact.
        const float want = (40.0f + 55.0f * en) * (1.0f - 0.5f * m->rest);
        const float sp = jd_sqrt(m->wvx * m->wvx + m->wvy * m->wvy);
        if (sp > 1.0f) {
            float k = jd_approach(sp, want, dt, 2.5f) / sp;
            if (F->onset > 0.0f) k *= 1.0f + 0.05f * F->onset_strength;
            m->wvx *= k; m->wvy *= k;
        }
        const float EX = 640.0f - 118.0f, EY = 360.0f - 108.0f;   // the bell's half extent
        const float BX = EX - 170.0f, BY = EY - 120.0f;             // where the push starts
        float ax_ = 0.0f, ay_ = 0.0f;
        if (m->wx >  BX) ax_ -= 0.9f * (m->wx - BX);
        if (m->wx < -BX) ax_ += 0.9f * (-BX - m->wx);
        if (m->wy >  BY) ay_ -= 1.1f * (m->wy - BY);
        if (m->wy < -BY) ay_ += 1.1f * (-BY - m->wy);
        m->wvx += ax_ * dt; m->wvy += ay_ * dt;
        m->wx += m->wvx * dt; m->wy += m->wvy * dt;
        // the hard limit, only ever a safety net
        if (m->wx >  EX) { m->wx =  EX; if (m->wvx > 0.0f) m->wvx = -0.3f * m->wvx; }
        if (m->wx < -EX) { m->wx = -EX; if (m->wvx < 0.0f) m->wvx = -0.3f * m->wvx; }
        if (m->wy >  EY) { m->wy =  EY; if (m->wvy > 0.0f) m->wvy = -0.3f * m->wvy; }
        if (m->wy < -EY) { m->wy = -EY; if (m->wvy < 0.0f) m->wvy = -0.3f * m->wvy; }
        m->wax = jd_approach(m->wax, ax_ * 0.4f, dt, 0.4f);
    }

    if (F->onset > 0.0f) m->spin_w += 0.25f * F->onset_strength;
    m->spin_w = jd_approach(m->spin_w, 0.6f * m->ts, dt, 1.6f);
    m->spin  += m->spin_w * dtp;
    if (m->spin > 1000.0f) m->spin -= 159.0f * WK_TWO_PI;
    m->lean   = jd_approach(m->lean, -F->balance * 0.26f, dt, 0.5f);
    m->shift  = jd_approach(m->shift, F->balance, dt, 0.6f);
    m->tilt   = jd_approach(m->tilt, F->pitch * 0.34f, dt, 0.7f);
    m->spread = jd_approach(m->spread, F->width, dt, 0.8f);
    m->rip_a  = jd_approach(m->rip_a, 0.03f + 0.20f * F->mid * live, dt, 0.07f);
    m->rip_b  = jd_approach(m->rip_b, 0.02f + 0.16f * F->lowmid * live, dt, 0.09f);
    m->ph1 += dtp * m->ts * (1.4f + 2.4f * F->mid);
    m->ph2 += dtp * m->ts * (1.0f + 1.6f * F->lowmid);
    m->ph3 += dtp * m->ts * (0.8f + 0.6f * F->width);
    if (m->ph1 > 1000.0f) m->ph1 -= 159.0f * WK_TWO_PI;
    if (m->ph2 > 1000.0f) m->ph2 -= 159.0f * WK_TWO_PI;
    if (m->ph3 > 1000.0f) m->ph3 -= 159.0f * WK_TWO_PI;
    m->kick *= jd_expn(dt / 0.35f);
    // Paused / silent: sink slowly, dim the core, keep breathing; resume rises
    // with a wobble.
    m->rest = jd_approach(m->rest, F->silence, dt, F->silence > m->rest ? 2.2f : 0.8f);
    if (m->last_sil > 0.5f && F->silence < 0.5f) { m->bounce_v += 0.35f; m->jig_v += 0.4f; }
    m->last_sil = F->silence;
    m->glow   = jd_approach(m->glow, 0.62f - 0.32f * m->rest + 0.5f * F->rms * live + 0.35f * m->kick, dt, 0.18f);
    m->bright = jd_approach(m->bright, 0.9f + 0.12f * F->rms * live, dt, 0.5f);
    m->wph += dtp * m->ts * (0.22f + 0.35f * F->lowmid * live);
    if (m->t > 3600.0f) m->t -= 3600.0f;             // the clocks below only need phase
}

// --- the pose: everything a build needs, one frame's worth -------------------
// Computed on the render thread (jd_place), copied into the build's snapshot,
// read on the generation worker.  morph = 0 means "JellyWave": the build then
// takes the untouched wave_gel.h path, bit for bit.
typedef struct {
    float   morph;          // 0 wave .. 1 bell, eased by the caller's clock
    jw_vec3 P;              // the bell's centre, world
    float   S;              // world units per viewBox unit, swell included
    float   S0;             // ... without the swell (the core scales itself)
    float   R[9];           // local -> world, column-major: X, Y, Z columns
    jw_vec3 tc;             // toward the camera (unit)
    float   t, ts;
    float   swell, drop, jig, sq, shear, rip_a, rip_b, ph1, ph2, ph3, spread;
    float   glow, kick, spin, orbit, wph, rest;
} jd_pose;

// Where the bell sits: the screen point (cx, cy) in NDC (+y up), its height as
// a fraction of the screen height, and the distance from the eye it floats at
// (the design's DIST, 9.5).
#define JD_DIST     9.5f
#define JD_VIEWBOX  72.0f

static inline void jd_m3_mul(const float *a, const float *b, float *o)   // o = a * b, column-major
{
    int r, c;
    for (c = 0; c < 3; c++)
        for (r = 0; r < 3; r++)
            o[c * 3 + r] = a[0 * 3 + r] * b[c * 3 + 0] + a[1 * 3 + r] * b[c * 3 + 1]
                         + a[2 * 3 + r] * b[c * 3 + 2];
}

static inline void jd_place(const jd_motion *m, float morph, float aspect,
                            float cx_ndc, float cy_ndc, float h_frac, jd_pose *o)
{
    jw_vec3 dir;
    float   g, s0, ex, ey, ez, bank, dive, t;
    float   Rb[9], Rz[9], Rx[9], Ry[9], T1[9], T2[9];
    if (!m || !o) return;
    if (!(aspect > 0.01f)) aspect = 16.0f / 9.0f;
    memset(o, 0, sizeof *o);
    o->morph = jd_clamp(morph, 0.0f, 1.0f);
    t = m->t;

    // The wander, stage px (1280x720, y down) -> NDC.
    cx_ndc += m->wx * (1.0f / 640.0f);
    cy_ndc -= m->wy * (1.0f / 360.0f);

    dir = jw_norm(jw_v3(JW_FWD_X + JW_RIGHT_X * (cx_ndc * aspect / JW_FOCAL) + JW_UP_X * (cy_ndc / JW_FOCAL),
                        JW_FWD_Y + JW_RIGHT_Y * (cx_ndc * aspect / JW_FOCAL) + JW_UP_Y * (cy_ndc / JW_FOCAL),
                        JW_FWD_Z + JW_RIGHT_Z * (cx_ndc * aspect / JW_FOCAL) + JW_UP_Z * (cy_ndc / JW_FOCAL)));
    // world height of the screen at DIST is 2 * DIST / focal
    s0 = (2.0f * JD_DIST / JW_FOCAL) * h_frac / JD_VIEWBOX;
    o->P = jw_v3(JW_EYE_X + dir.x * JD_DIST, JW_EYE_Y + dir.y * JD_DIST, JW_EYE_Z + dir.z * JD_DIST);
    o->P.x += m->shift * 6.0f * s0 + 2.2f * s0 * wk_sinf(t * 0.53f * m->ts);      // a slow float
    o->P.y += (m->bounce * 7.0f + 1.6f * wk_sinf(t * 0.7f * m->ts) - 12.0f * m->rest) * s0;

    // The camera (2026-09-27): it drifts slowly round the bell -- dollying in
    // and out and swinging above, below and to the sides -- on its own
    // real-time clock, so the angles change whatever the music does.
    {
        const float ct = m->cam_t;
        const float dolly = 0.28f * wk_sinf(ct * 0.105f) + 0.12f * wk_sinf(ct * 0.043f + 2.0f);
        s0 *= 1.0f + dolly;                                  // nearer = larger
    }
    g = 1.0f + 0.13f * m->swell + 0.3f * m->drop + 0.03f * m->kick + 0.02f * wk_sinf(t * 1.1f * m->ts)
      + 0.035f * m->rest * wk_sinf(t * 0.8f);
    o->S0 = s0;
    o->S  = s0 * g;

    // Local frame: the camera's (right, up, toward the camera), then the
    // design's Euler 'ZXY' -- R = Rbase * Rz * Rx * Ry.
    bank = jd_clamp(m->wvx * 0.0011f, -0.25f, 0.25f);
    dive = jd_clamp(m->wvy * 0.0010f, -0.18f, 0.18f);
    ex = 0.08f + m->tilt + 0.08f * wk_sinf(t * 0.41f * m->ts) + dive;
    ey = m->turn + 0.18f * wk_sinf(m->orbit) + m->yaw      // spins all the way round
       + 0.50f * wk_sinf(m->cam_t * 0.061f + 0.7f);        // the camera swings round it
    ex += 0.26f * wk_sinf(m->cam_t * 0.083f + 1.9f);       // ... and over and under it
    ez = m->lean + 0.1f * wk_sinf(t * 0.5f * m->ts) - 0.03f * m->bounce_v + 0.22f * wk_sinf(m->roll) - bank;
    Rb[0] = JW_RIGHT_X; Rb[1] = JW_RIGHT_Y; Rb[2] = JW_RIGHT_Z;
    Rb[3] = JW_UP_X;    Rb[4] = JW_UP_Y;    Rb[5] = JW_UP_Z;
    Rb[6] = -JW_FWD_X;  Rb[7] = -JW_FWD_Y;  Rb[8] = -JW_FWD_Z;
    {
        const float cz = jw_cosf(ez), sz = wk_sinf(ez);
        const float cxr = jw_cosf(ex), sxr = wk_sinf(ex);
        const float cy = jw_cosf(ey), sy = wk_sinf(ey);
        Rz[0] = cz;  Rz[1] = sz;  Rz[2] = 0.0f;  Rz[3] = -sz; Rz[4] = cz;  Rz[5] = 0.0f;
        Rz[6] = 0.0f; Rz[7] = 0.0f; Rz[8] = 1.0f;
        Rx[0] = 1.0f; Rx[1] = 0.0f; Rx[2] = 0.0f; Rx[3] = 0.0f; Rx[4] = cxr; Rx[5] = sxr;
        Rx[6] = 0.0f; Rx[7] = -sxr; Rx[8] = cxr;
        Ry[0] = cy;  Ry[1] = 0.0f; Ry[2] = -sy;  Ry[3] = 0.0f; Ry[4] = 1.0f; Ry[5] = 0.0f;
        Ry[6] = sy;  Ry[7] = 0.0f; Ry[8] = cy;
    }
    jd_m3_mul(Rz, Rx, T1);
    jd_m3_mul(T1, Ry, T2);
    jd_m3_mul(Rb, T2, o->R);

    o->tc    = jw_v3(-JW_FWD_X, -JW_FWD_Y, -JW_FWD_Z);
    o->t     = t;
    o->ts    = m->ts;
    o->swell = m->swell; o->drop = m->drop; o->jig = m->jig; o->sq = m->sq;
    o->shear = m->shear; o->rip_a = m->rip_a; o->rip_b = m->rip_b;
    o->ph1 = m->ph1; o->ph2 = m->ph2; o->ph3 = m->ph3; o->spread = m->spread;
    o->glow = m->glow; o->kick = m->kick; o->spin = m->spin; o->orbit = m->orbit;
    o->wph = m->wph; o->rest = m->rest;
}

// Local (viewBox units, y up, z toward the viewer) -> world.
static inline jw_vec3 jd_to_world(const jd_pose *p, float s, float lx, float ly, float lz)
{
    const float *R = p->R;
    return jw_v3(p->P.x + s * (R[0] * lx + R[3] * ly + R[6] * lz),
                 p->P.y + s * (R[1] * lx + R[4] * ly + R[7] * lz),
                 p->P.z + s * (R[2] * lx + R[5] * ly + R[8] * lz));
}

// --- the loft ----------------------------------------------------------------------
// jw_build_layer_k with a morph.  li is the SOLVER layer (0 near .. 2 far),
// which is also the design's layer index.  At morph 0 this IS
// jw_build_layer_k: the call is forwarded untouched.
static inline int jd_build_layer_k(const jw_layer *L, int li, const float *disp, int ndisp,
                                   float aspect, jw_vec3 key, const jd_pose *P,
                                   jw_vert *out, int cap)
{
    // cos / sin of 2*th_j for the section's twelve points, th_j = j/12 * 2pi
    static const float C2[JW_SECTION] = { 1.0f, 0.5f, -0.5f, -1.0f, -0.5f, 0.5f,
                                          1.0f, 0.5f, -0.5f, -1.0f, -0.5f, 0.5f };
    static const float S2[JW_SECTION] = { 0.0f, 0.8660254f, 0.8660254f, 0.0f, -0.8660254f, -0.8660254f,
                                          0.0f, 0.8660254f, 0.8660254f, 0.0f, -0.8660254f, -0.8660254f };
    static const float PASS_OFF[JW_LAYERS] = { 1.0f, 2.0f, 0.0f };
    const jd_layer *C;
    jw_layer Lb;
    float mk, e1, e2, sw, tw, oa, ob, ca, sa, cb, sb, Lk[9];
    float dp, pass, pk, SGk, tpos, t_amp, t, ts, du, dk;
    int i, j;

    if (!P || !(P->morph > 0.0f) || li < 0 || li >= JW_LAYERS)
        return jw_build_layer_k(L, disp, ndisp, aspect, key, out, cap);
    if (!L || !disp || !out) return 0;
    if (ndisp < 2 || cap < JW_VERTS) return 0;

    C  = &JD_LAYER[li];
    t  = P->t;
    ts = P->ts;
    mk = jd_clamp((P->morph - (float)li * 0.1f) / 0.8f, 0.0f, 1.0f);
    e1 = jw_smooth(0.0f, 0.5f, mk);
    e2 = jw_smooth(0.15f, 1.0f, mk);
    sw = wk_sinf(WK_PI * e2) * 0.9f;                  // stand up toward the viewer mid-flight
    tw = jw_smooth(0.0f, 0.3f, mk) * (1.0f - jw_smooth(0.7f, 1.0f, mk));

    Lb = *L;
    Lb.bright = L->bright * jw_lerp(1.0f, C->bright / JW_LAYER[li].bright, e2);

    // orbit frame: spin about the bell's vertical, then tilt the orbit plane
    oa = li ? (P->spin * (li == 1 ? 1.0f : -1.3f) + C->off) : 0.0f;
    ob = C->tilt + (li ? 0.08f * wk_sinf(P->orbit * 0.7f + (float)li) : 0.0f);
    ca = jw_cosf(oa); sa = wk_sinf(oa); cb = jw_cosf(ob); sb = wk_sinf(ob);
    Lk[0] = ca; Lk[1] = sb * sa; Lk[2] = -cb * sa;
    Lk[3] = 0.0f; Lk[4] = cb; Lk[5] = sb;
    Lk[6] = sa; Lk[7] = -sb * ca; Lk[8] = cb * ca;

    // one swell passes front to back through the three bells: glassy -> mark -> opaque
    dp = jd_frac(t * 0.14f * ts - PASS_OFF[li] * 0.12f);
    pass = jd_expn(((dp - 0.1f) / 0.07f) * ((dp - 0.1f) / 0.07f)) * (0.35f + 1.8f * P->rip_a);
    pk = 1.0f + 0.025f * wk_sinf(t * 1.3f * ts + (float)li * 2.09f) + 0.05f * P->jig * (float)li + 0.03f * pass;
    SGk = P->S * C->ls * pk;
    tpos = jd_frac(t * 0.07f * ts + (float)li * 0.41f);
    t_amp = C->tend * (2.5f + 22.0f * P->rip_a);

    du = (L->u1 - L->u0) / (float)(JW_STATIONS - 1);
    dk = 1.0f / (float)(JW_STATIONS - 1);

    for (i = 0; i < JW_STATIONS; i++) {
        const float *st = JD_RING[i];
        float u  = L->u0 + du * (float)i;
        float uk = dk * (float)i;
        float fl;                                           // 0..1..0 while in flight
        {
            // The morph runs ALONG the strand (2026-09-27): the middle leaves
            // first and the ends follow, so each ribbon zips into the bell
            // instead of the whole layer sliding over as one.
            const float ord = uk < 0.5f ? (0.5f - uk) * 2.0f : (uk - 0.5f) * 2.0f;
            const float ms  = jd_clamp(mk * 1.45f - 0.45f * ord, 0.0f, 1.0f);
            e1 = jw_smooth(0.0f, 0.5f, ms);
            e2 = jw_smooth(0.15f, 1.0f, ms);
            sw = wk_sinf(WK_PI * e2) * 0.9f;
            tw = jw_smooth(0.0f, 0.3f, ms) * (1.0f - jw_smooth(0.7f, 1.0f, ms));
            fl = wk_sinf(WK_PI * e2);
        }
        float uw = 0.5f + (u - 0.5f) * (1.0f - 0.6f * e1);    // the strands draw in
        float d0, hw, ht, stw, sl;
        jw_frame f;
        jw_vec3 p, s, up;
        float lumpSinA1, lumpCosA1, lumpSinA2, lumpCosA2, ripS, ripC;
        float tL, in_sgn, tp;

        {
            float sx_ = uk * (float)(ndisp - 1);
            int   k = (int)sx_;
            float fr;
            if (k < 0) k = 0;
            if (k > ndisp - 2) k = ndisp - 2;
            fr = sx_ - (float)k;
            d0 = (disp[k] + (disp[k + 1] - disp[k]) * fr) * L->disp_gain;
        }
        f  = jw_frame_at(uw, d0, L);
        hw = jw_half_w(uw, L) * (1.0f - 0.35f * e1);
        ht = jw_half_t(uw, L);
        p = f.p; s = f.s; up = f.up;

        {
            float a1 = JW_LUMP_K1 * uw * WK_PI + L->phase;
            float a2 = JW_LUMP_K2 * uw * WK_PI + L->phase;
            lumpSinA1 = wk_sinf(a1); lumpCosA1 = jw_cosf(a1);
            lumpSinA2 = wk_sinf(a2); lumpCosA2 = jw_cosf(a2);
        }

        // --- the bell frame, deformed in logo space
        sl = st[8];
        {
            float rip = 1.0f + P->rip_a * wk_sinf(WK_TWO_PI * 3.0f * sl - P->ph1);
            float dT  = sl - tpos, bump, life, X, Y, sxs, axv, hwl, htl, lx, ly, lz;
            float qx, qy, qz;
            jw_vec3 wp, sL, uL;
            dT -= jd_floor(dT + 0.5f);
            bump = jd_expn((dT / 0.06f) * (dT / 0.06f));
            life = C->life * (wk_sinf(WK_PI * 4.0f * sl + t * 0.9f + (float)li)
                              + 0.6f * wk_sinf(WK_PI * 10.0f * sl - t * 1.4f + (float)li * 2.0f))
                 + t_amp * bump;
            X = st[0] + st[5] * life * 0.55f;
            Y = st[1] + st[6] * life * 0.55f;
            sxs = X >= 0.0f ? 1.0f : -1.0f;
            axv = jd_clamp((X < 0.0f ? -X : X) * (1.0f / 30.0f), 0.0f, 1.0f);
            X += P->spread * 3.2f * sxs * axv * (0.5f + 0.5f * wk_sinf(P->ph3 * 1.3f + sxs));
            Y += P->spread * 4.2f * axv * wk_sinf(P->ph3 + sxs * 1.7f);
            hwl = st[2] * C->thin * (1.0f + 0.06f * P->swell + 0.12f * pass) * rip
                * (1.0f + 0.08f * wk_sinf(WK_PI * 8.0f * sl + t * 1.1f)) * (1.0f - 0.45f * bump * C->tend);
            htl = hwl * (1.0f + 0.14f * P->swell + 0.3f * P->drop);   // a round tube
            lx = Lk[0] * X + Lk[3] * Y;
            ly = Lk[1] * X + Lk[4] * Y;
            lz = Lk[2] * X + Lk[5] * Y;
            wp = jd_to_world(P, SGk, lx, ly, lz);
            qx = Lk[0] * st[3] + Lk[3] * st[4];
            qy = Lk[1] * st[3] + Lk[4] * st[4];
            qz = Lk[2] * st[3] + Lk[5] * st[4];
            sL = jw_v3(P->R[0] * qx + P->R[3] * qy + P->R[6] * qz,
                       P->R[1] * qx + P->R[4] * qy + P->R[7] * qz,
                       P->R[2] * qx + P->R[5] * qy + P->R[8] * qz);
            uL = jw_v3(P->R[0] * Lk[6] + P->R[3] * Lk[7] + P->R[6] * Lk[8],
                       P->R[1] * Lk[6] + P->R[4] * Lk[7] + P->R[7] * Lk[8],
                       P->R[2] * Lk[6] + P->R[5] * Lk[7] + P->R[8] * Lk[8]);
            p = jw_v3(p.x + (wp.x - p.x) * e2 + P->tc.x * sw,
                      p.y + (wp.y - p.y) * e2 + P->tc.y * sw,
                      p.z + (wp.z - p.z) * e2 + P->tc.z * sw);
            s  = jw_norm(jw_v3(s.x + (sL.x - s.x) * e2, s.y + (sL.y - s.y) * e2, s.z + (sL.z - s.z) * e2));
            up = jw_norm(jw_v3(up.x + (uL.x - up.x) * e2, up.y + (uL.y - up.y) * e2, up.z + (uL.z - up.z) * e2));
            if (fl > 0.001f) {
                // In flight the strand corkscrews about its own path.
                const float a  = 7.0f * uk + t * 2.2f + (float)li * 2.1f;
                const float rr = fl * SGk * 5.0f;
                const float ka = jw_cosf(a) * rr, kb = wk_sinf(a) * rr;
                p = jw_v3(p.x + s.x * ka + up.x * kb, p.y + s.y * ka + up.y * kb, p.z + s.z * ka + up.z * kb);
            }
            hw += (hwl * SGk - hw) * e2;
            ht += (htl * SGk - ht) * e2;
            // the logo's own gradient line: (12,30) -> (72,63) in the viewBox
            tL = jd_clamp(((X + 36.0f - 12.0f) * 60.0f + ((38.0f - Y) - 30.0f) * 33.0f) * (1.0f / 4689.0f),
                          0.0f, 1.0f);
        }
        // mid-flight the strands close to soft points
        {
            float uu = (float)i * dk;
            float q = jw_smooth(0.0f, 0.14f, uu) * jw_smooth(1.0f, 0.86f, uu);
            tp = 1.0f - tw * (1.0f - (0.12f + 0.88f * jd_sqrt(q)));
        }
        hw *= tp; ht *= tp;
        if (hw < 1.0e-4f) hw = 1.0e-4f;
        if (ht < 1.0e-4f) ht = 1.0e-4f;
        stw = hw - ht;
        if (stw < 1.0e-4f) stw = 1.0e-4f;
        in_sgn = -st[7];

        {
            const float a = WK_TWO_PI * 5.0f * sl + P->ph2;
            ripS = wk_sinf(a); ripC = jw_cosf(a);
        }

        for (j = 0; j < JW_SECTION; j++) {
            float   cx  = JW_SEC_AST[j] * stw + JW_SEC_AHT[j] * ht;
            float   cy  = JW_SEC_BHT[j] * ht;
            float   rim = JW_SEC_RIM[j];
            float   nx  = cx / hw, ny = cy / ht;
            jw_vec3 n, q, ev;
            jw_rgb  alb, lit_c, rimc;
            jw_proj pr;
            float   dW, dL, d, lit, vc, fres, dd, kd, tt, inward, core_add;
            jw_vert *v = &out[i * JW_SECTION + j];

            n = jw_norm(jw_v3(s.x * nx + up.x * ny, s.y * nx + up.y * ny, s.z * nx + up.z * ny));
            dW = jw_lump_fast(lumpSinA1, lumpCosA1, lumpSinA2, lumpCosA2, j) * (0.55f + 1.9f * ht);
            dL = P->rip_b * (S2[j] * ripC + C2[j] * ripS) * ht * 1.3f;       // sin(2 th + a)
            d  = dW + (dL - dW) * e2;
            q  = jw_v3(p.x + s.x * cx + up.x * cy + n.x * d,
                       p.y + s.y * cx + up.y * cy + n.y * d,
                       p.z + s.z * cx + up.z * cy + n.z * d);
            if (e2 > 0.0f) {                                   // the gel's jiggle and shear
                const float jy = 0.075f * P->jig - 0.06f * P->sq;
                const float dy0 = q.y - P->P.y;
                q.x = P->P.x + (q.x - P->P.x) * (1.0f - 0.5f * jy * e2) + P->shear * dy0 * e2;
                q.y = P->P.y + dy0 * (1.0f + jy * e2);
            }
            tt  = jw_axis_t(q);
            tt += (tL - tt) * e2;
            ev  = jw_eyevec(q);
            alb = jw_gel_color_t(tt, ny, rim, &Lb);
            lit_c = jw_shade_k(alb, n, ev, rim, key);

            vc = jw_dot(n, P->tc); if (vc < 0.0f) vc = -vc;
            fres = (1.0f - vc) * (1.0f - vc);
            // depth cue: the near side of an orbit reads brighter, the far side sinks
            dd = jd_clamp(((q.x - P->P.x) * P->tc.x + (q.y - P->P.y) * P->tc.y
                           + (q.z - P->P.z) * P->tc.z) / (30.0f * P->S), -1.0f, 1.0f) * e2;
            kd = 1.0f + 0.4f * dd + 0.22f * pass * e2 + 0.35f * fl;
            lit_c.r *= kd; lit_c.g *= kd; lit_c.b *= kd;
            // the core's light on the ring's inner wall
            inward = in_sgn * nx;
            core_add = e2 * P->glow * 0.42f * jw_smooth(0.15f, 1.0f, inward)
                     * (0.6f + 0.4f * (ny + 1.0f) * 0.5f) * jw_smooth(0.72f, 1.0f, P->morph);
            if (core_add > 0.0f) {
                lit_c.r += 0.62f * core_add;
                lit_c.g += 0.86f * core_add;
                lit_c.b += 1.00f * core_add;
            }

            lit = jw_key_lit_k(n, key);
            lit = lit * (0.8f + 0.2f * lit);
            {
                const float lf = 0.5f * (1.0f - fres) * e2;
                if (lf > lit) lit = lf;
            }
            rimc = jw_rim_color(rim, lit, &Lb);

            pr = jw_project(q, aspect);
            v->x  = pr.x;
            v->y  = pr.y;
            v->z  = pr.z;
            v->ok = pr.ok;
            v->r  = jw_u8(lit_c.r);
            v->g  = jw_u8(lit_c.g);
            v->b  = jw_u8(lit_c.b);
            v->rr = jw_u8(rimc.r);
            v->rg = jw_u8(rimc.g);
            v->rb = jw_u8(rimc.b);
        }
    }
    return JW_VERTS;
}

// The layer's alpha: the wave's, moving to the bell's with the layer's own
// stagger.
static inline unsigned char jd_layer_alpha(const jw_layer *L, int li, float morph)
{
    float mk, e2, a;
    if (!L) return 0;
    if (!(morph > 0.0f) || li < 0 || li >= JW_LAYERS) return L->alpha;
    mk = jd_clamp((morph - (float)li * 0.1f) / 0.8f, 0.0f, 1.0f);
    e2 = jw_smooth(0.15f, 1.0f, mk);
    a = (float)L->alpha + (JD_LAYER[li].alpha - (float)L->alpha) * e2;
    return (unsigned char)jd_clamp(a + 0.5f, 0.0f, 255.0f);
}

// --- the core ----------------------------------------------------------------------
// The bell's hole, lit from inside.  Three nested additive shells stand in for
// glow -- "three concentric alpha rings, never a blur" -- so there is no blur
// pass and no framebuffer read.  jd_core_shell() projects one shell: its
// centre and JD_CORE_K rim points, and the colour/alpha of each.
#define JD_CORE_SHELLS 3
static const float JD_CORE_SCALE[JD_CORE_SHELLS] = { 1.00f, 1.20f, 1.42f };
static const float JD_CORE_OP[JD_CORE_SHELLS]    = { 0.95f, 0.16f, 0.07f };
static const float JD_CORE_ZK[JD_CORE_SHELLS]    = { 1.00f, 0.35f, 0.18f };

// How lit the core is: 0 before the bell has closed.
static inline float jd_core_on(const jd_pose *P)
{
    return P ? jw_smooth(0.72f, 1.0f, P->morph) : 0.0f;
}

// Fills cx/cy (the centre) and rx/ry[JD_CORE_K]; returns 0 if any point is off
// the camera.  gain_c / gain_r are the centre's and the rim's brightness
// (0..~1.6), alpha_c / alpha_r their alphas (0..1).
static inline int jd_core_shell(const jd_pose *P, int k, float aspect,
                                float *cx, float *cy, float *rx, float *ry,
                                float *gain_c, float *gain_r, float *alpha_c, float *alpha_r)
{
    float appear, sc, pulse, s, sxk, syk, gl;
    int   i;
    jw_proj pc;
    if (!P || k < 0 || k >= JD_CORE_SHELLS) return 0;
    appear = jd_core_on(P);
    if (!(appear > 0.002f)) return 0;
    sc = P->S0 * (1.0f + 0.06f * P->swell + 0.3f * P->drop) * jd_back_out(appear);
    pulse = 1.0f + 0.12f * P->swell + 0.06f * P->jig + 0.07f * wk_sinf(P->t * 2.1f * P->ts);
    s = JD_CORE_SCALE[k] * pulse;
    sxk = s * (1.0f + 0.07f * P->sq);
    syk = s * (1.0f - 0.12f * P->sq);
    // centre, lifted toward the viewer by the pillow's height
    {
        const float h = 2.6f * JD_CORE_ZK[k] * (1.0f + 0.4f * P->swell);
        pc = jw_project(jd_to_world(P, sc, JD_CORE_C[0] * s, JD_CORE_C[1] * s, h * s), aspect);
        if (!pc.ok) return 0;
        *cx = pc.x; *cy = pc.y;
    }
    for (i = 0; i < JD_CORE_K; i++) {
        const float lx = JD_CORE_C[0] * s + (JD_CORE[i][0] - JD_CORE_C[0]) * sxk;
        const float ly = JD_CORE_C[1] * s + (JD_CORE[i][1] - JD_CORE_C[1]) * syk;
        jw_proj pr = jw_project(jd_to_world(P, sc, lx, ly, 0.0f), aspect);
        if (!pr.ok) return 0;
        rx[i] = pr.x; ry[i] = pr.y;
    }
    gl = P->glow * (1.0f + 0.6f * P->kick) * appear;
    *gain_c  = gl;
    *gain_r  = gl * 0.45f;
    *alpha_c = JD_CORE_OP[k];
    *alpha_r = k == 0 ? JD_CORE_OP[k] : 0.0f;            // the outer shells feather out
    return 1;
}

// --- the orbiting particles ----------------------------------------------------
// Depth by parallax: specks on tilted orbits round the bell's vertical axis.
// The far half of each orbit is behind the gel (drawn before it), the near
// half in front, larger and brighter -- the user's "particles moving around
// it".  They breathe with the bell, spin up with the mids and are thrown
// outward a little by every kick.
#define JD_MOTES 96

typedef struct {
    float    a[JD_MOTES], r[JD_MOTES], y[JD_MOTES], w[JD_MOTES];
    float    inc[JD_MOTES], roll[JD_MOTES], size[JD_MOTES], hue[JD_MOTES], ph[JD_MOTES];
    float    push, push_v;
    uint32_t rng;
} jd_motes;

typedef struct {
    float         x, y;           // clip
    float         r_px;           // radius, px at 1080p
    unsigned char r, g, b, a;     // a == 0: skip
    int           front;          // 1 = in front of the gel
} jd_sprite;

static inline float jd_rand(uint32_t *s)
{
    *s = *s * 1664525u + 1013904223u;
    return (float)(*s >> 8) * (1.0f / 16777216.0f);
}

static inline void jd_motes_init(jd_motes *m, uint32_t seed)
{
    int i;
    if (!m) return;
    memset(m, 0, sizeof *m);
    m->rng = seed ? seed : 0x4A44u;
    for (i = 0; i < JD_MOTES; i++) {
        const float band = jd_rand(&m->rng);
        m->a[i]    = jd_rand(&m->rng) * WK_TWO_PI;
        m->r[i]    = 44.0f + 52.0f * band * band;                    // viewBox units
        m->y[i]    = (jd_rand(&m->rng) - 0.5f) * 56.0f;
        m->w[i]    = (0.10f + 0.22f * jd_rand(&m->rng)) * (jd_rand(&m->rng) < 0.12f ? -1.0f : 1.0f);
        m->inc[i]  = (jd_rand(&m->rng) - 0.5f) * 1.1f;
        m->roll[i] = (jd_rand(&m->rng) - 0.5f) * 0.7f;
        m->size[i] = 0.7f + 1.6f * jd_rand(&m->rng) * jd_rand(&m->rng);
        m->hue[i]  = jd_rand(&m->rng);
        m->ph[i]   = jd_rand(&m->rng) * WK_TWO_PI;
    }
}

// dt seconds; ts the tempo scale; mid 0..1; kick 0..1 on a hit frame.
static inline void jd_motes_step(jd_motes *m, float dt, float ts, float mid, float kick)
{
    int i;
    if (!m || !(dt > 0.0f)) return;
    if (dt > 0.1f) dt = 0.1f;
    if (kick > 0.0f) m->push_v += 0.35f * kick;
    m->push_v += (-10.0f * m->push - 2.2f * m->push_v) * dt;
    m->push   += m->push_v * dt;
    for (i = 0; i < JD_MOTES; i++) {
        m->a[i] += m->w[i] * dt * ts * (1.0f + 0.9f * mid);
        if (m->a[i] > WK_TWO_PI)  m->a[i] -= WK_TWO_PI;
        if (m->a[i] < 0.0f)       m->a[i] += WK_TWO_PI;
        m->ph[i] += dt * (0.6f + 0.3f * m->hue[i]);
        if (m->ph[i] > WK_TWO_PI) m->ph[i] -= WK_TWO_PI;
    }
}

// One particle's sprite.  vis 0..1 fades the whole set (the morph).
static inline void jd_motes_sprite(const jd_motes *m, int i, const jd_pose *P, float aspect,
                                   float vis, float twinkle, jd_sprite *o)
{
    float rr, ca, sa, lx, ly, lz, ci, si, cr, sr, x1, y1, z1, x2, y2, depth, near_k, k, h, b;
    jw_vec3 w;
    jw_proj pc, pr;
    if (!o) return;
    o->a = 0;
    if (!m || !P || i < 0 || i >= JD_MOTES || !(vis > 0.004f)) return;
    rr = m->r[i] * (1.0f + m->push);
    ca = jw_cosf(m->a[i]); sa = wk_sinf(m->a[i]);
    lx = rr * ca;
    ly = m->y[i] + 5.0f * wk_sinf(m->a[i] * 2.0f + m->ph[i]) + JD_CORE_C[1] * 0.5f;
    lz = rr * sa;
    // tilt the orbit about X, then roll it about Z
    ci = jw_cosf(m->inc[i]); si = wk_sinf(m->inc[i]);
    y1 = ly * ci - lz * si; z1 = ly * si + lz * ci; x1 = lx;
    cr = jw_cosf(m->roll[i]); sr = wk_sinf(m->roll[i]);
    x2 = x1 * cr - y1 * sr; y2 = x1 * sr + y1 * cr;
    w  = jd_to_world(P, P->S0, x2, y2, z1);
    pr = jw_project(w, aspect);
    pc = jw_project(P->P, aspect);
    if (!pr.ok || !pc.ok) return;
    depth  = (w.x - P->P.x) * P->tc.x + (w.y - P->P.y) * P->tc.y + (w.z - P->P.z) * P->tc.z;
    near_k = jd_clamp(depth / (100.0f * P->S0) * 0.5f + 0.5f, 0.0f, 1.0f);   // 0 far .. 1 near
    o->front = depth > 0.0f;
    o->x = pr.x; o->y = pr.y;
    // perspective size, plus a little depth of field: the nearest swell
    o->r_px = m->size[i] * (pc.z / pr.z) * (2.2f + 4.4f * near_k * near_k);
    k = (0.45f + 0.55f * near_k) * vis;
    k *= 0.85f + 0.15f * wk_sinf(m->ph[i] * 3.0f) * (0.5f + twinkle);
    h = m->hue[i];
    // violet -> Jellyfin blue -> the cyan rim, as the palette runs
    {
        float r_, g_, b_;
        if (h < 0.5f) {
            const float u = h * 2.0f;
            r_ = jw_lerp(JW_PURPLE_R, JW_BLUE_R, u); g_ = jw_lerp(JW_PURPLE_G, JW_BLUE_G, u); b_ = jw_lerp(JW_PURPLE_B, JW_BLUE_B, u);
        } else {
            const float u = (h - 0.5f) * 2.0f;
            r_ = jw_lerp(JW_BLUE_R, JW_RIMC_R, u); g_ = jw_lerp(JW_BLUE_G, JW_RIMC_G, u); b_ = jw_lerp(JW_BLUE_B, JW_RIMC_B, u);
        }
        // lifted toward white so a speck reads as light, not paint
        r_ = r_ * 0.7f + 0.3f; g_ = g_ * 0.7f + 0.3f; b_ = b_ * 0.7f + 0.3f;
        o->r = jw_u8(r_);
        o->g = jw_u8(g_);
        o->b = jw_u8(b_);
    }
    b = jd_clamp(k * 235.0f, 0.0f, 255.0f);
    o->a = (unsigned char)b;
}

// --- the radiating field (2026-09-27) ------------------------------------------
// The JellyWave particle field, in JellyDrop: specks thrown out of the bell
// in every direction, across the whole screen, faster with the music and in a
// burst on each kick, reborn at the bell when they leave the screen.  Screen
// space around the bell's projected centre; half pass behind the gel.
#define JD_RAYS 180

typedef struct {
    float    a[JD_RAYS], r[JD_RAYS], v[JD_RAYS], size[JD_RAYS], hue[JD_RAYS], ph[JD_RAYS];
    unsigned char front[JD_RAYS];
    uint32_t rng;
} jd_rays;

static inline void jd_rays_seed(jd_rays *m, int i, int born)
{
    m->a[i]    = jd_rand(&m->rng) * WK_TWO_PI;
    m->r[i]    = born ? 0.03f + 0.06f * jd_rand(&m->rng) : 1.9f * jd_rand(&m->rng);
    m->v[i]    = 0.07f + 0.22f * jd_rand(&m->rng);
    m->size[i] = 0.6f + 1.8f * jd_rand(&m->rng) * jd_rand(&m->rng);
    m->hue[i]  = jd_rand(&m->rng);
    m->ph[i]   = jd_rand(&m->rng) * WK_TWO_PI;
    m->front[i] = jd_rand(&m->rng) < 0.5f;
}

static inline void jd_rays_init(jd_rays *m, uint32_t seed)
{
    int i;
    if (!m) return;
    memset(m, 0, sizeof *m);
    m->rng = seed ? seed : 0x5241u;
    for (i = 0; i < JD_RAYS; i++) jd_rays_seed(m, i, 0);
}

// energy 0..1, kick 0..1 on a hit frame.
static inline void jd_rays_step(jd_rays *m, float dt, float energy, float kick)
{
    int i;
    if (!m || !(dt > 0.0f)) return;
    if (dt > 0.1f) dt = 0.1f;
    const float go = 0.55f + 1.5f * jd_clamp(energy, 0.0f, 1.0f);
    for (i = 0; i < JD_RAYS; i++) {
        m->r[i] += m->v[i] * dt * go * (1.0f + 0.9f * m->r[i]) + kick * 0.05f * m->v[i] * 6.0f;
        m->ph[i] += dt * 1.3f;
        if (m->ph[i] > WK_TWO_PI) m->ph[i] -= WK_TWO_PI;
        if (m->r[i] > 1.9f) jd_rays_seed(m, i, 1);
    }
}

static inline void jd_rays_sprite(const jd_rays *m, int i, const jd_pose *P, float aspect,
                                  float vis, float twinkle, jd_sprite *o)
{
    jw_proj pc;
    float r, k, h;
    if (!o) return;
    o->a = 0;
    if (!m || !P || i < 0 || i >= JD_RAYS || !(vis > 0.004f)) return;
    pc = jw_project(P->P, aspect);
    if (!pc.ok) return;
    r = m->r[i];
    o->x = pc.x + jw_cosf(m->a[i]) * r / aspect;
    o->y = pc.y + wk_sinf(m->a[i]) * r;
    if (o->x < -1.1f || o->x > 1.1f || o->y < -1.1f || o->y > 1.1f) return;
    o->front = m->front[i];
    o->r_px = m->size[i] * (1.0f + 2.4f * r);          // nearer as it flies out
    k = jd_clamp(r / 0.14f, 0.0f, 1.0f) * (1.0f - jd_clamp((r - 1.2f) / 0.7f, 0.0f, 1.0f)) * vis;
    k *= 0.8f + 0.2f * wk_sinf(m->ph[i] * 3.0f) * (0.5f + twinkle);
    if (!o->front) k *= 0.6f;
    h = m->hue[i];
    {
        float r_, g_, b_;
        if (h < 0.5f) {
            const float u = h * 2.0f;
            r_ = jw_lerp(JW_PURPLE_R, JW_BLUE_R, u); g_ = jw_lerp(JW_PURPLE_G, JW_BLUE_G, u); b_ = jw_lerp(JW_PURPLE_B, JW_BLUE_B, u);
        } else {
            const float u = (h - 0.5f) * 2.0f;
            r_ = jw_lerp(JW_BLUE_R, JW_RIMC_R, u); g_ = jw_lerp(JW_BLUE_G, JW_RIMC_G, u); b_ = jw_lerp(JW_BLUE_B, JW_RIMC_B, u);
        }
        r_ = r_ * 0.7f + 0.3f; g_ = g_ * 0.7f + 0.3f; b_ = b_ * 0.7f + 0.3f;
        o->r = jw_u8(r_); o->g = jw_u8(g_); o->b = jw_u8(b_);
    }
    o->a = (unsigned char)jd_clamp(k * 225.0f, 0.0f, 255.0f);
}

#endif // WAVE_DROP_H
