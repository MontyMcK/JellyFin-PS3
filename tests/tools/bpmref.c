// Build: gcc -O2 -o bpmref tests/tools/bpmref.c -lm   Usage: bpmref track.raw name  (raw as for jw_analyze)
// Offline tempo reference: 100 Hz onset envelope (half-wave rectified change
// in log energy of a low band and a broadband) over the whole track,
// autocorrelated; prints the strongest periodicities in 60..200 BPM.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>

static float be(const unsigned char *p) {
    uint32_t u = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
    float f; memcpy(&f, &u, 4); return f;
}

int main(int argc, char **argv)
{
    FILE *f = fopen(argv[1], "rb"); if (!f) return 1;
    fseek(f, 0, SEEK_END); long len = ftell(f); fseek(f, 0, SEEK_SET);
    unsigned char *raw = malloc(len); if (fread(raw, 1, len, f) != (size_t)len) return 1; fclose(f);
    long frames = len / 8, hop = 480, nh = frames / hop;
    float *odf = calloc(nh, sizeof(float));
    double lp = 0, prevl = 0, prevb = 0;
    for (long h = 0; h < nh; h++) {
        double el = 0, eb = 0;
        for (long i = 0; i < hop; i++) {
            long k = (h * hop + i) * 2;
            float m = 0.5f * (be(raw + k * 4) + be(raw + (k + 1) * 4));
            lp += (m - lp) * 0.02;                 /* ~150 Hz low band */
            el += lp * lp; eb += m * m;
        }
        double ll = log(el + 1e-9), lb = log(eb + 1e-9);
        double d = (ll - prevl > 0 ? ll - prevl : 0) + 0.5 * (lb - prevb > 0 ? lb - prevb : 0);
        prevl = ll; prevb = lb;
        odf[h] = (float)d;
    }
    double mean = 0; for (long h = 0; h < nh; h++) mean += odf[h]; mean /= nh;
    for (long h = 0; h < nh; h++) odf[h] -= mean;
    double best[8] = {0}; int bl[8] = {0};
    double r0 = 0; for (long h = 0; h < nh; h++) r0 += odf[h] * odf[h];
    static double r[400];
    for (int L = 20; L <= 110; L++) {                 /* 300 .. 54 BPM at 100 Hz */
        double a = 0; for (long h = L; h < nh; h++) a += odf[h] * odf[h - L];
        r[L] = a / r0;
    }
    printf("%-10s", argv[2]);
    for (int L = 31; L <= 100; L++) {                 /* 194 .. 60 BPM, local maxima */
        if (r[L] > r[L - 1] && r[L] >= r[L + 1]) {
            for (int j = 0; j < 8; j++) if (r[L] > best[j]) {
                memmove(best + j + 1, best + j, (7 - j) * sizeof(double));
                memmove(bl + j + 1, bl + j, (7 - j) * sizeof(int));
                best[j] = r[L]; bl[j] = L; break;
            }
        }
    }
    for (int j = 0; j < 4; j++) if (bl[j]) printf("  %5.1f BPM (r %.2f)", 6000.0 / bl[j], best[j]);
    printf("\n");
    return 0;
}
