// Ask the console to change what the HDMI audio output carries, and put it
// back afterwards.
//
// PSL1GHT never bound cellAudioOut, so nothing in this app could express the
// request until audio_out_stub.S.  cellAudio (the port) carries our ordinary
// float LPCM; cellAudioOut decides what the PORT is turned into on the way out.
//
// What each mode is:
//
//   AC-3 / DTS      Ask the system to encode our LPCM to Dolby Digital or DTS.
//                   Used for its side effect: naming AC-3, which is 5.1 by
//                   definition, makes the console route the 8-wide port to the
//                   6-wide output as true 5.1.  Left alone, some chains fold 8
//                   channels to 6 in a way that drops the centre channel and
//                   the dialogue with it.  This is the default for 5.1.
//
//                   audioOutGetState does not say what ends up on the wire:
//                   its sound mode reads LPCM even while a receiver shows Dolby
//                   Digital, and depending on the PS3's own Audio Output
//                   Settings (Dolby Digital ticked or not) the request may
//                   become real Dolby Digital.  So the state is logged and
//                   never trusted as a verdict on the wire.
//
//   RAW (0xff)      CELL_AUDIO_OUT_CODING_TYPE_BITSTREAM, true passthrough.
//
//   PASSTHROUGH     The Dolby Digital audio output choice.  The video player
//                   opens a 2-channel port, asks for coding type 0xff, and
//                   sends each AC-3 frame untouched as an IEC 61937 burst
//                   (iec61937.c).  The receiver decodes it.  The burst is
//                   sent whatever audioOutGetState reports: that state is
//                   not authoritative.
//
// A wrong audio mode cannot lock anyone out: the worst case is silence with
// the UI usable.  Whatever this changes is journalled first and restored on
// exit and at the next launch, so an app that dies mid-change does not leave
// the console on a coding the next session cannot play.  Measurements and the
// history behind these rules are in docs/audio-bitstream-notes.md.

#include <stdio.h>
#include <string.h>

#include "audio_out.h"
#include "audio_bitstream.h"
#include "surround.h"
#include "jf_paths.h"
#include "plog.h"

#define BITSTREAM_FILE "jellyfin_bitstream.txt"

static bool s_engaged        = false;   // a compressed stream is on the wire
static bool s_pt_requested   = false;   // the video player asked for passthrough
static bool s_passthrough    = false;   // pack AC-3 frames instead of decoding
static bool s_applied        = false;   // we changed the config, engaged or not
static audioOutConfiguration s_applied_cfg;   // what begin() last applied
static u8   s_saved_encoder  = AUDIO_OUT_CODING_LPCM;
static u8   s_saved_channel  = 0;
static u32  s_saved_downmix  = 0;

static const char *mode_name(int m)
{
	switch (m) {
	case BITSTREAM_OFF:  return "off (LPCM)";
	case BITSTREAM_AC3:  return "AC-3";
	case BITSTREAM_DTS:  return "DTS";
	case BITSTREAM_RAW:  return "raw bitstream";
	case BITSTREAM_LPCM: return "LPCM re-configure";
	case BITSTREAM_LPCM_KICK: return "LPCM via 8ch transition";
	case BITSTREAM_PASSTHROUGH: return "passthrough";
	case BITSTREAM_PASSTHROUGH_FORCE: return "passthrough (forced)";
	default:             return "?";
	}
}

static int bitstream_file_value(void)
{
	FILE *f = fopen(jf_data_path(BITSTREAM_FILE), "r");
	if (!f) return -1;
	int v = BITSTREAM_OFF;
	if (fscanf(f, "%d", &v) != 1) v = BITSTREAM_OFF;
	fclose(f);
	if (v < BITSTREAM_OFF || v > BITSTREAM_PASSTHROUGH_FORCE) v = BITSTREAM_OFF;
	return v;
}

bool audio_passthrough_wanted(void)
{
	// The Dolby Digital audio output choice.  Always forced: the wire state is
	// not authoritative, so a mode that waits for it to change would back off
	// on a chain that is in fact carrying Dolby Digital.
	if (surround_get_mode() == SURROUND_BITSTREAM) return true;
	const int v = bitstream_file_value();
	return v == BITSTREAM_PASSTHROUGH || v == BITSTREAM_PASSTHROUGH_FORCE;
}

void audio_passthrough_request(bool on) { s_pt_requested = on; }
bool audio_passthrough_active(void)     { return s_passthrough; }

int bitstream_mode(void)
{
	// Passthrough replaces the decode entirely, so there is no 8 -> 6 fold to
	// protect and it outranks the 7.1 rule below.
	if (surround_get_mode() == SURROUND_BITSTREAM) return BITSTREAM_PASSTHROUGH_FORCE;
	if (audio_passthrough_wanted()) return bitstream_file_value();

	// 7.1 and the 5.1 routing request exclude each other, and 7.1 wins.  The
	// request names AC-3, which is 5.1 by definition, so on a chain wide enough
	// for 8 channels of LPCM it would only throw the rear pair away.
	if (surround_get_mode() == SURROUND_HD_71) return BITSTREAM_OFF;

	// On unless the file says otherwise: a dropped centre channel takes the
	// dialogue with it, and the request costs nothing on a chain that does not
	// need it.
	const int v = bitstream_file_value();
	return v < 0 ? BITSTREAM_AC3 : v;
}

static u8 coding_for(int mode)
{
	switch (mode) {
	case BITSTREAM_AC3: return AUDIO_OUT_CODING_AC3;
	case BITSTREAM_DTS: return AUDIO_OUT_CODING_DTS;
	case BITSTREAM_RAW:
	case BITSTREAM_PASSTHROUGH:
	case BITSTREAM_PASSTHROUGH_FORCE: return AUDIO_OUT_CODING_BITSTREAM;
	case BITSTREAM_LPCM:
	case BITSTREAM_LPCM_KICK:
	default:            return AUDIO_OUT_CODING_LPCM;
	}
}

// The output configuration is console-wide and outlives this process, so the
// state we change FROM is journalled to a file before the change and the file
// removed after the restore.  A launch that finds the file knows the last
// session died mid-change and puts the journalled state back
// (audio_bitstream_recover()); without it that broken state would be read as
// "what it was before" and faithfully restored by every later session.
#define JOURNAL_FILE "jellyfin_audioout.txt"

static void journal_write(void)
{
	FILE *f = fopen(jf_data_path(JOURNAL_FILE), "w");
	if (!f) return;
	fprintf(f, "%u %u %u\n", (unsigned)s_saved_encoder, (unsigned)s_saved_channel,
	        (unsigned)s_saved_downmix);
	fclose(f);
}

static void journal_clear(void)
{
	remove(jf_data_path(JOURNAL_FILE));
}

static s32 configure_lpcm(u8 ch, u32 downmix)
{
	audioOutConfiguration c;
	memset(&c, 0, sizeof(c));
	c.channel   = ch;
	c.encoder   = AUDIO_OUT_CODING_LPCM;
	c.downMixer = downmix;
	return audioOutConfigure(AUDIO_OUT_PRIMARY, &c, NULL, 1);
}

void audio_bitstream_recover(void)
{
	char b[160];
	unsigned enc = 0, ch = 0, dm = 0;
	FILE *f = fopen(jf_data_path(JOURNAL_FILE), "r");
	if (f) {
		const int got = fscanf(f, "%u %u %u", &enc, &ch, &dm);
		fclose(f);
		if (got == 3) {
			audioOutConfiguration back;
			memset(&back, 0, sizeof(back));
			back.encoder   = (u8)enc;
			back.channel   = (u8)(ch ? ch : 8);
			back.downMixer = dm;
			const s32 rc = audioOutConfigure(AUDIO_OUT_PRIMARY, &back, NULL, 1);
			snprintf(b, sizeof(b), "bitstream: last session died mid-change -- "
			         "restored encoder=%u ch=%u rc=%d", enc, ch, (int)rc);
			plog(b);
		}
		journal_clear();
		return;
	}
	// No journal, but the output is on a compressed coding anyway: a session
	// that predates the journal set it.  Put it back on LPCM, as wide as the
	// chain accepts.
	{
		audioOutConfiguration cur;
		memset(&cur, 0, sizeof(cur));
		if (audioOutGetConfiguration(AUDIO_OUT_PRIMARY, &cur, NULL) != 0) return;
		if (cur.encoder == AUDIO_OUT_CODING_LPCM) return;
		static const u8 WIDTHS[3] = { 8, 6, 2 };
		s32 rc = -1;
		unsigned used = 0;
		for (int i = 0; i < 3 && rc != 0; i++) {
			rc = configure_lpcm(WIDTHS[i], AUDIO_OUT_DOWNMIXER_NONE);
			used = WIDTHS[i];
		}
		snprintf(b, sizeof(b), "bitstream: output was left on encoder=%u ch=%u -- "
		         "reset to LPCM ch=%u rc=%d", (unsigned)cur.encoder,
		         (unsigned)cur.channel, used, (int)rc);
		plog(b);
	}
}

void audio_bitstream_begin(int port_channels)
{
	const int mode = bitstream_mode();
	if (mode == BITSTREAM_OFF) return;
	if (s_engaged) return;
	const bool pt = (mode == BITSTREAM_PASSTHROUGH || mode == BITSTREAM_PASSTHROUGH_FORCE);
	// Passthrough belongs to the video player alone; anything else opening
	// audio (the music player) makes no cellAudioOut calls under it.
	if (pt && !s_pt_requested) return;
	// The 5.1 routing request is only for a multichannel program: a stereo
	// port (music, stereo video) has no centre channel to fix, and every
	// reconfigure makes the TV or receiver drop and re-lock the HDMI audio,
	// which is the silent first seconds of a music session.
	if (!pt && port_channels < 6) return;

	// Remember exactly what we are changing, so the revert is a restore of the
	// real previous state rather than an assumption about what it was.
	audioOutConfiguration cur;
	memset(&cur, 0, sizeof(cur));
	if (audioOutGetConfiguration(AUDIO_OUT_PRIMARY, &cur, NULL) != 0) {
		plog("bitstream: getConfiguration failed, staying on LPCM");
		return;
	}
	s_saved_encoder = cur.encoder;
	s_saved_channel = cur.channel;
	s_saved_downmix = cur.downMixer;
	journal_write();          // before the change, so a crash can undo it

	char b[144];
	snprintf(b, sizeof(b), "bitstream: requesting %s (was encoder=%u ch=%u)",
	         mode_name(mode), (unsigned)cur.encoder, (unsigned)cur.channel);
	plog(b);

	// A compressed stream is carried as 5.1; asking for 8 makes no sense for
	// AC-3 or DTS, so cap it.  LPCM keeps whatever the port opened with.
	audioOutConfiguration want;
	memset(&want, 0, sizeof(want));
	want.channel   = (port_channels >= 6) ? 6 : (u8)port_channels;
	if (pt) want.channel = 2;   // IEC 61937 rides a stereo link
	want.encoder   = coding_for(mode);
	want.downMixer = AUDIO_OUT_DOWNMIXER_NONE;

	// LPCM via transition: go somewhere else first, so the call that lands on
	// 6ch LPCM is a real change rather than a request for what is already set.
	// 8ch is the detour because it is what the port is open at: if the second
	// call failed, being left wide is a far better failure than stereo.
	if (mode == BITSTREAM_LPCM_KICK) {
		audioOutConfiguration wide;
		memset(&wide, 0, sizeof(wide));
		wide.channel   = 8;
		wide.encoder   = AUDIO_OUT_CODING_LPCM;
		wide.downMixer = AUDIO_OUT_DOWNMIXER_NONE;
		const s32 krc = audioOutConfigure(AUDIO_OUT_PRIMARY, &wide, NULL, 1);
		if (krc == 0) s_applied = true;
		snprintf(b, sizeof(b), "bitstream: kick to 8ch LPCM rc=%d", (int)krc);
		plog(b);
	}

	const s32 rc = audioOutConfigure(AUDIO_OUT_PRIMARY, &want, NULL, 1);
	// Applied as soon as the call SUCCEEDS, not once the result is judged
	// good: a configure that returned 0 but did not land the encoder has still
	// written to the output, and end() must put it back.
	if (rc == 0) { s_applied = true; s_applied_cfg = want; }

	audioOutConfiguration after;
	memset(&after, 0, sizeof(after));
	audioOutGetConfiguration(AUDIO_OUT_PRIMARY, &after, NULL);
	snprintf(b, sizeof(b), "bitstream: configure rc=%d -> encoder=%u ch=%u",
	         (int)rc, (unsigned)after.encoder, (unsigned)after.channel);
	plog(b);

	if (rc != 0 || after.encoder != want.encoder) {
		// The console did not take it.  Do not leave a half-applied state.
		plog("bitstream: not accepted, reverting to LPCM");
		audio_bitstream_end();
		if (mode == BITSTREAM_PASSTHROUGH_FORCE) {
			// Forced: the bursts go out inside LPCM for a receiver that
			// detects them itself.
			s_passthrough = true;
			plog("bitstream: PASSTHROUGH ON, FORCED onto an LPCM wire");
		}
		return;
	}

	// The configuration read-back echoes what was asked for, and the output's
	// reported state does not reliably say what is on the wire either (see the
	// top of this file).  Both are logged; neither is a verdict.
	audioOutState st;
	memset(&st, 0, sizeof(st));
	const s32 srate = audioOutGetState(AUDIO_OUT_PRIMARY, 0, &st);
	snprintf(b, sizeof(b),
	         "bitstream: state rc=%d state=%u encoder=%u mode type=%u ch=%u fs=0x%02x",
	         (int)srate, (unsigned)st.state, (unsigned)st.encoder,
	         (unsigned)st.soundMode.type, (unsigned)st.soundMode.channel,
	         (unsigned)st.soundMode.fs);
	plog(b);

	if (pt) {
		const bool wire_bitstream = (srate == 0 && st.soundMode.type != AUDIO_OUT_CODING_LPCM);
		if (wire_bitstream || mode == BITSTREAM_PASSTHROUGH_FORCE) {
			s_passthrough = true;
			s_engaged     = true;
			snprintf(b, sizeof(b), "bitstream: PASSTHROUGH ON (GetState type=%u, not authoritative)",
			         (unsigned)st.soundMode.type);
			plog(b);
		} else {
			// Only the unforced research mode gets here: it trusts a state
			// that is not authoritative.
			plog("bitstream: passthrough refused, wire stayed LPCM -- decoding instead");
			audio_bitstream_end();
		}
		return;
	}

	if (srate == 0 && st.soundMode.type != want.encoder) {
		// Asked for a codec, and the state still says LPCM.  The request is
		// kept: its point is the 5.1 routing, not Dolby Digital.  Whether the
		// console also encodes depends on the PS3's Audio Output Settings and
		// cannot be read back, so this is logged as a request, not a result.
		snprintf(b, sizeof(b),
		         "bitstream: requested %s (GetState type=%u, not authoritative)",
		         mode_name(mode), (unsigned)st.soundMode.type);
		plog(b);
		s_engaged = false;      // not counted as a compressed stream
		return;
	}

	// The state reports the requested codec on the wire.  The console is then
	// encoding our audio to AC-3 or DTS on the way out.  Dolby Digital Live
	// runs at 640 kbps against roughly 4.6 Mbps for uncompressed 5.1 LPCM, so
	// that would throw away most of what the lossless decoder is for to fix a
	// routing problem the receiver may not have.  This app delivers lossless
	// audio, so it backs off and says why.
	if (srate == 0 && (mode == BITSTREAM_AC3 || mode == BITSTREAM_DTS)) {
		snprintf(b, sizeof(b),
		         "bitstream: wire reports %s (type=%u) -- that is LOSSY, "
		         "reverting to LPCM", mode_name(mode),
		         (unsigned)st.soundMode.type);
		plog(b);
		audio_bitstream_end();
		return;
	}

	s_engaged = true;
	snprintf(b, sizeof(b), "bitstream: ENGAGED %s at %u ch (wire type=%u)",
	         mode_name(mode), (unsigned)after.channel,
	         (unsigned)st.soundMode.type);
	plog(b);
}

void audio_bitstream_end(void)
{
	// Restore what we changed, and only if we changed it: an output this app
	// never touched is not written to, and "bitstream off" means no
	// cellAudioOut calls at all.
	s_passthrough = false;
	if (!s_applied) {
		s_engaged = false;
		journal_clear();
		return;
	}

	audioOutConfiguration back;
	memset(&back, 0, sizeof(back));
	back.channel   = s_saved_channel ? s_saved_channel : 8;
	back.encoder   = s_saved_encoder;
	back.downMixer = s_saved_downmix;
	const s32 rc = audioOutConfigure(AUDIO_OUT_PRIMARY, &back, NULL, 1);
	{
		char b[96];
		snprintf(b, sizeof(b), "bitstream: restored encoder=%u rc=%d",
		         (unsigned)s_saved_encoder, (int)rc);
		plog(b);
	}
	s_engaged = false;
	s_applied = false;
	journal_clear();
}

void audio_bitstream_reassert(const char *why)
{
	if (!s_applied) return;
	// A display mode change re-establishes the HDMI link and the system puts
	// its own audio setting back, so re-apply exactly what begin() applied:
	// the same channel count and coding, which differs between the 5.1
	// routing request (6 channels) and passthrough (2).
	audioOutConfiguration want = s_applied_cfg;
	const s32 rc = audioOutConfigure(AUDIO_OUT_PRIMARY, &want, NULL, 1);
	char b[128];
	snprintf(b, sizeof(b), "bitstream: re-asserted after %s rc=%d ch=%u encoder=%u",
	         why ? why : "?", (int)rc, (unsigned)want.channel, (unsigned)want.encoder);
	plog(b);
}

bool audio_bitstream_engaged(void) { return s_engaged; }
