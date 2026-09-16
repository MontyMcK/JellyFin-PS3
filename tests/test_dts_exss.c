/*
 * Host-side unit test for the DTS-HD / DTS:X extension substream header
 * sizing (source/audio/dts_exss.c).  The decoder uses this number to jump
 * over every extension substream in the stream; a wrong one lands mid-audio
 * and the core decoder then resyncs on whatever it finds, which sounds like
 * intermittent dropouts rather than like a bug in a header parser.
 *
 * Build & run:  make -f Makefile.host && ./test_dts_exss
 */
#include <stdio.h>
#include <string.h>

#include "dts_exss.h"

static int g_failures = 0;

static void expect_ok(const char *name, const uint8_t *p, int avail,
                      uint32_t want) {
    uint32_t got = 0;
    if (!dts_exss_size(p, avail, &got)) {
        printf("FAIL %s: rejected a valid header\n", name);
        g_failures++;
        return;
    }
    if (got != want) {
        printf("FAIL %s: size %u, want %u\n", (const char *)name,
               (unsigned)got, (unsigned)want);
        g_failures++;
    }
}

static void expect_reject(const char *name, const uint8_t *p, int avail) {
    uint32_t got = 0;
    if (dts_exss_size(p, avail, &got)) {
        printf("FAIL %s: accepted, size %u\n", name, (unsigned)got);
        g_failures++;
    }
}

/* Build a header with the given wide flag, header size and substream size.
 * Mirrors the field order in dts_exss.c (and ffmpeg's ff_dca_exss_parse):
 * sync32, user8, index2, wide1, header(8+4w), size(16+4w) — each stored as
 * value-1. */
static void build(uint8_t *buf, int wide, uint32_t header_size,
                  uint32_t exss_size) {
    memset(buf, 0, 16);
    /* bit writer, MSB first */
    int pos = 0;
    uint32_t fields[6];
    int      widths[6];
    fields[0] = 0x64582025u; widths[0] = 32;   /* sync */
    fields[1] = 0;           widths[1] = 8;    /* user defined bits */
    fields[2] = 0;           widths[2] = 2;    /* substream index */
    fields[3] = (uint32_t)wide; widths[3] = 1;
    fields[4] = header_size - 1; widths[4] = 8  + 4 * wide;
    fields[5] = exss_size  - 1;  widths[5] = 16 + 4 * wide;
    for (int f = 0; f < 6; f++) {
        for (int b = widths[f] - 1; b >= 0; b--) {
            uint32_t bit = (fields[f] >> b) & 1u;
            buf[pos >> 3] |= (uint8_t)(bit << (7 - (pos & 7)));
            pos++;
        }
    }
}

int main(void) {
    uint8_t buf[32];

    /* Short header form (the common one): 14-byte header, 2048-byte
     * substream. */
    build(buf, 0, 14, 2048);
    expect_ok("short", buf, sizeof(buf), 2048);

    /* Wide header form: the extra 4 bits per field push the maxima up. */
    build(buf, 1, 32, 300000);
    expect_ok("wide", buf, sizeof(buf), 300000);

    /* Maximum sizes representable in each form — the skip must not wrap. */
    build(buf, 0, 256, 65536);
    expect_ok("short-max", buf, sizeof(buf), 65536);
    build(buf, 1, 4096, 1048576);
    expect_ok("wide-max", buf, sizeof(buf), 1048576);

    /* Too few bytes to decide: must report "not yet", not guess.  The
     * decoder relies on this to keep buffering instead of skipping wildly. */
    build(buf, 0, 14, 2048);
    expect_reject("truncated", buf, DTS_EXSS_MIN_HEADER - 1);

    /* Not a substream at all (wrong sync word) */
    build(buf, 0, 14, 2048);
    buf[1] ^= 0xFF;
    expect_reject("bad-sync", buf, sizeof(buf));

    /* Implausible: substream smaller than its own header.  A random byte
     * pattern that happens to match the sync word usually looks like this,
     * and accepting it would skip a wild distance through real audio. */
    build(buf, 0, 200, 8);
    expect_reject("size<header", buf, sizeof(buf));

    /* Implausible: header shorter than the fields already read. */
    build(buf, 0, 4, 2048);
    expect_reject("header-too-small", buf, sizeof(buf));

    if (g_failures) {
        printf("test_dts_exss: %d FAILURES\n", g_failures);
        return 1;
    }
    printf("test_dts_exss: extension substream sizing correct\n");
    return 0;
}
