// JellyWave stage 0: the fixed camera.
//
//   wave_cam.h    world space -> clip space      (this file)
//   wave_light.h  the one shared light + its LUTs
//   wave_gel.h    the lofted translucent body
//
// WHY THERE IS A CAMERA AT ALL, AND WHY IT IS A CONSTANT
//
// The approved design (design-import-jellywave/jellyfin-ribbon.js) is a real
// 3-D object: a gel band lofted along an undulating spine, with a stadium
// cross-section, viewed from one fixed three-quarter camera.  Nothing in it
// orbits -- the hero framing is set once at the end of the file and never
// touched again.
//
// That single fact is what makes this affordable on RSX.  Because the camera
// never moves, the view-projection is a COMPILE-TIME CONSTANT, so the whole
// 3-D pipeline collapses to four dot products and one divide per vertex on the
// PPU.  There is no matrix upload, no vertex program change, no depth buffer
// and no new RSX binding: source/gfx/wave_shaders.h stays the two-instruction
// passthrough it has always been, and ui_wave.cpp keeps emitting exactly the
// WaveVert it already emits.  Every alternative -- a real MVP in the vertex
// program, a depth buffer, a widened vertex -- costs a change to something
// that is already proven on hardware, and buys nothing a fixed camera does not
// already give.
//
// HOW THE NUMBERS BELOW WERE DERIVED
//
// The design's own camera is
//
//     target = (0.1, -0.25, -0.9)
//     dir    = normalize(0.30, 0.17, 1.0)
//     eye    = target + dir * 12.6
//     fov    = 45 degrees vertical
//
// which frames the band across the MIDDLE of the screen -- measured, by
// projecting its spine at 16:9: ndc y in [-0.21, +0.27], with the body
// spanning [-0.42, +0.40].  That is right for a floating hero object and wrong
// for an XMB background, where the middle of the screen belongs to the card
// grid.
//
// So the camera is PANNED UP by JW_LIFT, carrying its target with it.  That is
// the one change made to the design's framing, and it was chosen over the
// alternatives for a specific reason:
//
//   IT IS A PURE PAN.  eye and target move together, so the view DIRECTION is
//   bit-for-bit unchanged.  The foreshortening of the top surface, the angle
//   the rolled edge is seen at, and therefore the entire three-dimensional
//   read of the object are exactly what the design shows.  Only the window
//   moves.  Moving the object down in world space instead would have tilted
//   the view onto it; shifting it down in clip space would have decoupled the
//   silhouette from the perspective.
//
// JW_LIFT is solved, not guessed: it is the pan that puts the near layer's
// spine at a mean clip-space y of -0.62, which sits just above the front
// ribbon's old crest (WAVE_BASEY[0] = 0.78 of screen height is ndc -0.56).
// The resulting framing, measured at 16:9 over the spans in wave_gel.h:
//
//     near  x [-1.10,+1.10]  spine y [-0.97,-0.37]  body y [-1.06,-0.16]
//     mid   x [-1.11,+1.11]  spine y [-0.82,-0.25]  body y [-0.88,-0.09]
//     far   x [-1.07,+1.12]  spine y [-0.73,-0.24]  body y [-0.78,-0.12]
//
// -- the band rises at the left, exits bottom-right, and its top edge stays
// below the horizontal midline.  tests/test_wave_cam.c asserts that box.
//
// House rules (.clinerules rule 7): header-only, pure C, no libm, no PS3
// headers, deterministic, caller-owned state, no globals.

#ifndef WAVE_CAM_H
#define WAVE_CAM_H

#include <stdint.h>
#include <string.h>     /* memcpy, for the rsqrt seed; not libm */

// --- the solved camera ----------------------------------------------------
//
// The basis is pre-normalised here rather than built at runtime from eye and
// target.  It is a constant, and a constant that is wrong by a rounding error
// in the last place is a constant that is right: what matters is that the
// three axes are orthonormal to float precision, which the derivation
// guarantees and test_wave_cam.c re-checks.
//
// JW_LIFT is recorded for the derivation's sake; nothing reads it, because it
// is already folded into JW_EYE_Y.
#define JW_LIFT        3.2568f

#define JW_EYE_X       3.673520f
#define JW_EYE_Y       5.031826f
#define JW_EYE_Z      11.011732f

// right = normalize(cross(fwd, world_up)), world_up = (0,1,0)
#define JW_RIGHT_X     0.957826f
#define JW_RIGHT_Y     0.000000f
#define JW_RIGHT_Z    -0.287348f

// up = cross(right, fwd)
#define JW_UP_X       -0.046181f
#define JW_UP_Y        0.987001f
#define JW_UP_Z       -0.153936f

// fwd = normalize(target - eye).  View-space z is measured along this, so it
// is POSITIVE in front of the camera.
#define JW_FWD_X      -0.283613f
#define JW_FWD_Y      -0.160714f
#define JW_FWD_Z      -0.945376f

// 1 / tan(fov/2) for the design's 45-degree vertical field of view.
#define JW_FOCAL       2.414214f

// Anything at or behind this view-space depth is treated as off-screen.  The
// design's near plane is 0.1 and the band never comes within 4 units of the
// camera, so this is a guard against a caller handing over a degenerate point,
// not a clip plane doing real work.
#define JW_MIN_Z       0.25f

typedef struct {
    float x, y, z;
} jw_vec3;

// Projected result.  `ok` is 0 when the point was at or behind JW_MIN_Z, in
// which case x and y are 0 and must not be used; z is always the view-space
// depth, which the painter's-algorithm sort in wave_gel.h needs even for a
// point it will not draw.
typedef struct {
    float x, y;     // clip space, [-1,+1], +y is UP -- ui_wave.cpp's convention
    float z;        // view-space depth, positive in front of the camera
    int   ok;
} jw_proj;

// 1/sqrt(x) without libm.  This is wave_ribbon.h's wr_rsqrt verbatim, and it
// is duplicated rather than included for one reason: wave_ribbon.h declares
// wr_vert, whose layout is a tested contract with ui_wave.cpp, and pulling
// that contract into every file that merely needs a reciprocal square root
// would make it look like a dependency of the camera.  Both copies are three
// Newton steps on the standard seed and both are checked against libm in their
// own test.
// Test-only call counter -- see wave_kernel.h's wk_sinf for the same pattern
// and why it does not cost the shipped build a global.
#ifdef JW_PROFILE
extern unsigned long g_jw_rsqrt_calls;
#endif

static inline float jw_rsqrt(float x)
{
    uint32_t i;
    float    h, y;
#ifdef JW_PROFILE
    g_jw_rsqrt_calls++;
#endif
    if (!(x > 0.0f)) return 0.0f;
    memcpy(&i, &x, sizeof i);
    i = 0x5f3759dfu - (i >> 1);
    memcpy(&y, &i, sizeof y);
    h = 0.5f * x;
    y = y * (1.5f - h * y * y);
    y = y * (1.5f - h * y * y);
    y = y * (1.5f - h * y * y);
    return y;
}

static inline jw_vec3 jw_v3(float x, float y, float z)
{
    jw_vec3 v; v.x = x; v.y = y; v.z = z; return v;
}

static inline float jw_dot(jw_vec3 a, jw_vec3 b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

static inline jw_vec3 jw_sub(jw_vec3 a, jw_vec3 b)
{
    return jw_v3(a.x - b.x, a.y - b.y, a.z - b.z);
}

static inline jw_vec3 jw_cross(jw_vec3 a, jw_vec3 b)
{
    return jw_v3(a.y * b.z - a.z * b.y,
                 a.z * b.x - a.x * b.z,
                 a.x * b.y - a.y * b.x);
}

// Normalise, returning (0,0,0) for a degenerate input so callers get a
// defined, finite vector rather than a NaN they will only notice on a TV.
static inline jw_vec3 jw_norm(jw_vec3 v)
{
    float inv = jw_rsqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    if (!(inv > 0.0f)) return jw_v3(0.0f, 0.0f, 0.0f);
    return jw_v3(v.x * inv, v.y * inv, v.z * inv);
}

static inline jw_vec3 jw_eye(void)   { return jw_v3(JW_EYE_X,   JW_EYE_Y,   JW_EYE_Z);   }
static inline jw_vec3 jw_right(void) { return jw_v3(JW_RIGHT_X, JW_RIGHT_Y, JW_RIGHT_Z); }
static inline jw_vec3 jw_up(void)    { return jw_v3(JW_UP_X,    JW_UP_Y,    JW_UP_Z);    }
static inline jw_vec3 jw_fwd(void)   { return jw_v3(JW_FWD_X,   JW_FWD_Y,   JW_FWD_Z);   }

// The unit vector from a world point back toward the camera.  Fresnel and the
// specular lobe both want this per vertex rather than a single view direction:
// the band is over two clip-space units wide, so the eye vector swings by
// about 25 degrees across it, and a constant view direction flattens exactly
// the grazing response the rolled edge exists to show.
static inline jw_vec3 jw_eyevec(jw_vec3 p)
{
    return jw_norm(jw_sub(jw_eye(), p));
}

// World -> clip.  `aspect` is display_width / display_height; the design's
// stage divides its horizontal field by the same ratio, so passing the live
// display aspect reproduces its framing at any resolution the client runs at.
//
// The divide is the only one in the vertex path and there is no reciprocal
// trick worth playing on it: the PPU's divide is pipelined, one per vertex is
// under two thousand a frame, and wr_rsqrt-style approximation of 1/z would
// put a visible wobble into the perspective.
static inline jw_proj jw_project(jw_vec3 p, float aspect)
{
    jw_proj  out;
    jw_vec3  r = jw_sub(p, jw_eye());
    float    z = jw_dot(r, jw_fwd());
    float    inv;

    out.x = 0.0f;
    out.y = 0.0f;
    out.z = z;
    out.ok = 0;

    if (!(z > JW_MIN_Z)) return out;            // also catches NaN
    if (!(aspect > 0.01f)) return out;

    inv   = 1.0f / z;
    out.x = (JW_FOCAL / aspect) * jw_dot(r, jw_right()) * inv;
    out.y =  JW_FOCAL           * jw_dot(r, jw_up())    * inv;
    out.ok = 1;
    return out;
}

#endif // WAVE_CAM_H
