/*
 * config.h — hand-written replacement for ffmpeg's configure-generated
 * header, for the vendored MLP/TrueHD decoder (see ../PROVENANCE.md).
 *
 * Built on two targets: powerpc64-ps3-elf (the PS3 PPU, big-endian) and the
 * host compiler for tests/ (x86-64, little-endian).  Everything configure
 * probes that this decoder actually reads is derived from compiler macros so
 * one header serves both — the same approach as source/audio/a52/config.h
 * and source/audio/dca/config.h.
 */
#ifndef JF_MLP_CONFIG_H
#define JF_MLP_CONFIG_H

/* Byte order.  get_bits.h and intreadwrite.h read the bitstream 32/64 bits at
 * a time and byte-swap according to these; the PPU is big-endian and getting
 * this wrong decodes garbage on every frame. */
#if defined(__BYTE_ORDER__) && (__BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)
#define HAVE_BIGENDIAN    1
#else
#define HAVE_BIGENDIAN    0
#endif

/* The PPU traps on unaligned 4/8-byte loads through its scalar unit in the
 * cases ffmpeg's "fast unaligned" paths assume are free, and the host build
 * gains nothing measurable here, so take the portable path on both. */
#define HAVE_FAST_UNALIGNED 0

/* AV_HAVE_BIGENDIAN / AV_HAVE_FAST_UNALIGNED are the *public* spellings and
 * belong to libavutil/avconfig.h, which is included independently of this
 * header.  Defining them here as well produced a redefinition warning on the
 * PPU build; worse, it invited the two headers to disagree about byte order,
 * which would decode noise rather than fail loudly.  avconfig.h derives them
 * from the same compiler macro this file uses. */

/* Both targets are 64-bit; neither has a fast count-leading-zeros intrinsic
 * wired up in this cut-down tree. */
#define HAVE_FAST_64BIT 1
#define HAVE_FAST_CLZ   0

/* No SIMD, no inline asm, no architecture-specific DSP init: this tree
 * vendors only the portable C decoder (ff_mlpdsp_init's ARCH_* branches are
 * compiled out, so no ppc/x86 DSP files are needed). */
#define HAVE_INLINE_ASM    0
#define HAVE_MMX           0
#define HAVE_SIMD_ALIGN_16 0
#define HAVE_SIMD_ALIGN_32 0
#define HAVE_SIMD_ALIGN_64 0
#define ARCH_X86      0
#define ARCH_X86_32   0
#define ARCH_X86_64   0
#define ARCH_PPC      0
#define ARCH_ARM      0
#define ARCH_AARCH64  0
#define ARCH_MIPS     0
#define ARCH_RISCV    0

/* The decoder runs on one thread (the app's adec thread); libavutil/thread.h
 * then supplies its single-threaded ff_thread_once. */
#define HAVE_THREADS 0

/* The FFCodec descriptor tables at the bottom of mlpdec.c are the only thing
 * these gate, and this app calls the decoder directly (mlp_api.c) instead of
 * going through libavcodec's codec table — so compile them out. */
#define CONFIG_MLP_DECODER    0
#define CONFIG_TRUEHD_DECODER 0

#define CONFIG_SMALL 1

#endif /* JF_MLP_CONFIG_H */
