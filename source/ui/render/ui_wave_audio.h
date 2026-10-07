#pragma once
#include <ppu-types.h>
#include "wave_render_map.h"
#include "wave_deform.h"
#include "viz_frame.h"

// The audio-reactive wave's console-side glue: it owns the analyser and the
// motion stage, and it is the only file in this subsystem that knows about
// threads, mutexes or gate files.
//
//   source/ui/render/wave_audio.h       PCM -> features
//   source/ui/render/wave_motion.h      features -> slew-limited parameters
//   source/ui/render/wave_render_map.h  parameters -> this renderer's numbers
//   source/ui/render/ui_wave_audio.cpp  the tap, the lock and the clock
//
// See docs/wave-audio-spec.md.

// Playback-thread tap.  Interleaved stereo float pairs at 48 kHz -- the same
// buffer and the same call site as music_viz_push(), for the same reason its
// comment gives: these are the samples about to reach the hardware DMA ring,
// so the wave moves with what is AUDIBLE rather than with what is buffered.
//
// Cheap and lock-scoped: fourteen one-pole filters per sample and one squared
// accumulate, no allocation, no logging.
void wave_audio_push(const float *lr, int n_pairs);

// UI thread, from wave_draw().  Advances the analyser and the motion stage by
// the real elapsed time and fills the three values wf_step() wants.
//
// SAFE TO CALL TWICE IN ONE FRAME.  wave_draw() runs twice on screens that
// composite the background twice, and the analyser is time-based rather than
// per-call; the second call sees a near-zero elapsed time, advances nothing
// and returns the same numbers.  That is handled here rather than at the call
// sites so neither of them has to know about it.
//
// Always fills all three, including when audio reactivity is off or has never
// started, in which case they are the idle values -- so the caller needs no
// second code path and no branch.
void wave_audio_frame(float *dt_scale, float *perturb, float *drive);

// UI thread, straight after wave_audio_frame().  The rest of the same frame's
// mapping: a height multiplier per solver layer and a colour multiplier, both
// exactly 1.0 at rest -- see wave_render_map.h.  Reads the cached result and
// advances nothing, so it costs a copy and needs no lock.
void wave_audio_look(float amp[3], float *lum);

// JellyWave 2.0: the body swell and the travelling accents, from the same
// cached frame as wave_audio_look() and with the same rules -- exactly
// neutral (1.0 and no live pulse) at rest and before the first frame.
void wave_audio_shape(float *thick, wrm_accent_set *acc);

// Per-solver-layer brightness from the distinct-band mapping (the highs'
// layer flickers brightest).  Exactly 1.0 at rest.  Multiplies with the single
// lum from wave_audio_look().
void wave_audio_lum3(float lum3[3]);

// JellyWave 2.0: this frame's shape deformation (wave_deform.h) -- mids as a
// travelling ripple, the waveform as fine detail, the beat as a macro pulse,
// stereo as a lean, treble as the rim gain.  The rest value when off.
void wave_audio_deform(wdf_look *out);

// Stage B's parameters for the motes (wave_motes.h), NULL when the analyser is
// off; and how present audio is, 0 at rest .. 1 with music.
const wm_params *wave_audio_params(void);
float wave_audio_presence(void);

// The distinct-band levels (lows, mids, highs; 0..1) and a pending sub-bass
// kick (returned once, then cleared) -- for the snow field.
void wave_audio_bands(float lvl[3], float *kick);
// JellyDrop's feed (2026-09-27): the same shaped levels and per-layer punch
// JellyWave's deform reads, the effective energy, the tempo, and the Wave
// Intensity gain.  All zero while audio-reactive is off.
void wave_audio_features(float punch[3], float *energy, float *tempo_hz,
                         float *tempo_conf, float *gain);

// True when the analyser is running (gate on, initialised).  For logging only.
bool wave_audio_active(void);

// Settings > Wave Intensity: 0 off, 1 normal, 2 strong (the default), 3 max.
// Persisted in jellyfin_wavereact.txt (the same file FTP has always set) and
// applied at once, no relaunch.  Render thread.
int         wave_audio_level(void);
const char *wave_audio_level_label(void);
void        wave_audio_set_level(int level);

// Shared per-frame features for every visualizer preset (viz_frame.h).
void wave_audio_viz(viz_frame *v);
