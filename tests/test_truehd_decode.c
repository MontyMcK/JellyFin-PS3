/*
 * Host-side end-to-end test of the TrueHD path: the SAME vendored FFmpeg MLP
 * decoder, the SAME framing (truehd_stream.c) and the SAME channel map
 * (truehd_map.c) the PS3 build compiles, decoding a real TrueHD file in which
 * every speaker carries a distinct sine tone.
 *
 * TrueHD is LOSSLESS, which makes this test unusually strict: as well as
 * checking that each PS3 slot is dominated by its own tone, it decodes the
 * file a second time through ffmpeg and requires the samples to match
 * BIT-FOR-BIT (after undoing the channel map).  A lossless decoder that is
 * even one LSB off is wrong, and this catches that.
 *
 * Generate the inputs (see tests/Makefile.host target tones51.thd):
 *   ffmpeg ... -c:a truehd -strict -2 -f truehd tones51.thd
 *   ffmpeg -i tones51.thd -c:a pcm_s32le -f s32le tones51.thd.ref
 *
 * Build & run:  make -f Makefile.host && ./test_truehd_decode tones51.thd tones51.thd.ref
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <inttypes.h>

#include "truehd_stream.h"
#include "truehd_map.h"
#include "mlp_api.h"

#define SLOTS 6
static const char  *SLOT_NAME[SLOTS] = {"FL", "FR", "FC", "LFE", "SL", "SR"};
static const double FREQS[SLOTS]     = {300, 500, 700, 60, 900, 1100};

#define MAX_FRAMES (48000 * 4)
static float g_pcm[MAX_FRAMES * SLOTS];
static int   g_frames = 0;

static void sink(void *user, const float *frames, int n) {
    (void)user;
    if (g_frames + n > MAX_FRAMES) return;
    memcpy(g_pcm + (size_t)g_frames * SLOTS, frames,
           (size_t)n * SLOTS * sizeof(float));
    g_frames += n;
}

static double goertzel(int s, double f) {
    double w  = 2.0 * M_PI * f / 48000.0;
    double cw = 2.0 * cos(w);
    double s0, s1 = 0, s2 = 0;
    for (int i = 0; i < g_frames; i++) {
        s0 = g_pcm[i * SLOTS + s] + cw * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    return s1 * s1 + s2 * s2 - cw * s1 * s2;
}

static uint8_t  g_in[32 * 1024 * 1024];
static int32_t  g_ref[MAX_FRAMES * SLOTS];
static uint8_t  g_dec_mem[1 << 20];
static truehd_stream_t g_strm;

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: %s tones51.thd tones51.thd.ref\n", argv[0]);
        return 2;
    }
    FILE *fp = fopen(argv[1], "rb");
    if (!fp) { perror(argv[1]); return 2; }
    int len = (int)fread(g_in, 1, sizeof(g_in), fp);
    fclose(fp);

    if ((int)sizeof(g_dec_mem) < mlp_api_instance_size()) {
        fprintf(stderr, "FAIL: decoder instance is %d bytes\n",
                mlp_api_instance_size());
        return 1;
    }
    printf("decoder instance = %d bytes, stream reader = %d bytes\n",
           mlp_api_instance_size(), (int)sizeof(truehd_stream_t));

    truehd_stream_t *s = &g_strm;
    if (!truehd_stream_init(s, g_dec_mem, SLOTS, sink, NULL)) {
        fprintf(stderr, "FAIL: decoder init\n");
        return 1;
    }

    /* feed in TS-payload-sized pieces, like the real demux does */
    for (int off = 0; off < len; off += 1316) {
        int n = (len - off < 1316) ? (len - off) : 1316;
        truehd_stream_feed(s, g_in + off, n);
    }
    printf("decoded %u access units (%u bad, %u resync bytes), %d PCM frames, "
           "ch=%d rate=%d mask=0x%llx\n",
           (unsigned)s->units, (unsigned)s->bad_units, (unsigned)s->resyncs,
           g_frames, s->first_channels, s->first_rate,
           (unsigned long long)s->first_mask);

    if (s->units < 100 || g_frames < 48000) {
        fprintf(stderr, "FAIL: too little audio decoded\n");
        return 1;
    }
    if (s->bad_units != 0) {
        fprintf(stderr, "FAIL: %u access units failed to decode\n",
                (unsigned)s->bad_units);
        return 1;
    }
    if (s->first_channels != SLOTS) {
        fprintf(stderr, "FAIL: expected a 5.1 test file, got %d channels\n",
                s->first_channels);
        return 1;
    }

    /* ---- bit-exactness against ffmpeg's own decoder ---- */
    FILE *rf = fopen(argv[2], "rb");
    if (!rf) { perror(argv[2]); return 2; }
    int ref_frames = (int)fread(g_ref, sizeof(int32_t) * SLOTS, MAX_FRAMES, rf);
    fclose(rf);
    printf("reference: %d PCM frames\n", ref_frames);
    if (ref_frames < g_frames) {
        fprintf(stderr, "FAIL: reference is shorter than our decode\n");
        return 1;
    }
    /* The map is an exact permutation for 5.1 (FL FR FC LFE BL BR ->
     * FL FR FC LFE SL SR), so undoing it is a rename, not arithmetic: our
     * float samples must be the reference int32 divided by 2^31, exactly. */
    int mismatches = 0;
    for (int i = 0; i < g_frames && mismatches < 4; i++) {
        for (int c = 0; c < SLOTS; c++) {
            float want = (float)g_ref[i * SLOTS + c] / 2147483648.0f;
            float got  = g_pcm[i * SLOTS + c];
            if (want != got) {
                printf("FAIL frame %d slot %s: got %.9f want %.9f\n",
                       i, SLOT_NAME[c], (double)got, (double)want);
                mismatches++;
            }
        }
    }
    if (mismatches) {
        printf("test_truehd_decode: NOT bit-exact vs ffmpeg\n");
        return 1;
    }
    printf("bit-exact vs ffmpeg over %d frames x %d channels\n",
           g_frames, SLOTS);

    /* ---- per-speaker routing ---- */
    int failures = 0;
    for (int s2 = 0; s2 < SLOTS; s2++) {
        double own = goertzel(s2, FREQS[s2]);
        double worst = 0;
        int worst_f = -1;
        for (int o = 0; o < SLOTS; o++) {
            if (o == s2) continue;
            double p = goertzel(s2, FREQS[o]);
            if (p > worst) { worst = p; worst_f = o; }
        }
        double ratio_db = 10.0 * log10(own / (worst > 0 ? worst : 1e-30));
        printf("%-3s own %.0fHz=%.3e  worst-foreign %.0fHz=%.3e  margin=%.1f dB\n",
               SLOT_NAME[s2], FREQS[s2], own,
               worst_f >= 0 ? FREQS[worst_f] : 0, worst, ratio_db);
        if (ratio_db < 20.0) {
            printf("FAIL: slot %s not dominated by its own tone\n",
                   SLOT_NAME[s2]);
            failures++;
        }
    }
    if (failures) return 1;
    printf("test_truehd_decode: all %d slots correctly routed\n", SLOTS);
    return 0;
}
