// Host test for source/ui/render/wave_light.h -- the JellyWave light rig.
//
// The two LUTs are LITERAL tables in the header, because generating them at
// startup would be file-scope dynamic initialisation and this target does not
// want any.  The cost of that choice is that a hand-edit can silently drift
// from the curve the table is supposed to be, so this test RE-DERIVES both
// from their documented formulas with libm and compares.  That is the point of
// the file; everything else here is bounds and monotonicity.
//
// The shapes matter more than the values, and the assertions say which:
//
//   The Fresnel table must be FLAT at its grazing end.  A flat top is what
//   makes a translucent edge read as rounded and inflated; a pow() curve
//   feathers instead, which reads as glow around a flat ribbon.  That was the
//   measured finding the table exists to honour, so it is asserted rather than
//   left to the formula.
//
//   The specular table must be a broad ramp PLUS a narrow core, and the core
//   must live in the last few samples.  One pow() cannot be both, which is the
//   whole argument for a table over an exponent.

#include <stdio.h>
#include <math.h>
#include "wave_light.h"

static int fails = 0;

static void ck(int cond, const char *what)
{
    if (!cond) { printf("  FAIL: %s\n", what); fails++; }
}

static void ck_near(float got, float want, float tol, const char *what)
{
    float d = fabsf(got - want);
    if (!(d <= tol)) {
        printf("  FAIL: %s (got %.7f want %.7f tol %.7f)\n", what, got, want, tol);
        fails++;
    }
}

// --- 1. the tables match their formulas ----------------------------------
static void test_lut_formulas(void)
{
    int   i;
    float worst_f = 0.0f, worst_s = 0.0f;

    printf("1. LUTs against their generating formulas\n");

    for (i = 0; i < JW_LUT_N; i++) {
        float g = (float)i / (float)(JW_LUT_N - 1);
        float want, d;

        // f(g) = 1 for g <= 0.12, else (1 - (g-0.12)/0.88)^4
        if (g <= 0.12f) want = 1.0f;
        else { float t = (g - 0.12f) / 0.88f; want = powf(1.0f - t, 4.0f); }
        d = fabsf(JW_FRESNEL_LUT[i] - want);
        if (d > worst_f) worst_f = d;

        // s(h) = 0.06 + 0.24*h^2 + 0.95*h^48
        want = 0.06f + 0.24f * g * g + 0.95f * powf(g, 48.0f);
        d = fabsf(JW_SPEC_LUT[i] - want);
        if (d > worst_s) worst_s = d;
    }
    printf("   worst deviation: fresnel %.3e, specular %.3e\n", worst_f, worst_s);
    ck(worst_f < 1.0e-5f, "JW_FRESNEL_LUT matches (1-(g-0.12)/0.88)^4");
    ck(worst_s < 1.0e-5f, "JW_SPEC_LUT matches 0.06 + 0.24h^2 + 0.95h^48");
}

// --- 2. the shapes the tables exist for ----------------------------------
static void test_lut_shapes(void)
{
    int   i, flat = 0, core = 0;
    float rise_low, rise_high;

    printf("2. LUT shapes\n");

    // FLAT TOP: several leading entries at exactly 1, not merely near it.
    for (i = 0; i < JW_LUT_N && JW_FRESNEL_LUT[i] >= 1.0f; i++) flat++;
    printf("   fresnel flat-top entries: %d of %d\n", flat, JW_LUT_N);
    ck(flat >= 3, "the Fresnel curve has a genuine flat top, not a feathered peak");
    ck(JW_FRESNEL_LUT[0] == 1.0f, "Fresnel saturates at full grazing");

    // Monotone decreasing, and it actually reaches the floor.
    for (i = 1; i < JW_LUT_N; i++)
        ck(JW_FRESNEL_LUT[i] <= JW_FRESNEL_LUT[i - 1] + 1.0e-7f,
           "the Fresnel curve never rises");
    ck(JW_FRESNEL_LUT[JW_LUT_N - 1] < 1.0e-4f, "Fresnel falls to ~0 head-on");

    // Specular: monotone increasing, and the CORE is confined to the top.
    for (i = 1; i < JW_LUT_N; i++)
        ck(JW_SPEC_LUT[i] >= JW_SPEC_LUT[i - 1] - 1.0e-7f,
           "the specular curve never falls");

    // The broad ramp is what the curve does over its first 29/32; the core is
    // the last 3.  If the split stops being lopsided, it has become one pow().
    rise_low  = JW_SPEC_LUT[JW_LUT_N - 4] - JW_SPEC_LUT[0];
    rise_high = JW_SPEC_LUT[JW_LUT_N - 1] - JW_SPEC_LUT[JW_LUT_N - 4];
    for (i = 0; i < JW_LUT_N; i++) if (JW_SPEC_LUT[i] > 0.35f) { core++; }
    printf("   specular rise: %.3f over the first %d samples, "
           "%.3f over the last 3\n", rise_low, JW_LUT_N - 4, rise_high);
    printf("   samples above 0.35: %d\n", core);
    ck(rise_high > rise_low * 3.0f,
       "the specular core is narrow -- most of the rise is in the last 3 samples");
    ck(core <= 3, "the wet highlight is confined to a few samples");
    ck(JW_SPEC_LUT[0] > 0.0f, "there is a sheen floor everywhere, not just a spike");
}

// --- 3. lookup -------------------------------------------------------------
static void test_lookup(void)
{
    int i;
    printf("3. jw_lut\n");

    // Exact at the sample points.
    for (i = 0; i < JW_LUT_N; i++) {
        float x = (float)i / (float)(JW_LUT_N - 1);
        ck_near(jw_lut(JW_FRESNEL_LUT, x), JW_FRESNEL_LUT[i], 1.0e-5f,
                "lookup is exact at a sample point");
    }
    // Midpoints interpolate.
    {
        float x = 0.5f / (float)(JW_LUT_N - 1);
        ck_near(jw_lut(JW_FRESNEL_LUT, x),
                0.5f * (JW_FRESNEL_LUT[0] + JW_FRESNEL_LUT[1]), 1.0e-5f,
                "lookup interpolates linearly between samples");
    }
    // Clamped, not read off the end.
    ck(jw_lut(JW_SPEC_LUT, -5.0f) == JW_SPEC_LUT[0], "lookup clamps below 0");
    ck(jw_lut(JW_SPEC_LUT,  5.0f) == JW_SPEC_LUT[JW_LUT_N - 1], "lookup clamps above 1");
    ck(jw_lut(JW_SPEC_LUT, (float)NAN) == JW_SPEC_LUT[0], "NaN in, defined out");
}

// --- 4. tone mapping -------------------------------------------------------
static void test_tonemap(void)
{
    int   i;
    float prev = -1.0f;

    printf("4. tone map\n");
    ck(jw_tonemap(0.0f) == 0.0f, "black maps to black");
    ck(jw_tonemap(-1.0f) == 0.0f, "negative input maps to black");
    ck(jw_tonemap((float)NAN) == 0.0f, "NaN in, defined out");

    for (i = 0; i <= 2000; i++) {
        float x = (float)i * 0.01f;
        float y = jw_tonemap(x);
        ck(y >= 0.0f && y <= 1.0f, "tone map output stays in [0,1]");
        ck(y >= prev - 1.0e-6f, "tone map is monotone");
        prev = y;
    }

    // A DARK TOE.  This is why ACES rather than a Reinhard shoulder: the gel's
    // violet-black underside has to stay black instead of being lifted into
    // grey.
    //
    // The toe is the BOTTOM of the range, not the low mid-tones -- this curve
    // crosses the identity at about 0.06 and sits slightly above it from there
    // to the shoulder, which is what ACES is supposed to do and which an
    // earlier version of this test wrongly asserted away by checking f(0.10).
    // What matters for this material is that deep values are pushed deeper.
    printf("   toe: f(0.02)=%.4f f(0.05)=%.4f f(0.10)=%.4f f(0.20)=%.4f\n",
           jw_tonemap(0.02f), jw_tonemap(0.05f),
           jw_tonemap(0.10f), jw_tonemap(0.20f));
    ck(jw_tonemap(0.02f) < 0.02f * 0.75f, "deep shadows are pushed deeper");
    ck(jw_tonemap(0.05f) < 0.05f, "the toe darkens below about 0.06");
    ck(jw_tonemap(1.0f) > 0.70f, "mid-to-high values still reach a bright result");
    // It approaches but never reaches 1, so a highlight rolls off instead of
    // flat-topping into a white blob.
    ck(jw_tonemap(100.0f) <= 1.0f, "an extreme value is bounded");
}

// --- 5. the rig ------------------------------------------------------------
static void test_rig(void)
{
    jw_vec3 k = jw_key_dir(), f = jw_fill_dir(), r = jw_rim_dir();
    int     i, j;

    printf("5. light rig\n");

    ck_near(jw_dot(k, k), 1.0f, 2.0e-5f, "key direction is unit length");
    ck_near(jw_dot(f, f), 1.0f, 2.0e-5f, "fill direction is unit length");
    ck_near(jw_dot(r, r), 1.0f, 2.0e-5f, "rim direction is unit length");

    // They are the design's own light positions, normalised.  A transposed
    // digit here changes where every highlight sits.
    {
        float l = sqrtf(9.0f * 9.0f + 7.5f * 7.5f + 9.0f * 9.0f);
        ck_near(k.x,  9.0f / l, 1.0e-5f, "key matches (9, 7.5, -9) normalised");
        ck_near(k.y,  7.5f / l, 1.0e-5f, "  ...");
        ck_near(k.z, -9.0f / l, 1.0e-5f, "  ...");
    }
    // The key is above and BEHIND: that is what makes the far rolled edge the
    // bright one, which is the design's signature backlit read.
    ck(k.y > 0.0f, "the key light is above the object");
    ck(k.z < 0.0f, "the key light is behind the object");

    // Ratios preserved from the design's 3.4 : 0.75 : 0.9.
    ck_near(JW_FILL_I / JW_KEY_I, 0.75f / 3.4f, 1.0e-4f, "fill:key ratio preserved");
    ck_near(JW_RIM_I  / JW_KEY_I, 0.90f / 3.4f, 1.0e-4f, "rim:key ratio preserved");

    // Irradiance is finite, non-negative and bounded over the whole sphere of
    // normals -- there is no orientation that produces a negative or runaway
    // light value for the loft to bake into a vertex.
    for (i = 0; i <= 40; i++) {
        for (j = 0; j <= 80; j++) {
            float th = (float)i / 40.0f * (float)M_PI;
            float ph = (float)j / 80.0f * 2.0f * (float)M_PI;
            jw_vec3 n = jw_v3(sinf(th) * cosf(ph), cosf(th), sinf(th) * sinf(ph));
            jw_rgb  o = jw_irradiance(n);
            jw_rgb  s;
            float   lit = jw_key_lit(n);
            ck(o.r >= 0.0f && o.g >= 0.0f && o.b >= 0.0f, "irradiance is non-negative");
            ck(o.r < 4.0f && o.g < 4.0f && o.b < 4.0f, "irradiance is bounded");
            ck(o.r == o.r && o.g == o.g && o.b == o.b, "irradiance is finite");
            ck(lit >= 0.0f && lit <= 1.0f, "key_lit is a 0..1 weight");

            // A full shade, with a mid albedo, always lands in gamut.
            s = jw_shade(jw_col(0.5f, 0.4f, 0.7f), n, jw_v3(0.0f, 0.0f, 1.0f), 0.9f);
            ck(s.r >= 0.0f && s.r <= 1.0f, "shade stays in [0,1]");
            ck(s.g >= 0.0f && s.g <= 1.0f, "shade stays in [0,1]");
            ck(s.b >= 0.0f && s.b <= 1.0f, "shade stays in [0,1]");
        }
    }

    // The ambient hemisphere actually differs top from bottom; if the two ends
    // collapse the depth cue the split exists for is gone.
    {
        jw_rgb up = jw_irradiance(jw_v3(0.0f,  1.0f, 0.0f));
        jw_rgb dn = jw_irradiance(jw_v3(0.0f, -1.0f, 0.0f));
        printf("   irradiance up  (%.3f %.3f %.3f)\n", up.r, up.g, up.b);
        printf("   irradiance down(%.3f %.3f %.3f)\n", dn.r, dn.g, dn.b);
        ck(up.b != dn.b, "the ambient hemisphere is not flat");
        // Facing down picks up the blue rim light; facing up does not.  That
        // is what keeps the underside translucent rather than merely shadowed.
        ck(dn.b > 0.0f, "the underside receives the blue rim light");
    }
}

// --- 6. Fresnel is a weight, not a glow ----------------------------------
static void test_fresnel_shape(void)
{
    // ONE NORMAL, TWO EYE VECTORS.  Holding the normal fixed holds the
    // irradiance fixed, so every difference below is view-dependence and
    // nothing else.  An earlier version of this test varied the NORMAL
    // instead and concluded Fresnel was flooding the surface -- it was not;
    // the "grazing" normal it picked happened to point almost straight at the
    // key light, so what it measured was the key, not the view.
    jw_vec3 n     = jw_v3(0.0f, 1.0f, 0.0f);          // a flat top face
    jw_vec3 head  = jw_v3(0.0f, 1.0f, 0.0f);          // looking straight down it
    jw_vec3 graze = jw_norm(jw_v3(1.0f, 0.02f, 0.0f));// looking along it
    jw_rgb  alb   = jw_col(0.30f, 0.20f, 0.45f);
    jw_rgb  a, b;

    printf("6. Fresnel behaviour\n");

    ck(jw_fresnel(n, head)  < 0.05f, "Fresnel is near zero head-on");
    ck(jw_fresnel(n, graze) > 0.90f, "Fresnel saturates at grazing");

    // THE REGRESSION THIS GUARDS.  Fresnel used to be added straight into the
    // result, at 0.38 of the key colour.  On a band seen from 9 degrees above,
    // nearly every visible surface is at grazing incidence, so that lifted the
    // whole object by the same amount and inverted the design -- measured, the
    // lower body came out at luma 0.365 against the rolled edge's 0.325.
    // Fresnel is now a weight on the key's highlight plus an edge-GATED term,
    // so a flat face with no geometric rim must barely move with view angle.
    b = jw_shade(alb, n, head,  0.0f);
    a = jw_shade(alb, n, graze, 0.0f);
    printf("   flat face (green): head-on %.3f, grazing %.3f\n", b.g, a.g);
    ck(fabsf(a.g - b.g) < 0.12f,
       "a flat face barely changes with view angle -- Fresnel is not a glow");

    // But on the rolled edge it must respond strongly, or the edge stops
    // reading as an edge.
    {
        jw_rgb e = jw_shade(alb, n, graze, 0.9f);
        printf("   rolled edge at grazing %.3f vs flat at grazing %.3f\n", e.g, a.g);
        ck(e.g > a.g + 0.15f, "the geometric rim gets a real grazing response");
    }
}

int main(void)
{
    printf("test_wave_light\n");
    test_lut_formulas();
    test_lut_shapes();
    test_lookup();
    test_tonemap();
    test_rig();
    test_fresnel_shape();
    if (fails) { printf("FAILED (%d)\n", fails); return 1; }
    printf("OK\n");
    return 0;
}
