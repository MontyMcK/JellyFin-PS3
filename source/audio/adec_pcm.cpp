// PCM decode path -- see adec_pcm.h for the module contract.

#include "adec_pcm.h"
#include "adec.h"
#include "adec_out.h"
#include "chan_map.h"
#include "plog.h"

#include <stdio.h>
#include <string.h>

#define SLICE 512

static bool   s_open  = false;
static bool   s_have  = false;               // a header has been read: the fields below hold
static int    s_ch = 0, s_bits = 0, s_rate = 0;
static bool   s_float = false, s_big = false;
static u8     s_carry[8 * 4];                // a sample frame cut by a chunk boundary (at most 8 channels of 4 bytes less one)
static int    s_carry_len = 0;
static bool   s_logged = false;

bool adec_pcm_open(int out_channels) {
    adec_pcm_close();
    adec_out_open(out_channels);
    s_open = true;
    s_have = false;
    s_carry_len = 0;
    s_logged = false;
    return true;
}

void adec_pcm_close(void) {
    adec_out_close();
    s_open = false;
    s_have = false;
    s_carry_len = 0;
}

void adec_pcm_reset(void) {
    s_carry_len = 0;
    s_have = false;
    adec_out_reset();
}

// One sample at p, as a float.
static inline float sample(const u8 *p) {
    if (s_float) {
        u32 v = s_big ? ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3]
                      : ((u32)p[3] << 24) | ((u32)p[2] << 16) | ((u32)p[1] << 8) | p[0];
        float f;
        memcpy(&f, &v, sizeof f);
        return f;
    }
    switch (s_bits) {
    case 8:  return ((int)p[0] - 128) * (1.0f / 128.0f);                       // 8-bit PCM is unsigned
    case 16: {
        const int v = s_big ? (int16_t)((p[0] << 8) | p[1]) : (int16_t)((p[1] << 8) | p[0]);
        return (float)v * (1.0f / 32768.0f);
    }
    case 24: {
        int v = s_big ? ((int)p[0] << 16) | ((int)p[1] << 8) | p[2] : ((int)p[2] << 16) | ((int)p[1] << 8) | p[0];
        if (v & 0x800000) v -= 0x1000000;
        return (float)v * (1.0f / 8388608.0f);
    }
    default: {
        const int32_t v = s_big ? (int32_t)(((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3])
                                : (int32_t)(((u32)p[3] << 24) | ((u32)p[2] << 16) | ((u32)p[1] << 8) | p[0]);
        return (float)((double)v * (1.0 / 2147483648.0));
    }
    }
}

// n whole sample frames at p, on to the output stage.
static void push_frames(const u8 *p, int n) {
    static float stage[SLICE * 8];
    unsigned char pos[8];
    chan_wave_positions(s_ch, pos);
    const int bps = s_bits / 8;
    for (int at = 0; at < n; at += SLICE) {
        const int k = n - at < SLICE ? n - at : SLICE;
        for (int i = 0; i < k * s_ch; i++) stage[i] = sample(p + ((size_t)at * (size_t)s_ch + (size_t)i) * (size_t)bps);
        adec_out_push(stage, k, s_ch, pos, s_rate);
    }
}

void adec_pcm_decode_payload(const u8 *es, int len, bool head) {
    if (!s_open || len <= 0) return;
    if (head) {
        s_carry_len = 0;
        s_have = false;
        if (len < PCM_HEADER_BYTES || es[0] != 'P' || es[1] != 'C') return;
        const int ch = es[2], bits = es[3], flags = es[4];
        const int rate = (es[5] << 16) | (es[6] << 8) | es[7];
        const bool fl = (flags & PCM_FLAG_FLOAT) != 0;
        if (ch < 1 || ch > 8 || rate <= 0 || !(bits == 8 || bits == 16 || bits == 24 || bits == 32) || (fl && bits != 32)) return;
        s_ch = ch; s_bits = bits; s_rate = rate; s_float = fl; s_big = (flags & PCM_FLAG_BIG) != 0;
        s_have = true;
        if (!s_logged) {
            s_logged = true;
            char b[80];
            snprintf(b, sizeof(b), "adec_pcm: hz=%d ch=%d bits=%d%s%s", rate, ch, bits, fl ? " float" : "", s_big ? " big-endian" : "");
            plog(b);
        }
        es += PCM_HEADER_BYTES;
        len -= PCM_HEADER_BYTES;
    }
    if (!s_have || len <= 0) return;
    const int frame = s_ch * (s_bits / 8);
    // finish a sample frame the last chunk ended in the middle of
    if (s_carry_len > 0) {
        const int need = frame - s_carry_len;
        const int take = len < need ? len : need;
        memcpy(s_carry + s_carry_len, es, (size_t)take);
        s_carry_len += take;
        es += take;
        len -= take;
        if (s_carry_len < frame) return;
        push_frames(s_carry, 1);
        s_carry_len = 0;
    }
    const int whole = len / frame;
    if (whole > 0) push_frames(es, whole);
    const int rest = len - whole * frame;
    if (rest > 0) { memcpy(s_carry, es + (size_t)whole * (size_t)frame, (size_t)rest); s_carry_len = rest; }
}
