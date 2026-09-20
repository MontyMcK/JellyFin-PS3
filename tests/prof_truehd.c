// Profiling driver for the vendored TrueHD/MLP decoder.
//
// The existing test_truehd_decode proves correctness in about a tenth of a
// second, which is far too short for a sampling profiler to say anything.
// This decodes the same fixture over and over so gprof gets tens of thousands
// of samples, and answers one question: which functions is the PPU actually
// inside when lossless audio starves the display thread?
//
//   gcc -I../source/audio/mlp/ff -I../source/audio/mlp/ff/libavcodec \
//       -Wno-dangling-else -I../source/audio -O2 -pg -g \
//       -o prof_truehd prof_truehd.c ../source/audio/truehd_stream.c \
//       ../source/audio/truehd_map.c ../source/audio/mlp_api.c \
//       ../source/audio/mlp_compat.c -lm
//   ./prof_truehd tones51.thd 400 && gprof ./prof_truehd gmon.out

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "truehd_stream.h"

static unsigned long long g_frames = 0;

static void sink(void *user, const float *frames, int n)
{
    (void)user; (void)frames;
    g_frames += (unsigned)n;
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: %s <file.thd> [reps]\n", argv[0]); return 2; }
    int reps = (argc > 2) ? atoi(argv[2]) : 200;

    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 1; }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *buf = malloc((size_t)len);
    if (fread(buf, 1, (size_t)len, f) != (size_t)len) { fprintf(stderr, "short read\n"); return 1; }
    fclose(f);

    void *dec_mem = malloc((size_t)mlp_api_instance_size());
    truehd_stream_t *s = malloc(sizeof(*s));

    for (int r = 0; r < reps; r++) {
        memset(s, 0, sizeof(*s));
        if (!truehd_stream_init(s, dec_mem, 8, sink, NULL)) {
            fprintf(stderr, "decoder init failed\n"); return 1;
        }
        // Feed in 4 KB pieces so the framing state machine is exercised the
        // same way the PES path exercises it, not handed one giant buffer.
        for (long off = 0; off < len; off += 4096) {
            int n = (int)((len - off) < 4096 ? (len - off) : 4096);
            truehd_stream_feed(s, buf + off, n);
        }
    }

    printf("reps=%d  units=%u  emitted_frames=%llu\n", reps, s->units, g_frames);
    return 0;
}
