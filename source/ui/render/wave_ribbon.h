// Stage 3 of the wave geometry pipeline: triangle-strip vertices.
//
//   wave_kernel.h  -> node positions
//   wave_spline.h  -> dense smooth curve through them
//   wave_ribbon.h  -> triangle-strip vertices for the renderer   (this file)
//
// THE VERTEX CONTRACT
//
// wr_vert mirrors ui_wave.cpp's WaveVert field for field.  It is NOT a
// 20-byte struct, and that is deliberate -- read ui_wave.cpp lines 121-143
// before changing it.  The payload is 20 bytes:
//
//   +0   float x        clip space, [-1, +1]
//   +4   float y        clip space, [-1, +1], +y is UP
//   +8   float z        always 0
//   +12  float w        always 1
//   +16  u8 r, g, b, a  pre-composited opaque colour, a always 255
//
// but sizeof is 24, because the struct is aligned(8).  Unpadded at 20 bytes
// every odd vertex starts on a 4-byte but not 8-byte boundary; GCC is free to
// merge two adjacent 4-byte fields into one 64-bit store, and a misaligned
// 64-bit store into RSX local memory faults and takes the GPU down with it --
// black screen, console off the network, power cycle.  ui_wave.cpp hit that
// for real when the palette became a runtime read.  The four trailing pad
// bytes cost nothing: rsxBindVertexArrayAttrib takes its stride from sizeof.
//
// The colour is ONE u32 rather than four u8 fields for the same reason: one
// aligned word store instead of four byte stores.  On the big-endian PPU
// WR_RGBA writes bytes r,g,b,a at +16..+19, which is exactly what the
// GCM_VERTEX_DATA_TYPE_U8 binding at vo+16 reads.  On a little-endian host
// the same u32 VALUE lays out in the opposite byte order -- the tests check
// the value, and check the byte order against the host's own endianness, so
// they stay meaningful in both builds.
//
// BLENDING STAYS OFF.  Colours arrive already composited against whatever is
// behind them and alpha is forced to 255, matching ui_wave.cpp's baked-opaque
// path.  This stage never emits a partial alpha.
//
// Header-only, pure C, no libm, no PS3 headers (.clinerules rule 7).

#ifndef WAVE_RIBBON_H
#define WAVE_RIBBON_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>     /* memcpy, for the rsqrt seed; not libm */

typedef struct { float x, y, z, w; uint32_t rgba; }
    __attribute__((aligned(8))) wr_vert;

#define WR_RGBA(r, g, b, a) (((uint32_t)(r) << 24) | ((uint32_t)(g) << 16) | \
                             ((uint32_t)(b) <<  8) |  (uint32_t)(a))

// Compile-time layout guard, so a consumer that gets this wrong fails to
// build rather than wedging a console.  The host test asserts the same things
// at runtime as well.
#define WR_SA_CONCAT_(a, b) a##b
#define WR_SA_CONCAT(a, b)  WR_SA_CONCAT_(a, b)
#define WR_STATIC_ASSERT(e) \
    typedef char WR_SA_CONCAT(wr_static_assert_, __LINE__)[(e) ? 1 : -1]

WR_STATIC_ASSERT(offsetof(wr_vert, x)    ==  0);
WR_STATIC_ASSERT(offsetof(wr_vert, y)    ==  4);
WR_STATIC_ASSERT(offsetof(wr_vert, z)    ==  8);
WR_STATIC_ASSERT(offsetof(wr_vert, w)    == 12);
WR_STATIC_ASSERT(offsetof(wr_vert, rgba) == 16);
WR_STATIC_ASSERT(sizeof(wr_vert)         == 24);   /* 20 payload + 8-byte align */

// Anything shorter than this is treated as a repeated point rather than a
// direction.  Squared length, so it is the square of the shortest segment the
// tangent is trusted for.
#define WR_TANGENT_EPS2  1.0e-20f

// 1/sqrt(x) without libm: the standard exponent-halving seed followed by
// three Newton steps, which reaches float precision.  memcpy rather than a
// union or a pointer cast so it is well defined in both C and C++; GCC turns
// a 4-byte memcpy into a single move.  The byte copy preserves the IEEE754
// bit pattern on either endianness, so the seed is identical on host and PPU.
//
// Returns 0 for zero, negative and NaN input; callers treat 0 as degenerate.
static inline float wr_rsqrt(float x)
{
    uint32_t i;
    float    h, y;
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

static inline float wr_clamp1(float v)
{
    if (v != v)     return 0.0f;      // NaN in, defined out
    if (v < -1.0f)  return -1.0f;
    if (v >  1.0f)  return  1.0f;
    return v;
}

static inline void wr_put(wr_vert *v, float x, float y,
                          uint8_t r, uint8_t g, uint8_t b)
{
    v->x = wr_clamp1(x);
    v->y = wr_clamp1(y);
    v->z = 0.0f;
    v->w = 1.0f;
    v->rgba = WR_RGBA(r, g, b, 255);   // alpha always opaque, blending is off
}

// Turn a polyline into a triangle strip of constant half-thickness.
//
//   px, py   the curve from stage 2 (ws_build's ox / oy), m points
//   half     half-thickness in clip-space units, offset along the normal
//   t*/b*    the two edge colours, already composited, already opaque
//   out, cap destination and its capacity in vertices
//
// Emits 2*m vertices: the pair (outer, inner) per point, which is the same
// top-then-bottom ordering per column that ui_wave.cpp's strips already use.
// Returns the vertex count, or 0 if the request is degenerate or would not
// fit, leaving out untouched.
//
// Stage 2's oz is deliberately not consumed: the vertex contract fixes z to 0
// (the bands are a flat 2-D overlay), so a depth term would be written and
// then ignored by the vertex program.  The channel exists in stage 2 because
// the chain genuinely has one, and it is there for a perspective variant.
//
// Normals come from a central difference, one-sided at the ends.  Where
// consecutive points repeat the tangent is undefined; that case reuses the
// last good normal, and a curve that is entirely one repeated point gets the
// +y normal, so the output is always finite.
static inline int wr_build(const float *px, const float *py, int m,
                           float half,
                           uint8_t tr, uint8_t tg, uint8_t tb,
                           uint8_t br, uint8_t bg, uint8_t bb,
                           wr_vert *out, int cap)
{
    int   i, n = 0;
    float nx = 0.0f, ny = 1.0f;       // last good normal, seeded pointing up

    if (!px || !py || !out) return 0;
    if (m < 2 || cap < 2 * m) return 0;
    if (!(half >= 0.0f)) return 0;    // rejects negative and NaN

    for (i = 0; i < m; i++) {
        int   ia = (i > 0)     ? i - 1 : i;
        int   ib = (i < m - 1) ? i + 1 : i;
        float dx = px[ib] - px[ia];
        float dy = py[ib] - py[ia];
        float l2 = dx * dx + dy * dy;

        if (l2 > WR_TANGENT_EPS2) {
            float inv = wr_rsqrt(l2);
            if (inv > 0.0f) {
                // Normal is the tangent rotated a quarter turn: for a curve
                // running along +x this is (0, +1), so the first vertex of
                // each pair is the upper edge.
                nx = -(dy * inv);
                ny =  (dx * inv);
            }
        }
        // else: repeated point, keep the previous normal.

        wr_put(&out[n++], px[i] + nx * half, py[i] + ny * half, tr, tg, tb);
        wr_put(&out[n++], px[i] - nx * half, py[i] - ny * half, br, bg, bb);
    }
    return n;
}

#endif // WAVE_RIBBON_H
