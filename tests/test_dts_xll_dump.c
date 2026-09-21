// Dump the vendored DTS decoder's PCM output for a REAL DTS-HD MA file.
//
// Why this exists: test_dts_hd.c says in its own header that it cannot cover
// XLL, because no free DTS-HD MA encoder exists to synthesise a fixture with.
// That is true, but a fixture does not have to be synthesised -- it can be
// EXTRACTED from a real disc rip with `ffmpeg -c:a copy`.  So the lossless
// path that ships to hardware can be tested after all, and this is the
// harness that does it.
//
// It writes raw interleaved samples in the DECODER's own channel order, so
// the result is directly comparable to `ffmpeg -i x.dts -c:a pcm_s32le`.
// Because DTS-HD MA is lossless, that reference is EXACT, not approximate.
//
// Every sample is written explicitly little-endian, byte by byte.  That is
// the entire point: the output file then encodes VALUES, not the host's byte
// order, so an x86 run and a big-endian PPC64 run produce identical bytes
// when -- and only when -- the decoder computes identical numbers.  Diffing
// the two is what turns "sounds wrong on the PS3" into a host-reproducible
// bug.
//
// usage: test_dts_xll_dump <in.dts> <out.s32le>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dca/dca.h"
#include "dts_stream.h"
#include "dcahd_api.h"

static FILE      *g_out;
static dcahd_dec *g_hd;
static uint64_t   g_samples;
static uint64_t   g_frames;
static uint64_t   g_lossless_frames;
static uint64_t   g_core_frames;
static uint64_t   g_checksum;     // position-weighted, endian-independent
static uint64_t   g_mask;
static int        g_channels;
static int        g_rate;

static void put_s32le(int32_t v)
{
    uint32_t u = (uint32_t)v;
    fputc((int)( u        & 0xFF), g_out);
    fputc((int)((u >>  8) & 0xFF), g_out);
    fputc((int)((u >> 16) & 0xFF), g_out);
    fputc((int)((u >> 24) & 0xFF), g_out);
}

static void on_packet(void *user, const uint8_t *pkt, int len)
{
    (void)user;
    int ch = 0, sr = 0, lossless = 0;
    uint64_t mask = 0;

    int n = dcahd_api_decode(g_hd, pkt, len, &ch, &sr, &mask, &lossless);
    if (n <= 0) return;

    g_frames++;
    if (lossless) g_lossless_frames++; else g_core_frames++;
    g_mask     = mask;
    g_channels = ch;
    g_rate     = sr;

    const int32_t *const *planes = dcahd_api_planes(g_hd);
    if (!planes) return;

    for (int k = 0; k < n; k++) {
        for (int c = 0; c < ch; c++) {
            int32_t s = planes[c][k];
            put_s32le(s);
            // Mixing the index in makes the sum sensitive to ORDER, so a
            // channel swap or a shifted plane changes it too.
            g_checksum += (uint64_t)(uint32_t)s * (uint64_t)(g_samples + 1u + (unsigned)c);
        }
        g_samples++;
    }
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        printf("usage: %s <in.dts> <out.s32le>\n", argv[0]);
        return 2;
    }

    FILE *f = fopen(argv[1], "rb");
    if (!f) { printf("cannot open %s\n", argv[1]); return 2; }
    if (fseek(f, 0, SEEK_END) != 0) { printf("seek failed\n"); return 2; }
    long fsz = ftell(f);
    rewind(f);
    if (fsz < 64) { printf("file too small\n"); return 2; }

    uint8_t *es = (uint8_t *)malloc((size_t)fsz);
    if (!es) { printf("oom\n"); return 2; }
    size_t es_len = fread(es, 1, (size_t)fsz, f);
    fclose(f);

    g_out = fopen(argv[2], "wb");
    if (!g_out) { printf("cannot write %s\n", argv[2]); return 2; }

    void *mem = malloc((size_t)dcahd_api_instance_size());
    void *pln = malloc(DCAHD_SAMPLE_BYTES);
    if (!mem || !pln) { printf("oom\n"); return 2; }
    g_hd = dcahd_api_open(mem, pln, (int)DCAHD_SAMPLE_BYTES);
    if (!g_hd) { printf("FAIL: decoder did not open\n"); return 1; }

    dca_state_t *st = dca_init(0);
    static dts_stream_t strm;
    dts_stream_init(&strm, st, 6, NULL, NULL);
    dts_stream_set_packet_mode(&strm, on_packet, NULL);

    // Awkward chunk sizes: the reader must reassemble frames across buffer
    // boundaries, which is where a framing bug hides.
    const int chunks[] = { 1, 7, 61, 512, 4096, 65535 };
    size_t pos = 0;
    for (int ci = 0; pos < es_len; ci++) {
        int want = chunks[ci % (int)(sizeof(chunks) / sizeof(chunks[0]))];
        size_t take = ((size_t)want < es_len - pos) ? (size_t)want : es_len - pos;
        dts_stream_feed(&strm, es + pos, (int)take);
        pos += take;
    }

    fclose(g_out);

    printf("frames=%llu lossless=%llu core=%llu samples=%llu ch=%d rate=%d "
           "mask=0x%llx checksum=0x%016llx\n",
           (unsigned long long)g_frames,
           (unsigned long long)g_lossless_frames,
           (unsigned long long)g_core_frames,
           (unsigned long long)g_samples,
           g_channels, g_rate,
           (unsigned long long)g_mask,
           (unsigned long long)g_checksum);

    if (g_samples == 0) { printf("FAIL: nothing decoded\n"); return 1; }
    return 0;
}
