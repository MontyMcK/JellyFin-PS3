#pragma once
#include <stdbool.h>

// -------------------------------------------------------------------------
//  Sample-rate conversion to the CellAudio port's 48 kHz
// -------------------------------------------------------------------------
//  The port is fixed at 48 kHz.  Audio from the server is asked for at 48 kHz, but
//  a file on a drive keeps the rate it was made at (44.1 kHz for most stereo AAC,
//  MP3 and FLAC), and playing that unconverted runs it 8.8 % fast.
//
//  A polyphase windowed-sinc converter (Kaiser window, DC gain exactly 1 in every
//  phase) for the rates below.  The filter passes up to 0.9 of the lower of the two
//  Nyquist frequencies (20 kHz, for 44.1 -> 48) and rejects from the lower Nyquist
//  itself, by about 96 dB; the cost is 126 multiply-adds per output frame and
//  channel for 44.1 kHz, up to ~500 for 176.4 and 192 kHz.  The coefficient table
//  (80 KB for 44.1 kHz, 322 KB for 11.025 kHz) is built when the converter is
//  opened; processing allocates nothing.  48 kHz input is a copy.
//
//  Pure (no PS3 headers): tests/test_resample.cpp compiles this file.
// -------------------------------------------------------------------------

#ifdef __cplusplus
extern "C" {
#endif

typedef struct Resampler Resampler;

// The rates it converts: 8000 11025 12000 16000 22050 24000 32000 44100 48000 64000 88200 96000
// 176400 192000.
bool resample_rate_supported(int in_rate);

// A converter from in_rate to 48000 for `channels` (1..8) interleaved channels, or NULL for an
// unsupported rate, channel count or no memory.
Resampler *resample_open(int in_rate, int channels);
void resample_close(Resampler *r);

// Forgets the signal so far (after a seek): the next frames start a new one.
void resample_reset(Resampler *r);

// Converts n_in interleaved frames.  Writes at most out_cap frames and returns how many; consumes
// all of the input (so out_cap must be at least resample_max_out(r, n_in), or the surplus is lost).
int resample_process(Resampler *r, const float *in, int n_in, float *out, int out_cap);

// An upper bound of the frames resample_process makes from n_in.
int resample_max_out(const Resampler *r, int n_in);

#ifdef __cplusplus
}
#endif
