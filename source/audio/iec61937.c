// IEC 61937 packing -- see iec61937.h.  Host-tested bit-exact against
// ffmpeg's spdif muxer (tests/test_iec61937.c).

#include "iec61937.h"

#include <string.h>

#define IEC61937_PA        0xF872
#define IEC61937_PB        0x4E1F
#define IEC61937_TYPE_AC3  0x01
#define IEC61937_HDR_BYTES 8

int iec61937_pack_ac3(const uint8_t *frame, int frame_len, int16_t *out)
{
	const int burst_bytes = IEC61937_AC3_FRAMES * 2 * 2;
	if (frame_len < 6 || frame[0] != 0x0B || frame[1] != 0x77)
		return 0;
	// bsid above 10 is E-AC-3, which needs a 4x-rate link this is not.
	if ((frame[5] >> 3) > 10)
		return 0;
	if (IEC61937_HDR_BYTES + frame_len > burst_bytes)
		return 0;

	memset(out, 0, (size_t)burst_bytes);
	out[0] = (int16_t)IEC61937_PA;
	out[1] = (int16_t)IEC61937_PB;
	// Type-dependent bits 8-10 carry bsmod, the frame's bitstream mode.
	out[2] = (int16_t)(IEC61937_TYPE_AC3 | ((frame[5] & 0x7) << 8));
	out[3] = (int16_t)(frame_len * 8);

	int16_t *w = out + 4;
	int i = 0;
	for (; i + 1 < frame_len; i += 2)
		*w++ = (int16_t)((frame[i] << 8) | frame[i + 1]);
	if (i < frame_len)
		*w = (int16_t)(frame[i] << 8);
	return IEC61937_AC3_FRAMES;
}
