// bg_day_blend's schedule and bg_apply_day's look (bg_gradient.h).
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <stdint.h>

#include "bg_gradient.h"      // hoststub/ppu-types.h stands in for the PSL1GHT one

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static float lum(u32 c) { return (float)((c >> 16) & 0xFF) + (float)((c >> 8) & 0xFF) + (float)(c & 0xFF); }

int main(void)
{
    // Plateaus.
    CHECK(bg_day_blend(0) == 0.0f);
    CHECK(bg_day_blend(3 * 60 + 59) == 0.0f);
    CHECK(bg_day_blend(6 * 60) == 1.0f);
    CHECK(bg_day_blend(12 * 60) == 1.0f);
    CHECK(bg_day_blend(17 * 60 + 59) == 1.0f);
    CHECK(bg_day_blend(20 * 60) == 0.0f);
    CHECK(bg_day_blend(23 * 60 + 59) == 0.0f);
    // Ramps: continuous at the edges, halfway at the midpoints, monotonic.
    CHECK(fabsf(bg_day_blend(4 * 60)) < 1e-6f);
    CHECK(fabsf(bg_day_blend(5 * 60) - 0.5f) < 1e-6f);
    CHECK(fabsf(bg_day_blend(18 * 60) - 1.0f) < 1e-6f);
    CHECK(fabsf(bg_day_blend(19 * 60) - 0.5f) < 1e-6f);
    float prev = 0.0f;
    for (int m = 4 * 60; m <= 6 * 60; m++) { float d = bg_day_blend(m); CHECK(d >= prev - 1e-6f); prev = d; }
    prev = 1.0f;
    for (int m = 18 * 60; m <= 20 * 60; m++) { float d = bg_day_blend(m); CHECK(d <= prev + 1e-6f); prev = d; }
    float worst = 0.0f;
    for (int m = 0; m < 1440; m++) {
        float s = fabsf(bg_day_blend(m + 1) - bg_day_blend(m));
        if (s > worst) worst = s;
    }
    CHECK(worst < 0.0126f);                 // no visible jump between minutes
    CHECK(bg_day_blend(1440 + 12 * 60) == 1.0f);
    CHECK(bg_day_blend(-60) == 0.0f);

    // The glow: none by day or night, full half way through a ramp.
    CHECK(bg_dusk_glow(0.0f) == 0.0f);
    CHECK(bg_dusk_glow(1.0f) == 0.0f);
    CHECK(fabsf(bg_dusk_glow(0.5f) - 1.0f) < 1e-6f);

    // Night is the theme bit for bit -- Jellywave's own corners.
    const u32 top = 0x00151A38u, bot = 0x0005060Cu, acc = 0x00AA5CC3u;
    bg_quad q = bg_from_two(top, bot);
    bg_quad n = bg_apply_day(q, 0.0f, acc);
    CHECK(memcmp(&n, &q, sizeof q) == 0);
    // Day is brighter at every corner, a flatter sky, and keeps the hue.
    bg_quad d = bg_apply_day(q, 1.0f, acc);
    for (int i = 0; i < 4; i++) CHECK(lum(d.c[i]) > lum(q.c[i]));
    CHECK(lum(d.c[BG_TL]) / lum(d.c[BG_BL]) < lum(q.c[BG_TL]) / lum(q.c[BG_BL]));
    CHECK((d.c[BG_TL] & 0xFF) > ((d.c[BG_TL] >> 16) & 0xFF));   // still blue over red
    CHECK(memcmp(&d.c[BG_TL], &d.c[BG_TR], sizeof(u32)) == 0);   // no glow by day
    // A warm theme stays warm.
    bg_quad g = bg_apply_day(bg_from_two(0x0017120Eu, 0x00040303u), 1.0f, 0x00E0A13Cu);
    CHECK(((g.c[BG_TL] >> 16) & 0xFF) > (g.c[BG_TL] & 0xFF));
    // Dusk: the top-right corner leans to the accent, the others do not.
    bg_quad k = bg_apply_day(q, 0.5f, acc);
    bg_quad kn = bg_apply_day(q, 0.5f, 0x00000000u);
    CHECK(memcmp(&k.c[BG_TL], &kn.c[BG_TL], sizeof(u32)) == 0);
    CHECK(((k.c[BG_TR] >> 16) & 0xFF) > ((kn.c[BG_TR] >> 16) & 0xFF));   // redder
    // Clamped: day > 1 is day.
    bg_quad over = bg_apply_day(q, 7.0f, acc);
    CHECK(memcmp(&over, &d, sizeof q) == 0);
    // Alpha is kept.
    bg_quad qa = bg_from_two(0xFF151A38u, 0xFF05060Cu);
    bg_quad da = bg_apply_day(qa, 1.0f, acc);
    for (int i = 0; i < 4; i++) CHECK((da.c[i] >> 24) == 0xFF);

    printf("%s\n", fails ? "FAIL" : "PASS");
    return fails ? 1 : 0;
}
