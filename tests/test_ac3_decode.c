/*
 * Host-side end-to-end test of the AC-3 path: the SAME vendored liba52
 * sources and the SAME ac3_map.c the PS3 build uses, compiled for the host,
 * decoding a real AC-3 5.1 file in which every speaker carries a distinct
 * sine tone.  A Goertzel detector then asserts each PS3 output slot is
 * dominated by its expected frequency — i.e. the whole
 * syncframe→a52_frame→a52_block→plane-map pipeline routes every channel to
 * the right speaker.
 *
 * Generate the input (frequencies must match FREQS below):
 *   ffmpeg -f lavfi -i "sine=frequency=300:duration=2"  \
 *          -f lavfi -i "sine=frequency=500:duration=2"  \
 *          -f lavfi -i "sine=frequency=700:duration=2"  \
 *          -f lavfi -i "sine=frequency=60:duration=2"   \
 *          -f lavfi -i "sine=frequency=900:duration=2"  \
 *          -f lavfi -i "sine=frequency=1100:duration=2" \
 *          -filter_complex "join=inputs=6:channel_layout=5.1(side):map=0.0-FL|1.0-FR|2.0-FC|3.0-LFE|4.0-SL|5.0-SR[a]" \
 *          -map "[a]" -c:a ac3 -b:a 640k -ar 48000 tones51.ac3
 *   (5.1(side) order: FL FR FC LFE SL SR → 300 500 700 60 900 1100)
 *
 * Build & run:  make -f Makefile.host && ./test_ac3_decode tones51.ac3
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <inttypes.h>

#include "ac3_map.h"
#include "a52/a52.h"

#define SLOTS 6
static const char  *SLOT_NAME[SLOTS] = {"FL", "FR", "FC", "LFE", "SL", "SR"};
static const double FREQS[SLOTS]     = {300, 500, 700, 60, 900, 1100};

#define MAX_FRAMES (48000 * 4)          /* up to 4 s of decoded audio */
static float g_pcm[MAX_FRAMES * SLOTS];
static int   g_frames = 0;

/* Goertzel power of frequency f in slot s over the decoded buffer */
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

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s tones51.ac3\n", argv[0]);
        return 2;
    }
    FILE *fp = fopen(argv[1], "rb");
    if (!fp) { perror(argv[1]); return 2; }
    static uint8_t buf[4 * 1024 * 1024];
    int len = (int)fread(buf, 1, sizeof(buf), fp);
    fclose(fp);

    a52_state_t *st = a52_init(0);
    if (!st) { fprintf(stderr, "a52_init failed\n"); return 2; }

    int off = 0, decoded_frames = 0, granted = -1;
    while (len - off >= 7 && g_frames + 1536 <= MAX_FRAMES) {
        if (buf[off] != 0x0B || buf[off + 1] != 0x77) { off++; continue; }
        int flags = 0, srate = 0, brate = 0;
        int fl = a52_syncinfo(buf + off, &flags, &srate, &brate);
        if (fl <= 0) { off++; continue; }
        if (len - off < fl) break;

        int f = A52_3F2R | A52_LFE | A52_ADJUST_LEVEL;
        sample_t level = 1.0f;
        if (a52_frame(st, buf + off, &f, &level, 0.0f)) { off += 2; continue; }
        a52_dynrng(st, NULL, NULL);
        if (decoded_frames == 0) {
            printf("stream: hz=%d bitrate=%d acmod=0x%x lfe=%d granted=0x%x\n",
                   srate, brate, flags & A52_CHANNEL_MASK,
                   (flags & A52_LFE) ? 1 : 0, f);
            granted = f;
        }
        for (int blk = 0; blk < 6; blk++) {
            if (a52_block(st)) break;
            ac3_map_block(a52_samples(st), f, g_pcm + g_frames * SLOTS, SLOTS);
            g_frames += 256;
        }
        decoded_frames++;
        off += fl;
    }
    printf("decoded %d syncframes, %d PCM frames\n", decoded_frames, g_frames);
    if (decoded_frames < 10) {
        fprintf(stderr, "FAIL: too few frames decoded\n");
        return 1;
    }
    if ((granted & A52_CHANNEL_MASK) != A52_3F2R || !(granted & A52_LFE)) {
        fprintf(stderr, "FAIL: file did not grant 3F2R|LFE — regenerate input\n");
        return 1;
    }

    /* Every slot must be dominated by its own tone: its Goertzel power must
     * exceed every foreign tone's power in that slot by >= 20 dB. */
    int failures = 0;
    for (int s = 0; s < SLOTS; s++) {
        double own = goertzel(s, FREQS[s]);
        double worst = 0;
        int worst_f = -1;
        for (int o = 0; o < SLOTS; o++) {
            if (o == s) continue;
            double p = goertzel(s, FREQS[o]);
            if (p > worst) { worst = p; worst_f = o; }
        }
        double ratio_db = 10.0 * log10(own / (worst > 0 ? worst : 1e-30));
        printf("%-3s own %.0fHz=%.3e  worst-foreign %.0fHz=%.3e  margin=%.1f dB\n",
               SLOT_NAME[s], FREQS[s], own,
               worst_f >= 0 ? FREQS[worst_f] : 0, worst, ratio_db);
        if (ratio_db < 20.0) {
            printf("FAIL: slot %s not dominated by its own tone\n", SLOT_NAME[s]);
            failures++;
        }
    }
    if (failures) return 1;
    printf("test_ac3_decode: all %d slots correctly routed\n", SLOTS);
    return 0;
}
