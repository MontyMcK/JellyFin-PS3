/*
 * Host-side end-to-end test of the DTS path: the SAME vendored libdca
 * sources and the SAME dts_map.c the PS3 build uses, compiled for the host,
 * decoding a real DTS 5.1 file in which every speaker carries a distinct
 * sine tone.  A Goertzel detector then asserts each PS3 output slot is
 * dominated by its expected frequency — i.e. the whole
 * syncframe→dca_frame→dca_block→plane-map pipeline routes every channel to
 * the right speaker.  Same shape as test_ac3_decode.c, and for the same
 * reason: DTS's plane order (centre first, LFE last) is the opposite of
 * AC-3's on both counts, and a swapped centre/left is inaudible in a log.
 *
 * Generate the input (frequencies must match FREQS below):
 *   ffmpeg -f lavfi -i "sine=frequency=300:duration=2"  \
 *          ... (see tests/Makefile.host target tones51.dts) ...
 *   (5.1(side) order: FL FR FC LFE SL SR → 300 500 700 60 900 1100)
 *
 * Build & run:  make -f Makefile.host && ./test_dts_decode tones51.dts
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <inttypes.h>

#include "dts_map.h"
#include "dca/dca.h"

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
        fprintf(stderr, "usage: %s tones51.dts\n", argv[0]);
        return 2;
    }
    FILE *fp = fopen(argv[1], "rb");
    if (!fp) { perror(argv[1]); return 2; }
    static uint8_t buf[8 * 1024 * 1024];
    int len = (int)fread(buf, 1, sizeof(buf), fp);
    fclose(fp);

    dca_state_t *st = dca_init(0);
    if (!st) { fprintf(stderr, "dca_init failed\n"); return 2; }

    int off = 0, decoded_frames = 0, granted = -1;
    while (len - off >= 16 && g_frames + 4096 <= MAX_FRAMES) {
        int flags = 0, srate = 0, brate = 0, flen = 0;
        int fsize = dca_syncinfo(st, buf + off, &flags, &srate, &brate, &flen);
        if (fsize <= 0) { off++; continue; }
        if (len - off < fsize) break;

        int f = DCA_3F2R | DCA_LFE | DCA_ADJUST_LEVEL;
        level_t level = 1.0f;
        if (dca_frame(st, buf + off, &f, &level, 0.0f)) { off += 2; continue; }
        /* NO dca_dynrng() here — see adec_dts.cpp: unlike a52_dynrng(), it
         * zeroes state->dynrange, the bitstream flag the subframe parser uses
         * to decide whether to READ the 8-bit dynamic-range coefficient
         * (dca_parse.c:708), which would desync the bit reader. */
        if (decoded_frames == 0) {
            printf("stream: hz=%d bitrate=%d amode=0x%x lfe=%d "
                   "frame_len=%d size=%d granted=0x%x blocks=%d\n",
                   srate, brate, flags & DCA_CHANNEL_MASK,
                   (flags & DCA_LFE) ? 1 : 0, flen, fsize, f,
                   dca_blocks_num(st));
            granted = f;
        }
        int nblocks = dca_blocks_num(st);
        for (int blk = 0; blk < nblocks; blk++) {
            if (dca_block(st)) break;
            if (g_frames + 256 > MAX_FRAMES) break;
            dts_map_block(dca_samples(st), f, g_pcm + g_frames * SLOTS, SLOTS);
            g_frames += 256;
        }
        decoded_frames++;
        off += fsize;
    }
    printf("decoded %d core frames, %d PCM frames\n", decoded_frames, g_frames);
    if (decoded_frames < 10) {
        fprintf(stderr, "FAIL: too few frames decoded\n");
        return 1;
    }
    if ((granted & DCA_CHANNEL_MASK) != DCA_3F2R || !(granted & DCA_LFE)) {
        fprintf(stderr, "FAIL: file did not grant 3F2R|LFE — regenerate input\n");
        return 1;
    }

    /* Output level sanity.  libdca scales its own way (dca_parse.c:259
     * doubles the caller's level, the QMF then divides by sqrt(2)*32768), so
     * a wrong level/bias convention would not misroute anything — it would
     * quietly clip every speaker (too hot) or bury the track (too quiet).
     * This is a gross-error guard only, NOT a gain assertion: the absolute
     * level is the encoder's business.  ffmpeg's experimental `dca` encoder,
     * which generates the input, attenuates by ~18 dB, so tones51.dts decodes
     * to a peak near 0.125 here — and `ffmpeg -i tones51.dts -af astats`
     * reports the same -18.06 dBFS from ffmpeg's own decoder, i.e. libdca
     * agrees with the reference decoder on level as well as routing. */
    float peak = 0.0f;
    for (int i = 0; i < g_frames * SLOTS; i++) {
        float a = g_pcm[i] < 0 ? -g_pcm[i] : g_pcm[i];
        if (a > peak) peak = a;
    }
    printf("peak sample = %.4f (encoder sets the absolute level)\n", (double)peak);
    if (peak > 1.5f || peak < 0.001f) {
        fprintf(stderr, "FAIL: peak %.4f is outside [0.001, 1.5] — the "
                        "level/bias convention is wrong\n", (double)peak);
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
    printf("test_dts_decode: all %d slots correctly routed\n", SLOTS);
    return 0;
}
