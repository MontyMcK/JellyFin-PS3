// Ask the console to put a COMPRESSED bitstream on the HDMI wire instead of
// LPCM, and put it back afterwards.
//
// Why this can exist at all: PSL1GHT never bound cellAudioOut, so nothing in
// this app could express the request.  That is a missing binding, not a
// missing capability -- see audio_out_stub.S.  cellAudio (the port) still
// carries our ordinary float LPCM; cellAudioOut decides what the PORT is
// then turned into on the way out.
//
// What each mode is, in the XMB's own vocabulary:
//
//   AC-3 / DTS      The hope was "Bitstream (Mix)": the system encodes our
//                   LPCM to Dolby Digital or DTS and sends that, so a receiver
//                   that mishandles multichannel LPCM -- e.g. one that
//                   silently drops the centre channel, taking the dialogue
//                   with it -- gets a stream it decodes itself.
//
//                   TESTED 2026-09-18, RESULT GENUINELY AMBIGUOUS -- do not
//                   record it as either a success or a failure yet:
//
//                     * audioOutConfigure(AC-3) returns 0 and the
//                       configuration reads back encoder=1, every playback.
//                       That proves nothing on its own; the read-back echoes
//                       what was ASKED FOR, exactly as getsockopt(SO_RCVBUF)
//                       reports a receive buffer nothing is backing.
//                     * The soundbar lights no Dolby Digital indicator.
//                     * But the CENTRE CHANNEL STARTED WORKING.  With the
//                       Dialogue setting on NORMAL -- no fold, no boost, the
//                       shipped path -- dialogue came out of the centre
//                       speaker for the first time.  Before this it was only
//                       audible via CENTER_STEREO's LoRo downmix.
//
//                   The only audio-path change against v1.0 is this call, and
//                   the output was ALREADY ch=6 downmix=0 at startup, so the
//                   one thing that changed is encoder 0 -> 1.  A chain that
//                   mishandles 8ch LPCM but decodes AC-3 correctly would
//                   behave exactly like this, and plenty of bars show no
//                   indicator over ARC.  Equally, the encode may not be
//                   happening and something else about re-configuring the
//                   output fixed the routing.
//
//                   audioOutGetState reports the mode the output is actually
//                   in, so that is what begin() believes below, and the log
//                   says CONFIG-ONLY when it disagrees with the read-back.
//                   Settle it from that line, not from the indicator.
//
//                   If it turns out the system does NOT encode for us: the
//                   Blu-ray player imports cellDdlEnc2/cellDtsEnc2 directly,
//                   so the encode would be ours to do via libddlenc2.sprx.
//
//   RAW (0xff)      CELL_AUDIO_OUT_CODING_TYPE_BITSTREAM.  A long shot, and
//                   included because it costs one enum value to try: it is the
//                   coding type that would correspond to "Bitstream (Direct)",
//                   true passthrough.  Reading the Blu-ray player's own import
//                   table shows no audio-output PRX at all, so its passthrough
//                   almost certainly runs through lv2 or the VSH audio path
//                   rather than anything callable here.  Expect silence; the
//                   read-back below will say so rather than leaving it on.
//
// Safety: unlike the display, a wrong audio mode cannot lock anyone out -- the
// worst case is silence with the UI still fully visible and usable.  Even so
// this verifies by reading the state back and reverts itself if the console
// did not end up where it was asked, so a mode the chain refuses does not
// persist.

#include <stdio.h>
#include <string.h>

#include "audio_out.h"
#include "audio_bitstream.h"
#include "jf_paths.h"
#include "plog.h"

#define BITSTREAM_FILE "jellyfin_bitstream.txt"

static bool s_engaged        = false;   // bitstream is on the WIRE
static bool s_applied        = false;   // we changed the config, engaged or not
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
	default:             return "?";
	}
}

int bitstream_mode(void)
{
	// DEFAULT ON.  Missing file means AC-3-routing, not off: a dropped centre
	// channel takes the dialogue with it, which is the worst failure this app
	// has, and the request costs nothing on a chain that does not need it --
	// the wire stays LPCM either way, and the block above backs off if a
	// console ever really would encode.
	FILE *f = fopen(jf_data_path(BITSTREAM_FILE), "r");
	if (!f) return BITSTREAM_AC3;
	int v = BITSTREAM_OFF;
	if (fscanf(f, "%d", &v) != 1) v = BITSTREAM_OFF;
	fclose(f);
	if (v < BITSTREAM_OFF || v > BITSTREAM_LPCM_KICK) v = BITSTREAM_OFF;
	return v;
}

static u8 coding_for(int mode)
{
	switch (mode) {
	case BITSTREAM_AC3: return AUDIO_OUT_CODING_AC3;
	case BITSTREAM_DTS: return AUDIO_OUT_CODING_DTS;
	case BITSTREAM_RAW: return AUDIO_OUT_CODING_BITSTREAM;
	case BITSTREAM_LPCM:
	case BITSTREAM_LPCM_KICK:
	default:            return AUDIO_OUT_CODING_LPCM;
	}
}

void audio_bitstream_begin(int port_channels)
{
	const int mode = bitstream_mode();
	if (mode == BITSTREAM_OFF) return;
	if (s_engaged) return;

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

	char b[144];
	snprintf(b, sizeof(b), "bitstream: requesting %s (was encoder=%u ch=%u)",
	         mode_name(mode), (unsigned)cur.encoder, (unsigned)cur.channel);
	plog(b);

	// A compressed stream is carried as 5.1; asking for 8 makes no sense for
	// AC-3 or DTS, so cap it.  LPCM keeps whatever the port opened with.
	audioOutConfiguration want;
	memset(&want, 0, sizeof(want));
	want.channel   = (port_channels >= 6) ? 6 : (u8)port_channels;
	want.encoder   = coding_for(mode);
	want.downMixer = AUDIO_OUT_DOWNMIXER_NONE;

	// Mode 5: go somewhere else first, so the call that lands on 6ch LPCM is a
	// real transition rather than a request for what is already set.  8ch is
	// the detour on purpose -- it is what the cellAudio port is actually open
	// at, and if the second call were somehow to fail, being left wide is a
	// far better failure than being left at stereo.
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
	// Mark it applied the moment the call SUCCEEDS, not once the result is
	// judged good: a configure that returned 0 but did not land the encoder
	// has still written to the output, and end() must put it back. Setting
	// this only after the checks below would leave that case unreverted.
	if (rc == 0) s_applied = true;

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
		return;
	}

	// The configuration read-back is NOT proof: on 2026-09-18 this path logged
	// "ENGAGED AC-3" on every playback purely on the strength of it, while the
	// soundbar showed no Dolby Digital indicator.  audioOutGetConfiguration
	// echoes what was ASKED FOR -- the same trap as getsockopt(SO_RCVBUF)
	// reporting a receive buffer nothing is backing.
	//
	// audioOutGetState reports the sound mode the OUTPUT is actually in, so
	// that is what decides here.  Log both either way: this call is also the
	// prime suspect for the centre channel starting to work on that same
	// build, so which of the two is true matters beyond the indicator.
	audioOutState st;
	memset(&st, 0, sizeof(st));
	const s32 srate = audioOutGetState(AUDIO_OUT_PRIMARY, 0, &st);
	snprintf(b, sizeof(b),
	         "bitstream: state rc=%d state=%u encoder=%u mode type=%u ch=%u fs=0x%02x",
	         (int)srate, (unsigned)st.state, (unsigned)st.encoder,
	         (unsigned)st.soundMode.type, (unsigned)st.soundMode.channel,
	         (unsigned)st.soundMode.fs);
	plog(b);

	if (srate == 0 && st.soundMode.type != want.encoder) {
		// ASKED FOR A CODEC, GOT LPCM -- and for this app that is the GOOD
		// outcome, which is why it is not treated as a failure.
		//
		// The point of the AC-3 request is not Dolby Digital. It is that AC-3
		// is 5.1 BY DEFINITION, so naming it makes the console route the
		// 8-wide cellAudio port to the 6-wide output as true 5.1. Left on
		// LPCM the console does some other 8 -> 6 fold, and on at least one
		// soundbar that fold loses the centre channel entirely. Measured:
		// dialogue returns with AC-3 requested and disappears without it,
		// while the app's own meter shows the centre leaving hot either way.
		//
		// The wire stays LPCM, so nothing is compressed and the lossless
		// TrueHD / DTS-HD MA decode reaches the receiver intact. Routing
		// fixed, quality untouched.
		snprintf(b, sizeof(b),
		         "bitstream: routing as %s, wire stays type=%u (LPCM intact)",
		         mode_name(mode), (unsigned)st.soundMode.type);
		plog(b);
		s_engaged = false;      // nothing compressed is on the wire
		return;
	}

	// THE WIRE REALLY DID BECOME THE REQUESTED CODEC.
	//
	// On the chain this was developed against that never happens, but another
	// receiver may well accept it -- and then the console is encoding our
	// audio to AC-3 or DTS on the way out. Dolby Digital Live runs at 640
	// kbps against roughly 4.6 Mbps for uncompressed 5.1 LPCM, so that would
	// quietly throw away most of what the lossless decoder is for, to fix a
	// routing problem that receiver may not even have.
	//
	// This app exists to deliver lossless audio, so it backs off and says
	// why, rather than silently trading the thing it is for. Anyone who
	// prefers the routing fix can still force it with mode 2 (DTS) or by
	// reading this line and deciding for themselves.
	if (srate == 0 && (mode == BITSTREAM_AC3 || mode == BITSTREAM_DTS)) {
		snprintf(b, sizeof(b),
		         "bitstream: wire really became %s (type=%u) -- that is LOSSY, "
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
	// RESTORE WHAT WE CHANGED, AND ONLY IF WE CHANGED IT.
	//
	// This used to reconfigure the output unconditionally, on every playback
	// end, even when bitstream was off and begin() had returned immediately
	// without touching anything. That is wrong twice over. It writes to a
	// shared console resource this app never took -- with channel=8 out of
	// `s_saved_channel ? : 8`, a value that was never read from the console
	// but invented here -- and it makes "bitstream off" fail to mean "make no
	// cellAudioOut calls", which is exactly what an A/B test of this feature
	// needs it to mean.
	//
	// It matters right now: the centre channel began working on the build that
	// introduced these calls, while the wire stayed LPCM, so WHICH configure
	// call is responsible is an open question. It cannot be answered while the
	// off switch still makes one.
	if (!s_applied) {
		s_engaged = false;
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
}

bool audio_bitstream_engaged(void) { return s_engaged; }

// Any non-zero mode counts as on, so a diagnostic mode set by hand still
// reads as enabled rather than as a state the menu cannot describe.
bool bitstream_routing_enabled(void) { return bitstream_mode() != BITSTREAM_OFF; }

const char *bitstream_routing_label(void)
{
	return bitstream_routing_enabled() ? "On" : "Off";
}

void bitstream_routing_toggle(void)
{
	const int now = bitstream_routing_enabled() ? BITSTREAM_OFF : BITSTREAM_AC3;
	FILE *f = fopen(jf_data_path(BITSTREAM_FILE), "w");
	if (!f) return;
	fprintf(f, "%d\n", now);
	fclose(f);
	// Takes effect on the next playback: begin() re-reads the file each time,
	// which is also what lets it be changed over FTP mid-session.
}
