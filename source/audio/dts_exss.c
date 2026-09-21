// DTS-HD / DTS:X extension substream header sizing.  See dts_exss.h.

#include "dts_exss.h"

// ---- Field layout (derived, not recalled) ----
//
// From ffmpeg's ff_dca_exss_parse(), libavcodec/dca_exss.c, which reads the
// asset header in this exact order (matching ETSI TS 102 114 / the DTS-HD
// substream specification):
//
//   32  SYNCEXSSH          extension substream sync word (0x64582025)
//    8  UserDefinedBits    (skipped)
//    2  nExtSSIndex
//    1  bHeaderSizeType    0 = short header, 1 = wide header
//   8+4*wide  nuBits4Header    → header size in bytes  = value + 1
//  16+4*wide  nuBits4ExSSFsize → substream size in bytes = value + 1
//
// So the widest case needs 32+8+2+1+12+20 = 75 bits = 10 bytes readable,
// which is DTS_EXSS_MIN_HEADER.  The substream size counts from the sync
// word, so it is exactly the distance to the next substream.
//
// Everything after these fields (static fields, mixing metadata, asset
// descriptors — where the DTS:X object metadata lives) is deliberately not
// parsed: the decoder skips the whole substream, and the track's HD/:X
// identity comes from the server's stream metadata, which is authoritative
// and free.  See docs/dts-hd.md §4.

// MSB-first bit reader over a byte buffer.  No bounds checks: the caller
// guarantees DTS_EXSS_MIN_HEADER readable bytes, and this reader never
// advances past 75 bits.
typedef struct {
    const uint8_t *buf;
    int            bitpos;
} exss_bits;

static uint32_t exss_get(exss_bits *b, int nbits) {
    uint32_t v = 0;
    for (int i = 0; i < nbits; i++) {
        int byte = b->bitpos >> 3;
        int bit  = 7 - (b->bitpos & 7);
        v = (v << 1) | ((b->buf[byte] >> bit) & 1u);
        b->bitpos++;
    }
    return v;
}

bool dts_exss_size(const uint8_t *p, int avail, uint32_t *out_size)
{
    if (avail < DTS_EXSS_MIN_HEADER || !out_size)
        return false;
    if (((uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 |
         (uint32_t)p[2] << 8  | (uint32_t)p[3]) != DTS_EXSS_SYNCWORD)
        return false;

    exss_bits b = { p, 32 };          // start just past the sync word
    exss_get(&b, 8);                  // UserDefinedBits
    exss_get(&b, 2);                  // nExtSSIndex
    int wide = (int)exss_get(&b, 1);  // bHeaderSizeType

    uint32_t header_size = exss_get(&b, 8  + 4 * wide) + 1;
    uint32_t exss_size   = exss_get(&b, 16 + 4 * wide) + 1;

    // Plausibility: the substream must contain its own header, and the header
    // must at least cover the fields just read.  A random byte pattern that
    // happens to match the sync word usually fails one of these, and the
    // caller then rescans from the next byte rather than skipping a wild
    // distance through real audio.
    if (header_size < DTS_EXSS_MIN_HEADER || exss_size < header_size)
        return false;

    *out_size = exss_size;
    return true;
}
