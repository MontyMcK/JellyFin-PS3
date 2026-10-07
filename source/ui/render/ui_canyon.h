#pragma once
// The Canyon music visualizer on the RSX -- the drawing side of canyon.h.
//
// The music screen draws it IN PLACE OF wave_draw() when the visualizer mode
// is Canyon (tap Square on the music screen: Wave <-> Canyon, persisted in
// jellyfin_visualizer.txt; hold Square: the next Canyon preset).
//
// Presets are read at runtime from the console's own
// /dev_flash/vsh/resource/qgl/canyon.qrc; without it the built-in look is used.
// The two vertex buffers (~0.8 MB each, RSX local memory) are allocated on
// first use and kept.

#include <ppu-types.h>

enum { VIZ_WAVE = 0, VIZ_CANYON = 1, VIZ_COUNT = 2 };

int  viz_mode(void);                 // loads jellyfin_visualizer.txt on first call
void viz_set_mode(int mode);         // set + persist
const char *viz_mode_name(int mode);

// Canyon.  canyon_draw() returns false if the visualizer could not start
// (allocation failed); the caller then draws the wave instead.
// alpha < 1: the canyon is fading in (or out) OVER whatever the caller drew
// first -- the music screen keeps the wave underneath until it reaches 1.
bool canyon_draw(float bright, bool paused, float alpha = 1.0f);
// The album's accent (0x00RRGGBB, 0 = none): every preset's colours lean
// `amount` of the way toward its hue at their own brightness, so each song
// keeps its own preset but takes a hint of the cover.  Eased, no pops.
void canyon_set_tint(u32 rgb, float amount);
void canyon_track(int track_index);  // a new song: cross-fade to its preset
void canyon_next_preset(void);       // Square: cross-fade to the next preset
const char *canyon_preset_name(void);
