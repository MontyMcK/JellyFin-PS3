#pragma once
#include <ppu-types.h>

// -------------------------------------------------------------------------
//  The end of a decoder: channels in the port's order, at 48 kHz, into the ring
// -------------------------------------------------------------------------
//  The decoders for files on a drive (AAC, FLAC, PCM) each produce interleaved floats at
//  the rate and in the channel order the file has.  This puts them where adec.cpp's ring
//  wants them: mapped to the port's frame (chan_map.h), converted to 48 kHz (resample.h),
//  and pushed with adec_push_frames.  One decoder is open at a time, as in adec.cpp, so the
//  state is one set of statics.
// -------------------------------------------------------------------------

// Starts an output of out_channels (6: FL FR FC LFE SL SR, or 2: FL FR) frames; forgets any rate.
void adec_out_open(int out_channels);
void adec_out_close(void);
// A seek: the resampler's history goes.
void adec_out_reset(void);

// Pushes `frames` interleaved frames of `ch` channels at `rate` Hz (floats, +-1.0).  `pos` says where
// each channel belongs (chan_map.h; NULL = the usual order).  A rate the resampler cannot convert is
// logged once and the audio dropped.
void adec_out_push(const float *in, int frames, int ch, const unsigned char *pos, int rate);
