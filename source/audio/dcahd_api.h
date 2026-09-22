#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// -------------------------------------------------------------------------
//  Entry points onto the vendored FFmpeg DTS decoder (source/audio/dcahd/)
// -------------------------------------------------------------------------
//  This is the LOSSLESS DTS path: it decodes the XLL extension substream of a
//  DTS-HD Master Audio track, which libdca (source/audio/dca/) cannot reach at
//  all -- see dcahd/PROVENANCE.md.  It also decodes plain DTS core, so it is a
//  superset of the old path rather than an alternative to it.
//
//  Shaped like mlp_api.h on purpose: one caller-owned instance block, one
//  caller-owned sample block, no allocation in the decode path, and output
//  left in the decoder's own channel order for a map to place.
//
//  Output is PLANAR int32 at full scale (the decoder's S32P convention:
//  24-bit data shifted up by 8), one plane per channel, in FFmpeg's native
//  "ascending channel-mask bit order".  That is the SAME convention the
//  TrueHD decoder emits, which is why both share truehd_map.c.

typedef struct dcahd_dec dcahd_dec;

// Ceiling on one decoded frame.  Upstream allows up to 65536 samples, but a
// real DTS-HD MA frame is 512-2048; reserving for the upstream maximum would
// mean a 4 MB block for no benefit.  A frame that asks for more than this is
// REFUSED by the buffer hand-out rather than silently overrunning.
#define DCAHD_MAX_CHANNELS 8
#define DCAHD_MAX_SAMPLES  4096
#define DCAHD_SAMPLE_BYTES ((size_t)DCAHD_MAX_CHANNELS * DCAHD_MAX_SAMPLES * 4)

// Bytes the caller must reserve for the instance (decoder state).
int dcahd_api_instance_size(void);

// `mem`    — DCAHD_API_INSTANCE_SIZE bytes, caller-owned.
// `planes` — DCAHD_SAMPLE_BYTES bytes, caller-owned, holds decoded samples.
dcahd_dec *dcahd_api_open(void *mem, void *planes, int planes_bytes);
void       dcahd_api_close(dcahd_dec *d);
void       dcahd_api_flush(dcahd_dec *d);

// Decode one DTS frame.  Returns the sample count (0 if the frame produced
// none, negative on error).  `lossless` reports whether the samples came from
// the XLL extension (true DTS-HD MA) or only from the lossy core.
int dcahd_api_decode(dcahd_dec *d, const uint8_t *data, int size,
                     int *channels, int *sample_rate, uint64_t *ch_mask,
                     int *lossless);

// Plane pointers for the last successful decode, indexed 0..channels-1 in
// FFmpeg native channel order.  Valid until the next decode or flush.
const int32_t *const *dcahd_api_planes(const dcahd_dec *d);

#ifdef __cplusplus
}
#endif
