#pragma once
#include <ppu-types.h>

// Initialize minimp3 state and PCM ring buffer.  Call once before playback.
void adec_init(void);

// Which decoder consumes the PES queue.  Selected at runtime from the PMT
// stream type by the TS demux (0x03/0x04 → MP3, 0x81/0x06+desc → AC-3) —
// never from a compile flag, because the server can refuse AC-3 and send
// MP3 anyway, and that must degrade to working stereo, not noise.
typedef enum {
    ADEC_CODEC_MP3 = 0,   // minimp3, stereo — the shipped path and default
    ADEC_CODEC_AC3 = 1,   // liba52, up to 5.1 (see adec_ac3.h)
} adec_codec_t;

// Switch decoder.  Call from the demux when the PMT selects the audio
// stream, BEFORE the first adec_push_pes() of that stream; flushes the PCM
// ring when the codec (or its channel width) actually changes.  Selection
// survives adec_flush(), so a seek keeps the codec the demux chose.
void adec_set_codec(adec_codec_t codec);
adec_codec_t adec_get_codec(void);

// Spawn the dedicated audio decode thread.  Call after adec_init().
void adec_start(void);

// Signal the audio decode thread to stop and join it.  Call before audio_close().
void adec_stop(void);

// Drain PES queue and PCM ring without stopping the decode thread.  Call on seek.
void adec_flush(void);

// Feed a complete audio PES packet (PES header included).
// Strips the header, runs mp3dec_decode_frame() on every frame found in the
// payload, and pushes the resulting stereo PCM into the ring buffer.
void adec_push_pes(const u8 *pes, int pes_len);

// Decoder back-pressure threshold: the decode thread idles once the PCM ring
// holds this many FRAMES (~1.0s @ 48 kHz, any width).  Exposed so the stats overlay
// can express ring occupancy against the level the decoder actually targets —
// 100% means a full second of runway, and a slide toward 0 is the starvation
// that produces the choppy-audio dropouts.
#define PCM_RING_HIGHWATER  48000

// PCM frames currently available in the ring.
int  adec_pcm_available(void);

// Copy up to n_frames interleaved float32 frames into buf[].  Each frame is
// adec_output_channels() floats wide.  Returns the number of frames written —
// may be less than n_frames if the ring is empty.
int  adec_read_pcm(float *buf, int n_frames);

// Interleave width of the frames adec_read_pcm() returns: 2 (stereo MP3, the
// shipped path).  The AC-3 decoder raises this to 6 when it owns the ring.
int  adec_output_channels(void);

// INTERNAL (adec_ac3.cpp → adec.cpp): push n interleaved float frames of
// adec_output_channels() width into the PCM ring and advance the write PTS.
// Runs on the adec thread.  Not for use outside the decoder modules.
void adec_push_frames(const float *frames, int n);

// PTS (stream microseconds) of the next sample adec_read_pcm() would return.
// Returns 0 until the first PES with a PTS has been decoded.
u64  adec_get_read_pts_us(void);
