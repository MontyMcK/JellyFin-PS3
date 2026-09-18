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
	default:             return "?";
	}
}

int bitstream_mode(void)
{
	FILE *f = fopen(jf_data_path(BITSTREAM_FILE), "r");
	if (!f) return BITSTREAM_OFF;
	int v = BITSTREAM_OFF;
	if (fscanf(f, "%d", &v) != 1) v = BITSTREAM_OFF;
	fclose(f);
	if (v < BITSTREAM_OFF || v > BITSTREAM_RAW) v = BITSTREAM_OFF;
	return v;
}

static u8 coding_for(int mode)
{
	switch (mode) {
	case BITSTREAM_AC3: return AUDIO_OUT_CODING_AC3;
	case BITSTREAM_DTS: return AUDIO_OUT_CODING_DTS;
	case BITSTREAM_RAW: return AUDIO_OUT_CODING_BITSTREAM;
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

	const s32 rc = audioOutConfigure(AUDIO_OUT_PRIMARY, &want, NULL, 1);

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
	s_applied = true;

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
		// Accepted on paper, something else on the wire.  Leave it applied --
		// it is harmless, and on this chain it coincided with the centre
		// channel finally working -- but do not call it engaged, because the
		// output is not reporting the coding type we asked for.
		snprintf(b, sizeof(b),
		         "bitstream: CONFIG-ONLY -- asked %s, wire reports type=%u",
		         mode_name(mode), (unsigned)st.soundMode.type);
		plog(b);
		s_engaged = false;
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
	// Runs even when begin() bailed part-way, which is the point: the console
	// is a shared resource and the next app -- or the XMB -- should not inherit
	// a coding type this one asked for.
	audioOutConfiguration back;
	memset(&back, 0, sizeof(back));
	back.channel   = s_saved_channel ? s_saved_channel : 8;
	back.encoder   = s_saved_encoder;
	back.downMixer = s_saved_downmix;
	const s32 rc = audioOutConfigure(AUDIO_OUT_PRIMARY, &back, NULL, 1);
	if (s_applied) {
		char b[96];
		snprintf(b, sizeof(b), "bitstream: restored encoder=%u rc=%d",
		         (unsigned)s_saved_encoder, (int)rc);
		plog(b);
	}
	s_engaged = false;
	s_applied = false;
}

bool audio_bitstream_engaged(void) { return s_engaged; }
