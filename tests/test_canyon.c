// Host tests for the Canyon visualizer: the analyser (sv_spectrum.h) and the
// terrain / preset / emit logic (canyon.h).
//
//   make -f Makefile.host test_canyon && ./test_canyon

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "sv_spectrum.h"
#include "canyon.h"

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static sv_spec_t sv;
static float lr[SV_N * 2], L[SV_BINS], R[SV_BINS];

static int argmax(const float *v, int n) {
    int b = 0;
    for (int i = 1; i < n; i++) if (v[i] > v[b]) b = i;
    return b;
}

static void test_window(void) {
    // Edge-subtracted Gaussian: zero at both ends, peak in the middle, ~1.
    CHECK(fabsf(sv.win[0]) < 1e-6f && fabsf(sv.win[SV_N]) < 1e-6f, "window ends %g %g", sv.win[0], sv.win[SV_N]);
    CHECK(argmax(sv.win, SV_N + 1) == 256, "window peak at %d", argmax(sv.win, SV_N + 1));
    CHECK(sv.win[256] > 0.95f && sv.win[256] < 1.0f, "window peak %g", sv.win[256]);
    CHECK(sv.kern[8] > sv.kern[0] && fabsf(sv.kern[0]) < 1e-6f, "kernel shape");
    // Remap: band 0 reads bin 0, band 255 lands exactly on bin 255.
    CHECK(sv.remap_i[0] == 0, "remap[0]=%d", sv.remap_i[0]);
    CHECK(sv.remap_i[255] == 255 || (sv.remap_i[255] == 254 && sv.remap_f[255] > 0.99f),
          "remap[255]=%d+%g", sv.remap_i[255], sv.remap_f[255]);
}

static void test_silence(void) {
    memset(lr, 0, sizeof lr);
    sv_spec_run(&sv, lr, L, R);
    for (int i = 0; i < SV_BINS; i++) CHECK(L[i] == 0.0f && R[i] == 0.0f, "silence band %d = %g/%g", i, L[i], R[i]);
}

static void test_sine(float hz, int lo, int hi) {
    for (int i = 0; i < SV_N; i++) {
        lr[2 * i]     = sinf(2.0f * 3.14159265f * hz * (float)i / 48000.0f);   // L: full scale
        lr[2 * i + 1] = 0.0f;                                                 // R: silent
    }
    sv_spec_run(&sv, lr, L, R);
    int pk = argmax(L, SV_BINS);
    CHECK(pk >= lo && pk <= hi, "%g Hz peaks at band %d, want %d..%d", hz, pk, lo, hi);
    CHECK(L[pk] > 1.4f && L[pk] < 2.6f, "%g Hz level %g (full-scale sine reads ~2)", hz, L[pk]);
    CHECK(argmax(R, SV_BINS) == 0 && R[0] == 0.0f, "silent R channel leaked");
    // Log levels, so quieter by 20 dB must read clearly lower but still > 0.
    for (int i = 0; i < SV_N; i++) lr[2 * i] *= 0.1f;
    sv_spec_run(&sv, lr, L, R);
    CHECK(L[pk] > 0.3f && L[pk] < 1.6f, "%g Hz at -20 dB reads %g", hz, L[pk]);
}

static void test_mnu(void) {
    cy_preset p = CY_DEFAULT;
    const char txt[] = "#MNU_1.0\r\nCOLOUR R:float:7\r\nCOLOUR G:float:5\r\nFOG MIN:float:-154.8\r\n"
                       "LINE HEIGHT:float:0.00997899\r\nUNKNOWN KEY:float:3\r\nPOS SPEED:float:1e+01\r\n";
    int n = cy_mnu_apply(&p, txt, (int)strlen(txt));
    CHECK(n == 5, "applied %d keys", n);
    CHECK(p.col_r == 7.0f && p.col_g == 5.0f, "colour %g %g", p.col_r, p.col_g);
    CHECK(fabsf(p.fog_min + 154.8f) < 1e-3f, "fog_min %g", p.fog_min);
    CHECK(fabsf(p.line_h - 0.00997899f) < 1e-7f, "line_h %g", p.line_h);
    CHECK(fabsf(p.speed - 10.0f) < 1e-4f, "speed %g", p.speed);
    cy_preset_sanitise(&p);
    CHECK(p.fog_min >= 0.0f && p.fog_max >= p.fog_min + 20.0f, "sanitise fog %g..%g", p.fog_min, p.fog_max);
    CHECK(p.speed >= 8.0f, "sanitise speed %g", p.speed);
}

// A synthetic QRCF with one base file and one override, laid out the way
// canyon.qrc is.
static void put32(uint8_t *d, uint32_t o, uint32_t v) {
    d[o] = (uint8_t)(v >> 24); d[o + 1] = (uint8_t)(v >> 16); d[o + 2] = (uint8_t)(v >> 8); d[o + 3] = (uint8_t)v;
}
static void test_qrcf(void) {
    static uint8_t d[4096];
    memset(d, 0, sizeof d);
    memcpy(d, "QRCF", 4);
    const uint32_t toc = 0x40, nm = 0x200, dat = 0x400;
    const char *names[2] = { "canyon/CANYON_COLOUR.mnu", "override/Blue01/canyon/CANYON_COLOUR.mnu" };
    const char *files[2] = { "#MNU_1.0\nCOLOUR R:float:4\nCOLOUR B:float:9\n", "#MNU_1.0\nCOLOUR R:float:1\n" };
    uint32_t noff = 0, doff = 0;
    for (int k = 0; k < 2; k++) {
        uint32_t e = toc + 0x3C * k;
        put32(d, e, 0xF); put32(d, e + 0x1C, 0x14); put32(d, e + 0x20, 6);
        put32(d, e + 0x24, doff); put32(d, e + 0x28, (uint32_t)strlen(files[k]));
        put32(d, e + 0x34, noff);
        memcpy(d + nm + noff + 4, names[k], strlen(names[k]) + 1);
        noff += 4 + (uint32_t)strlen(names[k]) + 1;
        memcpy(d + dat + doff, files[k], strlen(files[k]));
        doff += (uint32_t)strlen(files[k]);
    }
    put32(d, 8, toc); put32(d, 12, 0x3C * 2); put32(d, 16, nm); put32(d, 0x28, dat);
    static cy_bank b;
    int n = cy_bank_load(&b, d, sizeof d);
    CHECK(n == 1, "presets %d", n);
    CHECK(n == 1 && strcmp(b.name[0], "Blue01") == 0, "name %s", b.name[0]);
    CHECK(b.base.col_r == 4.0f && b.base.col_b == 9.0f, "base %g %g", b.base.col_r, b.base.col_b);
    CHECK(b.p[0].col_r == 1.0f && b.p[0].col_b == 9.0f, "override on base %g %g", b.p[0].col_r, b.p[0].col_b);
    CHECK(cy_bank_load(&b, (const uint8_t *)"nope", 4) < 0, "bad header accepted");
}

static void test_terrain(void) {
    static cy_state st;
    cy_init(&st);
    cy_preset p = CY_DEFAULT;
    cy_preset_sanitise(&p);
    // Bass-heavy spectrum: walls up at the edges, floor low in the middle.
    for (int i = 0; i < SV_BINS; i++) L[i] = R[i] = i < 32 ? 2.0f : 0.05f;
    for (int f = 0; f < 90; f++) { cy_feed(&st, L, R); cy_step(&st, &p, 1.0f / 60.0f); }
    const float early = cy_val(&st, 0, 2);
    for (int f = 90; f < 600; f++) { cy_feed(&st, L, R); cy_step(&st, &p, 1.0f / 60.0f); }
    CHECK(st.rows_pushed > 150, "rows pushed %d at %g/s", st.rows_pushed, p.speed);
    float edge = cy_val(&st, 5, 2), mid = cy_val(&st, 5, 64);
    // Responsiveness: a bass step must raise the walls at least halfway
    // within 1.5 s, or the canyon reads as ignoring the music.
    CHECK(early > 0.5f * edge, "walls too slow: %g after 1.5 s vs %g settled", early, edge);
    CHECK(edge > 0.5f && mid < 0.2f, "bass should raise the walls: edge %g mid %g", edge, mid);
    // Sony's mapping is not exactly mirrored (column 2 reads L band 7, its
    // mirror reads R band 11) -- but a mono input must be close to it.
    CHECK(fabsf(cy_val(&st, 5, 2) - cy_val(&st, 5, 125)) < 0.15f * edge,
          "mono input far from symmetric: %g vs %g", cy_val(&st, 5, 2), cy_val(&st, 5, 125));
    // Silence settles the land.
    for (int f = 0; f < 900; f++) { cy_feed_silence(&st); cy_step(&st, &p, 1.0f / 60.0f); }
    CHECK(cy_val(&st, 0, 2) < 0.1f * edge, "silence did not settle: %g", cy_val(&st, 0, 2));

    // Emit: exact counts, every vertex in front of the eye, sky covers NDC.
    static cy_vert v[CY_MAX_VERTS];
    cy_counts c;
    int n = cy_emit(&st, &p, 16.0f / 9.0f, 1.0f, v, CY_MAX_VERTS, &c);
    CHECK(n == c.sky_n + c.land_n + c.line_n && n <= CY_MAX_VERTS, "count %d", n);
    CHECK(c.land_n == CY_LAND_VERTS && c.line_n == CY_LINE_VERTS && c.sky_n == 4, "land %d line %d", c.land_n, c.line_n);
    int behind = 0;
    for (int i = 0; i < n; i++) if (!(v[i].w > 0.0f) || v[i].z < 0.0f || v[i].z > v[i].w) behind++;
    CHECK(behind == 0, "%d vertices behind the eye / outside depth", behind);
    // The newest row (the line) must be on screen: its bottom vertices inside NDC y.
    int off = 0;
    for (int i = c.line_off; i < c.line_off + c.line_n; i++) {
        float y = v[i].y / v[i].w;
        if (y < -1.0f || y > 1.0f) off++;
    }
    CHECK(off == 0, "%d line vertices off screen vertically", off);
    CHECK(cy_emit(&st, &p, 1.7f, 1.0f, v, 10, &c) == 0, "emit must refuse a short buffer");
    CHECK(sizeof(cy_vert) == 24, "cy_vert must stay 24 bytes (WaveVert layout), is %zu", sizeof(cy_vert));
}

int main(void) {
    sv_spec_init(&sv);
    test_window();
    test_silence();
    test_sine(1000.0f, 38, 52);
    test_sine(200.0f, 5, 22);
    test_sine(6000.0f, 130, 160);
    test_mnu();
    test_qrcf();
    test_terrain();
    printf("%s (%d failures)\n", fails ? "FAIL" : "PASS", fails);
    return fails ? 1 : 0;
}
