// Stage 2 of the wave geometry pipeline: a dense smooth polyline through the
// spring chain's nodes.
//
//   wave_kernel.h  -> node positions
//   wave_spline.h  -> dense smooth curve through them   (this file)
//   wave_ribbon.h  -> triangle-strip vertices for the renderer
//
// A uniform cubic B-spline, as docs/wave-spec.md section 3b recommends ("use
// a uniform cubic B-spline over a small control grid, not summed sines ...
// the basis is four cheap polynomials, and it gives continuity for free").
// The basis is the standard one:
//
//   B0 = (1 - 3t + 3t^2 -   t^3) / 6
//   B1 = (4      - 6t^2 + 3t^3) / 6
//   B2 = (1 + 3t + 3t^2 - 3t^3) / 6
//   B3 = (                 t^3) / 6
//
// Two properties of that basis are what this stage is actually for, and both
// are asserted in tests/test_wave_spline.c:
//
//   PARTITION OF UNITY.  B0+B1+B2+B3 = 1 identically, and each is >= 0 on
//   [0,1].  Every output sample is therefore a CONVEX COMBINATION of four
//   control values, so the curve can never leave [min(node), max(node)].
//   No overshoot is possible -- not "is unlikely", cannot happen.  That is
//   what keeps stage 3's vertices inside clip space without a clamp doing the
//   work.
//
//   CONTINUITY.  C2 across interior segment joins, for free, from the basis.
//
// ENDPOINTS.  A B-spline approximates its control points rather than
// interpolating them, so the raw curve would start somewhere near node 0
// rather than at it.  Control indices are therefore CLAMPED to [0, n-1],
// which is the standard way of triplicating the end controls, and the
// parameter runs over n+1 segments rather than n-3.  At the first sample the
// four controls collapse to (P0,P0,P0,P1) and the basis at t=0 is
// (1,4,1,0)/6, giving exactly P0; at the last they collapse to
// (P[n-2],P[n-1],P[n-1],P[n-1]) and the basis at t=1 is (0,1,4,1)/6, giving
// exactly P[n-1].  So the curve starts and ends ON the end nodes, and
// approximates smoothly in between.  Since the kernel pins those two nodes at
// zero, the curve is pinned at zero too.
//
// Header-only, pure C, no libm, no PS3 headers (.clinerules rule 7).

#ifndef WAVE_SPLINE_H
#define WAVE_SPLINE_H

// Number of parameter segments for n control values.  See ENDPOINTS above:
// the two extra segments are what the clamped end controls are evaluated
// over.
#define WS_SEGMENTS(n)  ((n) + 1)

// The four uniform cubic B-spline basis functions at t in [0,1].
//
// Two of these are deliberately NOT evaluated in the expanded form the header
// comment quotes, because the expanded form does not hold the two properties
// this stage depends on once it is in float:
//
//   B0 is computed as (1-t)^3/6, not as (1 - 3t + 3t^2 - t^3)/6.  Near t = 1
//   the expanded form is 1 - 3 + 3 - 1, which cancels to nothing but rounding
//   error -- measured at -4e-8, i.e. NEGATIVE, which quietly breaks the
//   convex-hull bound the whole stage rests on.  The factored form is
//   non-negative by construction, and it is cheaper.
//
//   B2 is computed as the remainder, 1 - B0 - B1 - B3, which makes the
//   partition of unity exact rather than exact-to-within-rounding.  Its true
//   value never falls below 1/6 on [0,1], so the subtraction has plenty of
//   headroom and cannot go negative.
//
// B1 and B3 are evaluated directly; neither cancels anywhere on [0,1].
static inline void ws_basis(float t, float *b0, float *b1, float *b2, float *b3)
{
    const float k = 1.0f / 6.0f;
    float t2 = t * t;
    float t3 = t2 * t;
    float u  = 1.0f - t;
    *b0 = (u * u * u) * k;
    *b1 = (4.0f - 6.0f * t2 + 3.0f * t3) * k;
    *b3 = t3 * k;
    *b2 = 1.0f - *b0 - *b1 - *b3;
}

// One control value, with the index clamped into range.  The clamping IS the
// endpoint condition; see the header comment.
static inline float ws_ctrl(const float *p, int n, int i)
{
    if (i < 0)  i = 0;
    if (i >= n) i = n - 1;
    return p[i];
}

// Evaluate segment j (0 .. WS_SEGMENTS(n)-1) at t in [0,1].
//
// Evaluated as a correction around one of the controls rather than as a
// straight weighted sum.  Because the basis sums to 1,
//
//   sum(b_i * c_i)  ==  c1 + sum(b_i * (c_i - c1))
//
// and the i = 1 term drops out.  The two forms are identical in exact
// arithmetic; in float the second one is strictly better here:
//
//   Identical controls give differences of exactly zero, so a flat stretch of
//   chain comes out EXACTLY flat.  The weighted-sum form drifted by an ulp,
//   which meant the rest state -- every node at zero -- did not sample to
//   exactly zero, and a curve sampled from constant input sat one ulp above
//   its own control range.
//
//   Both endpoints fall out exactly for the same reason.  At the first sample
//   the clamped controls are (P0,P0,P0,P1) with c1 = P0: two differences are
//   zero and the third is multiplied by b3, which is exactly 0 at t = 0.  At
//   the last they are (P[n-2],P[n-1],P[n-1],P[n-1]) with c1 = P[n-1], and the
//   only non-zero difference is multiplied by b0, which is exactly 0 at t = 1.
//   So the curve starts and ends ON the end nodes bit for bit, which is what
//   stage 3 needs if the chain's pinned ends are to land on the screen edge.
static inline float ws_segment(const float *p, int n, int j, float t)
{
    float b0, b1, b2, b3;
    int   base = j - 1;                     // controls base-1 .. base+2
    float c1   = ws_ctrl(p, n, base);
    ws_basis(t, &b0, &b1, &b2, &b3);
    return c1 + b0 * (ws_ctrl(p, n, base - 1) - c1)
              + b2 * (ws_ctrl(p, n, base + 1) - c1)
              + b3 * (ws_ctrl(p, n, base + 2) - c1);
}

// Map output sample k of m onto a (segment, t) pair.  The two ends are
// special-cased to exact (0, 0) and (S-1, 1) so the endpoint identities above
// hold bit-exactly rather than to within a rounding error.
static inline void ws_param(int k, int m, int nseg, int *out_j, float *out_t)
{
    float s;
    int j;
    if (k <= 0)      { *out_j = 0;        *out_t = 0.0f; return; }
    if (k >= m - 1)  { *out_j = nseg - 1; *out_t = 1.0f; return; }
    s = (float)k * (float)nseg / (float)(m - 1);
    j = (int)s;
    if (j < 0)        j = 0;
    if (j > nseg - 1) j = nseg - 1;
    *out_j = j;
    *out_t = s - (float)j;
}

// Sample the chain into a dense polyline of m points.
//
//   y, z   node values from stage 1 (wk_chain::y / ::z), n of each
//   x0, x1 the span the chain occupies along x; sample x is linear across it
//   ox/oy/oz  outputs, m of each, struct-of-arrays to match the house style
//
// x is generated linearly rather than splined.  The chain is a graph y(x)
// with evenly spaced nodes, not a free curve, so x must stay monotone and
// uniform; splining it would bunch samples toward the ends where the clamped
// controls repeat, and stage 3's normals would inherit the bunching.
//
// Returns m on success, 0 if the request is degenerate (fewer than 2 nodes,
// fewer than 2 samples, or any null pointer), leaving the outputs untouched.
static inline int ws_build(const float *y, const float *z, int n,
                           float x0, float x1,
                           float *ox, float *oy, float *oz, int m)
{
    int nseg, k;
    float dx;

    if (!y || !z || !ox || !oy || !oz) return 0;
    if (n < 2 || m < 2) return 0;

    nseg = WS_SEGMENTS(n);
    dx   = (x1 - x0) / (float)(m - 1);

    for (k = 0; k < m; k++) {
        int   j;
        float t;
        ws_param(k, m, nseg, &j, &t);
        ox[k] = (k == m - 1) ? x1 : (x0 + dx * (float)k);
        oy[k] = ws_segment(y, n, j, t);
        oz[k] = ws_segment(z, n, j, t);
    }

    // The endpoints need no correction here: ws_segment reaches them exactly,
    // for the reason set out in its comment.  test_wave_spline.c asserts it.
    return m;
}

#endif // WAVE_SPLINE_H
