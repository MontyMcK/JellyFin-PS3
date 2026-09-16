/*
 * Host-side test of the DTS elementary-stream reader (source/audio/dts_stream.c)
 * — the part that has to survive a real DTS-HD stream: core frames with
 * extension substreams wedged between them, arriving in chunks that split
 * frames, headers and substreams at arbitrary byte boundaries (a PES is not
 * aligned to anything the codec cares about).
 *
 * Method: take a real DTS 5.1 core stream, synthesise an extension substream
 * after every core frame to build a DTS-HD-shaped stream, then feed that
 * through the reader at several chunk sizes and assert that
 *   - every core frame is still decoded (none eaten by a mis-sized skip),
 *   - the audio is bit-identical to decoding the plain core stream,
 *   - the reader reports the extension substreams it skipped.
 * Chunking must not change ANY of that, which is the whole point.
 *
 * Build & run:  make -f Makefile.host && ./test_dts_stream tones51.dts
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

#include "dts_stream.h"
#include "dts_exss.h"

#define SLOTS 6
#define MAX_FRAMES (48000 * 4)

typedef struct {
    float *pcm;
    int    frames;
} sink_t;

static void sink_emit(void *user, const float *frames, int n) {
    sink_t *s = (sink_t *)user;
    if (s->frames + n > MAX_FRAMES) return;
    memcpy(s->pcm + (size_t)s->frames * SLOTS, frames,
           (size_t)n * SLOTS * sizeof(float));
    s->frames += n;
}

/* Build a syntactically valid extension substream of `size` bytes: a real
 * header (so dts_exss_size agrees with us about the length) followed by
 * payload bytes chosen to be hostile — including bytes that look like the
 * start of a core sync word, which is exactly what must NOT be resynced on
 * while skipping. */
static void build_exss(uint8_t *buf, uint32_t size) {
    memset(buf, 0, size);
    int wide = (size > 65536) ? 1 : 0;
    uint32_t header_size = 14;
    uint32_t fields[6]; int widths[6];
    fields[0] = 0x64582025u;    widths[0] = 32;
    fields[1] = 0;              widths[1] = 8;
    fields[2] = 0;              widths[2] = 2;
    fields[3] = (uint32_t)wide; widths[3] = 1;
    fields[4] = header_size - 1; widths[4] = 8  + 4 * wide;
    fields[5] = size - 1;        widths[5] = 16 + 4 * wide;
    int pos = 0;
    for (int f = 0; f < 6; f++)
        for (int b = widths[f] - 1; b >= 0; b--) {
            uint32_t bit = (fields[f] >> b) & 1u;
            buf[pos >> 3] |= (uint8_t)(bit << (7 - (pos & 7)));
            pos++;
        }
    for (uint32_t i = header_size; i < size; i++)
        buf[i] = (i % 4 == 0) ? 0x7F : (i % 4 == 1) ? 0xFE
               : (i % 4 == 2) ? 0x80 : 0x01;    /* decoy core sync words */
}

/* Decode a whole buffer through the reader, feeding it in `chunk`-byte
 * pieces (0 = one single feed). */
static int decode_all(const uint8_t *buf, int len, int chunk, sink_t *sink,
                      dts_stream_t *out_state) {
    dca_state_t *st = dca_init(0);
    if (!st) return -1;
    static dts_stream_t s;
    dts_stream_init(&s, st, SLOTS, sink_emit, sink);
    sink->frames = 0;
    if (chunk <= 0) chunk = len;
    for (int off = 0; off < len; off += chunk) {
        int n = (len - off < chunk) ? (len - off) : chunk;
        dts_stream_feed(&s, buf + off, n);
    }
    if (out_state) *out_state = s;
    dca_free(st);
    return 0;
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s tones51.dts\n", argv[0]);
        return 2;
    }
    FILE *fp = fopen(argv[1], "rb");
    if (!fp) { perror(argv[1]); return 2; }
    static uint8_t core[8 * 1024 * 1024];
    int core_len = (int)fread(core, 1, sizeof(core), fp);
    fclose(fp);

    /* ---- reference: the plain core stream, fed in one piece ---- */
    static float ref_pcm[MAX_FRAMES * SLOTS];
    sink_t ref = { ref_pcm, 0 };
    static dts_stream_t ref_state;
    if (decode_all(core, core_len, 0, &ref, &ref_state)) {
        fprintf(stderr, "dca_init failed\n");
        return 2;
    }
    printf("core stream: %u frames decoded, %d PCM frames, bad=%u\n",
           (unsigned)ref_state.core_frames, ref.frames,
           (unsigned)ref_state.bad_frames);
    if (ref_state.core_frames < 10 || ref.frames == 0) {
        fprintf(stderr, "FAIL: reference decode produced nothing\n");
        return 1;
    }
    if (ref_state.ext_count != 0) {
        fprintf(stderr, "FAIL: plain core stream reported %u extensions\n",
                (unsigned)ref_state.ext_count);
        return 1;
    }

    /* ---- build a DTS-HD-shaped stream: core frame, extension, core... ---- */
    static uint8_t hd[16 * 1024 * 1024];
    int hd_len = 0;
    uint32_t ext_total = 0, ext_n = 0;
    {
        dca_state_t *st = dca_init(0);
        int off = 0;
        /* extension sizes cycle, including one larger than the reader's
         * 32 KB carry buffer, which must be skipped by counter and never
         * buffered */
        const uint32_t sizes[] = { 512, 4096, 40000, 1024 };
        int si = 0;
        while (off < core_len) {
            int flags = 0, sr = 0, br = 0, fl = 0;
            int fsize = dca_syncinfo(st, core + off, &flags, &sr, &br, &fl);
            if (fsize <= 0 || off + fsize > core_len) break;
            if (hd_len + fsize > (int)sizeof(hd)) break;
            memcpy(hd + hd_len, core + off, (size_t)fsize);
            hd_len += fsize;
            off    += fsize;
            uint32_t es = sizes[si++ % 4];
            if (hd_len + (int)es > (int)sizeof(hd)) break;
            build_exss(hd + hd_len, es);
            hd_len    += (int)es;
            ext_total += es;
            ext_n++;
        }
        dca_free(st);
    }
    printf("synthesised DTS-HD stream: %d bytes, %u extension substreams "
           "(%u bytes)\n", hd_len, (unsigned)ext_n, (unsigned)ext_total);
    if (ext_n < 10) {
        fprintf(stderr, "FAIL: could not build the test stream\n");
        return 1;
    }

    /* ---- feed it at several chunk sizes; all must match the reference ---- */
    static float pcm[MAX_FRAMES * SLOTS];
    const int chunks[] = { 0, 1, 7, 188, 1316, 8192, 65536 };
    int failures = 0;
    for (size_t c = 0; c < sizeof(chunks) / sizeof(chunks[0]); c++) {
        sink_t got = { pcm, 0 };
        static dts_stream_t st;
        decode_all(hd, hd_len, chunks[c], &got, &st);
        const char *tag = chunks[c] ? "chunked" : "one feed";
        printf("%-8s chunk=%-6d core=%-4u ext=%-4u bad=%-3u pcm=%d\n",
               tag, chunks[c], (unsigned)st.core_frames,
               (unsigned)st.ext_count, (unsigned)st.bad_frames, got.frames);
        if (st.core_frames != ref_state.core_frames) {
            printf("FAIL chunk=%d: decoded %u core frames, want %u\n",
                   chunks[c], (unsigned)st.core_frames,
                   (unsigned)ref_state.core_frames);
            failures++;
        }
        if (st.ext_count != ext_n || st.ext_bytes != ext_total) {
            printf("FAIL chunk=%d: saw %u extensions (%u bytes), want %u (%u)\n",
                   chunks[c], (unsigned)st.ext_count, (unsigned)st.ext_bytes,
                   (unsigned)ext_n, (unsigned)ext_total);
            failures++;
        }
        if (st.bad_frames != 0) {
            printf("FAIL chunk=%d: %u bad frames — the skip landed in audio\n",
                   chunks[c], (unsigned)st.bad_frames);
            failures++;
        }
        if (got.frames != ref.frames) {
            printf("FAIL chunk=%d: %d PCM frames, want %d\n",
                   chunks[c], got.frames, ref.frames);
            failures++;
        } else if (memcmp(pcm, ref_pcm,
                          (size_t)got.frames * SLOTS * sizeof(float)) != 0) {
            printf("FAIL chunk=%d: PCM differs from the core-only decode\n",
                   chunks[c]);
            failures++;
        }
    }

    if (failures) {
        printf("test_dts_stream: %d FAILURES\n", failures);
        return 1;
    }
    printf("test_dts_stream: extension substreams skipped cleanly at every "
           "chunk size\n");
    return 0;
}
