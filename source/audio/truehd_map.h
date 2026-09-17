#pragma once

// FFmpeg-ordered TrueHD output → PS3 CellAudio channel order.
//
// Pure C, no PS3 headers, so tests/test_truehd_map.c can exercise every
// layout on the host.  Same job as ac3_map.c and dts_map.c, but the input
// differs in two ways that make it its own file: the vendored MLP decoder
// emits **interleaved int32**, not planar float, and its channel order is
// FFmpeg's (ascending channel-mask bit order) rather than a codec-specific
// plane order.
//
// PS3 8-channel frame order (derived in ac3_map.c from movian's ps3_audio.c):
//     0=FL 1=FR 2=FC 3=LFE 4=SL 5=SR 6=BL 7=BR
// A TrueHD 7.1 track fills all eight; 5.1 fills the first six and leaves
// 6/7 silent; stereo fills 0/1.

#include <stdint.h>
#include <stddef.h>   /* size_t — newlib's stdint.h does not pull it in */

#ifdef __cplusplus
extern "C" {
#endif

#define TRUEHD_MAX_SLOTS 8

// A built map: for each PS3 slot, which source channels contribute and at
// what gain.  Four contributors covers the widest case, a 7.1 mix folded to
// stereo (front + centre + surround + rear); everything else uses one or two.
#define TRUEHD_MAX_SRC 4

typedef struct {
    int   src[TRUEHD_MAX_SLOTS][TRUEHD_MAX_SRC];   // source index, -1 = unused
    float gain[TRUEHD_MAX_SLOTS][TRUEHD_MAX_SRC];
    int   out_ch;                     // 2, 6 or 8 — what map_block writes
} truehd_map_t;

// Build the map for a decoded layout.
//   ch_mask  : FFmpeg channel mask the decoder reported
//   nb_ch    : channels the decoder is emitting (must match the mask)
//   out_ch   : 8 (7.1 program), 6 (5.1 program) or 2 (stereo port)
// Returns 0 on success, -1 if the layout is unusable (map left silent).
int truehd_map_build(uint64_t ch_mask, int nb_ch, int out_ch,
                     truehd_map_t *m);

// Convert n interleaved int32 frames of nb_ch channels into n interleaved
// float frames of m->out_ch channels in PS3 order.  int32 full scale maps to
// +-1.0f.  Unused slots are written as 0.0f every frame.
void truehd_map_block(const int32_t *in, int nb_ch, int n,
                      const truehd_map_t *m, float *out);

// As above, but reading PLANAR input: planes[c] is a contiguous run of n
// int32 samples for source channel c.
//
// This exists for the vendored DTS-HD decoder (source/audio/dcahd/), which
// emits S32P rather than interleaved.  It shares this map file rather than
// getting its own because the CHANNEL ORDER is identical: both FFmpeg
// decoders number their output in ascending channel-mask bit order, so the
// map truehd_map_build() produces is correct for either one.  Only the
// memory layout differs, which is all this function changes.
void truehd_map_block_planar(const int32_t *const *planes, int nb_ch, int n,
                             const truehd_map_t *m, float *out);

#ifdef __cplusplus
}
#endif
