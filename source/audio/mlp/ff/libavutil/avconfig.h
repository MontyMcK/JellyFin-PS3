/*
 * avconfig.h — hand-written replacement for the header FFmpeg's configure
 * generates (see ../../PROVENANCE.md).
 *
 * MUST agree with ff/config.h: both are included, often in the same
 * translation unit, and AV_HAVE_BIGENDIAN decides whether intreadwrite.h
 * byte-swaps.  Getting the two out of step is not a warning-level problem —
 * on the big-endian PPU a value of 0 here would make the bitstream reader
 * swap bytes it should not, and every frame would decode to noise.  So this
 * derives byte order from the compiler exactly as config.h does, rather than
 * hardcoding the host's answer.
 */
#ifndef AVUTIL_AVCONFIG_H
#define AVUTIL_AVCONFIG_H

#if defined(__BYTE_ORDER__) && (__BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)
#define AV_HAVE_BIGENDIAN 1
#else
#define AV_HAVE_BIGENDIAN 0
#endif

#define AV_HAVE_FAST_UNALIGNED 0

#endif /* AVUTIL_AVCONFIG_H */
