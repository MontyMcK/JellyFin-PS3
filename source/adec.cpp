#define MINIMP3_IMPLEMENTATION
#include "minimp3.h"
#include "adec.h"
#include "plog.h"

#include <stdio.h>
#include <string.h>

// ~170 ms of stereo audio at 48 kHz.  Must be a power of two for cheap modulo.
#define PCM_RING_CAP 8192

static mp3dec_t s_dec;
static float    s_ring[PCM_RING_CAP * 2];  // interleaved L/R float32
static int      s_wr = 0;
static int      s_rd = 0;
static int      s_n  = 0;

void adec_init(void) {
    mp3dec_init(&s_dec);
    s_wr = s_rd = s_n = 0;
}

static void push_samples(const short *pcm, int n, int channels) {
    for (int i = 0; i < n; i++) {
        if (s_n >= PCM_RING_CAP) break;
        float l = pcm[i * channels    ] * (1.0f / 32768.0f);
        float r = (channels >= 2) ? pcm[i * channels + 1] * (1.0f / 32768.0f) : l;
        s_ring[s_wr * 2    ] = l;
        s_ring[s_wr * 2 + 1] = r;
        s_wr = (s_wr + 1) & (PCM_RING_CAP - 1);
        s_n++;
    }
}

void adec_push_pes(const u8 *pes, int pes_len) {
    if (pes_len < 9) return;
    if (pes[0] || pes[1] || pes[2] != 0x01) return;
    int hdr  = 9 + pes[8];
    if (hdr >= pes_len) return;
    const u8 *es   = pes + hdr;
    int       left = pes_len - hdr;
    while (left > 0) {
        mp3dec_frame_info_t info;
        short pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
        int samples = mp3dec_decode_frame(&s_dec, es, left, pcm, &info);
        if (info.frame_bytes <= 0) break;
        if (samples > 0) {
            static bool s_logged_frame = false;
            if (!s_logged_frame) {
                s_logged_frame = true;
                char fbuf[64];
                snprintf(fbuf, sizeof(fbuf), "adec_frame: hz=%d ch=%d samples=%d",
                         info.hz, info.channels, samples);
                plog(fbuf);
            }
            push_samples(pcm, samples, info.channels);
        }
        es   += info.frame_bytes;
        left -= info.frame_bytes;
    }
}

int adec_pcm_available(void) { return s_n; }

int adec_read_pcm(float *buf, int n_pairs) {
    int got = 0;
    while (got < n_pairs && s_n > 0) {
        buf[got * 2    ] = s_ring[s_rd * 2    ];
        buf[got * 2 + 1] = s_ring[s_rd * 2 + 1];
        s_rd = (s_rd + 1) & (PCM_RING_CAP - 1);
        s_n--;
        got++;
    }
    return got;
}
