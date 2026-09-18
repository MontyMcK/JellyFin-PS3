#pragma once
#include <ppu-types.h>

// -------------------------------------------------------------------------
//  cellAudioOut -- what the HDMI port CARRIES
// -------------------------------------------------------------------------
//  Distinct from PSL1GHT's audio.h (cellAudio), which only decides what
//  samples we hand the system mixer.  This family decides the wire format:
//  LPCM, or a compressed bitstream the receiver decodes itself.
//
//  PSL1GHT does not bind these, which is why the project's notes long said
//  "no bitstream passthrough exists on this platform".  The binding was
//  missing, not the capability -- see audio_out_stub.S.
//
//  Struct layouts follow the SDK as reimplemented in RPCS3's cellAudioOut.h.
//  The PPU is big-endian, so the be_t<> fields there are plain values here.

#ifdef __cplusplus
extern "C" {
#endif

#define AUDIO_OUT_PRIMARY        0
#define AUDIO_OUT_SECONDARY      1

// Coding type = the format on the wire.  LPCM is what this app has always
// used.  The rest are bitstream formats; which of them a given chain will
// actually accept is a property of the receiver, which is what
// audioOutGetSoundAvailability() and audioOutGetDeviceInfo() report.
#define AUDIO_OUT_CODING_LPCM            0
#define AUDIO_OUT_CODING_AC3             1
#define AUDIO_OUT_CODING_MPEG1           2
#define AUDIO_OUT_CODING_MP3             3
#define AUDIO_OUT_CODING_MPEG2           4
#define AUDIO_OUT_CODING_AAC             5
#define AUDIO_OUT_CODING_DTS             6
#define AUDIO_OUT_CODING_ATRAC           7
#define AUDIO_OUT_CODING_TRUEHD          8   // name unconfirmed
#define AUDIO_OUT_CODING_DDPLUS          9
#define AUDIO_OUT_CODING_DTS_HD_HIGHRES  10  // name unconfirmed
#define AUDIO_OUT_CODING_DTS_HD_MASTER   11  // name unconfirmed
#define AUDIO_OUT_CODING_BITSTREAM       0xff

#define AUDIO_OUT_FS_48KHZ       0x04

#define AUDIO_OUT_DOWNMIXER_NONE 0
#define AUDIO_OUT_DOWNMIXER_A    1
#define AUDIO_OUT_DOWNMIXER_B    2

typedef struct {
	u8  type;          // AUDIO_OUT_CODING_*
	u8  channel;
	u8  fs;
	u8  reserved;
	u32 layout;
} audioOutSoundMode;

typedef struct {
	u8  portType;
	u8  availableModeCount;
	u8  state;
	u8  reserved[3];
	u16 latency;
	audioOutSoundMode availableModes[16];
} audioOutDeviceInfo;

typedef struct {
	u8  state;
	u8  encoder;       // AUDIO_OUT_CODING_*
	u8  reserved[6];
	u32 downMixer;
	audioOutSoundMode soundMode;
} audioOutState;

typedef struct {
	u8  channel;
	u8  encoder;       // AUDIO_OUT_CODING_*
	u8  reserved[10];
	u32 downMixer;
} audioOutConfiguration;

// Bound in audio_out_stub.S / audio_out_fnid.c.
s32 audioOutGetNumberOfDevice(u32 audioOut);
s32 audioOutGetDeviceInfo(u32 audioOut, u32 deviceIndex, audioOutDeviceInfo *info);
s32 audioOutGetState(u32 audioOut, u32 deviceIndex, audioOutState *state);
s32 audioOutGetConfiguration(u32 audioOut, audioOutConfiguration *config, void *option);

// Returns the maximum channel count available for that coding type and sample
// rate, or 0 if the chain does not offer it.  A pure query -- it changes
// nothing, which is what makes it safe to call before anything else.
s32 audioOutGetSoundAvailability(u32 audioOut, u32 type, u32 fs, u32 option);
s32 audioOutGetSoundAvailability2(u32 audioOut, u32 type, u32 fs, u32 ch, u32 option);

s32 audioOutConfigure(u32 audioOut, audioOutConfiguration *config,
                      void *option, u32 waitForEvent);

// Read-only capability dump, logged at startup next to video_log_capabilities().
// Answers, from the hardware rather than from assumption, whether this chain
// will take AC-3 or DTS at all -- and therefore whether bitstream is worth
// pursuing on it.  Calls nothing that changes state.
void audio_out_log_capabilities(void);

// Widest LPCM the chain will take, in channels (2, 6, 8 ... ), or 0 if the
// query fails.  This is what decides whether a 7.1 output is offered at all:
// a soundbar that caps at 6 should not be shown a setting it cannot honour.
// Cached after the first call -- it is a property of the connected display,
// and the probe runs once at startup anyway.
int audio_out_lpcm_max_channels(void);

#ifdef __cplusplus
}
#endif
