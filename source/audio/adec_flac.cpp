// FLAC decode path -- see adec_flac.h for the module contract.

#include "adec_flac.h"
#include "adec.h"
#include "adec_out.h"
#include "chan_map.h"
#include "flac_dec.h"
#include "plog.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// What is held while a frame is incomplete.  A frame of 24-bit 8-channel audio at 4608 samples is up to
// ~110 KB (the carry holds one and the start of the next).
#define FLAC_CARRY_BYTES  (192 * 1024)
#define SLICE             1024

static u8        *s_carry     = NULL;
static int        s_carry_len = 0;
static FlacInfo   s_si;
static bool       s_have      = false;
static int32_t   *s_planes[FLAC_MAX_CHANNELS];   // what flac_decode_frame writes: STREAMINFO's channels, then the scratch plane
static int32_t   *s_mem[FLAC_MAX_CHANNELS + 1];  // what is allocated (the last is the scratch plane)
static int        s_cap       = 0;          // samples a plane holds
static int        s_planes_n  = 0;          // channels allocated
static bool       s_open      = false;
static bool       s_logged    = false;
static u32        s_bad       = 0;

static void free_planes(void) {
    for (int c = 0; c <= FLAC_MAX_CHANNELS; c++) { free(s_mem[c]); s_mem[c] = NULL; }
    for (int c = 0; c < FLAC_MAX_CHANNELS; c++) s_planes[c] = NULL;
    s_cap = 0;
    s_planes_n = 0;
}

bool adec_flac_open(int out_channels) {
    adec_flac_close();
    s_carry = (u8 *)malloc(FLAC_CARRY_BYTES);
    if (!s_carry) { plog("adec_flac: out of memory"); return false; }
    adec_out_open(out_channels);
    s_open = true;
    s_carry_len = 0;
    s_have = false;
    s_logged = false;
    s_bad = 0;
    char b[48];
    snprintf(b, sizeof(b), "adec_flac: open out_ch=%d", out_channels == 6 ? 6 : 2);
    plog(b);
    return true;
}

void adec_flac_close(void) {
    free(s_carry);
    s_carry = NULL;
    free_planes();
    adec_out_close();
    s_open = false;
    s_carry_len = 0;
    s_have = false;
}

void adec_flac_reset(void) {
    s_carry_len = 0;
    s_have = false;
    adec_out_reset();
}

// A decoded frame: planar int32 to interleaved float, on to the output stage.
static void deliver(const FlacFrame &fr) {
    const int ch = fr.channels;
    unsigned char pos[8];
    chan_wave_positions(ch, pos);
    const float scale = 1.0f / (float)(1u << (fr.bps - 1));
    static float stage[SLICE * FLAC_MAX_CHANNELS];
    if (!s_logged) {
        s_logged = true;
        char b[80];
        snprintf(b, sizeof(b), "adec_flac: hz=%d ch=%d bits=%d block=%d", fr.sample_rate, ch, fr.bps, fr.blocksize);
        plog(b);
    }
    for (int at = 0; at < fr.blocksize; at += SLICE) {
        const int n = fr.blocksize - at < SLICE ? fr.blocksize - at : SLICE;
        for (int i = 0; i < n; i++)
            for (int c = 0; c < ch; c++) stage[(size_t)i * (size_t)ch + (size_t)c] = (float)s_planes[c][at + i] * scale;
        adec_out_push(stage, n, ch, pos, fr.sample_rate);
    }
}

static void decode_carry(void) {
    int off = 0;
    for (;;) {
        if (!s_have) {
            // the stream header: "fLaC" and the metadata, ahead of the first frame
            int at = -1;
            for (int i = off; i + 4 <= s_carry_len; i++) if (!memcmp(s_carry + i, "fLaC", 4)) { at = i; break; }
            if (at < 0) { off = s_carry_len > off + 3 ? s_carry_len - 3 : off; break; }
            off = at;
            const int r = flac_parse_header(s_carry + off, s_carry_len - off, &s_si);
            if (r == 0) break;                                           // the rest of it has not arrived
            if (r < 0 || s_si.channels > FLAC_MAX_CHANNELS || s_si.bps < 4 || s_si.bps > 24) { off += 4; continue; }
            const int cap = s_si.max_block > 0 ? s_si.max_block : 4608;
            if (cap > s_cap || s_si.channels > s_planes_n) {
                free_planes();
                for (int c = 0; c <= s_si.channels; c++) {                // one more: the scratch plane
                    s_mem[c] = (int32_t *)malloc(sizeof(int32_t) * (size_t)cap);
                    if (!s_mem[c]) { free_planes(); plog("adec_flac: out of memory"); s_carry_len = 0; return; }
                }
                s_cap = cap;
                s_planes_n = s_si.channels;
            }
            // a frame with more channels than STREAMINFO says (a damaged one that passed its CRC) writes into the scratch plane
            for (int c = 0; c < FLAC_MAX_CHANNELS; c++) s_planes[c] = c < s_si.channels ? s_mem[c] : s_mem[s_planes_n];
            off += r;
            s_have = true;
            continue;
        }
        // the next frame: from its sync code
        while (off + 1 < s_carry_len && !(s_carry[off] == 0xFF && (s_carry[off + 1] & 0xFE) == 0xF8)) off++;
        if (off + 1 >= s_carry_len) {
            if (off < s_carry_len && s_carry[off] != 0xFF) off = s_carry_len;     // (a lone trailing 0xFF may be a sync code's first half)
            break;
        }
        FlacFrame fr;
        const int r = flac_decode_frame(s_carry + off, s_carry_len - off, &s_si, s_planes, s_cap, &fr);
        if (r == 0) break;                                               // the frame is not all here
        if (r < 0) {
            if ((s_bad++ % 128) == 0) {
                char b[64];
                snprintf(b, sizeof(b), "adec_flac: bad frame skipped, total=%u", (unsigned)s_bad);
                plog(b);
            }
            off++;
            continue;
        }
        if (fr.channels > s_si.channels) { off += r; continue; }       // not the stream's: dropped
        deliver(fr);
        off += r;
    }
    if (off > 0) {
        if (off > s_carry_len) off = s_carry_len;
        memmove(s_carry, s_carry + off, (size_t)(s_carry_len - off));
        s_carry_len -= off;
    }
}

void adec_flac_decode_payload(const u8 *es, int len) {
    if (!s_open || len <= 0) return;
    while (len > 0) {
        int space = FLAC_CARRY_BYTES - s_carry_len;
        if (space == 0) {
            plog("adec_flac: carry overflow, resync");               // a frame that never completes: start again
            s_carry_len = 0;
            space = FLAC_CARRY_BYTES;
        }
        const int take = len < space ? len : space;
        memcpy(s_carry + s_carry_len, es, (size_t)take);
        s_carry_len += take;
        es += take;
        len -= take;
        decode_carry();
    }
}
