/*
 * config.h — hand-written replacement for a52dec's autoconf-generated header.
 *
 * liba52 is vendored here (see PROVENANCE.md) and built by the app's own
 * Makefile on two targets: powerpc64-ps3-elf (the PS3 PPU, big-endian) and
 * the host compiler for tests/ (x86-64, little-endian).  Everything autoconf
 * used to probe is therefore derived from compiler-provided macros so one
 * header serves both.
 */
#ifndef A52DEC_CONFIG_H
#define A52DEC_CONFIG_H

/* Byte order: bitstream.h swaps 32-bit reads on little-endian hosts and
 * passes them through on big-endian ones.  The PPU is big-endian; getting
 * this wrong produces garbage decode on every frame, so key it off the
 * compiler instead of hardcoding either way. */
#if defined(__BYTE_ORDER__) && (__BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)
#define WORDS_BIGENDIAN 1
#endif

/* float samples (sample_t = float), not double, not fixed-point */
#undef LIBA52_DOUBLE
#undef LIBA52_FIXED
#undef LIBA52_DJBFFT

/* Both newlib (PS3) and glibc (host tests) provide memalign; parse.c
 * supplies its own prototype, so no header dependency either way. */
#define HAVE_MEMALIGN 1

/* gcc alignment attributes work on both targets */
#define ATTRIBUTE_ALIGNED_MAX 64

#endif /* A52DEC_CONFIG_H */
