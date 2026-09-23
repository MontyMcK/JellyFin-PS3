// JellyWave stage 0b: ONE light, shared by everything in the scene.
//
//   wave_cam.h    world space -> clip space
//   wave_light.h  the one shared light + its LUTs      (this file)
//   wave_gel.h    the lofted translucent body
//
// WHY ONE LIGHT, IN ITS OWN FILE
//
// The reverse-engineering pass on the reference XMB wave found that what makes
// its particles look like they belong to the wave is not their density -- it
// is that the motes and the ribbon are shaded by the SAME light vector, so a
// mote glints exactly when the travelling highlight passes it.  Correlation,
// not count.  Nothing enforces that if each consumer carries its own copy of
// the light; putting it in one header and having the gel and the motes both
// call into it is the mechanism, and it is the reason this file exists at all
// rather than four constants in wave_gel.h.
//
// WHAT IS REPRODUCED AND WHAT IS APPROXIMATED
//
// The design (design-import-jellywave/three-d-stage.js) lights the object with
// an ambient term, three directional lights, and a PMREM environment probe,
// then runs ACES filmic tone mapping at exposure 1.25.  The three directions
// and the four colours are taken from it verbatim -- they are the design.
//
// The ENVIRONMENT PROBE IS NOT reproduced.  It is a 1024x512 equirect baked to
// a mip chain, sampled per fragment by roughness; there is no texture unit in
// the wave's path and adding one is the single riskiest change available here.
// What the probe actually contributes to this object is a cool lobe above and
// to the right, a violet lobe below and to the left, and three small hard
// lobes that produce the crisp secondary speculars wet gel shows.  The first
// two are a hemispheric term and are folded into JW_AMB_SKY / JW_AMB_GND; the
// third is what JW_SPEC_LUT's narrow core stands in for.
//
// INTENSITIES ARE CALIBRATED HERE, NOT COPIED.  The design's numbers (key 3.4,
// fill 0.75, rim 0.9) are three.js intensities, which mean something specific
// inside its physically-based renderer -- a per-light division by pi, a
// different albedo convention, and a tone-map curve applied to the whole
// framebuffer.  Feeding those literals into this much simpler evaluator gives
// a different picture, so the RATIOS are kept and the absolute scale is set by
// JW_EXPOSURE, which is measured against the target luma bands in
// tests/test_wave_light.c rather than assumed.
//
// House rules (.clinerules rule 7): header-only, pure C, no libm, no PS3
// headers, deterministic, caller-owned state, no globals.  The LUTs are
// LITERAL tables rather than generated at init on purpose: a generated table
// would be file-scope dynamic initialisation, which wave_field.h's WF_DRIVE
// comment already rejects for this target.  The generating formulas are given
// with each table and test_wave_light.c re-derives them, so a hand-edit that
// drifts from the formula fails the build rather than the TV.

#ifndef WAVE_LIGHT_H
#define WAVE_LIGHT_H

#include "wave_cam.h"

// --- the rig --------------------------------------------------------------
//
// Directions are TOWARD the light, normalised.  The design places lights at
// world positions and three.js points a directional light from its position
// toward the origin, so a position doubles as a direction; these are those
// positions, normalised.
//
// KEY is the one that matters and the one wave_motes.h also reads: cool white
// from above, behind and to the right.  It is the same (9, 7.5, -9) the
// design's gelColor uses for its own `lit` term, so the baked body shading and
// the additive rim pass agree about where the light is.
#define JW_KEY_X       0.609208f
#define JW_KEY_Y       0.507673f
#define JW_KEY_Z      -0.609208f

// FILL: violet, low and to the left, in front.  This is the term that keeps
// the shadowed side coloured rather than merely dark.
#define JW_FILL_X     -0.792594f
#define JW_FILL_Y     -0.226455f
#define JW_FILL_Z      0.566139f

// RIM: Jellyfin blue from below and behind.  Rakes the underside of the band
// and is most of why the lower edge reads as translucent rather than as a
// shadow.
#define JW_RIM_X       0.267261f
#define JW_RIM_Y      -0.534522f
#define JW_RIM_Z      -0.801784f

// Light colours, 0..1 linear, straight from the design.
//   key  #dcefff   fill #8a4fb0   rim #00a4dc   ambient #2a2140
#define JW_KEY_R       0.862745f
#define JW_KEY_G       0.937255f
#define JW_KEY_B       1.000000f

#define JW_FILL_R      0.541176f
#define JW_FILL_G      0.309804f
#define JW_FILL_B      0.690196f

#define JW_RIM_R       0.000000f
#define JW_RIM_G       0.643137f
#define JW_RIM_B       0.862745f

// Intensities.  The design's ratios (3.4 : 0.75 : 0.9) are preserved; the
// common scale lives in JW_EXPOSURE.
#define JW_KEY_I       1.000000f
#define JW_FILL_I      0.220588f      /* 0.75 / 3.4 */
#define JW_RIM_I       0.264706f      /* 0.90 / 3.4 */

// Ambient, split into a sky term and a ground term so the environment probe's
// two big lobes survive as a hemispheric gradient, mixed by (n.y * 0.5 + 0.5).
// Both are the design's ambient #2a2140 moved toward one of the probe's lobes:
//
//   SKY = lerp(#2a2140, #2e6f96, 0.35)   the cool upper-right lobe
//   GND = lerp(#2a2140, #6a2f86, 0.15)   the violet lower-left lobe
//
// The two mix fractions are uneven because the probe's lobes are: the cool one
// is drawn at alpha 0.65 over a 560px radius and the violet at 0.55 over 480,
// and the object sits below the first and beside the second.  Setting them
// equal flattened the body into one ambient colour and cost the depth cue the
// hemispheric split exists for.
#define JW_AMB_SKY_R   0.170196f
#define JW_AMB_SKY_G   0.236471f
#define JW_AMB_SKY_B   0.369020f
#define JW_AMB_GND_R   0.202353f
#define JW_AMB_GND_G   0.137647f
#define JW_AMB_GND_B   0.292157f
#define JW_AMB_I       0.550000f

// Specular tint and strength.  Wet gel's highlight takes the light's colour,
// not the body's, which is what separates it from a merely bright patch of
// material.
#define JW_SPEC_I      0.620000f

// --- Fresnel, and the mistake that is easy to make here -------------------
//
// The first build of this rig added the Fresnel LUT straight into the result
// as its own glow term, at 0.38 of the key colour.  MEASURED, that inverted
// the design: the lower body came out at luma 0.365 and the rolled edge at
// 0.325 -- the edge DIMMER than the body it is supposed to define.
//
// The reason is geometry, not tuning.  The camera sits 9.25 degrees above the
// horizontal and the band lies almost flat, so its top and bottom faces are
// seen at about 81 degrees off their own normals -- n.v is 0.16 and the
// Fresnel LUT reads 0.73 across nearly the whole visible surface.  A flat add
// at grazing therefore brightens EVERYTHING by the same amount, which is
// exactly the "one bright mid-tone" failure that makes a translucent body look
// like flat plastic.  No value of a single gain fixes it; the term was the
// wrong shape.
//
// The design does not have this problem because its grazing response is
// DIRECTIONAL: clearcoat Fresnel there is a weight on an environment
// reflection, so a grazing surface only brightens when it happens to be
// reflecting one of the probe's lobes.  Fresnel is a weight on a specular
// term, not a term of its own.
//
// So it is two terms here, both cheap:
//
//   JW_FRES_SPEC   Fresnel multiplies the KEY's highlight, so grazing surfaces
//                  get a stronger specular rather than a brighter body.  This
//                  is the physically right structure and it restores the
//                  directionality the probe was supplying.
//
//   JW_FRES_EDGE   a Fresnel term gated by wave_gel.h's GEOMETRIC rim weight,
//                  so the rolled edge -- and only the rolled edge -- keeps the
//                  strong grazing response the design's flat-topped rim band
//                  is built to show.
#define JW_FRES_SPEC   1.600000f
#define JW_FRES_EDGE   0.450000f

// Tone-map exposure.  The design runs ACES filmic at 1.25 over a renderer
// whose light units differ from this evaluator's, so this is the calibration
// constant for the whole rig -- see INTENSITIES ARE CALIBRATED above.
// MEASURED in tests/test_wave_light.c against three bands:
//   a top-facing surface at the band's centre  -> luma 0.55 .. 0.75
//   the underside                              -> luma 0.04 .. 0.14
//   the rolled edge at its brightest           -> luma 0.85 .. 1.00
#define JW_EXPOSURE    1.250000f

// --- LUTs -----------------------------------------------------------------
//
// 33 entries, indexed by a value in [0,1] at 1/32 steps, linearly interpolated
// between.  33 rather than 32 so the last entry is exactly x = 1 and the top
// of the curve needs no special case -- the same reason wave_field.h uses 257
// samples.
#define JW_LUT_N       33

// FRESNEL, indexed by g = |n . eyevec|: 1 at grazing, 0 head-on.
//
//   f(g) = 1                        for g <= 0.12
//        = (1 - (g-0.12)/0.88)^4    above it
//
// THE FLAT TOP IS THE POINT, and it is why this is a table rather than a
// pow().  The reference wave's Fresnel LUT was measured and found to be
// saturated across its first tenth before decaying -- not a power curve at
// all.  A flat top is what makes a translucent edge read as ROUNDED AND
// INFLATED; a pow() curve feathers instead, and a feathered edge reads as
// glow around a flat ribbon, which is exactly the thing this design is not.
// The curve here is ours, chosen for that shape, not copied.
static const float JW_FRESNEL_LUT[JW_LUT_N] = {
    1.000000f, 1.000000f, 1.000000f, 1.000000f, 0.977466f, 0.845132f,
    0.726713f, 0.621197f, 0.527612f, 0.445021f, 0.372529f, 0.309276f,
    0.254442f, 0.207245f, 0.166940f, 0.132820f, 0.104220f, 0.080507f,
    0.061092f, 0.045420f, 0.032976f, 0.023283f, 0.015903f, 0.010434f,
    0.006514f, 0.003818f, 0.002061f, 0.000994f, 0.000407f, 0.000129f,
    0.000025f, 0.000002f, 0.000000f,
};

// SPECULAR, indexed by h = max(0, n . halfvector).
//
//   s(h) = 0.06 + 0.24*h^2 + 0.95*h^48
//
// Two terms on purpose, matching the two things the measured reference LUT's
// green channel did: a broad slow ramp (0.06 -> 0.30, the sheen a gel surface
// shows everywhere) plus a narrow core that only exists in the last three
// samples (the wet highlight).  One pow() cannot be both, which is the whole
// argument for a table.
static const float JW_SPEC_LUT[JW_LUT_N] = {
    0.060000f, 0.060234f, 0.060937f, 0.062109f, 0.063750f, 0.065859f,
    0.068437f, 0.071484f, 0.075000f, 0.078984f, 0.083437f, 0.088359f,
    0.093750f, 0.099609f, 0.105937f, 0.112734f, 0.120000f, 0.127734f,
    0.135938f, 0.144609f, 0.153750f, 0.163359f, 0.173438f, 0.183984f,
    0.195001f, 0.206491f, 0.218482f, 0.231132f, 0.245314f, 0.265536f,
    0.313826f, 0.492194f, 1.250000f,
};

typedef struct { float r, g, b; } jw_rgb;

static inline jw_rgb jw_col(float r, float g, float b)
{
    jw_rgb c; c.r = r; c.g = g; c.b = b; return c;
}

static inline float jw_clamp01(float v)
{
    if (v != v)    return 0.0f;        // NaN in, defined out
    if (v < 0.0f)  return 0.0f;
    if (v > 1.0f)  return 1.0f;
    return v;
}

// Linear lookup into a JW_LUT_N table over [0,1].  x is clamped, so a caller
// whose dot product came back a hair outside the range by rounding gets the
// endpoint rather than a read off the end of the array.
static inline float jw_lut(const float *t, float x)
{
    float s, fr;
    int   i;
    if (x != x) return t[0];
    if (x <= 0.0f) return t[0];
    if (x >= 1.0f) return t[JW_LUT_N - 1];
    s  = x * (float)(JW_LUT_N - 1);
    i  = (int)s;
    if (i > JW_LUT_N - 2) i = JW_LUT_N - 2;
    fr = s - (float)i;
    return t[i] + (t[i + 1] - t[i]) * fr;
}

static inline jw_vec3 jw_key_dir(void)  { return jw_v3(JW_KEY_X,  JW_KEY_Y,  JW_KEY_Z);  }
static inline jw_vec3 jw_fill_dir(void) { return jw_v3(JW_FILL_X, JW_FILL_Y, JW_FILL_Z); }
static inline jw_vec3 jw_rim_dir(void)  { return jw_v3(JW_RIM_X,  JW_RIM_Y,  JW_RIM_Z);  }

// Narkowicz's ACES filmic approximation, the curve the design's renderer runs.
//
// Used rather than a Reinhard shoulder because the two differ exactly where
// this material lives: ACES keeps its toe dark, so the gel's deep violet
// underside stays deep instead of being lifted into grey, and it rolls the
// specular core off without desaturating it into white. One divide a channel.
static inline float jw_tonemap(float x)
{
    float n, d;
    if (x != x) return 0.0f;
    if (x < 0.0f) x = 0.0f;
    n = x * (2.51f * x + 0.03f);
    d = x * (2.43f * x + 0.59f) + 0.14f;
    if (!(d > 1.0e-6f)) return 0.0f;
    return jw_clamp01(n / d);
}

// The diffuse half of the rig: ambient (hemispheric) + three lambert terms.
// Returns an irradiance to multiply an albedo by, NOT a final colour.
//
// Half-lambert is deliberately NOT used.  A gel body wants a real terminator
// on its shaded side -- that dark region IS the material reading as thick --
// and wrapping the lambert would wash it into the mid-tones, which is the
// single most common way translucency ends up looking like flat plastic.
static inline jw_rgb jw_irradiance(jw_vec3 n)
{
    float  sky = n.y * 0.5f + 0.5f;
    float  kd  = jw_dot(n, jw_key_dir());
    float  fd  = jw_dot(n, jw_fill_dir());
    float  rd  = jw_dot(n, jw_rim_dir());
    jw_rgb o;

    if (kd < 0.0f) kd = 0.0f;
    if (fd < 0.0f) fd = 0.0f;
    if (rd < 0.0f) rd = 0.0f;
    if (sky < 0.0f) sky = 0.0f;
    if (sky > 1.0f) sky = 1.0f;

    o.r = (JW_AMB_GND_R + (JW_AMB_SKY_R - JW_AMB_GND_R) * sky) * JW_AMB_I
        + JW_KEY_R * JW_KEY_I * kd + JW_FILL_R * JW_FILL_I * fd
        + JW_RIM_R * JW_RIM_I * rd;
    o.g = (JW_AMB_GND_G + (JW_AMB_SKY_G - JW_AMB_GND_G) * sky) * JW_AMB_I
        + JW_KEY_G * JW_KEY_I * kd + JW_FILL_G * JW_FILL_I * fd
        + JW_RIM_G * JW_RIM_I * rd;
    o.b = (JW_AMB_GND_B + (JW_AMB_SKY_B - JW_AMB_GND_B) * sky) * JW_AMB_I
        + JW_KEY_B * JW_KEY_I * kd + JW_FILL_B * JW_FILL_I * fd
        + JW_RIM_B * JW_RIM_I * rd;
    return o;
}

// The specular half: one lobe on the key light only.
//
// Only the key gets a highlight because only the key is a small bright source.
// Giving the fill and the rim their own lobes would put three wet highlights
// on a surface that should read as having one light on it, which is how a
// material stops looking like a material and starts looking like a shader.
static inline float jw_specular(jw_vec3 n, jw_vec3 eyev)
{
    jw_vec3 k = jw_key_dir();
    jw_vec3 h = jw_norm(jw_v3(k.x + eyev.x, k.y + eyev.y, k.z + eyev.z));
    float   d = jw_dot(n, h);
    if (d <= 0.0f) return 0.0f;
    return jw_lut(JW_SPEC_LUT, d) * JW_SPEC_I;
}

// The raw view-dependent grazing weight, 0..1 -- NOT a colour and not scaled
// by anything.  Separate from wave_gel.h's geometric rim: that one says "this
// vertex is on the rolled edge of the section", this one says "this vertex is
// turned away from the viewer", and a convincing edge needs both -- the first
// places the band, the second makes it respond as the ribbon twists.  See the
// Fresnel block above for why this is never added on its own.
static inline float jw_fresnel(jw_vec3 n, jw_vec3 eyev)
{
    float g = jw_dot(n, eyev);
    if (g < 0.0f) g = -g;
    return jw_lut(JW_FRESNEL_LUT, g);
}

// How lit a point is by the key alone, 0..1 -- the scalar wave_motes.h uses so
// a mote brightens on the same beat the ribbon's highlight does.  This is the
// correlation the file's opening comment is about; it is exposed rather than
// recomputed so there is exactly one definition of "lit" in the scene.
static inline float jw_key_lit(jw_vec3 n)
{
    float d = jw_dot(n, jw_key_dir());
    return (d > 0.0f) ? d : 0.0f;
}

// albedo * irradiance + specular + edge, tone-mapped, ready for a u8.
//
// `rim` is wave_gel.h's geometric rim weight for this vertex, 0 across the
// flat faces and ~0.9 on the rolled edges.  It gates the Fresnel edge term;
// passing 0 gives a surface with a highlight but no edge response, which is
// what the flat faces want.
static inline jw_rgb jw_shade(jw_rgb albedo, jw_vec3 n, jw_vec3 eyev, float rim)
{
    jw_rgb irr = jw_irradiance(n);
    float  fres = jw_fresnel(n, eyev);
    float  sp  = jw_specular(n, eyev) * (1.0f + JW_FRES_SPEC * fres);
    float  fr  = JW_FRES_EDGE * fres * jw_clamp01(rim);
    jw_rgb o;

    // Specular and Fresnel both take the KEY's colour rather than the body's:
    // a highlight is the light, reflected, and tinting it with the material is
    // what makes gel look like coloured plastic.
    o.r = jw_tonemap((albedo.r * irr.r + (sp + fr) * JW_KEY_R) * JW_EXPOSURE);
    o.g = jw_tonemap((albedo.g * irr.g + (sp + fr) * JW_KEY_G) * JW_EXPOSURE);
    o.b = jw_tonemap((albedo.b * irr.b + (sp + fr) * JW_KEY_B) * JW_EXPOSURE);
    return o;
}

static inline unsigned char jw_u8(float v)
{
    v = jw_clamp01(v);
    return (unsigned char)(v * 255.0f + 0.5f);
}

#endif // WAVE_LIGHT_H
