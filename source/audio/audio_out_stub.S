/*
 * Firmware bindings for the cellAudioOut family.
 *
 * PSL1GHT binds firmware functions by FNID -- see its ppu/sprx/libsysutil --
 * and its table covers cellVideoOut, msgDialog, osk and friends but NOT
 * cellAudioOut.  cellAudio (the mixer port PSL1GHT does expose) only decides
 * what samples we hand the system; cellAudioOut is the family that decides
 * what the HDMI port actually CARRIES -- LPCM, or a compressed bitstream.
 * Without it there is no way to ask the console for anything but LPCM, which
 * is why "no bitstream on this platform" looked settled for so long.  It is a
 * missing binding, not a missing capability.
 *
 * Rather than patch the toolchain -- which would mean anyone building this
 * fork needs a modified PSL1GHT -- the stubs live here, in the app.  The
 * mechanism is PSL1GHT's own, reproduced: an FNID table, a matching table of
 * stub slots the lv2 loader fills in at load time, and a trampoline per
 * function that calls through its slot.  sprxlinker works out each library's
 * import count after linking by measuring to the next FNID table, so a second
 * header naming cellSysutil sits alongside PSL1GHT's quite happily.
 *
 * The FNIDs are not magic numbers from a forum post: an FNID is
 * sha1(name + a fixed 16-byte suffix), first four bytes read little-endian.
 * That algorithm reproduces every one of PSL1GHT's existing cellVideoOut,
 * msgDialog and osk FNIDs exactly, which is how it was verified before being
 * trusted here.  tools/fnid.py regenerates them and re-runs that check.
 *
 * Each trampoline indexes jf_ao_stubs[] rather than naming a per-function
 * symbol, so the tables' contiguity is a property of one array in one
 * translation unit instead of of link order -- this app's Makefile flattens
 * every object into one directory and assembles .S files last, so anything
 * order-dependent here would be silently wrong.
 */

#include <ppc-asm.h>

/* AOEXPORT(name, idx) -- idx MUST match the position in jf_ao_stubs[] and
 * jf_ao_fnids[] in audio_out_fnid.c; the loader pairs them positionally. */
#define AOEXPORT(name, idx)						\
	.align 2;									\
	.section ".sceStub.text","ax";				\
	.globl __##name;							\
__##name:										\
	mflr	r0;									\
	std		r0,16(r1);							\
	std		r2,40(r1);							\
	stdu	r1,-128(r1);						\
	lis		r12,(jf_ao_stubs+((idx)*4))@ha;		\
	lwz		r12,(jf_ao_stubs+((idx)*4))@l(r12);	\
	lwz		r0,0(r12);							\
	lwz		r2,4(r12);							\
	mtctr	r0;									\
	bctrl;										\
	addi	r1,r1,128;							\
	ld		r2,40(r1);							\
	ld		r0,16(r1);							\
	mtlr	r0;									\
	blr;										\
	.align 3;									\
	.section ".opd","aw";						\
	.globl name;								\
name:											\
	.quad __##name,.TOC.@tocbase,0

AOEXPORT(audioOutGetNumberOfDevice,     0)
AOEXPORT(audioOutGetDeviceInfo,         1)
AOEXPORT(audioOutGetState,              2)
AOEXPORT(audioOutGetConfiguration,      3)
AOEXPORT(audioOutGetSoundAvailability,  4)
AOEXPORT(audioOutGetSoundAvailability2, 5)
AOEXPORT(audioOutConfigure,             6)

/* ---------------------------------------------------------------------------
 * Loader tables.
 *
 * These are written in assembly rather than C for one specific reason: the
 * pointer fields in the import header are 32-BIT, not native.  PSL1GHT gets
 * that by compiling its libexport.c with -m32 and assembling the result; here
 * the widths are simply stated.  It is not a cosmetic detail -- with native
 * 64-bit pointers the header is 56 bytes instead of 44 and every field after
 * zero2 lands in the wrong place, so the loader reads garbage where the FNID
 * table pointer should be.  That bug was caught by diffing this header against
 * PSL1GHT's own inside the linked ELF: theirs read `name=0011 1c08`, the C
 * version of this one read `name=0000 0000 0011 1bf8`.
 *
 * ORDER MATTERS.  Entry N of the FNID table must be the same function as slot
 * N of the stub table and as the AOEXPORT() index above: the loader pairs them
 * positionally, not by name.
 * ------------------------------------------------------------------------- */

	.section ".rodata.sceFNID","a"
	.align 2
jf_ao_fnids:
	.long 0xe5e2b09d	/* cellAudioOutGetNumberOfDevice     */
	.long 0x7663e368	/* cellAudioOutGetDeviceInfo         */
	.long 0xf4e3caa0	/* cellAudioOutGetState              */
	.long 0xed5d96af	/* cellAudioOutGetConfiguration      */
	.long 0xc01b4e7c	/* cellAudioOutGetSoundAvailability  */
	.long 0x2beac488	/* cellAudioOutGetSoundAvailability2 */
	.long 0x4692ab35	/* cellAudioOutConfigure             */

/* The lv2 loader overwrites each slot with the resolved function descriptor.
 * They start out pointing at our own trampolines only so the section carries
 * relocations and survives --gc-sections; the values are never used. */
	.section ".data.sceFStub.cellSysutil","aw"
	.align 2
	.globl jf_ao_stubs
jf_ao_stubs:
	.long __audioOutGetNumberOfDevice
	.long __audioOutGetDeviceInfo
	.long __audioOutGetState
	.long __audioOutGetConfiguration
	.long __audioOutGetSoundAvailability
	.long __audioOutGetSoundAvailability2
	.long __audioOutConfigure

	.section ".rodata.sceResident","a"
jf_ao_libname:
	.asciz "cellSysutil"

/* `imports` is left 0: sprxlinker patches it after linking by measuring from
 * this FNID table to the next library's. */
	.section ".lib.stub","aw"
	.align 2
	.long  0x2c000001	/* same header words PSL1GHT uses for cellSysutil */
	.short 0x0009
	.short 0		/* imports -- patched by sprxlinker */
	.long  0
	.long  0
	.long  jf_ao_libname
	.long  jf_ao_fnids
	.long  jf_ao_stubs
	.long  0
	.long  0
	.long  0
	.long  0
