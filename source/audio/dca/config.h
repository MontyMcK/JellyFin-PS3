/*
 * config.h — hand-written replacement for libdca's autoconf-generated header.
 *
 * libdca is vendored here (see PROVENANCE.md) and built by the app's own
 * Makefile on two targets: powerpc64-ps3-elf (the PS3 PPU, big-endian) and
 * the host compiler for tests/ (x86-64, little-endian).  Everything autoconf
 * used to probe is therefore derived from compiler-provided macros so one
 * header serves both.  Same shape as source/audio/a52/config.h, for the same
 * reasons.
 */
#ifndef LIBDCA_CONFIG_H
#define LIBDCA_CONFIG_H

/* Byte order: bitstream.h reads the stream 32 bits at a time and swaps on
 * little-endian hosts (swab32/swable32 pass through when WORDS_BIGENDIAN).
 * The PPU is big-endian; getting this wrong decodes garbage on every frame,
 * so key it off the compiler instead of hardcoding either way. */
#if defined(__BYTE_ORDER__) && (__BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)
#define WORDS_BIGENDIAN 1
#endif

/* float samples (sample_t = float), not double, not fixed-point — dca.h
 * picks float when neither macro is defined. */
#undef LIBDCA_DOUBLE
#undef LIBDCA_FIXED

/* Both newlib (PS3) and glibc (Linux host tests) provide memalign;
 * dca_parse.c supplies its own prototype, so no header dependency either way.
 * Note the guard in dca_parse.c is `HAVE_MEMALIGN && !__cplusplus` — the
 * vendored decoder is only ever compiled as C here.
 *
 * MinGW's CRT has no memalign (tests built with mingw-w64 fail to link
 * otherwise), so leave it undefined there: dca_parse.c then falls back to
 * plain malloc, whose 16-byte alignment on x86-64 already satisfies the one
 * call site (the 256*12-float sample buffer, dca_parse.c:82). */
#if !defined(__MINGW32__)
#define HAVE_MEMALIGN 1
#endif

#endif /* LIBDCA_CONFIG_H */
