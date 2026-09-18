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
//   AC-3 / DTS      "Bitstream (Mix)".  The system encodes our LPCM to Dolby
//                   Digital or DTS and sends that.  The receiver decodes it
//                   itself rather than being handed 6 channels of LPCM, which
//                   is the whole point on a chain that mishandles multichannel
//                   LPCM -- e.g. one that silently drops the centre channel.
//                   The console owns the encoder (libddlenc2.sprx /
//                   libdtsenc2.sprx); we do not have to load or drive it.
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

static bool s_engaged        = false;
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
	s_engaged = true;
	snprintf(b, sizeof(b), "bitstream: ENGAGED %s at %u ch",
	         mode_name(mode), (unsigned)after.channel);
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
	if (s_engaged) {
		char b[96];
		snprintf(b, sizeof(b), "bitstream: restored encoder=%u rc=%d",
		         (unsigned)s_saved_encoder, (int)rc);
		plog(b);
	}
	s_engaged = false;
}

bool audio_bitstream_engaged(void) { return s_engaged; }
