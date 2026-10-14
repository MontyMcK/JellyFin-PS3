// Physical 24Hz output for 24fps film.  See display_24p.h, and
// docs/24P_OUTPUT.md for the firmware evidence behind every call here.
//
// The rules this file is built around -- each one paid for on 2026-09-18,
// when a guessed mode blacked out the panel and needed a hard power-off:
//
//   1. Nothing draws or flips while an unverified mode is up.  The prompt is
//      drawn and flipped BEFORE the switch; the scan-out keeps showing it.
//   2. The abort marker is written BEFORE the mode changes.  If the console
//      dies in 24p, the next launch turns 24p off instead of trying again.
//   3. "The call returned 0" proves nothing.  The switch counts only when the
//      vblank handler is seen ticking at the rate the content needs, timed
//      against the timebase -- which is also the only honest way to tell
//      23.976 from 24.000, since the firmware's two 24Hz bits are unnamed.
//   4. Every failure path ends in the exact mode the session started in.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <ppu-types.h>
#include <ppu-asm.h>
#include <sys/systime.h>
#include <sysmodule/sysmodule.h>
#include <sysutil/video.h>
#include <sysutil/sysutil.h>
#include <rsx/gcm_sys.h>

#include "i18n.h"
#include "display_24p.h"
#include "display_mode.h"
#include "display_diag.h"
#include "timing.h"
#include "rsxutil.h"
#include "plog.h"
#include "lclog.h"
#include "audio_bitstream.h"   // the 6ch configure a mode change undoes

#define F_ENABLE    "/dev_hdd0/tmp/jellyfin_24p.txt"     // "1" = on
#define F_CONFIRMED "/dev_hdd0/tmp/jf_24p_confirmed.txt" // "1" seen, "0" not
#define F_PENDING   "/dev_hdd0/tmp/jf_24p_pending.txt"   // abort marker
#define F_MAP       "/dev_hdd0/tmp/jf_24p_map.txt"       // bit -> measured clock

#define JF_SYSMODULE_AVCONF_EXT 0x31      // see avconf_capture.cpp
#define GCM_FREQ_59_94HZ  1               // CELL_GCM_DISPLAY_FREQUENCY_*
#define GCM_FREQ_SCANOUT  2               // (RPCS3 gcm_enums.h)

#define CONFIRM_SECS      15
#define MEASURE_VBLANKS   48              // ~2 s at 24Hz, ~0.8 s at 59.94

extern "C" {
extern u32 jf_avx_stubs[4];               // avconf_stub.S
extern u64 jf_avx_unresolved;
}

static struct {
	bool      active;          // output is switched right now
	bool      module_loaded;
	bool      vbfreq_changed;  // we set SCANOUT and must put 59.94 back
	videoState orig;           // the output as the session found it
} S;

// ------------------------------------------------------------------ files

static bool file_exists(const char *p)
{
	FILE *f = fopen(p, "rb");
	if (!f) return false;
	fclose(f);
	return true;
}

static int read_int(const char *p, int def)
{
	FILE *f = fopen(p, "r");
	if (!f) return def;
	int v = def;
	if (fscanf(f, "%d", &v) != 1) v = def;
	fclose(f);
	return v;
}

static void write_text(const char *p, const char *s)
{
	FILE *f = fopen(p, "w");
	if (f) { fputs(s, f); fclose(f); }
}

// On unless the file says 0: automatic by default (2026-09-27).
bool d24_enabled(void) { return read_int(F_ENABLE, 1) != 0; }
void d24_set_enabled(bool on) { write_text(F_ENABLE, on ? "1\n" : "0\n"); }

// ---- the one-time TV check, remembered per TV --------------------------------
// A TV is told apart by the 1080 modes it advertises (FNV-1a of every
// (resolution, rates) pair): no EDID is reachable from a game, and a different
// set -- or the same set behind a different receiver -- nearly always
// advertises a different list.  jf_24p_confirmed.txt holds "<fp8hex> <0|1>"
// lines; an old single-number file is taken as the answer for the TV in use.
static u32 s_tv_fp = 0;

// The key a mode's answer is stored under: the TV itself for bit 0x10 (every
// answer given so far was for 0x10), the TV mixed with the bit for any other.
static u32 conf_key(u16 bit)
{
	return bit == 0x10 ? s_tv_fp : (s_tv_fp ^ (0x9E3779B9u * (u32)bit));
}

static int conf_get(u32 fp)
{
	FILE *f = fopen(F_CONFIRMED, "r");
	if (!f) return -1;
	char a[32]; int v = -1, r = -1; bool legacy = true;
	while (fscanf(f, "%31s", a) == 1) {
		if (strlen(a) == 8) {                 // "<fp> <v>"
			legacy = false;
			if (fscanf(f, "%d", &v) != 1) break;
			if ((u32)strtoul(a, NULL, 16) == fp) r = v;
		} else if (legacy) {                  // old format: one number
			if (fp == s_tv_fp) r = atoi(a);    // it was the answer for 0x10 only
			break;
		}
	}
	fclose(f);
	return r;
}

static void conf_put(u32 fp, int v)
{
	static u32 fps[32]; static int vs[32];
	int n = 0;
	FILE *f = fopen(F_CONFIRMED, "r");
	if (f) {
		char a[32]; int x;
		while (n < 31 && fscanf(f, "%31s", a) == 1) {
			if (strlen(a) != 8) {             // legacy one number: the 0x10 answer
				if (n == 0 && s_tv_fp != fp) { fps[n] = s_tv_fp; vs[n] = atoi(a); n++; }
				break;
			}
			if (fscanf(f, "%d", &x) != 1) break;
			const u32 k = (u32)strtoul(a, NULL, 16);
			if (k == fp) continue;
			fps[n] = k; vs[n] = x; n++;
		}
		fclose(f);
	}
	fps[n] = fp; vs[n] = v; n++;
	f = fopen(F_CONFIRMED, "w");
	if (!f) return;
	for (int i = 0; i < n; i++) fprintf(f, "%08x %d\n", (unsigned)fps[i], vs[i]);
	fclose(f);
}

void d24_boot_check(void)
{
	if (!file_exists(F_PENDING)) return;
	// The last run switched the output and never switched it back: a crash,
	// a hang or a power cut while in 24p.  Do not walk into it again.
	write_text(F_ENABLE, "0\n");
	remove(F_PENDING);
	plog("24p: previous session ended with the output switched -- 24p turned "
	     "OFF (write 1 to jellyfin_24p.txt to re-enable)");
}

// Measured clock per refresh bit, learned on this console + TV.  Lines of
// "<bit> <clock enum>".  Only ever an ordering hint: every switch is still
// measured, so a stale file cannot cause a wrong rate, only a wasted try.
static dm_clock map_get(u16 bit)
{
	FILE *f = fopen(F_MAP, "r");
	if (!f) return DM_CLK_UNKNOWN;
	unsigned b; int c; dm_clock r = DM_CLK_UNKNOWN;
	while (fscanf(f, "%x %d", &b, &c) == 2)
		if (b == bit) r = (dm_clock)c;
	fclose(f);
	return r;
}

static void map_put(u16 bit, dm_clock c)
{
	if (map_get(bit) == c) return;
	FILE *f = fopen(F_MAP, "a");
	if (f) { fprintf(f, "0x%02x %d\n", (unsigned)bit, (int)c); fclose(f); }
}

// ------------------------------------------------------------ measurement

// Mean vblank period over MEASURE_VBLANKS, in ns.  0 = the handler did not
// tick within the deadline, i.e. the display head is not running.
static u64 measure_period_ns(void)
{
	const u64 tb       = sysGetTimebaseFrequency();
	const u64 start    = timing_get_us();
	const u64 deadline = start + 4000000ULL;
	// Align to an edge first.  No edge within 500 ms = the head is not running
	// (even 24Hz ticks every 42 ms).
	const u64 c0 = timing_vsync_count();
	while (timing_vsync_count() == c0) {
		if (timing_get_us() - start > 500000ULL) return 0;
		usleep(100);
	}
	const u64 c1 = timing_vsync_count();
	const u64 t1 = __gettime();
	while (timing_vsync_count() < c1 + MEASURE_VBLANKS) {
		if (timing_get_us() > deadline) return 0;
		usleep(100);
	}
	const u64 t2 = __gettime();
	const u64 c2 = timing_vsync_count();
	if (c2 <= c1 || tb == 0) return 0;
	return (t2 - t1) * 1000000000ULL / tb / (c2 - c1);
}

// ------------------------------------------------------------- switching

static bool accepted_by_configure2(u16 bit)
{
	// The exact set VSH 4.93 accepts (vsh.self 0x128044); 60Hz (0x04) and
	// 30Hz (0x08) are NOT in it.
	return bit == 0 || bit == 0x01 || bit == 0x02 ||
	       bit == 0x10 || bit == 0x20 || bit == 0x40;
}

static s32 configure2(u16 refresh)
{
	videoConfiguration2 c;
	memset(&c, 0, sizeof(c));
	c.resolution   = S.orig.displayMode.resolution;
	c.format       = VIDEO_BUFFER_FORMAT_XRGB;
	c.aspect       = S.orig.displayMode.aspect;
	c.scanMode2    = VIDEO_SCANMODE2_PROGRESSIVE;
	c.refreshRates = refresh;
	c.pitch        = color_pitch;
	return videoConfigure2(0, &c, NULL, 0);
}

// Poll the reported refresh until it equals `bit` (or 1 s passes).
static u16 settle_state(u16 bit)
{
	videoState st;
	u16 got = 0;
	for (int i = 0; i < 50; i++) {
		memset(&st, 0, sizeof(st));
		if (videoGetState(0, 0, &st) == 0) {
			// Not st.state: PSL1GHT names ENABLED 1 where the SDK enum (per
			// RPCS3) has ENABLED 0, so that field is not trusted here.
			got = st.displayMode.refreshRates;
			if (got == bit) break;
		}
		usleep(20000);
	}
	return got;
}

static void revert(const char *why)
{
	char b[160];
	const u16 orig = S.orig.displayMode.refreshRates;
	s32 rc = -1;
	lc_logf("24p: revert begin (%s) -> refresh 0x%02x", why, (unsigned)orig);
	if (S.module_loaded && accepted_by_configure2(orig))
		rc = configure2(orig);
	if (rc != 0) {
		// Exactly what init_screen() did at boot: v1, refresh AUTO.
		videoConfiguration v;
		memset(&v, 0, sizeof(v));
		v.resolution = S.orig.displayMode.resolution;
		v.format     = VIDEO_BUFFER_FORMAT_XRGB;
		v.aspect     = S.orig.displayMode.aspect;
		v.pitch      = color_pitch;
		rc = videoConfigure(0, &v, NULL, 0);
	}
	const u16 now = settle_state(orig);
	if (S.vbfreq_changed) {
		gcmSetVBlankFrequency(GCM_FREQ_59_94HZ);
		S.vbfreq_changed = false;
	}
	timing_set_display_override(0, 0);
	audio_bitstream_reassert("24p revert");   // the HDMI re-lock reset the audio config
	// The mode change back drops the scan-out buffer registration just as
	// the switch did: without this the TV is back at 60 Hz but the head scans
	// nothing and the screen stays black for the rest of the run (hardware,
	// 2026-09-27: every failure path but session_end missed it).
	rsx_rebind_display();
	S.active = false;
	remove(F_PENDING);
	snprintf(b, sizeof(b), "24p: REVERT (%s) rc=0x%08x refresh now 0x%02x (was 0x%02x)%s",
	         why, (unsigned)rc, (unsigned)now, (unsigned)orig,
	         now == orig ? "" : " MISMATCH");
	plog(b);
	lc_logf("24p: revert done refresh=0x%02x%s", (unsigned)now,
	        now == orig ? " (back at the original rate)" : " MISMATCH");
}

// Switch to `bit` and measure.  Returns the measured clock (UNKNOWN on any
// failure, with the output already back where it started).
static dm_clock try_bit(u16 bit, u32 *num, u32 *den)
{
	char b[160];
	const s32 rc = configure2(bit);
	const u16 st = settle_state(bit);
	u64 ns = rc == 0 ? measure_period_ns() : 0;
	dm_clock clk = dm_classify_period(ns, num, den);

	// The vblank IRQ can be a fixed 59.94 timer rather than the scan-out
	// (CELL_GCM_DISPLAY_FREQUENCY_59_94HZ).  Sony's BD player selects SCANOUT
	// explicitly (sys_rsx_context_attribute 0x108, value 2); do the same only
	// if the measurement says we must.
	if (rc == 0 && (clk == DM_CLK_5994 || clk == DM_CLK_60) && !S.vbfreq_changed) {
		gcmSetVBlankFrequency(GCM_FREQ_SCANOUT);
		S.vbfreq_changed = true;
		plog("24p: vblank IRQ was not following scan-out; set SCANOUT");
		ns  = measure_period_ns();
		clk = dm_classify_period(ns, num, den);
	}
	snprintf(b, sizeof(b),
	         "24p: SWITCH try refresh=0x%02x rc=0x%08x state=0x%02x vblank=%llu.%03llu us clock=%s",
	         (unsigned)bit, (unsigned)rc, (unsigned)st,
	         (unsigned long long)(ns / 1000), (unsigned long long)(ns % 1000),
	         dm_clock_name(clk));
	plog(b);
	if (rc == 0 && ns == 0) {
		// Head not running: the 2026-09-18 failure shape.  Out, now.
		revert("vblank stopped");
		return DM_CLK_UNKNOWN;
	}
	if (clk == DM_CLK_23976 || clk == DM_CLK_24) map_put(bit, clk);
	return rc == 0 ? clk : DM_CLK_UNKNOWN;
}

static void phase(const d24_ui *ui, const char *what)
{
	lc_logf("24p: phase %s", what);
	if (ui->lifecycle) ui->lifecycle(what);
}

bool d24_session_begin(const d24_ui *ui)
{
	char b[160];
	u32 fnum, fden, w, h;
	dm_fps_source src;
	memset(&S, 0, sizeof(S));
	if (!display_diag_last(&fnum, &fden, &src, &w, &h)) return false;

	if (videoGetState(0, 0, &S.orig) != 0) return false;
	videoDeviceInfo di;
	memset(&di, 0, sizeof(di));
	dm_mode modes[32];
	int n = -1;
	u16 adv1080 = 0;
	if (videoGetDeviceInfo(0, 0, &di) == 0) {
		n = di.availableModeCount > 32 ? 32 : di.availableModeCount;
		for (int i = 0; i < n; i++) {
			modes[i].res   = di.availableModes[i].resolution;
			modes[i].rates = di.availableModes[i].refreshRates;
			if (modes[i].res == DM_RES_1080) adv1080 |= modes[i].rates;
		}
		u32 h = 2166136261u;
		for (int i = 0; i < n; i++) {
			h = (h ^ (u32)modes[i].res) * 16777619u;
			h = (h ^ (u32)modes[i].rates) * 16777619u;
		}
		s_tv_fp = h;
	}
	const dm_film film = dm_classify_film(fnum, fden);
	// 25fps content (PAL-rate) wants a 50Hz output -- an exact 2:2 -- the way
	// film wants 24Hz; the machinery below is the same, only the target differs.
	const bool f25 = film == DM_FILM_25;
	const char *hz = f25 ? "50Hz" : "24Hz";
	const dm_decision d = dm_decide(
		film, dm_fps_confident(src), w, h,
		S.orig.displayMode.resolution, S.orig.displayMode.refreshRates,
		S.orig.displayMode.scanMode == VIDEO_SCANMODE_PROGRESSIVE,
		f25 ? dm_display_rate_support(modes, n, DM_RES_1080, DM_RATE_50)
		    : dm_display_24p_support(modes, n, DM_RES_1080),
		d24_enabled());
	if (!d.attempt) return false;         // display_diag already said why

	int confirmed = conf_get(s_tv_fp);
	{
		char tb[96];
		snprintf(tb, sizeof tb, "24p: TV id %08x one-time check=%s", (unsigned)s_tv_fp,
		         confirmed == 1 ? "passed" : confirmed == 0 ? "failed" : "not yet asked");
		plog(tb);
	}
	if (confirmed == 0) {
		plog("24p: RESULT mode_switch=not_attempted (this TV failed the "
		     "one-time check; delete jf_24p_confirmed.txt to ask again)");
		return false;
	}

	// Candidate bits: advertised for 1080, known-right ones first, known-wrong
	// ones never.  Which of 0x10/0x20 is 23.976 is learned, not assumed.
	const dm_clock want = f25 ? DM_CLK_50
	                    : film == DM_FILM_23976 ? DM_CLK_23976 : DM_CLK_24;
	u16 cand[2];
	int nc = 0;
	if (f25) {
		// 50Hz is a public, named bit: no learning pass, but still measured.
		if (adv1080 & DM_RATE_50) cand[nc++] = DM_RATE_50;
	} else {
		const u16 bits[2] = { DM_RATE_24FAM_A, DM_RATE_24FAM_B };
		for (int pass = 0; pass < 2; pass++)
			for (int i = 0; i < 2; i++) {
				if (!(adv1080 & bits[i])) continue;
				const dm_clock k = map_get(bits[i]);
				if (pass == 0 ? k == want : k == DM_CLK_UNKNOWN) cand[nc++] = bits[i];
			}
	}
	if (nc == 0) {
		plog("24p: RESULT mode_switch=not_attempted (no advertised 24Hz bit is "
		     "known-good for this content's clock)");
		return false;
	}
	// The TV check is per mode: a set that shows 23.976 may still not show
	// 24.000 (2026-09-27: the first true-24 film would otherwise have switched
	// with no prompt, a black screen and no way back but the timeout).
	if (cand[0] != DM_RATE_24FAM_A) {
		confirmed = conf_get(conf_key(cand[0]));
		if (confirmed == 0) {
			plog("24p: RESULT mode_switch=not_attempted (this TV failed the "
			     "check for this 24Hz mode)");
			return false;
		}
	}

	const s32 lr = sysModuleLoad((sysModuleId)JF_SYSMODULE_AVCONF_EXT);
	S.module_loaded = (lr == 0);
	if (jf_avx_stubs[0] == (u32)(u64)&jf_avx_unresolved || jf_avx_stubs[0] == 0) {
		snprintf(b, sizeof(b), "24p: RESULT mode_switch=failure (cellVideoOutConfigure2 "
		         "did not resolve, sysModuleLoad rc=0x%08x)", (unsigned)lr);
		plog(b);
		if (S.module_loaded) sysModuleUnload((sysModuleId)JF_SYSMODULE_AVCONF_EXT);
		S.module_loaded = false;
		return false;
	}

	// Draw + flip + wait while the ORIGINAL mode is up.  After this nothing
	// touches the GPU until the switch is verified.
	// 50Hz wording is English only for now; the 24Hz strings are translated.
	if (confirmed != 1)
		ui->draw_prompt(f25 ? "Testing 1080p 50Hz output on this TV (one time only)."
		                    : TR("Testing 1080p 24Hz output on this TV (one time only)."),
		                TR("When the picture returns, press X if you can read this. O or 15 s = no."));
	else
		ui->draw_prompt(f25 ? "Switching the TV to 1080p 50Hz for this video..."
		                    : TR("Switching the TV to 1080p 24Hz for this film..."), "");
	rsxSync();

	// Baseline at the ORIGINAL rate.  Proves the measurement itself on this
	// hardware (it must read ~16683 us at 59.94) before it is trusted to judge
	// a new mode -- and is a useful line in the log even if the TV says no.
	{
		u32 bn = 0, bd = 0;
		const u64 ns = measure_period_ns();
		const dm_clock bc = dm_classify_period(ns, &bn, &bd);
		snprintf(b, sizeof(b), "24p: baseline vblank=%llu.%03llu us clock=%s",
		         (unsigned long long)(ns / 1000), (unsigned long long)(ns % 1000),
		         dm_clock_name(bc));
		plog(b);
		if (bc == DM_CLK_UNKNOWN) {
			plog("24p: RESULT mode_switch=not_attempted (baseline vblank did not "
			     "classify; measurement not trustworthy here)");
			if (S.module_loaded) sysModuleUnload((sysModuleId)JF_SYSMODULE_AVCONF_EXT);
			S.module_loaded = false;
			return false;
		}
	}

	write_text(F_PENDING, "switching\n");   // BEFORE the mode changes
	S.active = true;
	{
		char ph[64];
		snprintf(ph, sizeof(ph), "mode_switch_begin cand=%d first=0x%02x",
		         nc, (unsigned)cand[0]);
		phase(ui, ph);
	}

	u32 num = 0, den = 0;
	dm_clock got = DM_CLK_UNKNOWN;
	u16 used = 0;
	for (int i = 0; i < nc && got != want; i++) {
		got  = try_bit(cand[i], &num, &den);
		used = cand[i];
		if (!S.active) break;            // try_bit reverted: head died
	}
	if (!S.active || got != want) {
		phase(ui, "mode_switch_failed");
		if (S.active) revert(f25 ? "50Hz not measured at the content's clock"
		                         : "no 24Hz bit measured at the content's clock");
		snprintf(b, sizeof(b), "24p: RESULT mode_switch=failure resulting_refresh=0x%02x "
		         "presentation=unchanged", (unsigned)S.orig.displayMode.refreshRates);
		plog(b);
		sysModuleUnload((sysModuleId)JF_SYSMODULE_AVCONF_EXT);
		S.module_loaded = false;
		return false;
	}

	if (used != cand[0]) confirmed = conf_get(conf_key(used));   // fell to the other bit

	// The vblank was just measured ticking at the new rate, so a flip will
	// complete -- the 2026-09-18 hang was a flip with NO vblank.  Hardware
	// 2026-09-24 showed why this is needed: the TV synced to 23.976 but showed
	// black, because after the mode change nothing is visible until a new
	// frame is flipped, so the confirmation prompt could not be read.
	phase(ui, "mode_switch_verified");
	// A mode change can drop the scan-out buffer registration, leaving the
	// head scanning nothing (black) even though it ticks -- so register the
	// buffers again exactly as init_screen() did, then flip.  The flip wait is
	// bounded: a flip that does not land means the head is not presenting, and
	// that is a revert, never a hang.
	rsx_rebind_display();
	char hzl[24];
	snprintf(hzl, sizeof(hzl), "1080p %s", hz);
	if (!ui->draw_prompt(confirmed != 1
	                     ? (f25 ? "The TV is now at 1080p 50Hz. Press X if you can read this."
	                            : TR("The TV is now at 1080p 24Hz. Press X if you can read this."))
	                     : hzl, confirmed != 1 ? TR("O or 15 s = no, go back.") : "")) {
		phase(ui, "prompt_flip_timeout");
		revert("flip did not complete in the new mode");
		plog("24p: RESULT mode_switch=failure (flip timed out after the switch)");
		sysModuleUnload((sysModuleId)JF_SYSMODULE_AVCONF_EXT);
		S.module_loaded = false;
		return false;
	}
	phase(ui, "prompt_redrawn_after_switch");

	if (confirmed != 1) {
		// Only now does a press count: the switch is done and measured.
		ui->poll_answer();               // drop anything pressed during the switch
		const u64 t0    = timing_get_us();
		const u64 until = t0 + CONFIRM_SECS * 1000000ULL;
		u64 next_hb     = t0;
		int ans = 0;
		phase(ui, "confirm_wait_begin");
		while (ans == 0 && timing_get_us() < until) {
			sysUtilCheckCallback();
			ans = ui->poll_answer();
			// Keep flipping new frames the whole time (2026-09-27): drawn once,
			// the prompt never appeared on a TV that shows the Blu-ray player's
			// 24 Hz fine -- the set may only start presenting once it has
			// locked, which takes seconds, and then wants fresh frames.
			{
				char l2[80];
				const u64 left = (until - timing_get_us()) / 1000000ULL + 1;
				snprintf(l2, sizeof(l2), TR("Press X if you can read this.  O = no.  Going back in %llu s"),
				         (unsigned long long)left);
				ui->draw_prompt(f25 ? "The TV is now at 1080p 50Hz."
				                    : TR("The TV is now at 1080p 24Hz."), l2);
			}
			if (timing_get_us() >= next_hb) {
				char ph[48];
				snprintf(ph, sizeof(ph), "confirm_wait %llus",
				         (unsigned long long)((timing_get_us() - t0) / 1000000ULL));
				phase(ui, ph);
				next_hb += 1000000ULL;
			}
			usleep(20000);
		}
		{
			char ph[64];
			snprintf(ph, sizeof(ph), "confirm_wait_end ans=%d after %llums%s", ans,
			         (unsigned long long)((timing_get_us() - t0) / 1000ULL),
			         ans == 0 ? " (TIMEOUT)" : "");
			phase(ui, ph);
		}
		if (ans != 1) {
			conf_put(conf_key(used), 0);
			revert(ans < 0 ? "user said no picture" : "no answer within 15 s");
			plog("24p: RESULT mode_switch=failure (TV check not confirmed; will "
			     "not retry until jf_24p_confirmed.txt is deleted)");
			sysModuleUnload((sysModuleId)JF_SYSMODULE_AVCONF_EXT);
			S.module_loaded = false;
			phase(ui, "reverted_after_confirm_failure");
			return false;
		}
		conf_put(conf_key(used), 1);
		plog("24p: TV check confirmed by user");
	}

	// The switch re-established the HDMI link, and the system put its own
	// audio configuration back: re-apply the playback's (the centre channel).
	audio_bitstream_reassert("24p switch");

	// Hand the MEASURED rate to the timing engine and restart its cadence.
	timing_set_display_override(num, den);
	timing_init(fnum, fden);
	char cad[32];
	dm_cadence_str(num, den, fnum, fden, cad, sizeof(cad));
	snprintf(b, sizeof(b),
	         "24p: RESULT mode_switch=success refresh_bit=0x%02x resulting_refresh=%u/%u "
	         "(measured %s) presentation=%s",
	         (unsigned)used, (unsigned)num, (unsigned)den, dm_clock_name(got), cad);
	plog(b);
	phase(ui, "mode_switch_success timing_reinit");
	return true;
}

void d24_session_end(void)
{
	lc_logf("24p: session_end active=%d module=%d", (int)S.active, (int)S.module_loaded);
	if (S.active) {
		rsxSync();
		revert("playback ended");
		rsx_rebind_display();
	}
	if (S.module_loaded) {
		sysModuleUnload((sysModuleId)JF_SYSMODULE_AVCONF_EXT);
		S.module_loaded = false;
	}
}
