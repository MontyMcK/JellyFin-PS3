// Host test for source/ui/render/wave_gel.h -- the JellyWave lofted body.
//
// This runs the REAL solver (wave_field.h) into the REAL loft, so what is
// checked is the integration the console runs rather than a stand-in for it.
//
// Four of these assertions are load-bearing rather than hygienic, and it is
// worth saying which:
//
//   THE SECTION MUST BE CONVEX.  wave_gel.h draws twelve strips with no depth
//   buffer and orders them by mean view depth.  A painter's sort is only exact
//   when primitives cannot mutually overlap, and what guarantees that here is
//   convexity of the swept section -- along any view ray a convex tube is hit
//   at most twice, on opposite sides.  If someone later adds a section point
//   that dents the profile, the sort silently stops being correct and the
//   ribbon starts showing its own far side through its near side on a TV.
//   test_section_convex() is the guard.
//
//   THE FRAMING BOX.  wave_cam.h's pan was solved to put the band low and
//   diagonal.  Nothing in the source states where it lands -- it emerges from
//   the camera, the spans and the spine together -- so the box is asserted
//   here.  A change to any of the three that moves the band back into the card
//   grid fails here instead of on a TV.
//
//   THE BAND MUST LEAVE THE SCREEN.  The design is a floating hero object that
//   stops in mid-air at x = +/-0.79.  For a background that is a bug, so the
//   spans were extended; this checks all three layers still overrun both edges
//   at 16:9 AND at 4:3, because the spans were solved at one aspect only.
//
//   THE ROLLED EDGE MUST HAVE ON-SCREEN THICKNESS.  The entire point of the
//   exercise is that the band reads as a three-dimensional object.  If the
//   projected distance across the rolled edge collapses, it is a flat ribbon
//   again whatever the shading says.

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "wave_field.h"
#include "wave_gel.h"

static int fails = 0;

static void ck(int cond, const char *what)
{
    if (!cond) { printf("  FAIL: %s\n", what); fails++; }
}

static wf_field  s_field;
static jw_vert   s_v[JW_VERTS];

static const char *LNAME[JW_LAYERS] = { "near", "mid", "far" };

static int finite_f(float v) { return (v == v) && (v > -1.0e30f) && (v < 1.0e30f); }

// --- 0. the perf refactor is an identity, not an approximation -------------
//
// wave_gel.h replaced jw_frame_at's finite-difference tangent with the exact
// analytic derivative (jw_spine_tangent), and jw_lump's per-vertex trig with a
// precomputed angle-sum table (jw_lump_fast).  Both are claimed to be
// mathematical identities of what the old code computed, not new
// approximations of their own.  This section proves that claim by
// reconstructing the OLD path -- old-style finite-difference tangent, and the
// retained jw_lump() -- entirely from functions wave_gel.h still exports
// unchanged (jw_spine, jw_sub, jw_cross, jw_norm, jw_roll, jw_lump), and
// diffing every finished vertex against what jw_build_layer (the NEW path)
// actually produces.  If a future edit lets the two drift, this is what
// catches it -- not a TV.

// The exact old jw_frame_at: tangent from a central difference of jw_spine at
// u -/+ JW_DIFF_E, both samples using the SAME disp reading (da = db = d0),
// which is exactly the finite-difference call jw_build_layer used to make.
static inline jw_frame old_frame_at(float u, float disp, const jw_layer *L)
{
    jw_frame f;
    jw_vec3  a = jw_spine(u - JW_DIFF_E, disp, L);
    jw_vec3  b = jw_spine(u + JW_DIFF_E, disp, L);
    jw_vec3  t = jw_norm(jw_sub(b, a));
    jw_vec3  s = jw_norm(jw_cross(jw_v3(0.0f, 1.0f, 0.0f), t));
    jw_vec3  up = jw_norm(jw_cross(t, s));
    float    r  = jw_roll(u, L), cs = jw_cosf(r), sn = wk_sinf(r);

    f.p  = jw_spine(u, disp, L);
    f.s  = jw_v3(s.x * cs + up.x * sn, s.y * cs + up.y * sn, s.z * cs + up.z * sn);
    f.up = jw_v3(up.x * cs - s.x * sn, up.y * cs - s.y * sn, up.z * cs - s.z * sn);
    return f;
}

// The exact old jw_build_layer, verbatim except for calling old_frame_at and
// jw_lump instead of jw_frame_at and jw_lump_fast.  Any structural change to
// the real jw_build_layer that isn't ALSO one of the two claimed identities
// will show up here as a diff against it, which is the point.
static int old_build_layer(const jw_layer *L, const float *disp, int ndisp,
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
        float    d0;
        {
            float s = uk * (float)(ndisp - 1);
            int   k = (int)s;
            float fr;
            if (k < 0) k = 0;
            if (k > ndisp - 2) k = ndisp - 2;
            fr = s - (float)k;
            d0 = (disp[k] + (disp[k + 1] - disp[k]) * fr) * L->disp_gain;
        }

        f  = old_frame_at(u, d0, L);
        hw = jw_half_w(u, L);
        ht = jw_half_t(u, L);
        st = hw - ht;
        if (st < 1.0e-4f) st = 1.0e-4f;

        for (j = 0; j < JW_SECTION; j++) {
            float   cx  = JW_SEC_AST[j] * st + JW_SEC_AHT[j] * ht;
            float   cy  = JW_SEC_BHT[j] * ht;
            float   rim = JW_SEC_RIM[j];
            float   th  = ((float)j / (float)JW_SECTION) * WK_TWO_PI;
            float   nx  = cx / hw, ny = cy / ht;
            jw_vec3 n, p, ev;
            jw_rgb  alb, lit_c, rimc;
            jw_proj pr;
            float   d, lit;
            jw_vert *v = &out[i * JW_SECTION + j];

            n = jw_norm(jw_v3(f.s.x * nx + f.up.x * ny,
                              f.s.y * nx + f.up.y * ny,
                              f.s.z * nx + f.up.z * ny));

            d = jw_lump(u, th, L) * (0.55f + 1.9f * ht);

            p = jw_v3(f.p.x + f.s.x * cx + f.up.x * cy + n.x * d,
                      f.p.y + f.s.y * cx + f.up.y * cy + n.y * d,
                      f.p.z + f.s.z * cx + f.up.z * cy + n.z * d);

            ev  = jw_eyevec(p);
            alb = jw_gel_color(p, ny, rim, L);
            lit_c = jw_shade(alb, n, ev, rim);

            lit = jw_key_lit(n);
            lit = lit * (0.8f + 0.2f * lit);
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

static void test_tangent_matches_finite_difference(void)
{
    int l, i;
    float worst = 0.0f;

    printf("0a. analytic tangent matches the finite difference it replaced\n");
    for (l = 0; l < JW_LAYERS; l++) {
        const jw_layer *L = &JW_LAYER[l];
        for (i = 0; i < JW_STATIONS; i++) {
            float   u    = L->u0 + (L->u1 - L->u0) * (float)i
                         / (float)(JW_STATIONS - 1);
            float   disp = 0.3f;   /* representative, non-zero */
            jw_vec3 a  = jw_spine(u - JW_DIFF_E, disp, L);
            jw_vec3 b  = jw_spine(u + JW_DIFF_E, disp, L);
            jw_vec3 t_old = jw_norm(jw_sub(b, a));
            jw_vec3 t_new = jw_norm(jw_spine_tangent(u, L));
            float   dx = fabsf(t_old.x - t_new.x);
            float   dy = fabsf(t_old.y - t_new.y);
            float   dz = fabsf(t_old.z - t_new.z);
            float   d  = dx > dy ? (dx > dz ? dx : dz) : (dy > dz ? dy : dz);
            if (d > worst) worst = d;
        }
    }
    printf("   worst component difference: %.8f\n", worst);
    // The finite difference's own truncation error at JW_DIFF_E = 0.0015 is
    // O(e^2) against the true derivative -- a couple of orders of magnitude
    // below single-float epsilon at this scale, so a tight bound here is a
    // bound on the FD's OWN error, not a tolerance picked to pass.
    ck(worst < 1.0e-4f,
       "jw_spine_tangent matches the old finite-difference tangent");
}

static void test_lump_matches_reference(void)
{
    int l, i, j;
    float worst = 0.0f;

    printf("0b. jw_lump_fast matches jw_lump\n");
    for (l = 0; l < JW_LAYERS; l++) {
        const jw_layer *L = &JW_LAYER[l];
        for (i = 0; i < JW_STATIONS; i++) {
            float u = L->u0 + (L->u1 - L->u0) * (float)i
                    / (float)(JW_STATIONS - 1);
            float lumpA1 = JW_LUMP_K1 * u * WK_PI + L->phase;
            float lumpA2 = JW_LUMP_K2 * u * WK_PI + L->phase;
            float sinA1 = wk_sinf(lumpA1), cosA1 = jw_cosf(lumpA1);
            float sinA2 = wk_sinf(lumpA2), cosA2 = jw_cosf(lumpA2);
            for (j = 0; j < JW_SECTION; j++) {
                float th  = ((float)j / (float)JW_SECTION) * WK_TWO_PI;
                float old = jw_lump(u, th, L);
                float neu = jw_lump_fast(sinA1, cosA1, sinA2, cosA2, j);
                float d   = fabsf(old - neu);
                if (d > worst) worst = d;
            }
        }
    }
    printf("   worst difference: %.8f\n", worst);
    // Pure algebraic identity (sin(A+B) split) -- should match to a few ULPs
    // of float reassociation error, not an approximation tolerance.
    ck(worst < 4.0e-5f, "jw_lump_fast is the same function as jw_lump");
}

static jw_vert s_old_v[JW_VERTS];

static void test_full_vertex_positions_match(void)
{
    int l, i;
    float worst_xy = 0.0f;
    int   worst_col = 0;

    printf("0c. jw_build_layer matches the pre-refactor reference, vertex for vertex\n");
    wf_init(&s_field, 0);
    for (i = 0; i < 90; i++) wf_step(&s_field, 1.25f, 0.02f, 1.0f);

    for (l = 0; l < JW_LAYERS; l++) {
        int n_new = jw_build_layer(&JW_LAYER[l], s_field.sy[l], WF_SAMPLES,
                                   16.0f / 9.0f, s_v, JW_VERTS);
        int n_old = old_build_layer(&JW_LAYER[l], s_field.sy[l], WF_SAMPLES,
                                    16.0f / 9.0f, s_old_v, JW_VERTS);
        ck(n_new == JW_VERTS && n_old == JW_VERTS, "both paths build a full layer");
        if (n_new != JW_VERTS || n_old != JW_VERTS) continue;

        for (i = 0; i < JW_VERTS; i++) {
            float dx = fabsf(s_v[i].x - s_old_v[i].x);
            float dy = fabsf(s_v[i].y - s_old_v[i].y);
            int   dc = abs((int)s_v[i].r - (int)s_old_v[i].r)
                     + abs((int)s_v[i].g - (int)s_old_v[i].g)
                     + abs((int)s_v[i].b - (int)s_old_v[i].b)
                     + abs((int)s_v[i].rr - (int)s_old_v[i].rr)
                     + abs((int)s_v[i].rg - (int)s_old_v[i].rg)
                     + abs((int)s_v[i].rb - (int)s_old_v[i].rb);
            if (dx > worst_xy) worst_xy = dx;
            if (dy > worst_xy) worst_xy = dy;
            if (dc > worst_col) worst_col = dc;
            ck(s_v[i].ok == s_old_v[i].ok, "on/off-screen agrees old vs new");
        }
    }
    printf("   worst clip-space coordinate difference: %.8f\n", worst_xy);
    printf("   worst summed u8 colour-channel difference: %d\n", worst_col);
    ck(worst_xy < 1.0e-4f, "vertex positions match the pre-refactor reference");
    ck(worst_col <= 1, "vertex colours match the pre-refactor reference "
                       "(<=1 for u8 rounding)");
}

// --- 1. the section --------------------------------------------------------
static void test_section_convex(void)
{
    // A representative taper: the near layer at u = 0.
    const float hw = JW_HW0, ht = JW_HT0;
    const float st = hw - ht;
    float cx[JW_SECTION], cy[JW_SECTION];
    int   j, pos = 0, neg = 0;

    printf("1. section\n");

    for (j = 0; j < JW_SECTION; j++) {
        cx[j] = JW_SEC_AST[j] * st + JW_SEC_AHT[j] * ht;
        cy[j] = JW_SEC_BHT[j] * ht;
        ck(finite_f(cx[j]) && finite_f(cy[j]), "section point is finite");
    }

    // Convexity: every consecutive edge turns the same way.
    for (j = 0; j < JW_SECTION; j++) {
        int   a = j, b = (j + 1) % JW_SECTION, c = (j + 2) % JW_SECTION;
        float e1x = cx[b] - cx[a], e1y = cy[b] - cy[a];
        float e2x = cx[c] - cx[b], e2y = cy[c] - cy[b];
        float z   = e1x * e2y - e1y * e2x;
        if (z >  1.0e-7f) pos++;
        if (z < -1.0e-7f) neg++;
    }
    printf("   turns: %d one way, %d the other\n", pos, neg);
    ck(pos == 0 || neg == 0,
       "the section is convex -- the painter's sort is exact only if it is");

    // The loop closes: the last point is adjacent to the first, not a jump
    // across the section.
    {
        float dx = cx[0] - cx[JW_SECTION - 1];
        float dy = cy[0] - cy[JW_SECTION - 1];
        float d  = sqrtf(dx * dx + dy * dy);
        printf("   closing edge length %.4f (section is %.2f x %.2f)\n",
               d, 2.0f * hw, 2.0f * ht);
        ck(d < hw, "the section loop closes without a jump");
    }

    // The rim band has a FLAT TOP: two adjacent points at the same weight, so
    // the strip between them is a band rather than a spike.  This is the whole
    // reason NC=4 rather than 3.
    {
        int pairs = 0;
        for (j = 0; j < JW_SECTION; j++) {
            int j2 = (j + 1) % JW_SECTION;
            if (JW_SEC_RIM[j] > 0.5f && JW_SEC_RIM[j2] > 0.5f) {
                pairs++;
                ck(fabsf(JW_SEC_RIM[j] - JW_SEC_RIM[j2]) < 1.0e-5f,
                   "the two rim points of a band carry equal weight");
            }
        }
        printf("   flat-topped rim bands: %d\n", pairs);
        ck(pairs == 2, "there are exactly two rim bands, one per rolled edge");
    }

    // Six of twelve strips carry rim; the rest would add nothing.
    {
        int n = 0;
        for (j = 0; j < JW_SECTION; j++) n += jw_strip_has_rim(j) ? 1 : 0;
        printf("   strips carrying rim: %d of %d\n", n, JW_SECTION);
        ck(n == 6, "six strips carry the rolled edge");
    }
}

// --- 2. finiteness, over time ---------------------------------------------
static void test_finite(void)
{
    int l, i, frame;
    int bad = 0, offscreen = 0, total = 0;

    printf("2. finiteness over 600 frames\n");
    wf_init(&s_field, 0);

    for (frame = 0; frame < 600; frame++) {
        wf_step(&s_field, 1.25f, 0.02f, 1.0f);
        for (l = 0; l < JW_LAYERS; l++) {
            int n = jw_build_layer(&JW_LAYER[l], s_field.sy[l], WF_SAMPLES,
                                   16.0f / 9.0f, s_v, JW_VERTS);
            if (n != JW_VERTS) { bad++; continue; }
            for (i = 0; i < JW_VERTS; i++) {
                total++;
                if (!finite_f(s_v[i].x) || !finite_f(s_v[i].y) ||
                    !finite_f(s_v[i].z)) bad++;
                if (!s_v[i].ok) offscreen++;
                // Clip space is only meaningful in [-1,1]; the renderer clamps,
                // but a vertex tens of units out means the projection broke.
                if (s_v[i].ok && (fabsf(s_v[i].x) > 8.0f ||
                                  fabsf(s_v[i].y) > 8.0f)) bad++;
            }
        }
    }
    printf("   %d vertices, %d non-finite/absurd, %d behind the camera\n",
           total, bad, offscreen);
    ck(bad == 0, "no vertex is ever non-finite or absurd");
    ck(offscreen == 0, "no vertex ever falls behind the camera");
}

// --- 3. the framing box ----------------------------------------------------
static void test_framing(void)
{
    // Measured bounds from the derivation in wave_cam.h, with a tolerance that
    // allows the solver's own wander (its displacement replaces the design's
    // two sines, so the spine moves) but not a systematic shift.
    static const float WANT_TOP[JW_LAYERS] = { -0.16f, -0.09f, -0.12f };
    int   l, i, frame;
    float lo[JW_LAYERS], hi[JW_LAYERS], xlo[JW_LAYERS], xhi[JW_LAYERS];

    printf("3. framing at 16:9\n");
    wf_init(&s_field, 0);
    for (l = 0; l < JW_LAYERS; l++) {
        lo[l] = 1.0e9f; hi[l] = -1.0e9f; xlo[l] = 1.0e9f; xhi[l] = -1.0e9f;
    }

    for (frame = 0; frame < 900; frame++) {
        wf_step(&s_field, 1.25f, 0.02f, 1.0f);
        for (l = 0; l < JW_LAYERS; l++) {
            jw_build_layer(&JW_LAYER[l], s_field.sy[l], WF_SAMPLES,
                           16.0f / 9.0f, s_v, JW_VERTS);
            for (i = 0; i < JW_VERTS; i++) {
                if (!s_v[i].ok) continue;
                if (s_v[i].y < lo[l])  lo[l]  = s_v[i].y;
                if (s_v[i].y > hi[l])  hi[l]  = s_v[i].y;
                if (s_v[i].x < xlo[l]) xlo[l] = s_v[i].x;
                if (s_v[i].x > xhi[l]) xhi[l] = s_v[i].x;
            }
        }
    }

    for (l = 0; l < JW_LAYERS; l++) {
        printf("   %-4s x [%+.2f,%+.2f]  y [%+.2f,%+.2f]\n",
               LNAME[l], xlo[l], xhi[l], lo[l], hi[l]);
        // THE BAND STAYS LOW.  This is decision (b): the design's own framing
        // puts it across the middle of the screen, where the card grid lives.
        ck(hi[l] < WANT_TOP[l] + 0.16f,
           "the band's top edge stays below the card grid");
        ck(hi[l] < 0.0f, "no part of the band crosses the horizontal midline");
        // It must reach the bottom, or it floats.
        ck(lo[l] < -0.55f, "the band reaches well down the frame");
        // And it must leave the screen sideways rather than stopping in air.
        ck(xlo[l] <= -1.0f, "the band overruns the left edge");
        ck(xhi[l] >=  1.0f, "the band overruns the right edge");
    }
}

static void test_framing_aspect(void)
{
    // The spans were solved at 16:9.  4:3 widens the horizontal field, which
    // pulls the ends INWARD in clip space -- the case that would leave a
    // visible gap at the screen edge.
    int   l, i;
    printf("4. framing at 4:3 (spans were solved at 16:9)\n");
    wf_init(&s_field, 0);
    for (i = 0; i < 40; i++) wf_step(&s_field, 1.25f, 0.02f, 1.0f);

    for (l = 0; l < JW_LAYERS; l++) {
        float xlo = 1.0e9f, xhi = -1.0e9f;
        jw_build_layer(&JW_LAYER[l], s_field.sy[l], WF_SAMPLES,
                       4.0f / 3.0f, s_v, JW_VERTS);
        for (i = 0; i < JW_VERTS; i++) {
            if (!s_v[i].ok) continue;
            if (s_v[i].x < xlo) xlo = s_v[i].x;
            if (s_v[i].x > xhi) xhi = s_v[i].x;
        }
        printf("   %-4s x [%+.2f,%+.2f]\n", LNAME[l], xlo, xhi);
        ck(xlo <= -1.0f && xhi >= 1.0f, "the band still overruns both edges at 4:3");
    }
}

// --- 5. the object reads as three-dimensional -----------------------------
static void test_thickness(void)
{
    // The rolled edge is the strip between the two rim points of a cap.  Its
    // projected height is the visible thickness of the band -- the cue the
    // whole exercise exists to buy.  If it collapses, we have drawn a flat
    // ribbon with fancy colours on it.
    int   i, j, j2 = -1;
    float worst = 1.0e9f, best = 0.0f, acc = 0.0f;
    int   n = 0;

    printf("5. visible thickness of the rolled edge\n");
    wf_init(&s_field, 0);
    for (i = 0; i < 40; i++) wf_step(&s_field, 1.25f, 0.02f, 1.0f);

    for (j = 0; j < JW_SECTION; j++) {
        int k = (j + 1) % JW_SECTION;
        if (JW_SEC_RIM[j] > 0.5f && JW_SEC_RIM[k] > 0.5f) { j2 = j; break; }
    }
    ck(j2 >= 0, "found a rim band to measure");
    if (j2 < 0) return;

    jw_build_layer(&JW_LAYER[0], s_field.sy[0], WF_SAMPLES,
                   16.0f / 9.0f, s_v, JW_VERTS);
    for (i = 0; i < JW_STATIONS; i++) {
        const jw_vert *a = &s_v[i * JW_SECTION + j2];
        const jw_vert *b = &s_v[i * JW_SECTION + ((j2 + 1) % JW_SECTION)];
        float dx, dy, d;
        if (!a->ok || !b->ok) continue;
        dx = a->x - b->x; dy = a->y - b->y;
        d  = sqrtf(dx * dx + dy * dy);
        if (d < worst) worst = d;
        if (d > best)  best  = d;
        acc += d; n++;
    }
    printf("   near layer, clip-space width of the rim band: "
           "min %.4f mean %.4f max %.4f\n", worst, acc / (float)n, best);
    // At 1080p one clip-space unit is 540 rows, so 0.010 is about 5 rows --
    // the smallest band that still reads as an edge rather than as a seam.
    ck(worst > 0.010f, "the rolled edge is never thinner than ~5 rows at 1080p");
}

// --- 6. the painter's sort -------------------------------------------------
static void test_sort(void)
{
    int order[JW_SECTION];
    int seen[JW_SECTION];
    int i, j, l;

    printf("6. strip depth sort\n");
    wf_init(&s_field, 0);

    for (i = 0; i < 120; i++) {
        wf_step(&s_field, 1.25f, 0.02f, 1.0f);
        for (l = 0; l < JW_LAYERS; l++) {
            float mean[JW_SECTION];
            jw_build_layer(&JW_LAYER[l], s_field.sy[l], WF_SAMPLES,
                           16.0f / 9.0f, s_v, JW_VERTS);
            jw_strip_order(s_v, order);

            for (j = 0; j < JW_SECTION; j++) seen[j] = 0;
            for (j = 0; j < JW_SECTION; j++) {
                ck(order[j] >= 0 && order[j] < JW_SECTION, "order entry in range");
                if (order[j] >= 0 && order[j] < JW_SECTION) seen[order[j]]++;
            }
            for (j = 0; j < JW_SECTION; j++)
                ck(seen[j] == 1, "the order is a permutation, every strip once");

            // Recompute the means independently and check the order is
            // genuinely furthest-first.
            for (j = 0; j < JW_SECTION; j++) {
                float acc = 0.0f; int k, cnt = 0;
                int   jb = (j + 1) % JW_SECTION;
                for (k = 0; k < JW_STATIONS; k++) {
                    acc += s_v[k * JW_SECTION + j].z;
                    acc += s_v[k * JW_SECTION + jb].z;
                    cnt += 2;
                }
                mean[j] = acc / (float)cnt;
            }
            for (j = 1; j < JW_SECTION; j++)
                ck(mean[order[j - 1]] >= mean[order[j]] - 1.0e-6f,
                   "strips are ordered furthest first");
        }
    }
}

// --- 7. the material -------------------------------------------------------
static void test_material(void)
{
    // The design's identity: a dark violet-black body, a bright wet rolled
    // edge, and colour travelling along the brand axis from purple to blue.
    //
    // WHICH GEOMETRY IS VISIBLE, because getting this wrong made a correct
    // material look broken once already.  The first version of this test only
    // measured the half of the section that sorts NEAREST the camera, on the
    // assumption that a closed tube hides its far half.  That is true of a
    // tube seen end-on and false here: the band lies almost flat and the
    // camera is 9.25 degrees above it, so it is seen like a road.  Both long
    // edges are visible -- the far one ABOVE the top face on screen, the near
    // one below it -- and the only genuinely hidden surface is the bottom.
    //
    // That distinction matters because THE KEY LIGHT IS BEHIND THE OBJECT.  It
    // sits at (9, 7.5, -9) while the camera sits at +z, so the bright rolled
    // edge is the FAR one; restricting the measurement to the near half threw
    // the bright edge away and reported the rim as dimmer than the body.
    //
    // So: the bottom face (BHT < -0.9 on the strips that sort furthest back)
    // is excluded, and everything else counts.
    int   i, j;
    int   order[JW_SECTION], hidden[JW_SECTION];
    int   dark_n = 0, rim_n = 0, top_n = 0;
    float dark_acc = 0.0f, rim_acc = 0.0f, rim_max = 0.0f, top_acc = 0.0f;
    float vis_max = 0.0f, vis_min = 1.0e9f;
    float hue_left = 0.0f, hue_right = 0.0f;
    int   hl = 0, hr = 0;

    printf("7. material\n");
    wf_init(&s_field, 0);
    for (i = 0; i < 40; i++) wf_step(&s_field, 1.25f, 0.02f, 1.0f);
    jw_build_layer(&JW_LAYER[0], s_field.sy[0], WF_SAMPLES,
                   16.0f / 9.0f, s_v, JW_VERTS);

    // The bottom face: the underside points among the strips that sort to the
    // back.  These are painted over by the top face and never seen.
    jw_strip_order(s_v, order);
    for (j = 0; j < JW_SECTION; j++) hidden[j] = 0;
    for (j = 0; j < JW_SECTION / 2; j++)
        if (JW_SEC_BHT[order[j]] < -0.9f) hidden[order[j]] = 1;

    for (i = 0; i < JW_STATIONS; i++) {
        for (j = 0; j < JW_SECTION; j++) {
            const jw_vert *v = &s_v[i * JW_SECTION + j];
            float luma, rl;
            if (hidden[j]) continue;
            luma = (0.2126f * v->r + 0.7152f * v->g + 0.0722f * v->b)
                 * (1.0f / 255.0f);
            // The rolled edge's brightness on screen is the body colour PLUS
            // the additive rim pass, which is a separate draw.  Measuring only
            // the body understates it by most of its range.
            rl = luma + (0.2126f * v->rr + 0.7152f * v->rg + 0.0722f * v->rb)
                        * (1.0f / 255.0f);

            if (rl > vis_max) vis_max = rl;
            if (rl < vis_min) vis_min = rl;

            if (JW_SEC_BHT[j] < -0.4f) { dark_acc += luma; dark_n++; }
            if (JW_SEC_BHT[j] >  0.9f) { top_acc  += luma; top_n++;  }
            if (JW_SEC_RIM[j] > 0.5f)  {
                rim_acc += rl; rim_n++;
                if (rl > rim_max) rim_max = rl;
            }

            if (JW_SEC_BHT[j] > 0.9f) {
                float br = ((float)v->b - (float)v->r) * (1.0f / 255.0f);
                if (i < JW_STATIONS / 6)               { hue_left  += br; hl++; }
                if (i > JW_STATIONS - JW_STATIONS / 6) { hue_right += br; hr++; }
            }
        }
    }
    ck(dark_n > 0 && rim_n > 0 && top_n > 0 && hl > 0 && hr > 0,
       "sampled some of each region");
    if (!(dark_n && rim_n && top_n && hl && hr)) return;

    dark_acc /= (float)dark_n;
    rim_acc  /= (float)rim_n;
    top_acc  /= (float)top_n;
    hue_left  /= (float)hl;
    hue_right /= (float)hr;

    printf("   visible luma %.3f .. %.3f\n", vis_min, vis_max);
    printf("   lower body %.3f   top face %.3f   rolled edge mean %.3f peak %.3f\n",
           dark_acc, top_acc, rim_acc, rim_max);
    printf("   (blue-red) left %.3f, right %.3f\n", hue_left, hue_right);

    // The bands wave_light.h's JW_EXPOSURE is calibrated against.  These are
    // about the RANGE the material occupies: a translucent body that reads as
    // thick needs genuinely dark regions and genuinely bright ones in the same
    // object, and the failure mode being guarded against is everything
    // collapsing into one mid-tone.
    ck(dark_acc < 0.22f, "the lower body is a dark region");
    ck(top_acc > 0.40f && top_acc < 0.92f,
       "the top face sits in the upper mid-tones, lit but not blown out");
    ck(rim_max  > 0.75f, "the rolled edge reaches a genuinely bright peak");
    ck(vis_min < 0.12f,  "some visible part of the band is genuinely dark");
    ck(vis_max > 0.80f,  "some visible part of the band is genuinely bright");
    ck(rim_max > dark_acc * 4.0f,
       "there is real contrast between body and edge -- this is what stops it "
       "reading as flat plastic");

    // The brand axis runs purple upper-left to blue lower-right, and the band
    // descends to the right, so the right end must be bluer than the left.
    // This is the assertion that catches the axis being flipped or dropped.
    ck(hue_right > hue_left, "colour travels along the brand axis toward blue");

    // No orange, ever.  The source XMB wave is magenta and orange and this
    // design explicitly is not; red dominating green anywhere in the body
    // would mean the palette had drifted back toward it.
    {
        int warm = 0;
        for (i = 0; i < JW_VERTS; i++)
            if (s_v[i].r > s_v[i].g + 40 && s_v[i].g > s_v[i].b) warm++;
        printf("   warm (orange-ish) vertices: %d of %d\n", warm, JW_VERTS);
        ck(warm == 0, "no vertex is orange");
    }
}

// --- 8. determinism and degenerate input ----------------------------------
static void test_contract(void)
{
    static jw_vert a[JW_VERTS], b[JW_VERTS];
    int i, same = 1;

    printf("8. contract\n");
    wf_init(&s_field, 0);
    for (i = 0; i < 20; i++) wf_step(&s_field, 1.25f, 0.02f, 1.0f);

    jw_build_layer(&JW_LAYER[0], s_field.sy[0], WF_SAMPLES, 16.0f/9.0f, a, JW_VERTS);
    jw_build_layer(&JW_LAYER[0], s_field.sy[0], WF_SAMPLES, 16.0f/9.0f, b, JW_VERTS);
    for (i = 0; i < JW_VERTS; i++)
        if (a[i].x != b[i].x || a[i].y != b[i].y ||
            a[i].r != b[i].r || a[i].g != b[i].g || a[i].b != b[i].b) same = 0;
    ck(same, "the same input builds the same vertices");

    ck(jw_build_layer(NULL, s_field.sy[0], WF_SAMPLES, 1.0f, a, JW_VERTS) == 0,
       "a null layer is rejected");
    ck(jw_build_layer(&JW_LAYER[0], NULL, WF_SAMPLES, 1.0f, a, JW_VERTS) == 0,
       "a null displacement is rejected");
    ck(jw_build_layer(&JW_LAYER[0], s_field.sy[0], 1, 1.0f, a, JW_VERTS) == 0,
       "too few displacement samples is rejected");
    ck(jw_build_layer(&JW_LAYER[0], s_field.sy[0], WF_SAMPLES, 1.0f, a,
                      JW_VERTS - 1) == 0,
       "insufficient capacity is rejected");

    // THE UNDULATION GAIN, recomputed from wave_field.h's own constants
    // rather than compared against a copy of itself.  The literal table in
    // wave_gel.h exists so that header need not depend on wave_field.h; the
    // price is that the two can drift, and the symptom of drift is a wave that
    // is quietly too flat -- which is exactly what the first build did, at 55%
    // of the design's excursion, and which no other assertion here would
    // catch.
    for (i = 0; i < JW_LAYERS; i++) {
        float want = JW_DISP_PEAK / (WF_NOMINAL_PEAK * WF_DRIVE[i]);
        printf("   layer %d disp_gain %.4f (want %.4f)\n",
               i, JW_LAYER[i].disp_gain, want);
        ck(fabsf(JW_LAYER[i].disp_gain - want) < 5.0e-4f,
           "disp_gain = JW_DISP_PEAK / (WF_NOMINAL_PEAK * WF_DRIVE[l])");
    }

    // The design does NOT taper its undulation by layer -- only phase, offset,
    // scale and brightness differ.  So the gains must rise with layer index,
    // exactly undoing the solver's own per-layer calming.
    ck(JW_LAYER[0].disp_gain < JW_LAYER[1].disp_gain &&
       JW_LAYER[1].disp_gain < JW_LAYER[2].disp_gain,
       "the gains undo the solver's per-layer drive taper");

    // Layers are ordered near to far, which is the order ui_wave.cpp reverses
    // to draw back to front.  A table edited out of order would put the far
    // ribbon on top.
    ck(JW_LAYER[0].z_off > JW_LAYER[1].z_off &&
       JW_LAYER[1].z_off > JW_LAYER[2].z_off,
       "the layer table runs near to far");
    ck(JW_LAYER[0].alpha >= JW_LAYER[1].alpha &&
       JW_LAYER[1].alpha >= JW_LAYER[2].alpha,
       "further layers are no more opaque than nearer ones");
}

#ifdef JW_PROFILE
unsigned long g_wk_sinf_calls = 0;
unsigned long g_jw_rsqrt_calls = 0;

// Op-count sanity check for the perf refactor -- NOT a timing measurement.
// Real PPU cost is hardware-only (ui_wave.cpp's gen=/amort= log line); this
// only confirms the algorithmic call-count reduction the refactor claims.
static void profile_report(void)
{
    printf("9. JW_PROFILE op count (algorithmic check, not a timing proxy)\n");
    wf_init(&s_field, 0);
    for (int i = 0; i < 30; i++) wf_step(&s_field, 1.25f, 0.02f, 1.0f);

    g_wk_sinf_calls = 0;
    g_jw_rsqrt_calls = 0;
    jw_build_layer(&JW_LAYER[0], s_field.sy[0], WF_SAMPLES, 16.0f / 9.0f,
                   s_v, JW_VERTS);
    printf("   NEW path:  wk_sinf %.3f/vertex   jw_rsqrt %.3f/vertex\n",
           (double)g_wk_sinf_calls / JW_VERTS,
           (double)g_jw_rsqrt_calls / JW_VERTS);

    g_wk_sinf_calls = 0;
    g_jw_rsqrt_calls = 0;
    old_build_layer(&JW_LAYER[0], s_field.sy[0], WF_SAMPLES, 16.0f / 9.0f,
                    s_old_v, JW_VERTS);
    printf("   OLD path:  wk_sinf %.3f/vertex   jw_rsqrt %.3f/vertex\n",
           (double)g_wk_sinf_calls / JW_VERTS,
           (double)g_jw_rsqrt_calls / JW_VERTS);
}
#endif

int main(void)
{
    printf("test_wave_gel  (%d stations x %d section = %d vertices/layer)\n",
           JW_STATIONS, JW_SECTION, JW_VERTS);
    test_tangent_matches_finite_difference();
    test_lump_matches_reference();
    test_full_vertex_positions_match();
    test_section_convex();
    test_finite();
    test_framing();
    test_framing_aspect();
    test_thickness();
    test_sort();
    test_material();
    test_contract();
#ifdef JW_PROFILE
    profile_report();
#endif
    if (fails) { printf("FAILED (%d)\n", fails); return 1; }
    printf("OK\n");
    return 0;
}
