// What the music decoders make, written out in a byte-order-free form, so that a little-endian build (the host) and a
// big-endian one (the PS3's PPU, run under qemu-ppc64) can be compared byte for byte: tags and stream properties as
// text and every decoded sample as four little-endian bytes in a second file.  Every fixture of tests/fixtures/music is decoded (whole,
// and after a seek), and so is a WAVE file of every sample format built here from a fixed signal.
//
//   make -f Makefile.host endian_music
//
// The decoders read multi-byte numbers out of files with shifts, never by casting a buffer, and this is the test of
// that on the machine that matters: the one place a host test cannot see is a memcpy of bytes into a float.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "local_audio.h"

typedef struct { const uint8_t *p; size_t n; } Mem;

static int mem_read(void *c, uint64_t off, uint8_t *buf, int len) {
    const Mem *m = (const Mem *)c;
    if (off >= m->n) return 0;
    size_t k = m->n - (size_t)off;
    if (k > (size_t)len) k = (size_t)len;
    memcpy(buf, m->p + off, k);
    return (int)k;
}

static FILE *out;       // text: tags, properties, counts
static FILE *bin;       // the samples

static void put_float(float f) {
    uint32_t u;
    memcpy(&u, &f, 4);
    const unsigned char b[4] = { (unsigned char)u, (unsigned char)(u >> 8), (unsigned char)(u >> 16), (unsigned char)(u >> 24) };
    fwrite(b, 1, 4, bin);
}

static void dump(const char *name, const uint8_t *data, size_t size, LaKind kind, uint32_t seek_secs) {
    Mem m = { data, size };
    LaMeta meta;
    fprintf(out, "\n== %s\n", name);
    if (!la_read_meta(mem_read, &m, size, kind, &meta)) { fprintf(out, "no meta\n"); return; }
    fprintf(out, "title='%s' artist='%s' album='%s' track=%d disc=%d dur=%u rate=%d ch=%d bits=%d kbps=%d\n",
            meta.title, meta.artist, meta.album, meta.track_no, meta.disc_no, (unsigned)meta.duration_secs,
            meta.sample_rate, meta.channels, meta.bits, meta.bitrate_kbps);
    fprintf(out, "data_off=%llu data_len=%llu frames=%llu skip=%d gapless=%d toc=%d pic=%llu/%u %s\n",
            (unsigned long long)meta.data_off, (unsigned long long)meta.data_len, (unsigned long long)meta.total_frames,
            meta.mp3_skip, meta.mp3_gapless, meta.mp3_has_toc, (unsigned long long)meta.pic_off, (unsigned)meta.pic_len, meta.pic_mime);
    char line[48];
    la_format_line(&meta, line, sizeof line);
    fprintf(out, "format='%s' can_decode=%d\n", line, la_can_decode(&meta));
    LaDecoder *d = la_open(mem_read, &m, size, &meta);
    if (!d) { fprintf(out, "no decoder\n"); return; }
    if (seek_secs && !la_seek(d, seek_secs)) fprintf(out, "seek failed\n");
    long total = 0;
    float buf[1000 * 2];
    int n;
    fprintf(out, "samples (seek %u) start at byte %ld of the sample file\n", (unsigned)seek_secs, ftell(bin));
    while ((n = la_decode(d, buf, 1000)) > 0) {
        for (int i = 0; i < n * 2; i++) put_float(buf[i]);
        total += n;
    }
    fprintf(out, "\npairs=%ld status=%d\n", total, n);
    la_close(d);
}

// A WAVE file of one sample format from a fixed signal.
static void le16(unsigned char **p, unsigned v) { *(*p)++ = (unsigned char)v; *(*p)++ = (unsigned char)(v >> 8); }
static void le32(unsigned char **p, unsigned v) { le16(p, v & 0xFFFF); le16(p, v >> 16); }

static unsigned char *make_wav(int format, int ch, int rate, int bits, int frames, size_t *size_out) {
    const size_t data_bytes = (size_t)frames * (size_t)ch * (size_t)(bits / 8);
    unsigned char *b = (unsigned char *)malloc(44 + data_bytes + 16);
    unsigned char *p = b;
    memcpy(p, "RIFF", 4); p += 4; le32(&p, (unsigned)(36 + data_bytes)); memcpy(p, "WAVEfmt ", 8); p += 8;
    le32(&p, 16); le16(&p, (unsigned)format); le16(&p, (unsigned)ch); le32(&p, (unsigned)rate);
    le32(&p, (unsigned)(rate * ch * (bits / 8))); le16(&p, (unsigned)(ch * (bits / 8))); le16(&p, (unsigned)bits);
    memcpy(p, "data", 4); p += 4; le32(&p, (unsigned)data_bytes);
    for (int i = 0; i < frames; i++) {
        for (int c = 0; c < ch; c++) {
            // noise from integer arithmetic: every bit of every sample format is exercised, and no libm is involved
            const uint32_t h = ((uint32_t)i * 2654435761u + (uint32_t)c * 40503u) >> 12;
            const double v = 0.6 * ((double)(h & 0x7FFF) / 16384.0 - 1.0);
            if (format == 3 && bits == 32) {
                const float f = (float)v; uint32_t u; memcpy(&u, &f, 4); le32(&p, u);
            } else if (format == 3) {
                const double f = v; uint64_t u; memcpy(&u, &f, 8); le32(&p, (unsigned)u); le32(&p, (unsigned)(u >> 32));
            } else if (bits == 8) *p++ = (unsigned char)lrint(v * 127.0 + 128.0);
            else if (bits == 16) le16(&p, (unsigned)(int16_t)lrint(v * 32767.0));
            else if (bits == 24) { const int x = (int)lrint(v * 8388607.0); *p++ = (unsigned char)x; *p++ = (unsigned char)(x >> 8); *p++ = (unsigned char)(x >> 16); }
            else le32(&p, (unsigned)(int32_t)lrint(v * 2147483647.0));
        }
    }
    *size_out = (size_t)(p - b);
    return b;
}

int main(int argc, char **argv) {
    if (argc < 4) { fprintf(stderr, "usage: %s out.txt out.bin fixtures-dir\n", argv[0]); return 2; }
    out = fopen(argv[1], "wb");
    bin = fopen(argv[2], "wb");
    if (!out || !bin) return 2;
    static const char *const files[] = { "chirp44.flac", "chirp48.flac", "chirp96_24.flac", "surround6.flac", "chirp44_v24.mp3",
                                         "chirp44_v23.mp3", "plain_v1.mp3", "mono22.mp3", "silence_pad.mp3" };
    for (size_t i = 0; i < sizeof files / sizeof files[0]; i++) {
        char path[512];
        snprintf(path, sizeof path, "%s/%s", argv[3], files[i]);
        FILE *f = fopen(path, "rb");
        if (!f) { fprintf(stderr, "cannot open %s\n", path); return 1; }
        fseek(f, 0, SEEK_END);
        const long size = ftell(f);
        fseek(f, 0, SEEK_SET);
        uint8_t *data = (uint8_t *)malloc((size_t)size);
        if (fread(data, 1, (size_t)size, f) != (size_t)size) return 1;
        fclose(f);
        dump(files[i], data, (size_t)size, la_kind_of(files[i]), 0);
        dump(files[i], data, (size_t)size, la_kind_of(files[i]), 1);
        free(data);
    }
    static const struct { int fmt, ch, rate, bits; } wavs[] = {
        { 1, 2, 48000, 8 }, { 1, 2, 48000, 16 }, { 1, 2, 44100, 24 }, { 1, 1, 48000, 32 }, { 3, 2, 48000, 32 },
        { 3, 2, 96000, 64 }, { 1, 6, 48000, 16 }, { 3, 1, 22050, 32 },
    };
    for (size_t i = 0; i < sizeof wavs / sizeof wavs[0]; i++) {
        size_t size;
        unsigned char *w = make_wav(wavs[i].fmt, wavs[i].ch, wavs[i].rate, wavs[i].bits, 6000, &size);
        char name[64];
        snprintf(name, sizeof name, "wav fmt%d ch%d %dHz %dbit.wav", wavs[i].fmt, wavs[i].ch, wavs[i].rate, wavs[i].bits);
        dump(name, w, size, LAF_WAV, 0);
        free(w);
    }
    fclose(out);
    fclose(bin);
    return 0;
}
