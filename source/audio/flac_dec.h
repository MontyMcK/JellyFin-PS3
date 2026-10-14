#pragma once
#include <stdbool.h>
#include <stdint.h>

// -------------------------------------------------------------------------
//  FLAC frames: a decoder for a stream that arrives in pieces
// -------------------------------------------------------------------------
//  A FLAC stream is "fLaC", metadata blocks (the first is STREAMINFO), then frames.  In
//  Matroska the metadata is the track's CodecPrivate and every block is one frame; a file
//  is the whole stream.  Both feed this decoder the same way: the header once, then bytes
//  that may end in the middle of a frame, which the decoder reports (0) instead of guessing.
//
//  Pure C (no PS3 headers, no libFLAC): tests/test_flac.cpp compiles this file and checks
//  every sample against ffmpeg's decoder.  8 to 24 bits, 1 to 8 channels, every sample rate
//  and block size the format has; fixed and LPC prediction, Rice coding of both widths,
//  wasted bits, all four channel modes.  Frames are checked by their CRC-8 and CRC-16, so
//  a damaged one is skipped rather than played.
// -------------------------------------------------------------------------

#ifdef __cplusplus
extern "C" {
#endif

#define FLAC_MAX_CHANNELS 8
#define FLAC_MAX_BLOCK    65535

typedef struct {
    int      min_block, max_block;
    int      sample_rate, channels, bps;
    uint64_t total_samples;           // 0 = not stated
} FlacInfo;

// Reads "fLaC" and the metadata blocks at buf (STREAMINFO is required).  Returns the bytes up to the
// first frame; 0 when buf ends before the last block does (give it more); -1 when this is not a FLAC
// stream or has no STREAMINFO.
int flac_parse_header(const uint8_t *buf, int len, FlacInfo *info);

typedef struct {
    int blocksize;                    // samples in each channel of this frame
    uint64_t sample_pos;              // the stream's sample this frame starts at (from its header, so a seek can find where it is)
    int channels;
    int bps;                          // bits per sample (samples are right-justified in an int32)
    int sample_rate;
} FlacFrame;

// Decodes the frame that starts at buf into out[0..channels), blocksize samples each (left, right, ...
// already decorrelated).  Returns the bytes the frame took (> 0); 0 when buf ends inside it (or inside
// what could be its header): call again with more bytes; -1 when no valid frame starts here (the caller
// skips a byte and looks for the next sync 0xFFF8 / 0xFFF9) or a block is bigger than out_cap.
// `info` supplies the rate and sample size a frame header may leave to STREAMINFO.
int flac_decode_frame(const uint8_t *buf, int len, const FlacInfo *info, int32_t *const out[FLAC_MAX_CHANNELS],
                      int out_cap, FlacFrame *frame);

#ifdef __cplusplus
}
#endif
