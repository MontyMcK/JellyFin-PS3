// Read-only dump of what the audio chain will actually accept.
//
// This exists for the same reason video_log_capabilities() does: the question
// "can this console bitstream?" was answered for years by reading headers and
// assuming, and the assumption was wrong -- PSL1GHT simply never bound
// cellAudioOut, so nothing in the app could ask.  Now it can, so ask the
// hardware instead of arguing about it.
//
// Every call here is a query.  Nothing configures the output, nothing draws,
// nothing can leave the audio path in a state the user has to recover from --
// which matters, because the equivalent experiment on the VIDEO side did
// exactly that when it went beyond querying.

#include <stdio.h>
#include <string.h>

#include "audio_out.h"
#include "plog.h"

static const char *coding_name(u8 t)
{
	switch (t) {
	case AUDIO_OUT_CODING_LPCM:           return "LPCM";
	case AUDIO_OUT_CODING_AC3:            return "AC-3";
	case AUDIO_OUT_CODING_MPEG1:          return "MPEG1";
	case AUDIO_OUT_CODING_MP3:            return "MP3";
	case AUDIO_OUT_CODING_MPEG2:          return "MPEG2";
	case AUDIO_OUT_CODING_AAC:            return "AAC";
	case AUDIO_OUT_CODING_DTS:            return "DTS";
	case AUDIO_OUT_CODING_ATRAC:          return "ATRAC";
	case AUDIO_OUT_CODING_TRUEHD:         return "TrueHD?";
	case AUDIO_OUT_CODING_DDPLUS:         return "DD+";
	case AUDIO_OUT_CODING_DTS_HD_HIGHRES: return "DTS-HD HR?";
	case AUDIO_OUT_CODING_DTS_HD_MASTER:  return "DTS-HD MA?";
	case AUDIO_OUT_CODING_BITSTREAM:      return "BITSTREAM";
	default:                              return "?";
	}
}

void audio_out_log_capabilities(void)
{
	char b[160];

	const s32 ndev = audioOutGetNumberOfDevice(AUDIO_OUT_PRIMARY);
	snprintf(b, sizeof(b), "aout: devices=%d", (int)ndev);
	plog(b);
	// A negative return here means the binding itself did not resolve, which
	// is worth saying plainly rather than reporting "0 devices".
	if (ndev < 0) {
		plog("aout: cellAudioOut did not bind - no bitstream query possible");
		return;
	}

	// What the port is doing right now.
	{
		audioOutState st;
		memset(&st, 0, sizeof(st));
		if (audioOutGetState(AUDIO_OUT_PRIMARY, 0, &st) == 0) {
			snprintf(b, sizeof(b),
			         "aout: state=%u encoder=%u(%s) downmix=%u mode ch=%u fs=0x%02x layout=0x%x",
			         (unsigned)st.state, (unsigned)st.encoder,
			         coding_name(st.encoder), (unsigned)st.downMixer,
			         (unsigned)st.soundMode.channel, (unsigned)st.soundMode.fs,
			         (unsigned)st.soundMode.layout);
			plog(b);
		} else {
			plog("aout: getState failed");
		}
	}

	{
		audioOutConfiguration cfg;
		memset(&cfg, 0, sizeof(cfg));
		if (audioOutGetConfiguration(AUDIO_OUT_PRIMARY, &cfg, NULL) == 0) {
			snprintf(b, sizeof(b), "aout: config ch=%u encoder=%u(%s) downmix=%u",
			         (unsigned)cfg.channel, (unsigned)cfg.encoder,
			         coding_name(cfg.encoder), (unsigned)cfg.downMixer);
			plog(b);
		}
	}

	// Every mode the SINK advertises.  This is the equivalent of the video
	// probe's mode list, and it is the thing that settles whether the user's
	// receiver takes AC-3 or DTS at all.
	{
		audioOutDeviceInfo di;
		memset(&di, 0, sizeof(di));
		if (audioOutGetDeviceInfo(AUDIO_OUT_PRIMARY, 0, &di) == 0) {
			snprintf(b, sizeof(b),
			         "aout: portType=%u state=%u latency=%u modes=%u",
			         (unsigned)di.portType, (unsigned)di.state,
			         (unsigned)di.latency, (unsigned)di.availableModeCount);
			plog(b);
			int n = di.availableModeCount;
			if (n > 16) n = 16;
			for (int i = 0; i < n; i++) {
				const audioOutSoundMode *m = &di.availableModes[i];
				snprintf(b, sizeof(b),
				         "aout:   mode[%d] type=%u(%-10s) ch=%u fs=0x%02x layout=0x%x",
				         i, (unsigned)m->type, coding_name(m->type),
				         (unsigned)m->channel, (unsigned)m->fs,
				         (unsigned)m->layout);
				plog(b);
			}
		} else {
			plog("aout: getDeviceInfo failed");
		}
	}

	// Ask directly about the formats worth having.  The return value is the
	// maximum channel count offered, so 0 means "not on this chain".
	{
		static const u8 want[] = {
			AUDIO_OUT_CODING_LPCM, AUDIO_OUT_CODING_AC3, AUDIO_OUT_CODING_DTS,
			AUDIO_OUT_CODING_DDPLUS, AUDIO_OUT_CODING_TRUEHD,
			AUDIO_OUT_CODING_DTS_HD_MASTER, AUDIO_OUT_CODING_BITSTREAM,
		};
		for (unsigned i = 0; i < sizeof(want) / sizeof(want[0]); i++) {
			s32 ch = audioOutGetSoundAvailability(AUDIO_OUT_PRIMARY, want[i],
			                                      AUDIO_OUT_FS_48KHZ, 0);
			snprintf(b, sizeof(b), "aout: avail %-10s type=%-3u -> %d ch%s",
			         coding_name(want[i]), (unsigned)want[i], (int)ch,
			         ch > 0 ? "  <-- OFFERED" : "");
			plog(b);
		}
	}
}

int audio_out_lpcm_max_channels(void)
{
	static int s_cached = -1;
	if (s_cached >= 0) return s_cached;
	const s32 ch = audioOutGetSoundAvailability(AUDIO_OUT_PRIMARY,
	                                            AUDIO_OUT_CODING_LPCM,
	                                            AUDIO_OUT_FS_48KHZ, 0);
	s_cached = (ch > 0) ? (int)ch : 0;
	return s_cached;
}
