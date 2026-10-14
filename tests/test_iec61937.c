// IEC 61937 AC-3 bursts vs ffmpeg's spdif muxer, byte for byte.
//
//   test_iec61937 input.ac3 reference.spdif
//
// reference.spdif comes from `ffmpeg -i input.ac3 -c copy -f spdif`, which
// writes little-endian 16-bit words -- the same bytes this host's int16
// array holds, so the comparison is a plain memcmp per burst.
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "a52.h"
#include "iec61937.h"

static uint8_t *slurp(const char *path, long *len)
{
	FILE *f = fopen(path, "rb");
	if (!f) { perror(path); exit(2); }
	fseek(f, 0, SEEK_END);
	*len = ftell(f);
	fseek(f, 0, SEEK_SET);
	uint8_t *b = malloc((size_t)*len);
	if (fread(b, 1, (size_t)*len, f) != (size_t)*len) { perror(path); exit(2); }
	fclose(f);
	return b;
}

int main(int argc, char **argv)
{
	if (argc != 3) { fprintf(stderr, "usage: %s in.ac3 ref.spdif\n", argv[0]); return 2; }
	long alen, rlen;
	uint8_t *ac3 = slurp(argv[1], &alen);
	uint8_t *ref = slurp(argv[2], &rlen);
	static int16_t burst[IEC61937_AC3_FRAMES * 2];
	const long bb = (long)sizeof(burst);

	long off = 0, roff = 0;
	int frames = 0, bad = 0;
	while (off + 7 <= alen) {
		int flags, sr, br;
		int fl = a52_syncinfo(ac3 + off, &flags, &sr, &br);
		if (fl <= 0 || off + fl > alen) break;
		int n = iec61937_pack_ac3(ac3 + off, fl, burst);
		if (n != IEC61937_AC3_FRAMES) { printf("frame %d: pack returned %d\n", frames, n); return 1; }
		if (roff + bb > rlen) { printf("reference ran out at frame %d\n", frames); return 1; }
		if (memcmp(burst, ref + roff, (size_t)bb) != 0) {
			if (bad++ < 3) {
				const int16_t *r = (const int16_t *)(ref + roff);
				printf("frame %d differs: ours %04x %04x %04x %04x, ffmpeg %04x %04x %04x %04x\n",
				       frames, (uint16_t)burst[0], (uint16_t)burst[1], (uint16_t)burst[2],
				       (uint16_t)burst[3], (uint16_t)r[0], (uint16_t)r[1], (uint16_t)r[2],
				       (uint16_t)r[3]);
			}
		}
		off += fl;
		roff += bb;
		frames++;
	}
	if (roff != rlen) printf("reference has %ld extra bytes\n", rlen - roff);
	// Things the packer must refuse.
	uint8_t junk[16] = { 0x12, 0x34 };
	if (iec61937_pack_ac3(junk, 16, burst) != 0) { printf("accepted a non-AC-3 frame\n"); return 1; }
	uint8_t eac3[16] = { 0x0B, 0x77, 0, 0, 0, 16 << 3 };
	if (iec61937_pack_ac3(eac3, 16, burst) != 0) { printf("accepted E-AC-3\n"); return 1; }

	printf("%d frames, %d mismatched, %s\n", frames, bad,
	       (!bad && frames > 0 && roff == rlen) ? "PASS" : "FAIL");
	return (!bad && frames > 0 && roff == rlen) ? 0 : 1;
}
