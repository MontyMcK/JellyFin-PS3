/*
 * Import slots for cellSysutilAvconfExt.
 *
 * cellVideoOutConfigure2 is the only firmware entry point that carries a
 * refresh rate.  Its ABI is now read from the 4.93 firmware itself
 * (docs/24P_OUTPUT.md), not guessed:
 *
 *   s32 cellVideoOutConfigure2(u32 videoOut, CellVideoOutConfiguration2 *cfg,
 *                              CellVideoOutOption2 *opt, u32 waitForEvent)
 *
 * libsysutil_avconf_ext copies cfg (16 bytes) and opt (16 bytes, NULL allowed)
 * verbatim into VSH packet 0x4b08; VSH reads refresh as a u16 at cfg+0x0A.
 *
 * So slot 0 gets a trampoline (videoConfigure2, video_24p.h) and the others
 * stay deliberately UNCALLABLE: their layouts have not been read from firmware.
 * The loader fills each slot with the function's descriptor (OPD) address when
 * the library is loaded (sysModuleLoad(0x31)); avconf_capture.cpp reads them.
 *
 * FNIDs are sha1(name + suffix), first four bytes little-endian -- the
 * algorithm that reproduces PSL1GHT's own cellVideoOutConfigure (0x0bae8772)
 * and cellVideoOutGetState (0x887572d5).  Names are those RPCS3 registers for
 * this library (Modules/cellAvconfExt.cpp), which bind real games by the same
 * hash, so a wrong name would show up as an unresolved slot, not a crash.
 *
 * ORDER MATTERS: entry N of the FNID table pairs with slot N.
 */

#include <ppc-asm.h>

/* Trampoline for slot 0 only -- same shape as audio_out_stub.S's AOEXPORT. */
	.align 2
	.section ".sceStub.text","ax"
	.globl __videoConfigure2
__videoConfigure2:
	mflr	r0
	std		r0,16(r1)
	std		r2,40(r1)
	stdu	r1,-128(r1)
	lis		r12,(jf_avx_stubs+0)@ha
	lwz		r12,(jf_avx_stubs+0)@l(r12)
	lwz		r0,0(r12)
	lwz		r2,4(r12)
	mtctr	r0
	bctrl
	addi	r1,r1,128
	ld		r2,40(r1)
	ld		r0,16(r1)
	mtlr	r0
	blr
	.align 3
	.section ".opd","aw"
	.globl videoConfigure2
videoConfigure2:
	.quad __videoConfigure2,.TOC.@tocbase,0

	.section ".rodata.sceFNID","a"
	.align 2
jf_avx_fnids:
	.long 0x9faa12be	/* 0 cellVideoOutConfigure2                 */
	.long 0x21036f4e	/* 1 cellVideoOutGetResolutionAvailability2 */
	.long 0x269ffedd	/* 2 cellVideoOutSetupDisplay               */
	.long 0xfaa275a4	/* 3 cellVideoOutGetScreenSize (known ABI: a control) */

/* Placeholder the loader overwrites.  A slot that still holds it after the
 * module loaded is an FNID the firmware does not export. */
	.section ".data.sceFStub.cellSysutilAvconfExt","aw"
	.align 2
	.globl jf_avx_stubs
jf_avx_stubs:
	.long jf_avx_unresolved
	.long jf_avx_unresolved
	.long jf_avx_unresolved
	.long jf_avx_unresolved

	.section ".data","aw"
	.align 3
	.globl jf_avx_unresolved
jf_avx_unresolved:
	.quad 0

	.section ".rodata.sceResident","a"
jf_avx_libname:
	.asciz "cellSysutilAvconfExt"

/* 44-byte import header, 32-bit pointer fields -- see audio_out_stub.S for why
 * this is written in assembly.  Header words are the ones PSL1GHT uses for
 * its sysmodule-loaded libraries (cellPngDec, cellJpgDec). */
	.section ".lib.stub","aw"
	.align 2
	.long  0x2c000001
	.short 0x0009
	.short 0		/* imports -- patched by sprxlinker */
	.long  0
	.long  0
	.long  jf_avx_libname
	.long  jf_avx_fnids
	.long  jf_avx_stubs
	.long  0
	.long  0
	.long  0
	.long  0
