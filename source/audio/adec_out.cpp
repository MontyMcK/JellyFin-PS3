// The end of a decoder: see adec_out.h.

#include "adec_out.h"
#include "adec.h"
#include "chan_map.h"
#include "resample.h"
#include "plog.h"

#include <stdio.h>

static int        s_out_ch  = 2;
static Resampler *s_rs      = NULL;
static int        s_rs_rate = 0;
static int        s_bad_rate = 0;       // the last rate that could not be converted (logged once)

// In slices of 512 frames: from 8 kHz (the largest ratio, x6) that is 3072 frames and the filter's tail,
// which the converted buffer holds.
#define SLICE 512

static void drop_resampler(void) {
    if (s_rs) { resample_close(s_rs); s_rs = NULL; }
    s_rs_rate = 0;
}

void adec_out_open(int out_channels) {
    drop_resampler();
    s_out_ch = out_channels == 6 ? 6 : 2;
    s_bad_rate = 0;
}

void adec_out_close(void) { drop_resampler(); }

void adec_out_reset(void) { if (s_rs) resample_reset(s_rs); }

void adec_out_push(const float *in, int frames, int ch, const unsigned char *pos, int rate) {
    if (frames <= 0 || ch <= 0) return;
    static float mapped[SLICE * 6];
    static float converted[4096 * 6];
    if (rate != 48000 && (!s_rs || s_rs_rate != rate)) {
        drop_resampler();
        s_rs = resample_open(rate, s_out_ch);
        s_rs_rate = rate;
        if (!s_rs && s_bad_rate != rate) {
            s_bad_rate = rate;
            char b[64];
            snprintf(b, sizeof(b), "adec_out: cannot convert %d Hz, silent", rate);
            plog(b);
        }
    }
    if (rate != 48000 && !s_rs) return;
    for (int at = 0; at < frames; at += SLICE) {
        const int n = frames - at < SLICE ? frames - at : SLICE;
        chan_map_frames(in + (size_t)at * (size_t)ch, n, ch, pos, mapped, s_out_ch);
        if (rate == 48000) {
            adec_push_frames(mapped, n);
        } else {
            const int made = resample_process(s_rs, mapped, n, converted, 4096);
            if (made > 0) adec_push_frames(converted, made);
        }
    }
}
