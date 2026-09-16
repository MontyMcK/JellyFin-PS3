#pragma once

// Entry points onto the vendored MLP/TrueHD decoder (source/audio/mlp/, see
// PROVENANCE.md there).  Pure C, no PS3 headers.
//
// The decoder is compiled as a single translation unit by mlp_api.c, which
// #includes the vendored .c files: upstream keeps every entry point static
// (it is normally reached through an FFCodec table this app does not build),
// and the vendored directory is deliberately left out of the Makefile's
// SOURCES so nothing else compiles those files.

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Opaque-ish handle: the caller allocates the decoder context (its size is
// mlp_api_context_size()) and an AVCodecContext-shaped header.  Both are
// wrapped here so callers need no libavcodec types.
typedef struct mlp_dec mlp_dec;

// Bytes the caller must provide for a decoder instance.  Allocate once (the
// PS3 path does it with a single static buffer, no malloc in the audio path).
int mlp_api_instance_size(void);

// Initialise an instance in `mem` (mlp_api_instance_size() bytes, zeroed by
// this call).  `pcm` is where decoded audio lands: it must hold
// MLP_API_MAX_SAMPLES * 8 int32_t.  Returns NULL on failure.
#define MLP_API_MAX_SAMPLES 160    // 40 samples << 2 for 192 kHz streams

mlp_dec *mlp_api_open(void *mem, int32_t *pcm);

// Reset the decoder's inter-frame state (seek/flush).
void mlp_api_flush(mlp_dec *d);

// Decode one MLP/TrueHD access unit.  On success returns the number of
// sample FRAMES written to `pcm` (0 if the unit produced none) and fills
// *channels / *sample_rate / *ch_mask with the stream's current format;
// returns < 0 on a decode error.  Samples are interleaved int32 in FFmpeg's
// channel order for `ch_mask` (ascending channel-mask bit order).
int mlp_api_decode(mlp_dec *d, const uint8_t *data, int size,
                   int *channels, int *sample_rate, uint64_t *ch_mask);

#ifdef __cplusplus
}
#endif
