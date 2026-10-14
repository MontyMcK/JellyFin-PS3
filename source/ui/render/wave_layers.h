// Stage C of the audio-reactive wave: parameters + the shared spline curve ->
// per-layer geometry and colour, ready for wr_build.
//
//   wave_audio.h   -> features
//   wave_motion.h  -> smoothed, slew-limited parameters
//   wave_layers.h  -> per-layer geometry and colour        (this file)
//
// This is the stage that turns ONE solver run into four ribbons at four
// depths.  wave-spec.md section 4.1 concluded "one solver, four bands"; this
// file is that conclusion, with docs/wave-audio-spec.md's depth principle on
// top of it:
//
//   FREQUENCY IS DEPTH.  Low frequencies move the far, large, slow geometry;
//   high frequencies live on the near, fine, fast geometry.  The four
//   aerial-perspective cues -- falling contrast, falling spatial frequency,
//   falling speed and rising thickness with distance -- are, in that order,
//   what the frequency bands already do.  The perspective and the spectrum are
//   the same gradient, which is why four layers read as one image rather than
//   as four meters.
//
// WHAT THIS FILE DOES NOT DO.  It does not touch wave_kernel.h,
// wave_spline.h or wave_ribbon.h.  It consumes stage 2's dense polyline and
// produces a polyline of the same length plus two opaque edge colours, which
// is exactly wr_build's input contract.  The solver keeps its physics, the
// spline keeps its convex-hull guarantee, and the ribbon keeps its vertex
// layout -- none of which is worth re-earning.
//
// wave_kernel.h is included for wk_sinf ONLY.  That is deliberate reuse rather
// than a fourth polynomial sine in the same pipeline: the function is already
// measured against libm in test_wave_kernel.c, this file is downstream of that
// header in the same chain, and including it costs nothing because everything
// there is static inline.
//
// House rules (.clinerules rule 7): header-only, pure C, no libm, no PS3
// headers, deterministic, caller-owned state, no globals.

#ifndef WAVE_LAYERS_H
#define WAVE_LAYERS_H

#include <stdint.h>

#include "wave_kernel.h"        /* wk_sinf only */
#include "wave_motion.h"

#define WL_LAYERS       WM_LAYERS

#define WL_SWELL        0
#define WL_BODY         1
#define WL_FILAMENT     2
#define WL_SHEEN        3

// --- the band ------------------------------------------------------------
// Clip space, +y up.  The wave occupies the bottom 27% of the screen and
// nothing else, which is the single strongest XMB cue there is: XMB's wave is
// a HORIZON, and a wave that climbs into the middle of the screen stops being
// a horizon and becomes wallpaper.  0.54 of clip space is 292 px at 1080p,
// which brackets the design handoff's "bottom 200px" for the visible ribbons
// with room for the crest to rise.
#define WL_BAND_TOP     (-0.46f)
#define WL_BAND_BOT     (-1.00f)

// The largest |y| the SOLVER actually produces, measured rather than assumed.
//
// wk_softclip bounds its output at 1.0, but that is a safety net, not the
// working range, and sizing the layers against it would waste three quarters
// of the band.  Measured over ten minutes at 60 fps at the motion stage's
// worst case -- drive WM_MAX_DRIVE, perturb WM_MAX_PERTURB, dt 2.0 *
// WM_MAX_TIME -- the spline peaks at 0.739.  0.75 is that with a margin.
//
// Two things fell out of that measurement and are worth recording here
// because neither is obvious from the kernel:
//
//   PERTURBATION CONTRIBUTES ALMOST NOTHING TO PEAK AMPLITUDE.  0.030 and
//   0.085 both give 0.840 at drive 1.30, to three decimals.  It is purely a
//   liveliness term, exactly as wave_kernel.h's WK_NOISE_SCALE comment claims.
//
//   PEAK AMPLITUDE IS LINEAR IN DRIVE, at about 0.672 per unit.  That is what
//   sets WM_MAX_DRIVE at 1.10 rather than something higher: at 1.30 the chain
//   reaches 0.840 and spends part of its time inside WK_KNEE (0.80), and a
//   solver sitting in its own soft clip has stopped being a solver.
//
// test_wave_layers.c re-measures this and fails if it drifts.
#define WL_BASE_MAX     0.75f

// --- per-layer appearance -------------------------------------------------
// Rest position of each layer's crest, furthest first.  The spread is uneven
// on purpose: evenly spaced ribbons read as a ruler.
// These sit CLOSE TOGETHER -- 0.047, 0.046 and 0.036 of clip space apart,
// about 25 px at 1080p -- and that is the second half of the fix described
// under WL_SHIFT.  The offset stopped the layers' base terms from cancelling,
// but the layers still never met, because the excursion they actually use is
// about a quarter of what THE AMPLITUDE BUDGET allows (the budget is sized for
// a worst case that needs the solver at its peak, every amplitude at 1 and all
// four pulses stacked on one sample), and a quarter of it did not span a
// 0.078 gap.
//
// Closing the gaps is the right correction rather than inflating the
// amplitudes to reach across them: overlapping translucent veils whose bright
// crests cross IS the XMB image, and ribbons far enough apart to need large
// amplitudes to meet would be four separate waves that occasionally collide.
// The spread is still uneven -- evenly spaced ribbons read as a ruler.
static const float WL_BASE_Y[WL_LAYERS] = {
    -0.855f, -0.808f, -0.762f, -0.726f
};

// HORIZONTAL OFFSET of the shared solver curve, per layer, in normalised u.
//
// This is the constant that makes the layers CROSS, and it was added because
// test_wave_layers.c's test_crossings() found that they did not.  The reason
// is worth recording, because it is not obvious and it defeats the obvious
// fix:
//
//   Every layer carries the SAME base curve, scaled by a similar amount
//   (WL_AMP spans only 0.036 to 0.075).  So the base terms very nearly
//   cancel in the difference between two adjacent layers -- layer 0 minus
//   layer 1 keeps only (0.075 - 0.068) = 0.007 of it.  The layers are
//   therefore separated by their WL_BASE_Y gap and differentiated only by
//   their much smaller secondary waves, and over four minutes of music not one
//   adjacent pair ever met.  Four parallel ribbons.  Raising the secondary
//   amplitudes enough to close a 0.095 gap would have made them the dominant
//   term and lost the family resemblance that makes the layers read as one
//   wave.
//
// Offsetting WHERE each layer samples the shared curve fixes it at the root:
// the base terms no longer cancel, because they are no longer the same
// number.  It is also what XMB itself visibly does -- the same wave shape at
// different horizontal offsets -- and it costs one lerp per sample.
//
// The offsets are clamped, not wrapped.  Wrapping would splice u=1 onto u=0
// and put a discontinuity at the seam; clamping simply repeats the curve's
// pinned end value, so a shifted layer is flat in its BASE term over the
// outermost |shift| of one edge.  That is invisible: the secondary wave, the
// ripple and the pulses are all still computed from the true u, so nothing is
// actually flat there, and the outer edge of the screen is where the wave
// flattens anyway.
static const float WL_SHIFT[WL_LAYERS] = {
    -0.12f, 0.00f, 0.09f, 0.17f
};

// Scale on the shared solver curve.  Falls with proximity, so the distant
// swell carries the largest sweep -- a large far object and a small near one,
// which is the depth reading the layers exist to produce.
static const float WL_AMP[WL_LAYERS] = {
    0.105f, 0.092f, 0.072f, 0.052f
};

// The layer's OWN secondary wave: one sine at its own wavenumber in cycles
// across the span, at its own drifting phase (WM_PHASE_RATE).  This is what
// produces the crossings.
//
// The XMB wave's real signature is not that curves move, it is that several
// curves cross each other at shallow angles.  Parallel bands look like a
// graph; crossing bands look like light.  Because the layers share a base
// curve they keep a family resemblance, and because their secondary terms
// differ in both wavelength and drift rate they intersect at points that
// migrate slowly and never repeat on a short cycle -- 0.75 / 1.60 / 2.90 /
// 4.30 share no small common factor, and neither do the phase rates.
static const float WL_SEC_K[WL_LAYERS] = {
    0.75f, 1.60f, 2.90f, 4.30f
};
static const float WL_SEC_AMP[WL_LAYERS] = {
    0.040f, 0.034f, 0.026f, 0.018f
};

// Fine ripple: high spatial frequency, tiny amplitude, driven by the treble.
// Zero on the furthest layer, because a distant object does not show fine
// detail -- dropping it is what makes the depth reading work rather than
// looking like four copies of the same ribbon.
static const float WL_RIP_K[WL_LAYERS] = {
    0.0f, 11.0f, 19.0f, 29.0f
};
static const float WL_RIP_AMP[WL_LAYERS] = {
    0.0f, 0.0050f, 0.0068f, 0.0082f
};

// How much a travelling pulse lifts each layer.  Rises with proximity: a beat
// disturbs the nearest filament most, which is both physically sensible and
// the reason a beat reads as a gesture passing through rather than as the
// whole field flashing.
static const float WL_PULSE_AMP[WL_LAYERS] = {
    0.009f, 0.017f, 0.024f, 0.030f
};

// Vertical breath of the whole band with loudness.
static const float WL_LIFT[WL_LAYERS] = {
    0.008f, 0.014f, 0.018f, 0.022f
};

// Half-thickness in clip space.  RISES with distance: near things are thin and
// sharp, far things are broad and soft.  The ribbon's lower edge is allowed to
// run off the bottom of the screen, where wr_put clamps it -- that is correct
// for a veil and is what ui_wave.cpp's existing ribbons already do.
static const float WL_HALF[WL_LAYERS] = {
    0.105f, 0.082f, 0.055f, 0.030f
};

// Crest opacity relative to the theme's wave_alpha.  Body is the reference at
// 1.0; the swell behind it is dimmer and the sheen in front is dimmer still,
// because the nearest layer is the thinnest and a thin bright line at full
// opacity is a scratch, not a highlight.
static const float WL_ALPHA_W[WL_LAYERS] = {
    0.62f, 1.00f, 0.78f, 0.55f
};

// --- THE AMPLITUDE BUDGET -------------------------------------------------
// Every layer's total excursion is bounded at BUILD time, not clamped at
// runtime.  With base at WL_BASE_MAX, amplitude and detail at 1, every one of
// the WM_PULSES pulses at full amplitude and sitting on the same sample, and
// lift at 1 -- a combination that cannot actually occur -- each layer's crest
// still stays inside [WL_BAND_BOT, WL_BAND_TOP]:
//
//   layer   down to    up to     margin below / above
//     0     -0.974     -0.692      0.026  /  0.232
//     1     -0.916     -0.618      0.084  /  0.158
//     2     -0.849     -0.561      0.151  /  0.101
//     3     -0.791     -0.519      0.209  /  0.059
//
// wl_curve still clamps, for the same reason wk_softclip is still in the
// kernel: a bound that is asserted rather than assumed.  A clamp that ever
// engages here is a bug report, and test_wave_layers.c checks the margin
// rather than the clamp so it would catch one.

// --- CRT atmosphere, and what of it is not built yet ----------------------
// ONE cue is implemented: chromatic spread at the crest.  wave-spec.md section
// 3 recorded that the reference's bloom used a wider Gaussian radius for blue
// than for red in every configuration measured, and that the resulting colour
// fringe is what the eye reads as glare.  There is no bloom chain here -- it
// is several fullscreen passes and this renderer's constraint is pixel traffic
// -- but the fringe itself is three extra multiplies: the crest composites its
// blue channel at a higher alpha than its red, and the foot does the reverse,
// so the top edge of every ribbon fringes cool and the underside fringes warm.
//
// NOT implemented, and deliberately not faked: the quantised crest ramp that
// would read as phosphor banding on a CRT.  It needs the ribbon tessellated
// vertically into several bands so the steps have somewhere to live, and
// wr_build emits exactly two vertices per column by contract.  That is a
// change to a tested stage for a cosmetic gain and it is not worth making
// blind; if it is wanted later it is a new builder alongside wr_build, not an
// edit to it.
#define WL_FRINGE_TOP_R 0.84f
#define WL_FRINGE_TOP_G 0.94f
#define WL_FRINGE_TOP_B 1.00f
#define WL_FRINGE_BOT_R 0.10f
#define WL_FRINGE_BOT_G 0.06f
#define WL_FRINGE_BOT_B 0.04f

// Glow lifts the crest alpha of the two near layers on an onset.  The far two
// are excluded: a distant object does not glint.
#define WL_GLOW_GAIN    0.55f

typedef struct { uint8_t r, g, b; } wl_rgb;

static inline float wl_clampf(float v, float lo, float hi)
{
    if (v != v) return lo;                  // NaN in, defined out
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static inline uint8_t wl_u8(float v)
{
    if (v != v)      return 0;
    if (v <= 0.0f)   return 0;
    if (v >= 255.0f) return 255;
    return (uint8_t)(v + 0.5f);
}

// src over dst with a 0..1 alpha, in float.  ui_wave.cpp's over8 in the domain
// this stage works in; the rounding to u8 happens once, at the end, rather
// than once per compositing step.
static inline float wl_over(float src, float dst, float a)
{
    return dst + (src - dst) * a;
}

// Compact smooth bump, (1 - t^2)^2 over |t| < 1.
//
// A Gaussian would need an exp.  This is C1, it is two multiplies and a
// subtract, and -- the reason it is actually preferred rather than merely
// tolerated -- it has COMPACT SUPPORT, so a pulse only touches the samples
// within its own width instead of every sample on the ribbon.  Four pulses
// over 72 samples then cost about a quarter of what an exp-based bump would,
// before counting the exp itself.
static inline float wl_bump(float d, float width)
{
    float t, s;
    if (!(width > 0.0f)) return 0.0f;
    t = d / width;
    if (t < 0.0f) t = -t;
    if (t >= 1.0f) return 0.0f;
    s = 1.0f - t * t;
    return s * s;
}

// Half-thickness for a layer, for the caller to hand wr_build.
static inline float wl_half(int layer)
{
    if (layer < 0 || layer >= WL_LAYERS) return 0.0f;
    return WL_HALF[layer];
}

// Build layer `layer`'s crest curve from the shared spline output.
//
//   base_y  stage 2's oy, m samples, the ONE solver curve every layer shares
//   m       sample count, >= 2
//   p       stage B's parameters
//   out_y   destination, m samples; may not alias base_y
//
// x is not produced here: stage 2's ox is already linear across the span and
// every layer uses the same one, which is both correct (the layers must line
// up horizontally to cross) and one fewer array per layer.
//
// Returns m on success, 0 if the request is degenerate, leaving out untouched.
static inline int wl_curve(const float *base_y, int m,
                           const wm_params *p, int layer, float *out_y)
{
    float inv, shift, amp, sec_a, rip_a, sec_ph, base_y_l, lift;
    int   k, i;

    if (!base_y || !out_y || !p) return 0;
    if (m < 2) return 0;
    if (layer < 0 || layer >= WL_LAYERS) return 0;

    inv      = 1.0f / (float)(m - 1);
    shift    = WL_SHIFT[layer] * (float)(m - 1);
    amp      = WL_AMP[layer]     * wl_clampf(p->amp[layer],    0.0f, 1.0f);
    sec_a    = WL_SEC_AMP[layer] * (0.35f + 0.65f * wl_clampf(p->amp[layer],
                                                              0.0f, 1.0f));
    rip_a    = WL_RIP_AMP[layer] * wl_clampf(p->detail[layer], 0.0f, 1.0f);
    sec_ph   = p->phase[layer];
    base_y_l = WL_BASE_Y[layer];
    lift     = WL_LIFT[layer] * wl_clampf(p->lift, 0.0f, 1.0f);

    for (k = 0; k < m; k++) {
        float u  = (float)k * inv;
        float ph = u * WK_TWO_PI;
        float y;

        // The shared solver curve, sampled at this layer's own horizontal
        // offset and scaled.  This is the term that makes the layers a family
        // -- they all carry the same curve -- and, through WL_SHIFT, also the
        // term that makes them cross.  Linear interpolation between the two
        // neighbouring samples, with the index clamped at both ends.
        {
            float fs = (float)k + shift;
            int   i0;
            float fr;
            if (fs <= 0.0f)            { i0 = 0;     fr = 0.0f; }
            else if (fs >= (float)(m - 1)) { i0 = m - 2; fr = 1.0f; }
            else { i0 = (int)fs; fr = fs - (float)i0; }
            y = (base_y[i0] + (base_y[i0 + 1] - base_y[i0]) * fr) * amp;
        }

        // The layer's own secondary wave.  Its amplitude never falls to zero
        // with the audio -- the 0.35 floor above -- because this is the term
        // that keeps the crossings alive through a quiet passage, and a
        // silent screen with four parallel ribbons on it is the one failure
        // mode this whole design is trying to avoid.
        y += sec_a * wk_sinf(WL_SEC_K[layer] * ph + sec_ph);

        // Treble grain.  Rides on the same drifting phase so it does not read
        // as a separate, static texture laid over a moving curve.
        if (rip_a > 0.0f)
            y += rip_a * wk_sinf(WL_RIP_K[layer] * ph - sec_ph * 2.0f);

        // Travelling pulses.  Compact support, so this loop does almost no
        // work for the samples a pulse is not over.
        for (i = 0; i < WM_PULSES; i++) {
            const wm_pulse *q = &p->pulse[i];
            if (!q->live || q->amp <= 0.0f) continue;
            y += WL_PULSE_AMP[layer] * q->amp * wl_bump(u - q->x, q->width);
        }

        y += base_y_l + lift;

        // Should never engage -- see THE AMPLITUDE BUDGET above.
        out_y[k] = wl_clampf(y, WL_BAND_BOT, WL_BAND_TOP);
    }
    return m;
}

// The two opaque edge colours for a layer, ready for wr_build.
//
//   p          stage B's parameters
//   layer      which layer
//   accent     theme accent      (the violet end of the hue line)
//   accent_alt theme accent_alt  (the cyan end)
//   bg_crest   background colour where this layer's crest sits
//   bg_foot    background colour where its lower edge sits
//   wave_alpha the theme's wave_alpha token, 0..255
//   suppress   0..1 extra attenuation; handoff section 1.5 puts the wave at a
//              third whenever a hero backdrop is on screen, so that is
//              suppress = 0.333 rather than a second set of constants
//
// BLENDING STAYS OFF.  The colours come back already composited against the
// background, alpha implied, which is exactly wr_build's contract and the
// path ui_wave.cpp already proves on hardware.  Nothing here emits a partial
// alpha.
static inline void wl_shade(const wm_params *p, int layer,
                            wl_rgb accent, wl_rgb accent_alt,
                            wl_rgb bg_crest, wl_rgb bg_foot,
                            uint8_t wave_alpha, float suppress,
                            wl_rgb *top, wl_rgb *bot)
{
    float hue, bright, glow, a, tr, tg, tb;

    if (!top || !bot) return;
    if (!p || layer < 0 || layer >= WL_LAYERS) {
        if (top) { top->r = bg_crest.r; top->g = bg_crest.g; top->b = bg_crest.b; }
        if (bot) { bot->r = bg_foot.r;  bot->g = bg_foot.g;  bot->b = bg_foot.b;  }
        return;
    }

    // Hue is already restricted to the middle of the accent line by stage B
    // (WM_HUE_LO / WM_HUE_SPAN), so this is a straight mix.  Those two theme
    // tokens mean something specific in this UI -- purple for what the user is
    // pointing at, blue for what the file is -- and a background that reached
    // either endpoint would compete with a focus ring.
    hue    = wl_clampf(p->hue, 0.0f, 1.0f);
    bright = wl_clampf(p->bright, 0.0f, 1.0f);
    glow   = wl_clampf(p->glow, 0.0f, 1.0f);

    tr = (float)accent.r + ((float)accent_alt.r - (float)accent.r) * hue;
    tg = (float)accent.g + ((float)accent_alt.g - (float)accent.g) * hue;
    tb = (float)accent.b + ((float)accent_alt.b - (float)accent.b) * hue;

    // Brightness lifts the tint toward white rather than scaling it, so a loud
    // passage reads as more LIGHT rather than as more colour.  Scaling would
    // saturate the hue at high energy and make the whole thing look like a
    // level meter.
    {
        float lift = 0.18f + 0.30f * bright;
        tr = tr + (255.0f - tr) * lift;
        tg = tg + (255.0f - tg) * lift;
        tb = tb + (255.0f - tb) * lift;
    }

    a = (float)wave_alpha * (1.0f / 255.0f) * WL_ALPHA_W[layer];
    a *= 0.45f + 0.55f * bright;
    // Only the two near layers glint; a distant object does not.
    if (layer >= WL_FILAMENT) a *= 1.0f + WL_GLOW_GAIN * glow;
    a *= wl_clampf(suppress, 0.0f, 1.0f);
    a  = wl_clampf(a, 0.0f, 1.0f);

    // Chromatic spread: the crest composites blue at a higher alpha than red,
    // the foot the reverse.  Cool fringe above, warm fringe below, no bloom.
    top->r = wl_u8(wl_over(tr, (float)bg_crest.r, a * WL_FRINGE_TOP_R));
    top->g = wl_u8(wl_over(tg, (float)bg_crest.g, a * WL_FRINGE_TOP_G));
    top->b = wl_u8(wl_over(tb, (float)bg_crest.b, a * WL_FRINGE_TOP_B));

    bot->r = wl_u8(wl_over(tr, (float)bg_foot.r, a * WL_FRINGE_BOT_R));
    bot->g = wl_u8(wl_over(tg, (float)bg_foot.g, a * WL_FRINGE_BOT_G));
    bot->b = wl_u8(wl_over(tb, (float)bg_foot.b, a * WL_FRINGE_BOT_B));
}

#endif // WAVE_LAYERS_H
