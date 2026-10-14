#pragma once
#include <stdbool.h>
#include <stdint.h>

// -------------------------------------------------------------------------
//  AAC in ADTS: what the player's transport stream carries it as
// -------------------------------------------------------------------------
//  Matroska stores AAC as raw frames with an AudioSpecificConfig in CodecPrivate;
//  an MPEG transport stream carries it as ADTS frames (a 7-byte header before each
//  raw frame).  video/mkv_ts writes the second form so the demuxer and the decoder
//  (adec_aac) see what a real transport stream gives them.
//
//  Pure (no PS3 headers): tests/test_aac.cpp compiles this file.
// -------------------------------------------------------------------------

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int object_type;       // the core's: 1 Main, 2 LC, 3 SSR, 4 LTP (HE-AAC and PS name an LC core)
    int sf_index;          // the core's sampling frequency index (0..12)
    int sample_rate;       // the core's rate; an SBR decoder outputs twice it
    int channel_config;    // 1..7
    int channels;          // 1, 2, 3, 4, 5, 6, 8
} AacConfig;

// Reads an AudioSpecificConfig (MP4 / Matroska CodecPrivate).  False for what ADTS cannot carry or the
// decoder cannot play: a channel layout set in a program config element, more than 8 channels, an explicit
// sampling rate off the standard list, or an audio object that is not AAC Main / LC / SSR / LTP (or HE-AAC /
// PS on an LC core).
bool aac_parse_asc(const uint8_t *asc, int len, AacConfig *out);

// The 7-byte ADTS header (no CRC) for a raw frame of raw_len bytes.  False when the frame is too big for
// the 13-bit length (8184 bytes of payload at most).
bool aac_make_adts(const AacConfig *cfg, int raw_len, uint8_t hdr[7]);

// If p starts an ADTS frame: its length including the header (7 or 9 bytes of header, then the frame), else
// 0.  Needs at least 7 bytes.  The header's own checks only (sync word, layer 0, a known rate, a length of at
// least the header's): it cannot tell a header from the same bytes inside a frame, so the caller confirms with
// the next frame's header where it matters.
int aac_adts_frame_len(const uint8_t *p, int avail);

// A header's fields, for a stream that carries its own ("ADTS file": no ASC).
bool aac_parse_adts(const uint8_t *p, int avail, AacConfig *out);

#ifdef __cplusplus
}
#endif
