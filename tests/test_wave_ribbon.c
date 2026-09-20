// Host test for stage 3 of the wave geometry pipeline,
// source/ui/render/wave_ribbon.h, and for the pipeline end to end.
//
// The layout checks here are the point of this file.  The vertex struct is
// what the RSX fetches straight out of local memory, and ui_wave.cpp's own
// comment records what getting it wrong costs: a misaligned 64-bit store into
// that buffer faults, takes the GPU down, and needs a power cycle.  So the
// offsets, the size, the alignment and the colour byte order are all asserted
// explicitly rather than assumed from the declaration.
//
// NOTE ON THE 20-BYTE FIGURE.  The vertex PAYLOAD is 20 bytes -- four floats
// then four colour bytes, at offsets 0, 4, 8, 12 and 16 -- and that is what
// the vertex attribute bindings in ui_wave.cpp read.  sizeof is 24, because
// the struct is aligned(8) and the last four bytes are padding.  Both numbers
// are checked below; they are not in conflict, and the stride the RSX is given
// follows sizeof.
//
// libm is used here; the headers may not.

#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <math.h>
#include <string.h>

#include "../source/ui/render/wave_kernel.h"
#include "../source/ui/render/wave_spline.h"
#include "../source/ui/render/wave_ribbon.h"

#define NODES   96
#define SAMPLES 256
#define MAXV    (2 * SAMPLES)

static int failures = 0;

#define CHECK(cond, ...) do {                                   \
    if (!(cond)) {                                              \
        printf("  FAIL %s:%d: ", __FILE__, __LINE__);           \
        printf(__VA_ARGS__);                                    \
        printf("\n");                                           \
        failures++;                                             \
    }                                                           \
} while (0)

static float   cx[SAMPLES], cy[SAMPLES], cz[SAMPLES];
static wr_vert vb[MAXV + 8];

// --- the vertex contract -------------------------------------------------

static void test_vertex_layout(void)
{
    printf("  layout: sizeof = %u, alignof = %u, payload ends at %u\n",
           (unsigned)sizeof(wr_vert), (unsigned)_Alignof(wr_vert),
           (unsigned)(offsetof(wr_vert, rgba) + sizeof(uint32_t)));

    CHECK(offsetof(wr_vert, x) == 0,  "x is at %u, expected 0",  (unsigned)offsetof(wr_vert, x));
    CHECK(offsetof(wr_vert, y) == 4,  "y is at %u, expected 4",  (unsigned)offsetof(wr_vert, y));
    CHECK(offsetof(wr_vert, z) == 8,  "z is at %u, expected 8",  (unsigned)offsetof(wr_vert, z));
    CHECK(offsetof(wr_vert, w) == 12, "w is at %u, expected 12", (unsigned)offsetof(wr_vert, w));
    CHECK(offsetof(wr_vert, rgba) == 16,
          "colour is at %u, expected 16 -- this is the offset the "
          "GCM_VERTEX_ATTRIB_COLOR0 binding uses", (unsigned)offsetof(wr_vert, rgba));

    CHECK(sizeof(float) == 4, "float is not 4 bytes");
    CHECK(sizeof(uint32_t) == 4, "uint32_t is not 4 bytes");

    // The 20-byte payload.
    CHECK(offsetof(wr_vert, rgba) + sizeof(uint32_t) == 20,
          "the payload is %u bytes, expected 20",
          (unsigned)(offsetof(wr_vert, rgba) + sizeof(uint32_t)));

    // The 24-byte stride, and why.
    CHECK(sizeof(wr_vert) == 24,
          "sizeof is %u, expected 24 -- unpadded at 20 every odd vertex starts "
          "4-byte but not 8-byte aligned, and a merged 64-bit store into RSX "
          "local memory at that offset wedges the GPU",
          (unsigned)sizeof(wr_vert));
    CHECK(_Alignof(wr_vert) == 8, "alignof is %u, expected 8",
          (unsigned)_Alignof(wr_vert));

    // Every vertex in an array therefore starts 8-byte aligned.
    {
        size_t i;
        for (i = 0; i < 8; i++)
            CHECK((((uintptr_t)&vb[i]) & 7u) == 0 || ((i * sizeof(wr_vert)) & 7u) == 0,
                  "vertex %u is not 8-byte aligned within the array", (unsigned)i);
    }
}

// The colour is stored as one u32 rather than four u8 fields, so the byte
// order in memory depends on the target.  On the big-endian PPU the bytes land
// r,g,b,a at +16..+19, which is what the U8 attribute binding reads.  This
// checks the VALUE, which is target-independent, and then checks the bytes
// against the host's own endianness, so it stays a real check when this file
// is built for ppc64 as well as for x86.
static void test_colour_packing(void)
{
    wr_vert    v;
    uint8_t    bytes[4];
    uint32_t   probe = 1;
    int        little = *(const uint8_t *)&probe;

    wr_put(&v, 0.0f, 0.0f, 0x11, 0x22, 0x33);

    CHECK(v.rgba == 0x112233FFu, "packed colour is 0x%08X, expected 0x112233FF",
          v.rgba);
    CHECK((v.rgba & 0xFFu) == 0xFFu, "alpha is not 255 -- blending is off, so "
          "every vertex this stage emits must be fully opaque");

    memcpy(bytes, (const char *)&v + offsetof(wr_vert, rgba), 4);
    printf("  colour: host is %s-endian, bytes at +16 are %02X %02X %02X %02X\n",
           little ? "little" : "big", bytes[0], bytes[1], bytes[2], bytes[3]);

    if (little) {
        CHECK(bytes[0] == 0xFF && bytes[1] == 0x33 &&
              bytes[2] == 0x22 && bytes[3] == 0x11,
              "little-endian byte order is wrong");
    } else {
        CHECK(bytes[0] == 0x11 && bytes[1] == 0x22 &&
              bytes[2] == 0x33 && bytes[3] == 0xFF,
              "big-endian bytes at +16 are not r,g,b,a -- this is the order "
              "the GCM_VERTEX_DATA_TYPE_U8 binding reads on the console");
    }
}

static void test_rsqrt(void)
{
    float x, worst = 0.0f;
    for (x = 1.0e-6f; x < 100.0f; x *= 1.0009f) {
        float got = wr_rsqrt(x);
        float ref = 1.0f / sqrtf(x);
        float rel = fabsf(got - ref) / ref;
        if (rel > worst) worst = rel;
    }
    printf("  rsqrt: worst relative error over [1e-6, 100] = %.2e\n", worst);
    CHECK(worst < 1.0e-6f, "rsqrt relative error %.3e is too large", worst);

    CHECK(wr_rsqrt(0.0f) == 0.0f, "rsqrt(0) should be 0");
    CHECK(wr_rsqrt(-1.0f) == 0.0f, "rsqrt of a negative should be 0");
    CHECK(wr_rsqrt((float)NAN) == 0.0f, "rsqrt(NaN) should be 0");
}

// --- generation ----------------------------------------------------------

static void fill_arc(int m, float x0, float x1, float amp)
{
    int i;
    for (i = 0; i < m; i++) {
        float u = (float)i / (float)(m - 1);
        cx[i] = x0 + (x1 - x0) * u;
        cy[i] = amp * wk_sinf(u * WK_TWO_PI);
        cz[i] = 0.0f;
    }
}

static void test_degenerate_requests(void)
{
    fill_arc(SAMPLES, -0.8f, 0.8f, 0.3f);

    CHECK(wr_build(cx, cy, 1, 0.02f, 1,2,3, 4,5,6, vb, MAXV) == 0, "m=1 should fail");
    CHECK(wr_build(cx, cy, 0, 0.02f, 1,2,3, 4,5,6, vb, MAXV) == 0, "m=0 should fail");
    CHECK(wr_build(cx, cy, -4, 0.02f, 1,2,3, 4,5,6, vb, MAXV) == 0, "negative m should fail");
    CHECK(wr_build(NULL, cy, SAMPLES, 0.02f, 1,2,3, 4,5,6, vb, MAXV) == 0, "null px should fail");
    CHECK(wr_build(cx, NULL, SAMPLES, 0.02f, 1,2,3, 4,5,6, vb, MAXV) == 0, "null py should fail");
    CHECK(wr_build(cx, cy, SAMPLES, 0.02f, 1,2,3, 4,5,6, NULL, MAXV) == 0, "null out should fail");
    CHECK(wr_build(cx, cy, SAMPLES, -0.1f, 1,2,3, 4,5,6, vb, MAXV) == 0,
          "a negative half-thickness should fail");
    CHECK(wr_build(cx, cy, SAMPLES, (float)NAN, 1,2,3, 4,5,6, vb, MAXV) == 0,
          "a NaN half-thickness should fail");

    // Capacity is exactly 2*m; one short must be refused, not overrun.
    CHECK(wr_build(cx, cy, SAMPLES, 0.02f, 1,2,3, 4,5,6, vb, MAXV - 1) == 0,
          "insufficient capacity should fail");
    CHECK(wr_build(cx, cy, SAMPLES, 0.02f, 1,2,3, 4,5,6, vb, MAXV) == MAXV,
          "exact capacity should succeed");

    // Zero thickness is legal: a degenerate ribbon, not an error.
    CHECK(wr_build(cx, cy, SAMPLES, 0.0f, 1,2,3, 4,5,6, vb, MAXV) == MAXV,
          "zero half-thickness should be accepted");
}

static void check_contract(const wr_vert *v, int n, const char *what)
{
    int i;
    for (i = 0; i < n; i++) {
        if (!(v[i].x >= -1.0f && v[i].x <= 1.0f)) {
            CHECK(0, "%s: vertex %d x = %f is outside [-1,1]", what, i, v[i].x);
            return;
        }
        if (!(v[i].y >= -1.0f && v[i].y <= 1.0f)) {
            CHECK(0, "%s: vertex %d y = %f is outside [-1,1]", what, i, v[i].y);
            return;
        }
        if (v[i].z != 0.0f) { CHECK(0, "%s: vertex %d z = %f, expected 0", what, i, v[i].z); return; }
        if (v[i].w != 1.0f) { CHECK(0, "%s: vertex %d w = %f, expected 1", what, i, v[i].w); return; }
        if ((v[i].rgba & 0xFFu) != 0xFFu) {
            CHECK(0, "%s: vertex %d alpha = %u, expected 255", what, i, v[i].rgba & 0xFFu);
            return;
        }
    }
}

static void test_contract_holds(void)
{
    int n;

    fill_arc(SAMPLES, -0.8f, 0.8f, 0.3f);
    n = wr_build(cx, cy, SAMPLES, 0.03f, 200, 210, 255, 20, 30, 60, vb, MAXV);
    CHECK(n == MAXV, "expected %d vertices, got %d", MAXV, n);
    check_contract(vb, n, "normal arc");

    // The two edge colours must actually differ per vertex of the pair.
    CHECK(vb[0].rgba == WR_RGBA(200, 210, 255, 255), "outer edge colour is wrong");
    CHECK(vb[1].rgba == WR_RGBA(20, 30, 60, 255), "inner edge colour is wrong");

    // A curve that would push outside clip space must be clamped, not emitted
    // out of range.
    fill_arc(SAMPLES, -1.0f, 1.0f, 0.99f);
    n = wr_build(cx, cy, SAMPLES, 0.5f, 1,2,3, 4,5,6, vb, MAXV);
    CHECK(n == MAXV, "clamped build failed");
    check_contract(vb, n, "deliberately over-range arc");
}

static void test_thickness(void)
{
    const float half = 0.04f;
    int   i, n;
    float worst = 0.0f;

    // Kept clear of the clip-space edges so the clamp never engages and the
    // measurement is of the geometry, not of the clamp.
    fill_arc(SAMPLES, -0.6f, 0.6f, 0.2f);
    n = wr_build(cx, cy, SAMPLES, half, 1,2,3, 4,5,6, vb, MAXV);
    CHECK(n == MAXV, "build failed");

    for (i = 0; i < n; i += 2) {
        float dx = vb[i].x - vb[i+1].x;
        float dy = vb[i].y - vb[i+1].y;
        float d  = sqrtf(dx * dx + dy * dy);
        float e  = fabsf(d - 2.0f * half);
        if (e > worst) worst = e;
    }
    printf("  thickness: nominal %.4f, worst deviation %.2e\n", 2.0f * half, worst);
    CHECK(worst < 1.0e-5f, "ribbon thickness varies by %.3e", worst);

    // The first vertex of each pair is the one on the +y side for a curve
    // running left to right.
    CHECK(vb[0].y > vb[1].y, "the outer edge is not the upper one");
}

static void test_repeated_and_degenerate_points(void)
{
    int i, n;

    // Every point identical: the tangent is undefined everywhere, so the
    // seeded normal has to carry the whole ribbon.
    for (i = 0; i < SAMPLES; i++) { cx[i] = 0.0f; cy[i] = 0.0f; }
    n = wr_build(cx, cy, SAMPLES, 0.05f, 1,2,3, 4,5,6, vb, MAXV);
    CHECK(n == MAXV, "all-identical build failed");
    check_contract(vb, n, "all points identical");
    for (i = 0; i < n; i++)
        CHECK(isfinite(vb[i].x) && isfinite(vb[i].y),
              "all-identical input produced a non-finite vertex at %d", i);

    // A run of repeats in the middle of a real curve must not produce a kink
    // or a NaN -- the normal is carried forward instead.
    fill_arc(SAMPLES, -0.7f, 0.7f, 0.25f);
    for (i = 100; i < 140; i++) { cx[i] = cx[100]; cy[i] = cy[100]; }
    n = wr_build(cx, cy, SAMPLES, 0.03f, 1,2,3, 4,5,6, vb, MAXV);
    CHECK(n == MAXV, "repeated-run build failed");
    check_contract(vb, n, "repeated interior points");
    for (i = 0; i < n; i++)
        CHECK(isfinite(vb[i].x) && isfinite(vb[i].y),
              "repeated points produced a non-finite vertex at %d", i);

    // Two points only, the minimum strip.
    cx[0] = -0.5f; cy[0] = 0.0f;
    cx[1] =  0.5f; cy[1] = 0.1f;
    n = wr_build(cx, cy, 2, 0.02f, 1,2,3, 4,5,6, vb, 4);
    CHECK(n == 4, "two-point build returned %d, expected 4", n);
    check_contract(vb, n, "two points");
}

static void test_determinism(void)
{
    static wr_vert a[MAXV];
    int n1, n2;

    fill_arc(SAMPLES, -0.8f, 0.8f, 0.3f);
    n1 = wr_build(cx, cy, SAMPLES, 0.03f, 9, 8, 7, 6, 5, 4, a,  MAXV);
    n2 = wr_build(cx, cy, SAMPLES, 0.03f, 9, 8, 7, 6, 5, 4, vb, MAXV);
    CHECK(n1 == n2, "vertex counts differ");
    CHECK(memcmp(a, vb, (size_t)n1 * sizeof(wr_vert)) == 0,
          "the same input produced different vertices");
}

// --- the three stages together -------------------------------------------

static void test_pipeline_end_to_end(void)
{
    static float y[NODES], z[NODES], vy[NODES], vz[NODES];
    wk_chain c;
    int i, m, n;

    CHECK(wk_init(&c, y, z, vy, vz, NODES, 20250920u) == 1, "chain init failed");
    for (i = 0; i < 900; i++) wk_step(&c, 2.0f, 0.1f, 1.0f);

    m = ws_build(y, z, NODES, -1.0f, 1.0f, cx, cy, cz, SAMPLES);
    CHECK(m == SAMPLES, "spline stage returned %d", m);

    n = wr_build(cx, cy, m, 0.02f, 210, 225, 255, 24, 36, 72, vb, MAXV);
    CHECK(n == 2 * m, "ribbon stage returned %d, expected %d", n, 2 * m);

    check_contract(vb, n, "end to end");

    // The chain pins both ends at zero and the spline carries that through
    // exactly, so the ribbon's first and last pairs straddle y = 0.
    CHECK(fabsf(0.5f * (vb[0].y + vb[1].y)) < 1.0e-6f,
          "the ribbon does not start centred on the pinned node (%.9f)",
          0.5f * (vb[0].y + vb[1].y));
    CHECK(fabsf(0.5f * (vb[n-2].y + vb[n-1].y)) < 1.0e-6f,
          "the ribbon does not end centred on the pinned node (%.9f)",
          0.5f * (vb[n-2].y + vb[n-1].y));

    printf("  pipeline: %d nodes -> %d samples -> %d vertices (%u bytes)\n",
           NODES, m, n, (unsigned)((size_t)n * sizeof(wr_vert)));
}

int main(void)
{
    printf("wave_ribbon\n");
    test_vertex_layout();
    test_colour_packing();
    test_rsqrt();
    test_degenerate_requests();
    test_contract_holds();
    test_thickness();
    test_repeated_and_degenerate_points();
    test_determinism();
    test_pipeline_end_to_end();

    if (failures) {
        printf("wave_ribbon: %d FAILURE(S)\n", failures);
        return 1;
    }
    printf("wave_ribbon: all checks passed\n");
    return 0;
}
