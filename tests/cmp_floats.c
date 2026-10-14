// Compares two files of little-endian float32 samples: how many differ at all and by how much at most.  The
// samples of a little-endian and a big-endian build of the same decoder agree to the last bit except where a
// libm function (the resampler's table) or a fused multiply-add rounds differently; a byte-order bug is not a
// last-bit difference, so a small tolerance separates the two.
//
//   cmp_floats a.bin b.bin tolerance

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static float *load(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); exit(2); }
    fseek(f, 0, SEEK_END);
    const long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *b = (unsigned char *)malloc((size_t)size + 4);
    if (fread(b, 1, (size_t)size, f) != (size_t)size) exit(2);
    fclose(f);
    *n = (size_t)size / 4;
    float *v = (float *)malloc(*n * sizeof(float) + 4);
    for (size_t i = 0; i < *n; i++) {
        const uint32_t u = (uint32_t)b[i * 4] | ((uint32_t)b[i * 4 + 1] << 8) | ((uint32_t)b[i * 4 + 2] << 16) | ((uint32_t)b[i * 4 + 3] << 24);
        memcpy(&v[i], &u, 4);
    }
    free(b);
    return v;
}

int main(int argc, char **argv) {
    if (argc < 4) { fprintf(stderr, "usage: cmp_floats a.bin b.bin tolerance\n"); return 2; }
    size_t na, nb;
    const float *a = load(argv[1], &na), *b = load(argv[2], &nb);
    const double tol = atof(argv[3]);
    if (na != nb) { printf("FAIL: %zu samples against %zu\n", na, nb); return 1; }
    size_t differ = 0;
    double worst = 0;
    for (size_t i = 0; i < na; i++) {
        if (memcmp(&a[i], &b[i], 4) != 0) differ++;
        const double d = fabs((double)a[i] - (double)b[i]);
        if (!(d <= worst)) worst = d;          // (also catches a NaN)
    }
    printf("%zu samples, %zu not bit-identical, largest difference %.3g (tolerance %.3g)\n", na, differ, worst, tol);
    return worst <= tol ? 0 : 1;
}
