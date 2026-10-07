// wave_look.h (haze, silhouette, the 59.94 Hz blend fraction) and
// wave_light.h's drifting key.
#include <stdio.h>
#include <string.h>
#include <math.h>

#include "wave_look.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

int main(void)
{
    // --- haze ---------------------------------------------------------------
    jw_vert v = { 0.1f, -0.4f, 12.0f, 1, 200, 100, 50, 80, 160, 240 };
    jw_vert w = v;
    jwl_haze(&w, 10.0f, 20.0f, 30.0f, 0.0f);
    CHECK(memcmp(&w, &v, sizeof v) == 0);                     // h = 0: bit for bit
    CHECK(JWL_HAZE[0] == 0.0f);                               // the near layer is never hazed
    w = v; jwl_haze(&w, 10.0f, 20.0f, 30.0f, 1.0f);
    CHECK(w.r == 10 && w.g == 20 && w.b == 30);               // full haze: the sky
    CHECK(w.rr < v.rr && w.rr > 0);                           // the rim dims, not gone
    w = v; jwl_haze(&w, 10.0f, 20.0f, 30.0f, JWL_HAZE[2]);
    CHECK(w.r < v.r && w.r > 10);                             // the far layer: part way
    CHECK(w.x == v.x && w.y == v.y && w.z == v.z && w.ok == v.ok);   // colour only
    w = v; jwl_haze(&w, 10.0f, 20.0f, 30.0f, 9.0f);          // clamped
    CHECK(w.r == 10);

    // --- silhouette -----------------------------------------------------------
    {
        static jw_vert st[JW_VERTS];
        memset(st, 0, sizeof st);
        for (int i = 0; i < JW_STATIONS; i++)
            for (int j = 0; j < JW_SECTION; j++) {
                jw_vert *q = &st[i * JW_SECTION + j];
                q->ok = 1;
                q->x  = -1.0f + 2.0f * (float)i / (float)(JW_STATIONS - 1);
                q->y  = -0.5f + 0.01f * (float)((j * 5 + i) % JW_SECTION);   // a shuffled section
            }
        int t, b;
        for (int i = 0; i < JW_STATIONS; i++) {
            CHECK(jwl_silhouette(st, i, &t, &b));
            for (int j = 0; j < JW_SECTION; j++) {
                CHECK(st[i * JW_SECTION + j].y <= st[i * JW_SECTION + t].y);
                CHECK(st[i * JW_SECTION + j].y >= st[i * JW_SECTION + b].y);
            }
        }
        // an undrawable point is never chosen, and an empty station says so
        st[3 * JW_SECTION + 0].y = 9.0f; st[3 * JW_SECTION + 0].ok = 0;
        CHECK(jwl_silhouette(st, 3, &t, &b) && t != 0);
        for (int j = 0; j < JW_SECTION; j++) st[4 * JW_SECTION + j].ok = 0;
        CHECK(!jwl_silhouette(st, 4, &t, &b));
        CHECK(!jwl_silhouette(st, -1, &t, &b));
        CHECK(!jwl_silhouette(st, JW_STATIONS, &t, &b));
    }

    // --- the blend fraction: frames, not milliseconds ---------------------------
    CHECK(jwl_interp_t(0, 2) == 0.5f);
    CHECK(jwl_interp_t(1, 2) == 1.0f);
    CHECK(jwl_interp_t(7, 2) == 1.0f);         // a late build: hold the newest
    CHECK(jwl_interp_t(0, 1) == 1.0f);         // rebuilt every call: nothing to blend
    CHECK(fabsf(jwl_interp_t(0, 3) - 1.0f / 3.0f) < 1e-6f);
    CHECK(fabsf(jwl_interp_t(1, 3) - 2.0f / 3.0f) < 1e-6f);
    CHECK(jwl_interp_t(2, 3) == 1.0f);
    CHECK(jwl_interp_t(-4, 2) == 0.5f);
    // every step is the same size, and the interval ends exactly on the build
    for (int every = 2; every <= 8; every++) {
        float prev = 0.0f;
        for (int k = 0; k < every; k++) {
            const float t = jwl_interp_t(k, every);
            CHECK(fabsf((t - prev) - 1.0f / (float)every) < 1e-6f);
            prev = t;
        }
        CHECK(prev == 1.0f);
    }
    // the signature mix is order-sensitive (a swapped strip order must differ)
    CHECK(jwl_sig_mix(jwl_sig_mix(1u, 2u), 3u) != jwl_sig_mix(jwl_sig_mix(1u, 3u), 2u));

    // --- the drifting key ---------------------------------------------------------
    {
        const jw_vec3 k0 = jw_key_dir();
        const jw_vec3 d0 = jw_key_drift(0.0f);
        CHECK(fabsf(d0.x - k0.x) < 1e-5f && fabsf(d0.y - k0.y) < 1e-5f && fabsf(d0.z - k0.z) < 1e-5f);
        float worst = 1.0f, moved = 1.0f;
        for (float t = 0.0f; t < 400.0f; t += 0.5f) {
            const jw_vec3 d = jw_key_drift(t);
            const float len = sqrtf(d.x * d.x + d.y * d.y + d.z * d.z);
            CHECK(fabsf(len - 1.0f) < 1e-3f);
            const float c = d.x * k0.x + d.y * k0.y + d.z * k0.z;
            if (c < worst) worst = c;
            if (c < moved) moved = c;
            // a 60 Hz frame never jumps: at most ~0.02 degrees per 1/60 s
            const jw_vec3 e = jw_key_drift(t + 1.0f / 60.0f);
            CHECK(d.x * e.x + d.y * e.y + d.z * e.z > 0.99999f);
        }
        CHECK(worst > cosf(16.0f * 3.14159265f / 180.0f));    // never more than ~15 deg away
        CHECK(moved < cosf(10.0f * 3.14159265f / 180.0f));    // but it does travel
        // with the fixed key the shading is what it always was
        const jw_vec3 n = jw_norm(jw_v3(0.2f, 0.9f, -0.3f));
        const jw_vec3 ev = jw_norm(jw_v3(0.0f, 0.1f, 1.0f));
        const jw_rgb a = jw_shade(jw_col(0.5f, 0.3f, 0.8f), n, ev, 0.4f);
        const jw_rgb b = jw_shade_k(jw_col(0.5f, 0.3f, 0.8f), n, ev, 0.4f, jw_key_dir());
        CHECK(a.r == b.r && a.g == b.g && a.b == b.b);
    }

    // --- the budget -----------------------------------------------------------------
    CHECK(JWL_FRINGE_VERTS == 2 * (2 * JW_STATIONS + 2));
    CHECK(JWL_GLOW_VERTS == 4 * JW_STATIONS + 2);

    printf("%s\n", fails ? "FAIL" : "PASS");
    return fails ? 1 : 0;
}
