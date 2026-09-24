// The calibration seam: stage B's unitless parameters -> the three arguments
// ui_wave.cpp already passes wf_step().
//
//   wave_audio.h       PCM -> features
//   wave_motion.h      features -> slew-limited parameters
//   wave_render_map.h  parameters -> this renderer's numbers   (this file)
//
// WHY THIS IS A SEPARATE FILE AND NOT THREE LINES IN THE GLUE
//
// wave_motion.h's ranges are chosen for the MODEL: drive runs 0.35 at rest to
// 1.10 at full, because that is where the spring chain behaves.  ui_wave.cpp's
// ranges are chosen for the SCREEN: it divides wf_disp's output by
// WF_NOMINAL_PEAK (0.653, measured at drive 1.0) to land the ribbons back on
// their authored WAVE_AMP pixel heights, so the pixel amplitude is directly
// proportional to whatever drive it is handed, and drive 1.0 is "today's
// look".
//
// Those two ranges do not line up, and feeding one straight into the other
// gets both ends wrong:
//
//   AT REST.  Stage B idles at drive 0.35.  Through ui_wave.cpp's fixed
//   normalisation that is 35% of today's ribbon height -- 10 px, 8 px and 5 px
//   for the three layers.  That does not read as "calm", it reads as broken,
//   and the XMB with no music playing is the state this client spends most of
//   its life in.
//
//   AT FULL.  Stage B's ceiling is 1.10 because that is the largest drive that
//   keeps the chain clear of WK_KNEE (0.80) -- see WL_BASE_MAX.  Any mapping
//   that scales ABOVE 1.10 to get a bigger swing puts the solver inside its own
//   soft clip, where it stops responding to level at all.
//
// So the mapping is a compressed lerp between two endpoints that were each
// picked for a reason, and both reasons are testable.  It is header-only and
// PS3-header-free so test_wave_layers.c can assert them against the real
// kernel (.clinerules rule 7).

#ifndef WAVE_RENDER_MAP_H
#define WAVE_RENDER_MAP_H

#include "wave_motion.h"

// --- drive ---------------------------------------------------------------
// 0.62 at rest rather than 1.0: silence SHOULD be calmer than music, and 62%
// of the authored amplitude is visibly quieter while still reading as a wave
// (19 / 14 / 9 px against today's 30 / 22 / 15).  Going to 1.0 here would make
// the idle state identical to today and leave only a 10% swing for music,
// which is not worth building any of this for.
#define WRM_DRIVE_IDLE  0.62f

// 1.10 at full, straight from WM_MAX_DRIVE: this is the knee limit, not a look
// choice, and test_wave_layers.c re-measures that the chain stays under
// WK_KNEE when driven here through wf_step's own per-layer WF_DRIVE scaling.
#define WRM_DRIVE_MAX   WM_MAX_DRIVE

// Derived, not typed, so the endpoints stay exact if WM_MAX_DRIVE moves again.
#define WRM_DRIVE_GAIN  ((WRM_DRIVE_MAX - WRM_DRIVE_IDLE) / \
                         (WM_MAX_DRIVE  - WM_IDLE_DRIVE))

// --- timescale -----------------------------------------------------------
// This one is normalised so REST IS EXACTLY TODAY.  ui_wave.cpp's
// WAVE_FIELD_DT of 1.25 was calibrated against the drift the old sine had
// (WK_W1 * 1.25 = 0.0081 rad/frame against WAVE_DPHASE[0] of 0.008), and there
// is no reason for the idle XMB to drift at a different rate than it does
// now -- the audio system existing should not make anything worse when there
// is no audio.  Music then speeds it up to 1.44x at the fastest tempo the beat
// estimator will report.
#define WRM_TS_IDLE     1.00f
#define WRM_TS_GAIN     0.55f
#define WRM_TS_MIN      0.90f
#define WRM_TS_MAX      1.50f

// Stage B's perturbation passes through UNCHANGED, and that is deliberate:
// WM_IDLE_PERTURB is 0.02, which is exactly the literal ui_wave.cpp already
// passes, so the idle texture is bit-identical to today's and only the audio
// can raise it.  No mapping needed and none wanted.

typedef struct {
    float dt_scale;     // multiplies WAVE_FIELD_DT
    float perturb;      // straight to wf_step
    float drive;        // straight to wf_step
} wrm_out;

static inline float wrm_clamp(float v, float lo, float hi)
{
    if (v != v) return lo;              // NaN in, defined out
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

// The whole mapping.  p may be NULL, which yields the rest values -- so a
// caller whose analyser is disabled or failed to start gets the idle look from
// this one call and needs no second code path.
static inline void wrm_map(const wm_params *p, wrm_out *out)
{
    if (!out) return;
    if (!p) {
        out->dt_scale = WRM_TS_IDLE;
        out->perturb  = WM_IDLE_PERTURB;
        out->drive    = WRM_DRIVE_IDLE;
        return;
    }
    out->dt_scale = wrm_clamp(
        WRM_TS_IDLE + (p->timescale - WM_IDLE_TIME) * WRM_TS_GAIN,
        WRM_TS_MIN, WRM_TS_MAX);
    out->perturb  = wrm_clamp(p->perturb, 0.0f, WM_MAX_PERTURB);
    out->drive    = wrm_clamp(
        WRM_DRIVE_IDLE + (p->drive - WM_IDLE_DRIVE) * WRM_DRIVE_GAIN,
        WRM_DRIVE_IDLE, WRM_DRIVE_MAX);
}

#endif // WAVE_RENDER_MAP_H
