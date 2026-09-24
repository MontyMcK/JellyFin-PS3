// The seam between the wave geometry pipeline and the renderer.
//
//   wave_kernel.h  -> node positions         (stage 1)
//   wave_spline.h  -> dense smooth polyline  (stage 2)
//   wave_ribbon.h  -> triangle-strip verts   (stage 3)
//   wave_field.h   -> three animated layers the XMB can sample   (this file)
//
// WHAT THIS REPLACES.  ui_wave.cpp gets every ribbon's crest from one sine:
//
//     wave_crest(li, fx, W, H)
//         = H*WAVE_BASEY[li] + sinf(fx/W * FREQ[li]*pi + phase[li]) * AMP[li]
//
// -- a fixed shape sliding sideways, which is exactly what docs/wave-spec.md
// section 3b says not to do ("use a uniform cubic B-spline over a small
// control grid, not summed sines").  Stages 1-3 replace it with a driven,
// damped spring chain, but nothing has been able to CALL them: they take a
// node array and hand back a polyline, while the renderer wants a crest
// height at an arbitrary pixel column.  This file is that adapter and nothing
// more.
//
// WHAT THIS DELIBERATELY IS NOT.  No RSX state, no vertex-array bindings, no
// video memory, no pixel constants.  WAVE_BASEY / WAVE_AMP / WAVE_COLOR stay
// in ui_wave.cpp where they already live, because the mapping from a unitless
// displacement to a screen row is the renderer's business and the physics is
// this file's.  The whole integration on the renderer side is one expression
// (see INTEGRATION below), and every line that could wedge a GPU stays on the
// far side of the seam.
//
// NO GLOBAL STATE.  wf_field is caller-owned, the same contract wk_chain
// already uses for its node arrays: ui_wave.cpp declares one static instance
// and passes its address.  Nothing here allocates and nothing here is shared.
//
// Header-only, pure C, no libm, no PS3 headers (.clinerules rule 7).

#ifndef WAVE_FIELD_H
#define WAVE_FIELD_H

#include "wave_kernel.h"
#include "wave_spline.h"
#include "wave_ribbon.h"

// Three layers, because the XMB background has always had three ribbons and
// the renderer's WAVE_* tables are sized for three.
#define WF_LAYERS    3

// 96 nodes per chain, 257 samples per curve.  Both are sizes the stage tests
// already exercise (tests/test_wave_spline.c runs a 96-node chain), and
// 257 = 256+1 makes the sample spacing an exact power-of-two fraction of the
// span, so sample k sits at k/256 with no accumulated rounding.
#define WF_NODES     96
#define WF_SAMPLES   257

// --- per-layer separation -------------------------------------------------
//
// The kernel advances its travelling-wave phases at the FIXED rates WK_W1 and
// WK_W2 per unit time.  Three chains stepped with the same dt therefore move
// in lockstep, and three identical ribbons look like a rendering bug.
//
// The only knob in the existing API that changes a layer's apparent speed is
// dt itself, so that is what varies here: layer l is advanced by dt*WF_RATE[l].
// That is physically coherent rather than a fudge -- the whole layer runs at a
// different rate, phases and spring response together -- and it needs no
// change to wave_kernel.h, which is the file whose numbers are already tested.
//
// WF_DRIVE scales the travelling-wave field per layer (wk_step's `drive`), so
// the back layers are calmer than the front one, and the distinct seeds give
// each chain its own noise realisation.  Rates are deliberately not simple
// ratios: 0.78 and 1.34 against 1.0 have no small common multiple, so the
// three layers do not re-synchronise on any period a viewer would notice.
//
// MEASURED.  Over 20,000 frames at dt=2 the mean pairwise separation between
// layers (max |disp| difference across the curve) is 0.44, 0.41 and 0.37 --
// the same order as the amplitude itself, so the layers are genuinely
// independent rather than near-copies of one another.
static const float    WF_RATE [WF_LAYERS] = { 1.00f, 0.78f, 1.34f };
static const float    WF_DRIVE[WF_LAYERS] = { 1.00f, 0.85f, 0.70f };
static const uint32_t WF_SEED [WF_LAYERS] = { 0x9E3779B9u, 0x85EBCA6Bu,
                                              0xC2B2AE35u };

// --- warm-up --------------------------------------------------------------
//
// MEASURED REASON THIS EXISTS.  wk_init leaves every node at zero, and the
// drive spring is weak (WK_DRIVE_K = 0.02), so a cold chain is a FLAT LINE
// that takes about t = 20 to reach its working amplitude:
//
//     t= 2.0  max|y|=0.027      t=14.0  max|y|=0.607
//     t= 6.0  max|y|=0.186      t=18.0  max|y|=0.681   <- initial overshoot
//     t=10.0  max|y|=0.415      t=20.0  max|y|=0.666
//
// Without a warm-up the XMB opens on a dead straight wave that visibly grows
// in over the first few seconds, every launch.  wf_init therefore steps each
// chain to WF_WARM_T before returning, which lands it inside the steady band
// rather than on the ramp or on the overshoot peak.
//
// WF_WARM_DT is the substep the warm-up is taken in.  It is well under
// WK_DT_MAX, so the kernel's own CFL subdivision does the rest.
#define WF_WARM_T    32.0f
#define WF_WARM_DT    2.0f

// --- measured output range ------------------------------------------------
//
// wf_disp returns a unitless displacement.  It is bounded by +/-1 by
// construction -- stage 1 soft-clips at WK_LIMIT and stage 2 is a convex
// combination of stage 1's nodes, so it cannot overshoot them -- but in normal
// operation it uses far less of that range than the sine it replaces did.
//
// MEASURED over 20,000 frames at dt=2, perturb=0.02, drive=1: the per-frame
// peak wanders in [0.198, 0.653] and the absolute peak is 0.653.  The soft
// clip therefore never engages, which is the intent: WK_KNEE is 0.80 and the
// clip is the exact identity below it, so the physics runs untouched.
//
// This matters at the seam.  The old sine reached +/-1 and was multiplied by
// WAVE_AMP, so a drop-in replacement is about 65% of the previous visual
// amplitude.  That is a LOOK decision, not a correctness one, so it is not
// baked in here -- a caller that wants the old amplitude multiplies by
// 1/WF_NOMINAL_PEAK.  See INTEGRATION.
#define WF_NOMINAL_PEAK  0.653f

typedef struct {
    wk_chain ch[WF_LAYERS];
    float    ny[WF_LAYERS][WF_NODES];      // node displacement, y
    float    nz[WF_LAYERS][WF_NODES];      //                    z
    float    vy[WF_LAYERS][WF_NODES];      // node velocity
    float    vz[WF_LAYERS][WF_NODES];
    float    sx[WF_SAMPLES];               // sample x, normalised [0,1]
    float    sy[WF_LAYERS][WF_SAMPLES];    // sampled displacement
    float    sz[WF_LAYERS][WF_SAMPLES];    // depth channel, see wave_ribbon.h
    int      live;                         // 0 = inert; every call is a no-op
} wf_field;

// Resample every layer's chain into its dense polyline.  Called by wf_init
// and wf_step; exposed because a caller that mutates a chain by hand has to
// be able to catch the curves up.
static inline void wf_resample(wf_field *f)
{
    int l;
    if (!f || !f->live) return;
    for (l = 0; l < WF_LAYERS; l++)
        ws_build(f->ny[l], f->nz[l], WF_NODES, 0.0f, 1.0f,
                 f->sx, f->sy[l], f->sz[l], WF_SAMPLES);
}

// Advance every layer by dt, with the spec's PERTURBATION and a drive scale.
//
// Each layer is advanced by dt*WF_RATE[l] and drive*WF_DRIVE[l]; see the
// per-layer separation block.  A non-finite or non-positive dt is rejected by
// wk_step itself, so a stalled frame cannot corrupt the chain.
static inline void wf_step(wf_field *f, float dt, float perturb, float drive)
{
    int l;
    if (!f || !f->live) return;
    for (l = 0; l < WF_LAYERS; l++)
        wk_step(&f->ch[l], dt * WF_RATE[l], perturb, drive * WF_DRIVE[l]);
    wf_resample(f);
}

// Bind the chains and run the warm-up.  Returns 1 on success, 0 if the field
// is degenerate, in which case it is marked inert and every other call here
// is a safe no-op.
//
// seed offsets the per-layer seeds rather than replacing them, so the layers
// stay distinct from one another whatever the caller passes, and seed 0 gives
// the documented default field.
static inline int wf_init(wf_field *f, uint32_t seed)
{
    int   l, i;
    float t;

    if (!f) return 0;
    f->live = 0;
    for (i = 0; i < WF_SAMPLES; i++) f->sx[i] = 0.0f;
    for (l = 0; l < WF_LAYERS; l++) {
        for (i = 0; i < WF_SAMPLES; i++) {
            f->sy[l][i] = 0.0f;
            f->sz[l][i] = 0.0f;
        }
        if (!wk_init(&f->ch[l], f->ny[l], f->nz[l], f->vy[l], f->vz[l],
                     WF_NODES, WF_SEED[l] + seed))
            return 0;
    }
    f->live = 1;

    // Warm-up.  Perturbation is the spec's nominal 0.02 and the drive is full,
    // so the chain settles into the same band it will run in.
    for (t = 0.0f; t < WF_WARM_T; t += WF_WARM_DT)
        wf_step(f, WF_WARM_DT, 0.02f, 1.0f);

    wf_resample(f);
    return 1;
}

// Displacement of layer l at normalised horizontal position u in [0,1].
//
// This is the call that replaces the sine.  u is clamped, so a caller that
// hands over a column slightly past the right edge (ui_wave.cpp appends an
// extra column at exactly W) gets the endpoint rather than a read off the end.
//
// Linear interpolation between samples is deliberate.  The curve is already
// C2 from stage 2 and there are 257 samples across the span -- at 1080p that
// is more samples than the renderer has columns -- so interpolating the
// SAMPLES with anything higher-order would be re-smoothing something that is
// already smooth, at a cost on every column of every frame.
//
// Returns 0 for an inert field, an out-of-range layer, or NaN u, so the
// renderer falls back to a flat ribbon rather than a wild one.
static inline float wf_disp(const wf_field *f, int l, float u)
{
    float s, frac;
    int   i;

    if (!f || !f->live) return 0.0f;
    if (l < 0 || l >= WF_LAYERS) return 0.0f;
    if (u != u) return 0.0f;                  // NaN in, flat out
    if (u <= 0.0f) return f->sy[l][0];
    if (u >= 1.0f) return f->sy[l][WF_SAMPLES - 1];

    s    = u * (float)(WF_SAMPLES - 1);
    i    = (int)s;
    if (i > WF_SAMPLES - 2) i = WF_SAMPLES - 2;
    frac = s - (float)i;
    return f->sy[l][i] + (f->sy[l][i + 1] - f->sy[l][i]) * frac;
}

// --- stage 3 bridge -------------------------------------------------------
//
// Build one layer's curve into a triangle strip of constant half-thickness,
// mapped into clip space.
//
//   x0, x1   horizontal span in clip space, normally -1 and +1
//   ybase    clip-space y the layer sits at when its displacement is zero
//   yscale   clip-space units per unit displacement
//   half     half-thickness, clip-space units
//
// NOT used by the renderer's current look.  ui_wave.cpp draws each ribbon as a
// veil from its crest down to the bottom of the screen, which is a quad per
// column, not a stroked curve of fixed width.  This exists because stage 3 is
// written and tested and the STROKED look is the other thing the spec asks
// for; wiring it up needs a new draw call and a new vertex budget, which is
// renderer work (.clinerules rule 8) and not this file's to do.
//
// Writes into the caller's scratch, needs 2*WF_SAMPLES vertices of capacity,
// and returns the vertex count or 0 if the request is degenerate.
static inline int wf_strip(const wf_field *f, int l,
                           float x0, float x1, float ybase, float yscale,
                           float half,
                           uint8_t tr, uint8_t tg, uint8_t tb,
                           uint8_t br, uint8_t bg, uint8_t bb,
                           float *scratch_x, float *scratch_y,
                           wr_vert *out, int cap)
{
    int i;
    if (!f || !f->live || !scratch_x || !scratch_y) return 0;
    if (l < 0 || l >= WF_LAYERS) return 0;

    for (i = 0; i < WF_SAMPLES; i++) {
        scratch_x[i] = x0 + (x1 - x0) * f->sx[i];
        scratch_y[i] = ybase + f->sy[l][i] * yscale;
    }
    return wr_build(scratch_x, scratch_y, WF_SAMPLES, half,
                    tr, tg, tb, br, bg, bb, out, cap);
}

// --- INTEGRATION ----------------------------------------------------------
//
// In ui_wave.cpp, one static and three edited lines:
//
//     #include "wave_field.h"
//     static wf_field s_field;
//
//     // in wave_init(), beside the existing setup:
//     wf_init(&s_field, 0);
//
//     // in wave_draw(), where s_wave_phase[] is advanced today:
//     wf_step(&s_field, dt, 0.02f, 1.0f);
//
//     // and wave_crest's body becomes:
//     float wy = H * WAVE_BASEY[li]
//              + wf_disp(&s_field, li, fx / W) * WAVE_AMP[li];
//
// Everything downstream -- wave_bg, wave_node, the NDC conversion, the strip
// layout, the attrib bindings, both submission paths and the gate file -- is
// untouched, because the only thing that changed is where the crest height
// came from.
//
// Two things to decide when you do it, neither of which belongs here:
//
//   AMPLITUDE.  Multiply by 1/WF_NOMINAL_PEAK (about 1.53) to keep the
//   previous visual amplitude; leave it out for a calmer wave.  See the
//   measured range above.
//
//   dt.  wk_step's time unit is the spec's TIMESTEP, which tops out at 6.72,
//   not seconds and not frames.  s_wave_phase[] is currently advanced by a
//   fixed WAVE_DPHASE per CALL, so the wave already runs at frame rate rather
//   than wall-clock rate; passing a fixed dt preserves exactly that behaviour,
//   and passing a real frame delta is the better fix but is a change in what
//   the animation does, not a change in how it is computed.

#endif // WAVE_FIELD_H
