// wave_nav.h: the wave answering the pad.
#include <stdio.h>
#include <string.h>
#include <math.h>

#include "wave_nav.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

#define N 96

static void run(wnv_state *st, float secs, wnv_fx *fx)
{
    for (float t = 0.0f; t < secs; t += 1.0f / 60.0f) wnv_step(st, 1.0f / 60.0f, fx);
}

// the deepest dip, and whether anything rose, across all layers
static void measure(const wnv_state *st, float *deepest, int *rose, float *centre)
{
    wnv_look lk;
    wnv_snapshot(st, &lk);
    *deepest = 0.0f; *rose = 0;
    float best = 0.0f; int at = -1;
    for (int li = 0; li < 3; li++) {
        float d[N];
        memset(d, 0, sizeof d);
        wnv_apply(&lk, li, d, N);
        for (int k = 0; k < N; k++) {
            if (d[k] > 0.0f) *rose = 1;
            if (d[k] < *deepest) *deepest = d[k];
            if (li == 0 && d[k] < best) { best = d[k]; at = k; }
        }
    }
    if (centre) *centre = at < 0 ? -1.0f : (float)at / (float)(N - 1);
}

int main(void)
{
    wnv_state st;
    wnv_fx fx;
    float deep, c0, c1;
    int rose;

    // Rest is rest: nothing live, nothing added, the fx neutral.
    wnv_init(&st);
    run(&st, 1.0f, &fx);
    {
        wnv_look lk; wnv_snapshot(&st, &lk);
        CHECK(lk.live == 0);
        float d[N]; for (int k = 0; k < N; k++) d[k] = 0.25f;
        wnv_apply(&lk, 0, d, N);
        for (int k = 0; k < N; k++) CHECK(d[k] == 0.25f);
        for (int li = 0; li < 3; li++) CHECK(lk.bob[li] == 0.0f);
    }
    CHECK(fx.wind_x == 0.0f && fx.wind_y == 0.0f && fx.jostle == 0.0f && fx.perturb == 1.0f);

    // RIGHT: a dip that runs LEFT (the content moves left), never a rise.
    wnv_init(&st);
    wnv_event(&st, 1, 0);
    run(&st, 0.25f, &fx);
    measure(&st, &deep, &rose, &c0);
    CHECK(rose == 0);
    CHECK(deep < -0.03f);                       // visible: most of WNV_PUSH_A
    CHECK(deep >= -WNV_DIP_MAX - 1e-6f);
    CHECK(fx.wind_x < 0.0f);                    // the particles blow left too
    run(&st, 0.5f, &fx);
    measure(&st, &deep, &rose, &c1);
    CHECK(c1 < c0 - 0.1f);                      // and it travelled left
    CHECK(rose == 0);

    // LEFT: the mirror image.
    wnv_init(&st);
    wnv_event(&st, -1, 0);
    run(&st, 0.25f, &fx);
    measure(&st, &deep, &rose, &c0);
    run(&st, 0.5f, &fx);
    measure(&st, &deep, &rose, &c1);
    CHECK(c1 > c0 + 0.1f);
    CHECK(fx.wind_x > 0.0f);

    // It is gone once its life is over, and so is the gust.
    run(&st, 2.5f, &fx);
    measure(&st, &deep, &rose, NULL);
    CHECK(deep == 0.0f);
    CHECK(fabsf(fx.wind_x) < 1e-3f);

    // Stacking ten pushes never passes the limit, and never rises.
    wnv_init(&st);
    for (int i = 0; i < 10; i++) { wnv_event(&st, 1, 0); wnv_step(&st, 0.02f, &fx); }
    for (int f = 0; f < 60; f++) {
        wnv_step(&st, 1.0f / 60.0f, &fx);
        measure(&st, &deep, &rose, NULL);
        CHECK(rose == 0);
        CHECK(deep >= -WNV_DIP_MAX - 1e-6f);
    }

    // UP / DOWN: the band presses down and springs back to exactly 0.
    wnv_init(&st);
    wnv_event(&st, 0, 1);
    float lowest = 0.0f, highest = -1.0f;
    for (int f = 0; f < 120; f++) {
        wnv_step(&st, 1.0f / 60.0f, &fx);
        if (st.bob < lowest) lowest = st.bob;
        if (st.bob > highest) highest = st.bob;
    }
    CHECK(lowest < -0.02f);                     // ~5 px or more at 1080p
    CHECK(highest <= 0.0f);                     // never above rest
    run(&st, 2.0f, &fx);
    CHECK(st.bob == 0.0f && st.bob_v == 0.0f);
    // DOWN blows the particles up, UP blows them down
    wnv_init(&st); wnv_event(&st, 0, 1); wnv_step(&st, 1.0f / 60.0f, &fx); CHECK(fx.wind_y > 0.0f);
    wnv_init(&st); wnv_event(&st, 0, -1); wnv_step(&st, 1.0f / 60.0f, &fx); CHECK(fx.wind_y < 0.0f);
    // a held press never drives the bob past its floor
    wnv_init(&st);
    for (int i = 0; i < 40; i++) { wnv_event(&st, 0, 1); run(&st, 0.05f, &fx); CHECK(st.bob >= -WNV_BOB_MAX - 1e-6f); }

    // The shake: tapping (3/s) does not shake, a held scroll (7/s) does, and
    // it settles once the scroll stops.
    wnv_init(&st);
    for (int i = 0; i < 12; i++) { wnv_event(&st, 0, 1); run(&st, 1.0f / 3.0f, &fx); }
    CHECK(st.shake < 0.05f);
    CHECK(fx.perturb < 1.1f);
    wnv_init(&st);
    for (int i = 0; i < 28; i++) { wnv_event(&st, 0, 1); run(&st, 1.0f / 7.1f, &fx); }
    CHECK(st.shake > 0.8f);
    CHECK(fx.perturb > 2.5f);
    CHECK(fx.jostle > 1.3f);
    measure(&st, &deep, &rose, NULL);
    CHECK(rose == 0);                           // the shiver dips too
    CHECK(deep < -0.005f);
    run(&st, 3.0f, &fx);
    CHECK(st.shake == 0.0f);
    CHECK(fx.perturb == 1.0f);

    // Hostile dt never makes a NaN.
    wnv_init(&st);
    wnv_event(&st, 1, 1);
    wnv_step(&st, NAN, &fx);
    wnv_step(&st, 1e9f, &fx);
    wnv_step(&st, -5.0f, &fx);
    CHECK(st.bob == st.bob && st.shake == st.shake && fx.perturb == fx.perturb);

    printf("%s\n", fails ? "FAIL" : "PASS");
    return fails ? 1 : 0;
}
