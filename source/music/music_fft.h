#pragma once
// Audio-reactive spectrum bands for the music player's visualizer —
// MediaWave's band math (mediawave-fft.py) ported to C.  The decoder taps
// mono samples into a ring; every UI frame music_viz_bands() runs a Hann
// window + 1024-pt FFT over the newest samples and folds the magnitudes
// into MUSIC_VIZ_BANDS log-spaced bands (40 Hz – 16 kHz), normalized to the
// frame max and smoothed with fast-attack / slow-decay so the bars dance.

#include <ppu-types.h>

#define MUSIC_VIZ_BANDS 28

// Reset tap + smoothing state (call at playback start).
void music_viz_reset(void);

// Decoder-side tap: push n interleaved stereo float pairs (the same buffer
// that just went into the PCM ring).  Thread-safe against music_viz_bands.
void music_viz_push(const float *lr, int n_pairs);

// UI-side: fill out[MUSIC_VIZ_BANDS] with smoothed 0..1 band levels.
// When no new audio has arrived since the last call (paused / stalled /
// stopped) the bands decay smoothly toward zero.
void music_viz_bands(float *out);
