// Capture the decrypted firmware code for cellVideoOutConfigure2.  Research
// tooling for the 24p question -- see avconf_capture.h and docs/24P_OUTPUT.md.
//
// Why a capture and not a call.  Configure2's argument layout is unknown, and
// the last experiment that configured the display on a guess (2026-09-18,
// videoConfigure with resolution id 0x82) blacked out the panel, wedged the
// RSX flip and needed a hard power-off.  The layout has to come from the code
// itself.  The SELF on disk is encrypted, but the lv2 loader decrypts a module
// into this process's memory to run it, so once it is loaded the code is
// simply readable here -- no keys, no PS3MAPI, no running anything unknown.
//
// Safety:
//   * off unless the flag file exists;
//   * one-shot: skipped once the index has been written;
//   * a "started" marker is written BEFORE any read, so if a read ever faults
//     the next launch sees the marker without an index and skips the capture
//     instead of crash-looping;
//   * nothing here is a function call into cellSysutilAvconfExt.

#include <stdio.h>
#include <string.h>

#include <ppu-types.h>
#include <sysmodule/sysmodule.h>
// Types only.  PSL1GHT's sys/prx.h and lv2/prx.h share the include guard
// __LV2_PRX_H__, so whichever is included first hides the other; the two
// functions needed are declared below instead.
#include <sys/prx.h>

#include "avconf_capture.h"
#include "plog.h"

// CELL_SYSMODULE_AVCONF_EXT.  PSL1GHT's enum has no name for it; the value is
// its position in the firmware's sysmodule table (RPCS3 cellSysmodule.cpp),
// cross-checked against the neighbours PSL1GHT does name: USERINFO 0x32,
// SAVEDATA 0x33.
#define JF_SYSMODULE_AVCONF_EXT 0x31

#define CAP_FLAG   "/dev_hdd0/tmp/jellyfin_fwcapture.txt"
#define CAP_START  "/dev_hdd0/tmp/jf_fwcap_started.txt"
#define CAP_INDEX  "/dev_hdd0/tmp/jf_fwcap_index.txt"
#define CAP_PREFIX "/dev_hdd0/tmp/jf_fwcap_"

extern "C" {
// avconf_stub.S -- slots the loader fills with OPD addresses.
extern u32 jf_avx_stubs[4];
extern u64 jf_avx_unresolved;
// PSL1GHT's own slots for cellSysutil, used to locate libsysutil.
extern const u32 videoConfigure_stub;
extern const u32 videoGetResolutionAvailability_stub;
// Both exported by PSL1GHT's liblv2 (sysPrxForUser).  GetModuleIdByAddress
// (FNID 0x0341bb97) is declared in none of its headers.
sysPrxId sysPrxGetModuleIdByAddress(u32 addr);
s32 sysPrxGetModuleInfo(sysPrxId id, sysPrxFlags flags, sysPrxModuleInfo *info);
}

static const char *const kAvxNames[4] = {
	"cellVideoOutConfigure2",
	"cellVideoOutGetResolutionAvailability2",
	"cellVideoOutSetupDisplay",
	"cellVideoOutGetScreenSize",
};

static bool file_exists(const char *p)
{
	FILE *f = fopen(p, "rb");
	if (!f) return false;
	fclose(f);
	return true;
}

// Dump every segment of the module that contains `code`.  Returns the number
// of segment files written.
static int dump_module(FILE *idx, const char *tag, u32 code)
{
	const sysPrxId id = sysPrxGetModuleIdByAddress(code);
	fprintf(idx, "module[%s] code=0x%08x id=0x%08x\n", tag, (unsigned)code, (unsigned)id);
	if (id < 0) return 0;

	sysPrxSegmentInfo seg[8];
	char fname[256];
	sysPrxModuleInfo info;
	memset(seg, 0, sizeof(seg));
	memset(fname, 0, sizeof(fname));
	memset(&info, 0, sizeof(info));
	info.size          = sizeof(info);
	info.segments      = (u32)(u64)seg;
	info.segments_num  = 8;
	info.filename      = (u32)(u64)fname;
	info.filename_size = sizeof(fname);
	const s32 r = sysPrxGetModuleInfo(id, 0, &info);
	fprintf(idx, "  info rc=0x%08x name=%.30s ver=%u.%u attr=0x%x file=%s nseg=%u/%u\n",
	        (unsigned)r, info.name, (unsigned)(u8)info.version[0],
	        (unsigned)(u8)info.version[1], (unsigned)info.modattribute, fname,
	        (unsigned)info.segments_num, (unsigned)info.all_segments_num);
	if (r != 0) return 0;

	int written = 0;
	const u32 n = info.segments_num < 8 ? info.segments_num : 8;
	for (u32 i = 0; i < n; i++) {
		fprintf(idx, "  seg[%u] base=0x%08llx filesz=0x%llx memsz=0x%llx idx=%llu type=%llu\n",
		        (unsigned)i, (unsigned long long)seg[i].base,
		        (unsigned long long)seg[i].filesize, (unsigned long long)seg[i].memsize,
		        (unsigned long long)seg[i].index, (unsigned long long)seg[i].type);
		if (seg[i].base == 0 || seg[i].filesize == 0 || seg[i].filesize > (16u << 20))
			continue;
		char path[96];
		snprintf(path, sizeof(path), CAP_PREFIX "%s_seg%u_%08llx.bin", tag,
		         (unsigned)i, (unsigned long long)seg[i].base);
		FILE *o = fopen(path, "wb");
		if (!o) { fprintf(idx, "    open failed: %s\n", path); continue; }
		const size_t w = fwrite((const void *)(uintptr_t)seg[i].base, 1,
		                        (size_t)seg[i].filesize, o);
		fclose(o);
		fprintf(idx, "    wrote %s (%lu bytes)\n", path, (unsigned long)w);
		written++;
	}
	return written;
}

void avconf_capture_run(void)
{
	if (!file_exists(CAP_FLAG)) return;
	if (file_exists(CAP_INDEX)) {
		plog("fwcap: index already written, skipping (delete it to recapture)");
		return;
	}
	if (file_exists(CAP_START)) {
		plog("fwcap: a previous capture started and never finished - NOT retrying");
		return;
	}
	{
		FILE *m = fopen(CAP_START, "w");
		if (!m) { plog("fwcap: cannot write start marker, skipping"); return; }
		fputs("started\n", m);
		fclose(m);
	}

	const s32 lr = sysModuleLoad((sysModuleId)JF_SYSMODULE_AVCONF_EXT);
	char b[160];
	snprintf(b, sizeof(b), "fwcap: sysModuleLoad(avconf_ext) rc=0x%08x", (unsigned)lr);
	plog(b);

	FILE *idx = fopen(CAP_INDEX ".tmp", "w");
	if (!idx) {
		plog("fwcap: cannot open index");
		if (lr == 0) sysModuleUnload((sysModuleId)JF_SYSMODULE_AVCONF_EXT);
		return;
	}
	fprintf(idx, "jf_fwcap v1\nsysModuleLoad(0x31) rc=0x%08x\n", (unsigned)lr);

	const u32 unresolved = (u32)(u64)&jf_avx_unresolved;
	u32 avx_code = 0;
	for (int i = 0; i < 4; i++) {
		const u32 opd = jf_avx_stubs[i];
		if (opd == unresolved || opd == 0) {
			fprintf(idx, "slot[%d] %s UNRESOLVED\n", i, kAvxNames[i]);
			continue;
		}
		const u32 code = ((const u32 *)(uintptr_t)opd)[0];
		const u32 toc  = ((const u32 *)(uintptr_t)opd)[1];
		fprintf(idx, "slot[%d] %s opd=0x%08x code=0x%08x toc=0x%08x\n",
		        i, kAvxNames[i], (unsigned)opd, (unsigned)code, (unsigned)toc);
		if (!avx_code) avx_code = code;
	}
	const u32 cfg_opd = videoConfigure_stub;
	const u32 cfg_code = cfg_opd ? ((const u32 *)(uintptr_t)cfg_opd)[0] : 0;
	const u32 ava_opd = videoGetResolutionAvailability_stub;
	fprintf(idx, "cellVideoOutConfigure opd=0x%08x code=0x%08x\n",
	        (unsigned)cfg_opd, (unsigned)cfg_code);
	fprintf(idx, "cellVideoOutGetResolutionAvailability opd=0x%08x code=0x%08x\n",
	        (unsigned)ava_opd,
	        (unsigned)(ava_opd ? ((const u32 *)(uintptr_t)ava_opd)[0] : 0));
	fflush(idx);

	int files = 0;
	if (avx_code) files += dump_module(idx, "avconfext", avx_code);
	if (cfg_code) files += dump_module(idx, "sysutil", cfg_code);
	fprintf(idx, "done files=%d\n", files);
	fclose(idx);
	rename(CAP_INDEX ".tmp", CAP_INDEX);
	remove(CAP_START);

	if (lr == 0) sysModuleUnload((sysModuleId)JF_SYSMODULE_AVCONF_EXT);
	snprintf(b, sizeof(b), "fwcap: done, %d segment files, index %s", files, CAP_INDEX);
	plog(b);
}
