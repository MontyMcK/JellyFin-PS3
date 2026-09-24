// JellyWave stage 1-6: the lofted translucent body.
//
//   wave_cam.h    world space -> clip space
//   wave_light.h  the one shared light + its LUTs
//   wave_gel.h    the lofted translucent body           (this file)
//
// WHAT THIS IS
//
// The approved design (design-import-jellywave/jellyfin-ribbon.js) builds a
// gel band by sweeping a stadium cross-section along an undulating spine,
// displacing each ring point along its own normal by a travelling "lump"
// field, and writing the material's identity into VERTEX COLOUR.  This file is
// that construction, with two substitutions and two reductions, all four of
// which are listed below so the next reader can tell design from adaptation.
//
// SUBSTITUTION 1 -- THE UNDULATION COMES FROM THE SOLVER.
// The design's spine carries `0.85*sin(13.4u) + 0.34*sin(6.1u)`: a fixed shape
// sliding sideways, which is the exact thing docs/wave-spec.md section 3b says
// not to do and which this project replaced with a damped spring chain three
// stages ago.  That term -- and only that term -- is replaced by wave_field.h's
// displacement.  Everything else in the spine is the design's, literally:
//
//     the -0.95u tilt        the band descends left to right; this is most of
//                            what makes the silhouette read as the design's
//                            rather than as a generic wave
//     the z ramp 1.1 - 2.9u  the band recedes along its own length, which is
//                            what gives the taper something to be perspective
//                            OF rather than merely a narrowing strip
//     the 0.45*sin(3.0u) z   a slow lateral meander
//     roll(u)                the band twists about its own axis
//     halfW / halfT tapers   1.05 -> 0.63 and 0.245 -> 0.176
//
// SUBSTITUTION 2 -- THE CAMERA IS PANNED.  See wave_cam.h.  The design frames
// this object across the middle of the screen; the XMB's middle belongs to the
// card grid, so the camera pans up and the band sits low and diagonal.  It is
// a pure pan, so the object itself is seen exactly as designed.
//
// REDUCTION 1 -- THE SECTION IS 12 POINTS, NOT 30.
// The design uses NC=9 cap samples and NS=6 straight samples per side.  This
// uses NC=4 and NS=2, by the design's own construction, so every point and
// every rim weight falls out of the same formulas.  NC=4 is the smallest value
// that KEEPS THE FLAT TOP ON THE RIM BAND: the cap samples land at +/-90 and
// +/-30 degrees, and the two at 30 degrees both carry rim 0.9018, so the strip
// between them is a band of near-constant rim rather than a single bright
// vertex smeared by interpolation.  At NC=3 the samples are 0 and +/-90, one
// vertex carries rim 1 and its neighbours carry 0, and the rolled edge
// degenerates into a triangular ramp -- a feathered glow around a flat ribbon,
// which is precisely the failure the measured Fresnel work warned about.
//
// REDUCTION 2 -- TWO OF THE FOUR LUMP TERMS ARE DROPPED, AND THIS IS MEASURED.
// The design's lump field is four sines in u.  Over the widest layer's u span
// of 2.289, at JW_STATIONS = 80 samples, they land at:
//
//     0.050 * sin(11.5*u*pi + 1.7t)    13.2 cycles    6.1 samples/cycle   kept
//     0.034 * sin( 6.2*u*pi - 2.4t)     7.1 cycles   11.3 samples/cycle   kept
//     0.020 * sin(23.0*u*pi + 0.9t)    26.3 cycles    3.0 samples/cycle   DROPPED
//     0.012 * sin(37.0*u*pi - 3.1t)    42.3 cycles    1.9 samples/cycle   DROPPED
//
// The fourth term is BELOW NYQUIST -- it cannot be sampled at this station
// count, and what it would actually produce is a low-frequency beat pattern
// that looks like a bug.  The third is at three samples a cycle, which is
// above Nyquist and still visibly ropy.  Raising JW_STATIONS to fix them is
// not worth it: 96 stations only takes the fourth term to 2.3 samples a cycle,
// so the aliasing survives and the vertex count grows 20% for nothing.
//
// The dropped amplitude is NOT redistributed into the surviving terms.  Doing
// that would preserve the total wobble by making the big lumps bigger, which
// changes the character of the silhouette rather than restoring the detail
// that was lost.  Fine surface grain is stage 7's job, by a means that does
// not need more stations.
//
// House rules (.clinerules rule 7): header-only, pure C, no libm, no PS3
// headers, deterministic, caller-owned state, no globals.  wave_kernel.h is
// included for wk_sinf ONLY -- the same deliberate reuse wave_layers.h makes,
// for the same reason: that function is already measured against libm in
// test_wave_kernel.c and a fourth polynomial sine in one pipeline is a
// liability, not an optimisation.

#ifndef WAVE_GEL_H
#define WAVE_GEL_H

#include "wave_cam.h"
#include "wave_light.h"
#include "wave_kernel.h"        /* wk_sinf only */

// --- sizes ----------------------------------------------------------------
//
// 12 points around the section (NC=4, NS=2 -- see REDUCTION 1), so 12 strips
// once the loop closes.
#define JW_SECTION      12

// 80 stations along the flow.  Sized by the two constraints that actually
// bind: the surviving lump terms want at least six samples a cycle (see
// REDUCTION 2), and the solver's own content is bounded by WK_K2 = 5 cycles
// across the span, which 80 stations sample 16 times a cycle.
#define JW_STATIONS     80

#define JW_VERTS        (JW_SECTION * JW_STATIONS)

// --- the design's constants ----------------------------------------------
#define JW_LEN          13.0f

#define JW_TILT         (-0.95f)     /* spine y, linear in u */
#define JW_YBASE          0.30f
#define JW_Z0             1.10f
#define JW_ZRAMP        (-2.90f)
#define JW_ZWAVE_A        0.45f
#define JW_ZWAVE_K        3.00f
#define JW_ZWAVE_P        1.10f

#define JW_HW0            1.05f      /* half-width  at u=0 */
#define JW_HW_TAPER       0.40f
#define JW_HT0            0.245f     /* half-thick  at u=0 */
#define JW_HT_TAPER       0.28f

#define JW_ROLL_A         0.30f
#define JW_ROLL_K         5.40f
#define JW_ROLL_P         0.90f
#define JW_ROLL_LIN       0.10f

#define JW_LUMP_A1        0.050f
#define JW_LUMP_K1       11.50f
#define JW_LUMP_T1        1.70f
#define JW_LUMP_A2        0.034f
#define JW_LUMP_K2        6.20f
#define JW_LUMP_T2      (-2.40f)
#define JW_LUMP_P2        2.10f

// Historical: the design's central-difference step for the tangent. No longer
// read anywhere -- jw_spine_tangent() differentiates jw_spine's analytic terms
// directly instead of sampling it twice -- kept only so the derivation in
// jw_spine_tangent's comment can still cite the step size it replaces.
#define JW_DIFF_E         0.0015f

// --- palette --------------------------------------------------------------
// Straight from the design, which took the first two from the Jellyfin brand
// SVG.  These are also g_theme's accent / accent_alt, so a theme that repaints
// the UI repaints the wave with it -- see jw_palette_from_theme().
//   PURPLE #AA5CC3   BLUE #00A4DC   RIM #d8f4ff   DEEP #150a22   FRINGE #7fd0ff
#define JW_PURPLE_R    0.666667f
#define JW_PURPLE_G    0.360784f
#define JW_PURPLE_B    0.764706f
#define JW_BLUE_R      0.000000f
#define JW_BLUE_G      0.643137f
#define JW_BLUE_B      0.862745f
#define JW_RIMC_R      0.847059f
#define JW_RIMC_G      0.956863f
#define JW_RIMC_B      1.000000f
#define JW_DEEP_R      0.082353f
#define JW_DEEP_G      0.039216f
#define JW_DEEP_B      0.133333f
#define JW_FRINGE_R    0.498039f
#define JW_FRINGE_G    0.815686f
#define JW_FRINGE_B    1.000000f

// The brand gradient axis: 30 degrees below horizontal, purple upper-left to
// blue lower-right.  Verified from jellyfin-ux's icon-transparent.svg, whose
// linearGradient runs (110.25,213.3) -> (496.14,436.09) -- exactly -30 degrees.
// This is the design's AX, and the reason the colour is a property of WHERE a
// point is in space rather than a gradient paint laid over the finished image.
#define JW_AXIS_X      0.866025f
#define JW_AXIS_Y     (-0.500000f)
#define JW_AXIS_BIAS   6.40f
#define JW_AXIS_SPAN  12.00f

// Terracing: the design quantises its shade term to 5 steps and mixes the
// result back at 45%.  That banding is a named feature of the approved look --
// the internal terraced structure a thick translucent body shows -- so the
// numbers are kept exactly.
#define JW_TERRACE_N    5.0f
#define JW_TERRACE_MIX  0.45f

#define JW_SHADE_BASE   0.38f
#define JW_SHADE_GAIN   0.78f
#define JW_DEEP_MIX     0.34f
#define JW_RIM_MIX      0.20f

// Additive rim pass.
#define JW_RIMPASS_BASE 0.10f
#define JW_RIMPASS_LIT  0.90f
#define JW_RIMPASS_I    0.62f
#define JW_FRINGE_BASE  0.30f
#define JW_FRINGE_GAIN  0.35f

// --- per-layer description ------------------------------------------------
//
// The design's LAYERS table, plus the u span each layer needs to run off both
// screen edges (solved in wave_cam.h's derivation) and the alpha its opacity
// becomes.  `phase` offsets the static sines; the SOLVER phase is separate and
// already differs per layer, because wave_field.h runs three chains at
// different rates with different seeds.
typedef struct {
    float         phase;
    float         y_off, z_off;
    float         scale;
    float         bright;
    float         u0, u1;
    float         disp_gain;
    unsigned char alpha;
} jw_layer;

// --- the undulation gain, and why it is not 1 -----------------------------
//
// The solver's output and the design's undulation are in different units, and
// feeding one straight into the other gets the silhouette visibly wrong: the
// first build of this file did exactly that and the band came out at 55% of
// the design's excursion -- a flat wave with correct colours on it.
//
// BOTH SIDES ARE MEASURED, not assumed.
//
//   The design's term, 0.85*sin(13.4u) + 0.34*sin(6.1u), sampled over the
//   widest layer's span at all three layer phases:   peak 1.187, rms 0.664.
//
//   wave_field.h's sy[], over 6,000 frames at dt 1.25, perturb 0.02, drive 1:
//     layer 0  peak 0.6506  rms 0.2124      layer 1  peak 0.5563  rms 0.1834
//     layer 2  peak 0.4549  rms 0.1462
//   The layer 0 peak agrees with WF_NOMINAL_PEAK (0.653) to three decimals,
//   which is the cross-check that these are the same quantity.
//
// MATCHED ON PEAK, NOT RMS, and the difference is not small.  The solver's
// curve has a crest factor of 3.06 against the design's 1.79 -- a driven
// spring chain spends most of its time well below its peak, two summed sines
// do not.  Matching rms would need a gain of 3.13 and would then throw peaks
// to 2.04, well past the 1.187 the design's silhouette is drawn around and far
// enough to push the band off the top of the frame.  Matching peak keeps the
// envelope the design authored and leaves the typical excursion calmer, which
// is the same trade ui_wave.cpp's wave_field_px already makes against
// WF_NOMINAL_PEAK.
//
// DIVIDED BY WF_DRIVE[l] AS WELL, for the reason wave_field_px records: the
// solver runs its back layers at 0.85 and 0.70 to calm them, but the DESIGN
// does not taper its undulation by layer at all -- only phase, offset, scale
// and brightness differ between near, mid and far, and the spine's two sines
// are identical in all three.  Leaving the solver's taper in would apply a
// depth cue the design expresses another way, on top of the one it does.  What
// the per-layer drive still buys is what it is for: the back layers carry less
// fine detail and drift more slowly.
//
//   disp_gain[l] = 1.187 / (WF_NOMINAL_PEAK * WF_DRIVE[l])
//
// test_wave_gel.c recomputes that from wave_field.h's own constants, so a
// change to either side fails the build rather than quietly flattening the
// wave again.
#define JW_DISP_PEAK   1.187f

// near / mid / far, furthest LAST -- the array is in the design's own order and
// callers draw it back to front.
#define JW_LAYERS 3

static const jw_layer JW_LAYER[JW_LAYERS] = {
    /* near */ { 0.00f,  0.00f,  0.0f, 1.00f, 1.00f, -0.2776f, 1.2785f, 1.8177f, 255 },
    /* mid  */ { 0.42f, -0.78f, -3.2f, 0.90f, 0.66f, -0.5976f, 1.3862f, 2.1385f, 235 },
    /* far  */ { 0.85f, -1.55f, -6.2f, 0.78f, 0.42f, -0.8000f, 1.4890f, 2.5967f, 179 },
};

// --- the section ----------------------------------------------------------
//
// The design's section() with NC=4, NS=2, pre-evaluated.  Each point is
//   cx = a_st * st + a_ht * ht      st = max(hw - ht, eps)
//   cy = b_ht * ht
// so the table is independent of the layer's taper and the per-station work is
// two multiplies and an add.
//
// The order walks the closed loop: right cap bottom-to-top, across the top
// right-to-left, left cap top-to-bottom, across the bottom left-to-right.
// 0.901824 is smoothstep(0.60, 0.93, cos 30deg), the design's rim weight.
static const float JW_SEC_AST[JW_SECTION] = {
     1.0f,  1.0f,  1.0f,  1.0f,  0.333333f, -0.333333f,
    -1.0f, -1.0f, -1.0f, -1.0f, -0.333333f,  0.333333f
};
static const float JW_SEC_AHT[JW_SECTION] = {
     0.0f,  0.866025f,  0.866025f, 0.0f, 0.0f, 0.0f,
     0.0f, -0.866025f, -0.866025f, 0.0f, 0.0f, 0.0f
};
static const float JW_SEC_BHT[JW_SECTION] = {
    -1.0f, -0.5f,  0.5f,  1.0f,  1.0f,  1.0f,
     1.0f,  0.5f, -0.5f, -1.0f, -1.0f, -1.0f
};
static const float JW_SEC_RIM[JW_SECTION] = {
     0.0f, 0.901824f, 0.901824f, 0.0f, 0.0f, 0.0f,
     0.0f, 0.901824f, 0.901824f, 0.0f, 0.0f, 0.0f
};

// --- one finished vertex --------------------------------------------------
//
// Deliberately NOT wr_vert and deliberately NOT WaveVert.  wave_ribbon.h's
// layout is a tested contract with RSX local memory whose alignment rules cost
// this project a wedged console once already; this struct never goes near
// video memory.  ui_wave.cpp reads it and writes WaveVert itself, which keeps
// the one dangerous layout in the one file that documents why it is that way.
typedef struct {
    float         x, y;          // clip space
    float         z;             // view depth, for the painter's sort
    int           ok;            // 0 = behind the camera, do not draw
    unsigned char r, g, b;       // body colour, already lit and tone-mapped
    unsigned char rr, rg, rb;    // additive rim-pass colour
} jw_vert;

static inline float jw_smooth(float e0, float e1, float x)
{
    float t;
    if (!(e1 - e0 > 1.0e-9f) && !(e0 - e1 > 1.0e-9f)) return 0.0f;
    t = (x - e0) / (e1 - e0);
    if (t != t)   return 0.0f;
    if (t <= 0.0f) return 0.0f;
    if (t >= 1.0f) return 1.0f;
    return t * t * (3.0f - 2.0f * t);
}

static inline float jw_cosf(float x) { return wk_sinf(x + WK_HALF_PI); }

static inline float jw_lerp(float a, float b, float t) { return a + (b - a) * t; }

// --- the spine ------------------------------------------------------------
//
// `disp` is wave_field.h's displacement for this layer at this station -- the
// solver term that replaces the design's two sines.  Everything else here is
// the design's, unchanged.
static inline jw_vec3 jw_spine(float u, float disp, const jw_layer *L)
{
    float y = disp + JW_TILT * u + JW_YBASE + L->y_off;
    float z = JW_Z0 + JW_ZRAMP * u
            + JW_ZWAVE_A * wk_sinf(JW_ZWAVE_K * u + JW_ZWAVE_P + L->phase)
            + L->z_off;
    return jw_v3(JW_LEN * (u - 0.5f), y, z);
}

static inline float jw_half_w(float u, const jw_layer *L)
{
    return JW_HW0 * (1.0f - JW_HW_TAPER * u) * L->scale;
}

static inline float jw_half_t(float u, const jw_layer *L)
{
    return JW_HT0 * (1.0f - JW_HT_TAPER * u) * L->scale;
}

static inline float jw_roll(float u, const jw_layer *L)
{
    return JW_ROLL_A * wk_sinf(JW_ROLL_K * u + JW_ROLL_P + L->phase)
         + JW_ROLL_LIN * u;
}

// Viscous lumping, along the point's own normal.  Two terms; see REDUCTION 2.
//
// Kept for the identity test only (test_wave_gel.c cross-checks jw_lump_fast
// against this on every station/vertex).  jw_build_layer calls jw_lump_fast,
// not this.
static inline float jw_lump(float u, float th, const jw_layer *L)
{
    return JW_LUMP_A1 * wk_sinf(JW_LUMP_K1 * u * WK_PI + JW_LUMP_T1 * th + L->phase)
         + JW_LUMP_A2 * wk_sinf(JW_LUMP_K2 * u * WK_PI + JW_LUMP_T2 * th
                                + JW_LUMP_P2 + L->phase);
}

// --- jw_lump, station-amortised -------------------------------------------
//
// jw_lump ran once PER VERTEX (2 wk_sinf calls each -- 24/station, the single
// biggest per-vertex trig cost measured on hardware), even though its two
// arguments split cleanly: `th` takes exactly JW_SECTION fixed values,
// independent of station/layer/frame, and JW_LUMP_T1/T2/P2 are compile-time
// constants.  So sin(A+B) = sinA*cosB + cosA*sinB splits each term into a
// STATION-level part (A = K*u*PI + phase, unknown until u is picked) and a
// SECTION-INDEX-level part (B = T*th_j, or T*th_j + P2 for term 2 -- known at
// compile time), and the section-index part needs computing exactly once,
// ever, not per station and not per vertex.
//
// The tables below are hand-computed decimal literals, not sinf()/cosf() --
// house rule 7 forbids libm, and a static initializer cannot fold a libm call
// anyway.  Generated by evaluating cos(T1*th_j), sin(T1*th_j),
// cos(T2*th_j+P2), sin(T2*th_j+P2) for th_j = (j/JW_SECTION)*2*pi,
// j = 0..JW_SECTION-1, T1 = JW_LUMP_T1 = 1.70, T2 = JW_LUMP_T2 = -2.40,
// P2 = JW_LUMP_P2 = 2.10.  test_wave_gel.c recomputes them at runtime via
// wk_sinf/jw_cosf and compares, so a hand-edit that drifts from the formula
// fails the build rather than the TV -- the same contract wave_light.h's LUTs
// already keep.
static const float JW_LUMP_COSB1[JW_SECTION] = {
     1.000000f,  0.629320f, -0.207912f, -0.891007f, -0.913545f, -0.258819f,
     0.587785f,  0.998630f,  0.669131f, -0.156434f, -0.866025f, -0.933580f,
};
static const float JW_LUMP_SINB1[JW_SECTION] = {
     0.000000f,  0.777146f,  0.978148f,  0.453990f, -0.406737f, -0.965926f,
    -0.809017f, -0.052336f,  0.743145f,  0.987688f,  0.500000f, -0.358368f,
};
static const float JW_LUMP_COSB2[JW_SECTION] = {
    -0.504846f,  0.664955f,  0.915811f, -0.098953f, -0.976967f, -0.504846f,
     0.664955f,  0.915811f, -0.098953f, -0.976967f, -0.504846f,  0.664955f,
};
static const float JW_LUMP_SINB2[JW_SECTION] = {
     0.863209f,  0.746884f, -0.401610f, -0.995092f, -0.213391f,  0.863209f,
     0.746884f, -0.401610f, -0.995092f, -0.213391f,  0.863209f,  0.746884f,
};

// sinA1/cosA1/sinA2/cosA2 are the station-level "A" terms -- computed ONCE
// per station in jw_build_layer, not passed u/th here at all.  j indexes the
// precomputed "B" tables above.  Zero trig calls; four multiplies, two adds
// per term.
static inline float jw_lump_fast(float sinA1, float cosA1,
                                 float sinA2, float cosA2, int j)
{
    return JW_LUMP_A1 * (sinA1 * JW_LUMP_COSB1[j] + cosA1 * JW_LUMP_SINB1[j])
         + JW_LUMP_A2 * (sinA2 * JW_LUMP_COSB2[j] + cosA2 * JW_LUMP_SINB2[j]);
}

// The exact derivative of jw_spine's ANALYTIC terms -- x = JW_LEN*(u-0.5), so
// dx/du = JW_LEN; y = disp + JW_TILT*u + const, and disp is what the caller
// already treats as locally constant across the tangent step (jw_build_layer
// passed the same reading for both finite-difference samples), so dy/du =
// JW_TILT; z = JW_Z0 + JW_ZRAMP*u + JW_ZWAVE_A*sin(JW_ZWAVE_K*u+JW_ZWAVE_P+phase)
// + const, so dz/du = JW_ZRAMP + JW_ZWAVE_A*JW_ZWAVE_K*cos(...).
//
// This REPLACES sampling jw_spine at u-JW_DIFF_E and u+JW_DIFF_E and
// subtracting: that finite difference was already degenerating to exactly
// this derivative (same disp reading on both sides, so the only thing the two
// samples actually differed by was these three analytic terms), so this is
// the identity the old code was numerically approximating, not a new
// approximation of its own -- one jw_cosf call in place of two full jw_spine
// constructions plus a subtract.
static inline jw_vec3 jw_spine_tangent(float u, const jw_layer *L)
{
    float dzdu = JW_ZRAMP + JW_ZWAVE_A * JW_ZWAVE_K
               * jw_cosf(JW_ZWAVE_K * u + JW_ZWAVE_P + L->phase);
    return jw_v3(JW_LEN, JW_TILT, dzdu);
}

// The rotation-minimising-ish frame the design uses: tangent from the
// analytic derivative above, side from crossing world up into it, up from
// crossing back, then both rolled about the tangent.
//
// The world-up cross degenerates where the tangent is vertical.  It cannot be
// here -- the spine advances JW_LEN along x over the span while its y moves by
// at most a couple of units, so the tangent is never within 60 degrees of
// vertical -- but jw_norm returns a zero vector rather than a NaN if it ever
// is, and the caller's normalise of the surface normal then falls back to the
// section's own axis instead of writing garbage into the vertex buffer.
typedef struct { jw_vec3 p, s, up; } jw_frame;

static inline jw_frame jw_frame_at(float u, float disp, const jw_layer *L)
{
    jw_frame f;
    jw_vec3  t = jw_norm(jw_spine_tangent(u, L));
    jw_vec3  s = jw_norm(jw_cross(jw_v3(0.0f, 1.0f, 0.0f), t));
    jw_vec3  up = jw_norm(jw_cross(t, s));
    float    r  = jw_roll(u, L);
    float    cs = jw_cosf(r), sn = wk_sinf(r);

    f.p  = jw_spine(u, disp, L);
    f.s  = jw_v3(s.x * cs + up.x * sn, s.y * cs + up.y * sn, s.z * cs + up.z * sn);
    f.up = jw_v3(up.x * cs - s.x * sn, up.y * cs - s.y * sn, up.z * cs - s.z * sn);
    return f;
}

// --- the material ---------------------------------------------------------
//
// The design's gelColor, kept verbatim, including its `shade` term.
//
// THAT TERM LOOKS LIKE DOUBLE SHADING AND IT IS THE DESIGN'S OWN STRUCTURE.
// gelColor bakes a top-bright/bottom-dark ramp into the vertex colour, and the
// design then feeds that colour to a MeshPhysicalMaterial which lights it
// again.  Removing the ramp here "because the light rig already does it" would
// be correcting the design rather than implementing it, and it would cost the
// terraced banding, which lives inside that ramp and is a named feature of the
// approved look.  The overall level the doubling produces is what
// JW_EXPOSURE in wave_light.h is calibrated against.
//
//   p        world position, for the brand-axis hue
//   topness  cy/ht in [-1,+1]
//   rim      the section's geometric rim weight
static inline jw_rgb jw_gel_color(jw_vec3 p, float topness, float rim,
                                  const jw_layer *L)
{
    float  t, top, shade, dm;
    jw_rgb c;

    t = (p.x * JW_AXIS_X + p.y * JW_AXIS_Y + JW_AXIS_BIAS) * (1.0f / JW_AXIS_SPAN);
    t = jw_clamp01(t);

    c.r = jw_lerp(JW_PURPLE_R, JW_BLUE_R, t);
    c.g = jw_lerp(JW_PURPLE_G, JW_BLUE_G, t);
    c.b = jw_lerp(JW_PURPLE_B, JW_BLUE_B, t);

    top   = jw_smooth(-1.0f, 1.0f, topness);
    shade = JW_SHADE_BASE + JW_SHADE_GAIN * top;
    // Terracing.  (int)(x + 0.5f) is round-half-up, and shade is positive
    // across its whole range (0.38 .. 1.16), so it needs no negative case.
    shade = shade * (1.0f - JW_TERRACE_MIX)
          + ((float)(int)(shade * JW_TERRACE_N + 0.5f) / JW_TERRACE_N)
            * JW_TERRACE_MIX;

    // The thick flat body darkens to violet-black; the rolled edge does not.
    dm  = JW_DEEP_MIX * (1.0f - top) * (1.0f - rim);
    c.r = jw_lerp(c.r, JW_DEEP_R, dm);
    c.g = jw_lerp(c.g, JW_DEEP_G, dm);
    c.b = jw_lerp(c.b, JW_DEEP_B, dm);

    c.r *= shade * L->bright;
    c.g *= shade * L->bright;
    c.b *= shade * L->bright;

    {
        float rm = rim * JW_RIM_MIX * L->bright;
        c.r = jw_lerp(c.r, JW_RIMC_R, rm);
        c.g = jw_lerp(c.g, JW_RIMC_G, rm);
        c.b = jw_lerp(c.b, JW_RIMC_B, rm);
    }
    return c;
}

// The design's rimColor: the additive companion pass, the flat-topped band
// along the rolled edge only, fringed toward blue at its outer sliver.
static inline jw_rgb jw_rim_color(float rim, float lit, const jw_layer *L)
{
    float  k = rim * (JW_RIMPASS_BASE + JW_RIMPASS_LIT * lit)
             * JW_RIMPASS_I * L->bright;
    float  f = JW_FRINGE_BASE + JW_FRINGE_GAIN * rim;
    jw_rgb c;
    c.r = jw_lerp(JW_RIMC_R, JW_FRINGE_R, f) * k;
    c.g = jw_lerp(JW_RIMC_G, JW_FRINGE_G, f) * k;
    c.b = jw_lerp(JW_RIMC_B, JW_FRINGE_B, f) * k;
    return c;
}

// --- the loft -------------------------------------------------------------
//
// Samples one layer into JW_VERTS finished vertices, in station-major order:
// vertex (i, j) is out[i * JW_SECTION + j].
//
//   L       the layer
//   disp    the solver's displacement, `ndisp` samples over u in [0,1]
//   aspect  display_width / display_height
//
// The GEOMETRY u runs over the layer's own extended span [u0, u1] -- past both
// ends of the design's authored domain, so the band leaves the screen instead
// of stopping in mid-air -- while the SOLVER u runs over the full [0,1] of the
// chain.  Mapping them separately is what keeps the solver's motion spread
// across the whole visible band; clamping the extended u into the chain's
// domain instead would have left the outer eighth of each end frozen.
//
// Returns JW_VERTS on success, 0 if the request is degenerate.
static inline int jw_build_layer(const jw_layer *L, const float *disp, int ndisp,
                                 float aspect, jw_vert *out, int cap)
{
    int   i, j;
    float du, dk;

    if (!L || !disp || !out) return 0;
    if (ndisp < 2 || cap < JW_VERTS) return 0;

    du = (L->u1 - L->u0) / (float)(JW_STATIONS - 1);
    dk = 1.0f / (float)(JW_STATIONS - 1);

    for (i = 0; i < JW_STATIONS; i++) {
        float    u   = L->u0 + du * (float)i;
        float    uk  = dk * (float)i;
        float    hw, ht, st;
        jw_frame f;

        // The solver curve, sampled at this station.  A linear read of the
        // dense spline; it is already C2 from wave_spline.h, so interpolating
        // it with anything higher would be re-smoothing something smooth at a
        // cost per station.
        float d0;
        {
            float s = uk * (float)(ndisp - 1);
            int   k = (int)s;
            float fr;
            if (k < 0) k = 0;
            if (k > ndisp - 2) k = ndisp - 2;
            fr = s - (float)k;
            d0 = (disp[k] + (disp[k + 1] - disp[k]) * fr) * L->disp_gain;
        }

        f  = jw_frame_at(u, d0, L);
        hw = jw_half_w(u, L);
        ht = jw_half_t(u, L);
        st = hw - ht;
        if (st < 1.0e-4f) st = 1.0e-4f;

        // jw_lump_fast's station-level "A" terms -- see jw_lump_fast's
        // comment.  Computed once per station, not once per vertex: this is
        // the whole point of the split.
        float lumpA1 = JW_LUMP_K1 * u * WK_PI + L->phase;
        float lumpA2 = JW_LUMP_K2 * u * WK_PI + L->phase;
        float lumpSinA1 = wk_sinf(lumpA1), lumpCosA1 = jw_cosf(lumpA1);
        float lumpSinA2 = wk_sinf(lumpA2), lumpCosA2 = jw_cosf(lumpA2);

        for (j = 0; j < JW_SECTION; j++) {
            float   cx  = JW_SEC_AST[j] * st + JW_SEC_AHT[j] * ht;
            float   cy  = JW_SEC_BHT[j] * ht;
            float   rim = JW_SEC_RIM[j];
            float   nx  = cx / hw, ny = cy / ht;
            jw_vec3 n, p, ev;
            jw_rgb  alb, lit_c, rimc;
            jw_proj pr;
            float   d, lit;
            jw_vert *v = &out[i * JW_SECTION + j];

            // The design's normal: the section point read as a direction in
            // the frame's own basis.  It is an ellipse normal rather than the
            // stadium's true one, which is the design's choice and a good one
            // -- across the flat top it tilts gently with cx, so the top face
            // shades as a softly inflated surface instead of a flat plane.
            n = jw_norm(jw_v3(f.s.x * nx + f.up.x * ny,
                              f.s.y * nx + f.up.y * ny,
                              f.s.z * nx + f.up.z * ny));

            d = jw_lump_fast(lumpSinA1, lumpCosA1, lumpSinA2, lumpCosA2, j)
              * (0.55f + 1.9f * ht);

            p = jw_v3(f.p.x + f.s.x * cx + f.up.x * cy + n.x * d,
                      f.p.y + f.s.y * cx + f.up.y * cy + n.y * d,
                      f.p.z + f.s.z * cx + f.up.z * cy + n.z * d);

            ev  = jw_eyevec(p);
            alb = jw_gel_color(p, ny, rim, L);
            lit_c = jw_shade(alb, n, ev, rim);

            // The rim pass is lit by the KEY alone, to the design's 0.8 power.
            // wave_light.h owns that dot product so the motes later agree with
            // the ribbon about where the light is.
            lit = jw_key_lit(n);
            lit = lit * (0.8f + 0.2f * lit);     // cheap stand-in for pow(x,0.8)
            rimc = jw_rim_color(rim, lit, L);

            pr = jw_project(p, aspect);

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

// --- draw order -----------------------------------------------------------
//
// PAINTER'S ALGORITHM, AND WHY IT IS EXACT HERE RATHER THAN A HEURISTIC.
//
// There is no depth buffer: ui_wave.cpp disables depth test and depth write,
// and enabling them would mean allocating a depth surface and changing surface
// state that the whole UI shares.  So the twelve strips have to be drawn in a
// correct order instead.
//
// A painter's sort is exact when the primitives cannot mutually overlap in
// depth, and this section CANNOT: it is a convex closed curve swept along a
// spine, so along any view ray the surface is hit at most twice, and the two
// hits are always on opposite sides of the section.  Sorting the strips by
// mean view depth therefore puts the far side of the tube behind the near side
// every time, for every roll angle -- there is no configuration of the roll
// that makes strip A partly in front of and partly behind strip B.
//
// Twelve elements, so an insertion sort, which is both the fastest thing at
// this size and stable -- two strips at equal depth keep their section order,
// which is what keeps the seam between them from flickering frame to frame.
static inline void jw_strip_order(const jw_vert *v, int *order)
{
    float mean[JW_SECTION];
    int   i, j, k;

    if (!v || !order) return;

    for (j = 0; j < JW_SECTION; j++) {
        float acc = 0.0f;
        int   n   = 0;
        int   j2  = (j + 1) % JW_SECTION;
        for (i = 0; i < JW_STATIONS; i++) {
            const jw_vert *a = &v[i * JW_SECTION + j];
            const jw_vert *b = &v[i * JW_SECTION + j2];
            if (a->ok) { acc += a->z; n++; }
            if (b->ok) { acc += b->z; n++; }
        }
        // A strip entirely behind the camera sorts to the back, where it will
        // be emitted first and contribute nothing, rather than to an
        // unpredictable place in the order.
        mean[j]  = (n > 0) ? (acc / (float)n) : 1.0e9f;
        order[j] = j;
    }

    // Descending mean depth: furthest first.
    for (i = 1; i < JW_SECTION; i++) {
        int   oi = order[i];
        float mi = mean[oi];
        k = i - 1;
        while (k >= 0 && mean[order[k]] < mi) {
            order[k + 1] = order[k];
            k--;
        }
        order[k + 1] = oi;
    }
}

// True when a strip carries the rolled edge, i.e. either of its two section
// points has a non-zero rim weight.  The additive rim pass emits only these --
// six strips of the twelve -- because the other six would contribute exactly
// zero and cost their fill anyway.
static inline int jw_strip_has_rim(int j)
{
    int j2 = (j + 1) % JW_SECTION;
    if (j < 0 || j >= JW_SECTION) return 0;
    return (JW_SEC_RIM[j] > 0.0f) || (JW_SEC_RIM[j2] > 0.0f);
}

#endif // WAVE_GEL_H
