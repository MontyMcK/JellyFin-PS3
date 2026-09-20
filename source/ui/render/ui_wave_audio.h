#pragma once
#include <ppu-types.h>

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

// True when the analyser is running (gate on, initialised).  For logging only.
bool wave_audio_active(void);
