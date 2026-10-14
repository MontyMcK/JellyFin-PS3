#pragma once
// viz_frame -- one frame of audio features shared by every visualizer preset
// (JellyWave, Canyon, JellyCanyon, JellyCrystal).  Filled once per frame from
// the ui_wave_audio.h getters; renderers read it and never touch the analyser.
// Not wired in yet: JellyCanyon stage 1.  JellyWave keeps its own getters.

#include <stdbool.h>

typedef struct {
    float band[6];          // wave_audio.h WA_SUB..WA_AIR, 0..1
    float lvl3[3];          // distinct lows / mids / highs (wave_audio_bands)
    float kick;             // pending sub-bass kick, 0 = none
    float flux;             // onset strength 0..1
    float centroid;         // spectral tilt 0 dark .. 1 bright (pitch stand-in)
    float beat_hz, beat_conf, beat_phase;
    float stereo_bal;       // -1 left .. +1 right (wave_scope.h)
    float stereo_width;     // 0 mono .. 1 wide
    float presence;         // 0 at rest .. 1 with music
    const float *specL, *specR;   // sv_spectrum.h, 256 bands each; may be NULL
    float dt;
    bool  paused;
} viz_frame;
