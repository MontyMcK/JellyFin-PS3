#pragma once
// Stereo PCM tap for the Canyon visualizer (sv_spectrum.h wants L and R
// separately; music_fft.cpp's tap is mono).  Same contract as music_viz_push:
// the decoder pushes every buffer it hands to the PCM ring, the UI thread
// takes the newest 512 pairs once per frame.

#include <ppu-types.h>

void music_sv_reset(void);

// Decoder side: n interleaved stereo float pairs.  Thread-safe.
void music_sv_push(const float *lr, int n_pairs);

// UI side: copy the newest 512 pairs into lr[1024].  Returns false when no
// new audio has arrived since the last call (paused / stalled / stopped).
bool music_sv_latest(float *lr);
