#pragma once
// IEC 61937 framing: how a compressed audio frame rides a two-channel,
// 16-bit, 48 kHz PCM link (S/PDIF, and HDMI's "non-PCM" audio).
//
// Each compressed frame becomes one burst that occupies exactly as many
// stereo sample frames as the frame decodes to, so timing is unchanged:
//
//   word 0  Pa = 0xF872   \  sync preamble
//   word 1  Pb = 0x4E1F   /
//   word 2  Pc = burst info: data type (bits 0-4) | type-dependent bits 8-12
//   word 3  Pd = payload length in bits
//   ...     the frame's bytes as big-endian 16-bit words (odd length: the
//           last word's low byte is 0)
//   ...     zeros to the end of the repetition period
//
// Words fill L, R, L, R ... so the burst for an AC-3 frame is 1536 stereo
// frames = 6144 bytes whatever the frame's bitrate.

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define IEC61937_AC3_FRAMES 1536   // stereo sample frames per AC-3 burst

// Pack one complete AC-3 syncframe (starting 0x0B 0x77) into `out`, which
// must hold IEC61937_AC3_FRAMES * 2 int16 samples.  Returns the number of
// stereo frames written (IEC61937_AC3_FRAMES), or 0 if the frame is not
// AC-3 or does not fit a burst.
int iec61937_pack_ac3(const uint8_t *frame, int frame_len, int16_t *out);

#ifdef __cplusplus
}
#endif
