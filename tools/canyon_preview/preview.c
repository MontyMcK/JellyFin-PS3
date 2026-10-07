// Host preview for the Canyon visualizer.
//
// Runs the real analyser (source/music/sv_spectrum.h) and the real terrain +
// vertex emitter (source/ui/render/canyon.h) over a synthetic mix, then
// software-rasterises exactly the triangles the PS3 would be handed (same
// strips, same packed colours, same blend per pass) into PPM frames.
//
//   cc -O2 -I../../source/music -I../../source/ui/render -o preview preview.c -lm
//   ./preview [inflated-canyon.qrcf [preset-index]] [--wav in.f32]
//
// The QRCF image is optional and never committed: extract it yourself from
// the console's /dev_flash/vsh/resource/qgl/canyon.qrc (QRCC header + zlib).

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stddef.h>
#include "sv_spectrum.h"
#include "canyon.h"

#define W 960
#define H 540

static float fb[H][W][3];

static void unpack(uint32_t c, float *r, float *g, float *b, float *a) {
    *r = (float)((c >> 24) & 255) / 255.0f; *g = (float)((c >> 16) & 255) / 255.0f;
    *b = (float)((c >> 8) & 255) / 255.0f;  *a = (float)(c & 255) / 255.0f;
}

// Clip-space triangle -> screen, barycentric raster with perspective-correct
// colour (the RSX interpolates with perspective too).  Triangles with any
// w <= 0 are dropped (cy__proj never produces them).
static void tri(const cy_vert *a, const cy_vert *b, const cy_vert *c, int additive, int blend) {
    const cy_vert *v[3] = { a, b, c };
    float sx[3], sy[3], iw[3], col[3][4];
    for (int k = 0; k < 3; k++) {
        if (v[k]->w <= 0.0f) return;
        iw[k] = 1.0f / v[k]->w;
        sx[k] = (v[k]->x * iw[k] * 0.5f + 0.5f) * W;
        sy[k] = (1.0f - (v[k]->y * iw[k] * 0.5f + 0.5f)) * H;
        unpack(v[k]->rgba, &col[k][0], &col[k][1], &col[k][2], &col[k][3]);
    }
    float area = (sx[1] - sx[0]) * (sy[2] - sy[0]) - (sx[2] - sx[0]) * (sy[1] - sy[0]);
    if (fabsf(area) < 1e-6f) return;
    int x0 = (int)floorf(fminf(sx[0], fminf(sx[1], sx[2]))), x1 = (int)ceilf(fmaxf(sx[0], fmaxf(sx[1], sx[2])));
    int y0 = (int)floorf(fminf(sy[0], fminf(sy[1], sy[2]))), y1 = (int)ceilf(fmaxf(sy[0], fmaxf(sy[1], sy[2])));
    if (x0 < 0) x0 = 0; if (y0 < 0) y0 = 0; if (x1 > W - 1) x1 = W - 1; if (y1 > H - 1) y1 = H - 1;
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++) {
            float px = x + 0.5f, py = y + 0.5f;
            float w0 = ((sx[1] - px) * (sy[2] - py) - (sx[2] - px) * (sy[1] - py)) / area;
            float w1 = ((sx[2] - px) * (sy[0] - py) - (sx[0] - px) * (sy[2] - py)) / area;
            float w2 = 1.0f - w0 - w1;
            if (w0 < 0 || w1 < 0 || w2 < 0) continue;
            float p0 = w0 * iw[0], p1 = w1 * iw[1], p2 = w2 * iw[2], ps = p0 + p1 + p2;
            float c[4];
            for (int k = 0; k < 4; k++) c[k] = (col[0][k] * p0 + col[1][k] * p1 + col[2][k] * p2) / ps;
            for (int k = 0; k < 3; k++) {
                float d = fb[y][x][k];
                if (!blend) d = c[k];
                else if (additive) d = d + c[k] * c[3];
                else d = d * (1.0f - c[3]) + c[k] * c[3];
                fb[y][x][k] = d > 1.0f ? 1.0f : d;
            }
        }
}

static void strip(const cy_vert *v, int n, int additive, int blend) {
    for (int i = 0; i + 2 < n; i++) tri(&v[i], &v[i + 1], &v[i + 2], additive, blend);
}

static void save(const char *path) {
    FILE *f = fopen(path, "wb");
    fprintf(f, "P6\n%d %d\n255\n", W, H);
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++)
            for (int k = 0; k < 3; k++) fputc((int)(fb[y][x][k] * 255.0f + 0.5f), f);
    fclose(f);
}

// A little arrangement: kick on the beat, a bass line, a pad, off-beat hats.
static float synth(double t, unsigned *seed) {
    const double bpm = 118.0, beat = 60.0 / bpm;
    double bt = fmod(t, beat), bar = fmod(t, beat * 4);
    double s = 0.0;
    s += 0.9 * sin(2 * M_PI * (50 + 90 * exp(-bt * 30)) * bt) * exp(-bt * 7);          // kick
    static const double bassn[4] = { 55.0, 55.0, 65.4, 49.0 };
    s += 0.35 * sin(2 * M_PI * bassn[(int)(bar / beat)] * t) * (0.6 + 0.4 * exp(-bt * 3));
    s += 0.12 * (sin(2 * M_PI * 220 * t) + sin(2 * M_PI * 277.2 * t) + sin(2 * M_PI * 329.6 * t));
    double ht = fmod(t + beat / 2, beat);
    *seed = *seed * 1664525u + 1013904223u;
    double noise = ((*seed >> 9) & 0xFFFF) / 32768.0 - 1.0;
    s += 0.25 * noise * exp(-ht * 40);
    if (t > 8.0 && t < 12.0) s *= 0.15;                                                   // a breakdown
    return (float)(0.5 * s);
}

int main(int argc, char **argv) {
    static cy_bank bank;
    cy_preset preset = CY_DEFAULT;
    const char *label = "default";
    if (argc > 1) {
        FILE *f = fopen(argv[1], "rb");
        if (!f) { perror(argv[1]); return 1; }
        fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
        uint8_t *d = malloc(n); fread(d, 1, n, f); fclose(f);
        int np = cy_bank_load(&bank, d, (uint32_t)n);
        printf("bank: %d presets, %d keys\n", np, bank.keys);
        int idx = argc > 2 ? atoi(argv[2]) : 0;
        if (np > 0) { preset = bank.p[idx % np]; label = bank.name[idx % np]; }
        free(d);
    }
    cy_preset_sanitise(&preset);
    printf("preset %s: col %.1f/%.1f/%.1f scale %.2f bias %.2f fog %.0f..%.0f height %.0f speed %.0f zoom %.0f\n",
           label, preset.col_r, preset.col_g, preset.col_b, preset.col_scale, preset.col_bias,
           preset.fog_min, preset.fog_max, preset.height, preset.speed, preset.zoom);

    static sv_spec_t sv;
    sv_spec_init(&sv);
    static cy_state st;
    cy_init(&st);
    static cy_vert verts[CY_MAX_VERTS];
    float L[256], R[256], lr[1024];
    unsigned seed = 1;
    const double fps = 60.0;
    const int shots[] = { 240, 420, 600, 780 };
    for (int frame = 0; frame <= 780; frame++) {
        double t0 = frame / fps;
        for (int i = 0; i < 512; i++) {
            double t = t0 - (511 - i) / 48000.0;
            float m = synth(t, &seed);
            lr[2 * i] = m * (1.0f + 0.2f * (float)sin(t));          // a little stereo
            lr[2 * i + 1] = m * (1.0f - 0.2f * (float)sin(t));
        }
        sv_spec_run(&sv, lr, L, R);
        cy_feed(&st, L, R);
        cy_step(&st, &preset, (float)(1.0 / fps));
        for (unsigned k = 0; k < sizeof(shots) / sizeof(shots[0]); k++) {
            if (frame != shots[k]) continue;
            cy_counts c;
            int n = cy_emit(&st, &preset, (float)W / H, 1.0f, verts, CY_MAX_VERTS, &c);
            strip(verts + c.sky_off, c.sky_n, 0, 0);
            strip(verts + c.land_off, c.land_n, 0, 1);
            strip(verts + c.line_off, c.line_n, 1, 1);
            char path[64];
            snprintf(path, sizeof path, "canyon_%s_%03d.ppm", label, frame);
            save(path);
            printf("%s: %d verts (land %d line %d)  L[0]=%.2f L[128]=%.2f q=%.2f/%.2f/%.2f/%.2f\n",
                   path, n, c.land_n, c.line_n, L[0], L[128],
                   st.quarter[0], st.quarter[1], st.quarter[2], st.quarter[3]);
        }
    }
    return 0;
}
