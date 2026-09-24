// Host test for source/ui/render/wave_cam.h -- the JellyWave fixed camera.
//
// What this is actually guarding.  wave_cam.h hard-codes an orthonormal basis
// and a focal length that were SOLVED offline, not typed by hand, and the
// whole 3-D pipeline collapses onto them.  A single digit wrong in one of the
// nine basis components skews every vertex in a way that looks, on a TV, like
// the geometry is broken rather than the camera -- so the basis is checked
// here against the definition it came from (right = normalize(cross(fwd, up)),
// up = cross(right, fwd)) rather than against a copy of its own numbers.
//
// Compare against libm freely: the KERNEL may not use libm, the test may, and
// checking jw_rsqrt against the real sqrtf is a far better assertion than a
// table of expected values.

#include <stdio.h>
#include <math.h>
#include "wave_cam.h"

static int fails = 0;

static void ck(int cond, const char *what)
{
    if (!cond) { printf("  FAIL: %s\n", what); fails++; }
}

static void ck_near(float got, float want, float tol, const char *what)
{
    float d = got - want;
    if (d < 0.0f) d = -d;
    if (!(d <= tol)) {
        printf("  FAIL: %s (got %.7f want %.7f tol %.7f)\n", what, got, want, tol);
        fails++;
    }
}

// --- 1. the reciprocal square root ---------------------------------------
static void test_rsqrt(void)
{
    int   i;
    float worst = 0.0f;
    printf("1. jw_rsqrt vs libm\n");
    for (i = 1; i <= 200000; i++) {
        float x = (float)i * 0.001f;
        float g = jw_rsqrt(x);
        float w = 1.0f / sqrtf(x);
        float e = fabsf(g - w) / w;
        if (e > worst) worst = e;
    }
    printf("   worst relative error %.3e over x in [0.001, 200]\n", worst);
    ck(worst < 1.0e-6f, "jw_rsqrt within 1e-6 relative of libm");

    // Defined, finite answers for the degenerate inputs -- callers treat 0 as
    // "this vector had no length" and must never see a NaN.
    ck(jw_rsqrt(0.0f)  == 0.0f, "jw_rsqrt(0) is 0");
    ck(jw_rsqrt(-1.0f) == 0.0f, "jw_rsqrt(negative) is 0");
    ck(jw_rsqrt((float)NAN) == 0.0f, "jw_rsqrt(NaN) is 0");
}

// --- 2. the basis is orthonormal and matches its own derivation -----------
static void test_basis(void)
{
    jw_vec3 r = jw_right(), u = jw_up(), f = jw_fwd();
    jw_vec3 rr, uu;

    printf("2. camera basis\n");

    ck_near(jw_dot(r, r), 1.0f, 2.0e-5f, "right is unit length");
    ck_near(jw_dot(u, u), 1.0f, 2.0e-5f, "up is unit length");
    ck_near(jw_dot(f, f), 1.0f, 2.0e-5f, "fwd is unit length");

    ck_near(jw_dot(r, u), 0.0f, 2.0e-5f, "right . up = 0");
    ck_near(jw_dot(r, f), 0.0f, 2.0e-5f, "right . fwd = 0");
    ck_near(jw_dot(u, f), 0.0f, 2.0e-5f, "up . fwd = 0");

    // The derivation, re-run: right comes from crossing world up into fwd, and
    // up from crossing back.  This is what catches a transposed digit.
    rr = jw_norm(jw_cross(f, jw_v3(0.0f, 1.0f, 0.0f)));
    ck_near(fabsf(jw_dot(rr, r)), 1.0f, 2.0e-5f,
            "right is parallel to normalize(cross(fwd, worldup))");

    uu = jw_cross(r, f);
    ck_near(jw_dot(uu, u), 1.0f, 2.0e-5f, "up = cross(right, fwd)");

    // right has no y component by construction -- it is a horizontal axis, so
    // the horizon stays level.  A non-zero value here means the band would be
    // drawn rotated in the frame.
    ck_near(JW_RIGHT_Y, 0.0f, 1.0e-6f, "right.y is exactly 0");

    // 45-degree vertical field of view.
    ck_near(JW_FOCAL, 1.0f / tanf((float)(45.0 * M_PI / 180.0) * 0.5f), 1.0e-5f,
            "focal = 1/tan(fov/2) for fov = 45 degrees");

    // The camera looks DOWN slightly and toward -z.  If either sign flips the
    // whole scene is behind the viewer.
    ck(JW_FWD_Z < 0.0f, "camera looks toward -z");
    ck(JW_FWD_Y < 0.0f, "camera looks slightly downward");
    printf("   depression angle %.2f degrees\n",
           asinf(-JW_FWD_Y) * 180.0f / (float)M_PI);
}

// --- 3. projection ---------------------------------------------------------
static void test_project(void)
{
    jw_vec3 eye = jw_eye();
    jw_proj p;
    float   aspect = 16.0f / 9.0f;

    printf("3. projection\n");

    // A point straight ahead lands at the centre of the frame.
    {
        jw_vec3 ahead = jw_v3(eye.x + jw_fwd().x * 10.0f,
                              eye.y + jw_fwd().y * 10.0f,
                              eye.z + jw_fwd().z * 10.0f);
        p = jw_project(ahead, aspect);
        ck(p.ok, "a point 10 units ahead projects");
        ck_near(p.x, 0.0f, 1.0e-4f, "  ... to x = 0");
        ck_near(p.y, 0.0f, 1.0e-4f, "  ... to y = 0");
        ck_near(p.z, 10.0f, 1.0e-3f, "  ... at view depth 10");
    }

    // Offsetting along `up` moves it UP the frame: +y is up, which is
    // ui_wave.cpp's clip-space convention and the opposite of a screen row.
    {
        jw_vec3 a = jw_v3(eye.x + jw_fwd().x * 10.0f + jw_up().x,
                          eye.y + jw_fwd().y * 10.0f + jw_up().y,
                          eye.z + jw_fwd().z * 10.0f + jw_up().z);
        p = jw_project(a, aspect);
        ck(p.ok && p.y > 0.0f, "+up projects to +y");
    }
    {
        jw_vec3 a = jw_v3(eye.x + jw_fwd().x * 10.0f + jw_right().x,
                          eye.y + jw_fwd().y * 10.0f + jw_right().y,
                          eye.z + jw_fwd().z * 10.0f + jw_right().z);
        p = jw_project(a, aspect);
        ck(p.ok && p.x > 0.0f, "+right projects to +x");
    }

    // A wider frame puts the same point closer to the centre horizontally and
    // leaves its height alone -- the vertical field is what fov fixes.
    {
        jw_vec3 a = jw_v3(eye.x + jw_fwd().x * 10.0f + jw_right().x,
                          eye.y + jw_fwd().y * 10.0f + jw_right().y,
                          eye.z + jw_fwd().z * 10.0f + jw_right().z);
        jw_proj w = jw_project(a, 16.0f / 9.0f);
        jw_proj n = jw_project(a, 4.0f / 3.0f);
        ck(w.x < n.x, "a wider aspect pulls x toward the centre");
        ck_near(w.y, n.y, 1.0e-6f, "aspect does not change y");
    }

    // Behind the camera, and at the guard plane.
    {
        jw_vec3 behind = jw_v3(eye.x - jw_fwd().x * 5.0f,
                               eye.y - jw_fwd().y * 5.0f,
                               eye.z - jw_fwd().z * 5.0f);
        p = jw_project(behind, aspect);
        ck(!p.ok, "a point behind the camera is rejected");
        ck(p.x == 0.0f && p.y == 0.0f, "  ... and returns zeroed x/y");
        ck(p.z < 0.0f, "  ... but still reports its (negative) depth");
    }
    {
        p = jw_project(jw_v3((float)NAN, 0.0f, 0.0f), aspect);
        ck(!p.ok, "a NaN point is rejected rather than propagated");
    }
    {
        jw_vec3 ahead = jw_v3(eye.x + jw_fwd().x * 10.0f,
                              eye.y + jw_fwd().y * 10.0f,
                              eye.z + jw_fwd().z * 10.0f);
        p = jw_project(ahead, 0.0f);
        ck(!p.ok, "a degenerate aspect is rejected");
    }
}

// --- 4. the eye vector -----------------------------------------------------
static void test_eyevec(void)
{
    jw_vec3 eye = jw_eye();
    jw_vec3 p   = jw_v3(0.0f, 0.0f, 0.0f);
    jw_vec3 e   = jw_eyevec(p);

    printf("4. eye vector\n");
    ck_near(jw_dot(e, e), 1.0f, 2.0e-5f, "eyevec is unit length");

    // It points FROM the surface TOWARD the camera, so it agrees in sign with
    // (eye - p).  Getting this backwards inverts every Fresnel term.
    {
        jw_vec3 d = jw_sub(eye, p);
        ck(jw_dot(e, d) > 0.0f, "eyevec points toward the camera");
    }
    // Degenerate: a point exactly at the eye has no direction; the contract is
    // a zero vector, not a NaN.
    {
        jw_vec3 z = jw_eyevec(eye);
        ck(z.x == 0.0f && z.y == 0.0f && z.z == 0.0f,
           "a point at the eye yields a zero vector, not NaN");
    }
}

int main(void)
{
    printf("test_wave_cam\n");
    test_rsqrt();
    test_basis();
    test_project();
    test_eyevec();
    if (fails) { printf("FAILED (%d)\n", fails); return 1; }
    printf("OK\n");
    return 0;
}
