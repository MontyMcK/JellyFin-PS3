// End-to-end test of the LOSSLESS DTS path: dts_stream packet mode ->
// vendored FFmpeg decoder (source/audio/dcahd/) -> truehd_map -> PS3 order.
//
// Method is the same one test_dts_decode.c and test_ac3_decode.c use, because
// it is the one that actually catches channel-order mistakes: encode a file
// with a DISTINCT TONE PER SPEAKER, decode it through the real path, and
// assert every PS3 slot is dominated by its own tone.  A transposed pair or
// an off-by-one in the map shows up immediately; a bit-comparison against a
// reference decode would not tell you WHICH channel moved.
//
// SCOPE, stated honestly: ffmpeg's `dca` encoder produces DTS CORE only --
// there is no free DTS-HD MA encoder in existence -- so this exercises the
// framing, the API, the fixed-point output path and the channel map, but it
// cannot synthesise an XLL extension substream. XLL itself is verified by
// playing a real DTS-HD MA track and reading `ch peak` in the stats overlay.
//
// usage: test_dts_hd tones51.dts

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dca/dca.h"
#include "dts_stream.h"
#include "dcahd_api.h"
#include "truehd_map.h"

#define SR        48000
#define OUT_CH    6
#define MAXFRAMES (SR * 4)

// Tone assigned to each PS3 slot by the generator below: FL FR FC LFE SL SR.
static const double kTone[OUT_CH] = { 400, 700, 1100, 60, 1900, 2600 };

static float  *g_out;
static size_t  g_out_n;

static dcahd_dec  *g_hd;
static truehd_map_t g_map;
static uint64_t     g_mask;

static void on_packet(void *user, const uint8_t *pkt, int len)
{
    (void)user;
    int ch = 0, sr = 0, lossless = 0;
    uint64_t mask = 0;
    int n = dcahd_api_decode(g_hd, pkt, len, &ch, &sr, &mask, &lossless);
    if (n <= 0) return;

    if (mask != g_mask) {
        if (truehd_map_build(mask, ch, OUT_CH, &g_map) < 0) return;
        g_mask = mask;
    }
    if (g_out_n + (size_t)n > MAXFRAMES) return;

    const int32_t *const *planes = dcahd_api_planes(g_hd);
    const int32_t *slice[8];
    for (int off = 0; off < n; off += 256) {
        int take = (n - off < 256) ? (n - off) : 256;
        for (int c = 0; c < ch && c < 8; c++) slice[c] = planes[c] + off;
        truehd_map_block_planar(slice, ch, take, &g_map,
                                g_out + g_out_n * OUT_CH);
        g_out_n += (size_t)take;
    }
}

// Goertzel magnitude of `freq` in one PS3 slot.
static double tone_power(int slot, double freq, size_t n)
{
    const double w = 2.0 * M_PI * freq / SR;
    const double c = 2.0 * cos(w);
    double s1 = 0, s2 = 0;
    for (size_t i = 0; i < n; i++) {
        double x  = g_out[i * OUT_CH + slot];
        double s0 = x + c * s1 - s2;
        s2 = s1; s1 = s0;
    }
    return s1 * s1 + s2 * s2 - c * s1 * s2;
}

int main(int argc, char **argv)
{
    if (argc < 2) { printf("usage: %s tones51.dts\n", argv[0]); return 2; }

    FILE *f = fopen(argv[1], "rb");
    if (!f) { printf("cannot open %s\n", argv[1]); return 2; }
    static uint8_t es[4 << 20];
    size_t es_len = fread(es, 1, sizeof(es), f);
    fclose(f);
    if (es_len < 64) { printf("file too small\n"); return 2; }

    g_out = calloc(MAXFRAMES * OUT_CH, sizeof(float));
    void *mem = malloc((size_t)dcahd_api_instance_size());
    void *pln = malloc(DCAHD_SAMPLE_BYTES);
    g_hd = dcahd_api_open(mem, pln, (int)DCAHD_SAMPLE_BYTES);
    if (!g_hd) { printf("FAIL: decoder did not open\n"); return 1; }

    dca_state_t *st = dca_init(0);
    static dts_stream_t strm;
    dts_stream_init(&strm, st, OUT_CH, NULL, NULL);
    dts_stream_set_packet_mode(&strm, on_packet, NULL);

    // Feed in awkward chunks: the reader must reassemble frames across
    // boundaries, which is where a framing bug hides.
    const int chunks[] = { 1, 7, 61, 512, 4096, 65535 };
    size_t pos = 0;
    for (int ci = 0; pos < es_len; ci++) {
        int want = chunks[ci % (int)(sizeof(chunks) / sizeof(chunks[0]))];
        size_t take = ((size_t)want < es_len - pos) ? (size_t)want : es_len - pos;
        dts_stream_feed(&strm, es + pos, (int)take);
        pos += take;
    }

    printf("decoded %lu frames, %u packets, mask 0x%llx\n",
           (unsigned long)g_out_n, (unsigned)strm.core_frames,
           (unsigned long long)g_mask);
    if (g_out_n < SR / 4) { printf("FAIL: too little audio decoded\n"); return 1; }

    // Skip the first 100 ms: the encoder's ramp-in is not the thing under test.
    const size_t skip = SR / 10;
    const size_t n    = g_out_n - skip;
    float *base = g_out;
    g_out += skip * OUT_CH;

    int bad = 0;
    for (int slot = 0; slot < OUT_CH; slot++) {
        double own = tone_power(slot, kTone[slot], n);
        double worst_other = 0;
        int    worst_slot  = -1;
        for (int o = 0; o < OUT_CH; o++) {
            if (o == slot) continue;
            double p = tone_power(slot, kTone[o], n);
            if (p > worst_other) { worst_other = p; worst_slot = o; }
        }
        double db = 10.0 * log10((own + 1e-30) / (worst_other + 1e-30));
        printf("  slot %d: own %.1f dB above worst bleed (from slot %d)\n",
               slot, db, worst_slot);
        // 20 dB is a wide margin for "this slot carries its own tone" while
        // tolerating a lossy core's quantisation noise.
        if (db < 20.0) { printf("    ^^ FAIL\n"); bad = 1; }
    }
    g_out = base;

    printf(bad ? "test_dts_hd: FAILED\n"
               : "test_dts_hd: channel mapping correct end to end\n");
    return bad;
}
