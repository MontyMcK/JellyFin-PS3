// The calibration seam: stage B's unitless parameters -> the three arguments
// ui_wave.cpp already passes wf_step(), plus a height multiplier per ribbon
// and a colour multiplier, which are the two things the renderer can take
// without a geometry or GPU-state change.
//
// JellyWave 2.0 adds two more, both still CPU-side numbers handed to code
// that already takes them: a body SWELL from the low-mids (a multiplier on
// the layer's scale, i.e. its half-width and half-thickness), and stage B's
// travelling pulses as an ACCENT -- a smooth bump added to the solver's
// displacement curve before the loft reads it.  Neither touches a vertex
// format, a buffer, a draw call or GPU state.
//
// STILL NOT CARRIED, and why: detail[] and phase[] have no input on the spring
// chain (it makes its own fine detail from perturb and its own phase from
// integration); lift moves the whole band, which would shift its framing; hue
// would have to re-tint wave_gel.h's lit colour per vertex.  wave_layers.h
// consumes all of them and is not wired in.
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

// --- per-ribbon amplitude ------------------------------------------------
// drive moves all three chains together.  This is what makes them move
// DIFFERENTLY: stage B's amp[] is already split by band through
// WM_LAYER_MIX, and this carries it to the renderer as a multiplier on each
// ribbon's authored height.
//
// Keyed by SOLVER layer, not by screen depth, because the two renderers
// disagree about depth -- JellyWave draws solver layer 0 nearest, the legacy
// ribbons draw it furthest back.  What both agree on is that layer 0 is the
// widest, slowest chain and layer 2 the narrowest, fastest one (WF_DRIVE
// 1.00 / 0.85 / 0.70, WF_RATE 1.00 / 0.78 / 1.34), so:
//
//   solver 0  <- Swell                 bass       the big slow swing
//   solver 1  <- mean(Body, Filament)  low-mid / mid
//   solver 2  <- Sheen                 high / air  the quick, fine one
//
// Centred on WM_IDLE_AMP so REST IS EXACTLY 1.0 and the no-music look does not
// change: stage B idles every layer at 0.34, and dividing by that instead
// (the obvious mapping) would have multiplied a full band by 2.9.
//
// WRM_AMP_MAX IS MEASURED.  The real solver at WRM_DRIVE_MAX, WM_MAX_PERTURB
// and WRM_TS_MAX, lofted through wave_gel.h with disp_gain scaled up, first
// breaks test_wave_gel.c's framing box at 1.75x -- the near layer's top edge
// reaches the horizontal midline.  At 1.30x that edge peaks at -0.099 in clip
// space, a tenth of the screen's half-height below the line; on the legacy
// ribbons it is 30 px * 1.10 * 1.30 = 43 px against a crest at 78% of the
// height.  test_wave_layers.c re-runs that measurement against this constant.
#define WRM_AMP_MAX     1.30f
#define WRM_AMP_GAIN    ((WRM_AMP_MAX - 1.0f) / (1.0f - WM_IDLE_AMP))
// Derived: where amp = 0 lands, 0.845.  A band that is silent while the
// others play makes its ribbon calmer than rest, which is the point.
#define WRM_AMP_MIN     (1.0f - WRM_AMP_GAIN * WM_IDLE_AMP)

// --- luminance ------------------------------------------------------------
// One multiplier on the ribbon's colour: louder is brighter, and an onset adds
// a short sheen on top.  Also centred so rest is exactly 1.0.
//
// KEPT SMALL ON PURPOSE.  wave_motion.h's own rule is that a beat which
// brightens the screen is a strobe, and this client has had a real strobe on
// this exact ribbon.  So glow is a garnish, not a flash:
//
//   bright  0.35 (quiet music) .. 1.00 (loud)  ->  0.94 .. 1.135
//   glow    0 .. 1                             ->  +0 .. +0.08
//
// and the step between frames is bounded by stage B's slews rather than by
// anything here: at 60 fps the worst case is WM_SLEW_BRIGHT * 0.30 +
// WM_SLEW_GLOW_UP * 0.08 = 0.017 a frame, so the full rise takes a fifth of a
// second.  test_wave_layers.c asserts that bound.
#define WRM_LUM_BRIGHT  0.30f
#define WRM_LUM_GLOW    0.08f
#define WRM_LUM_MIN     0.85f
#define WRM_LUM_MAX     1.20f

// --- body swell (JellyWave 2.0) ---------------------------------------
// Low-mids thicken the body: stage B's Body layer (0.46 bass, 0.40 low-mid,
// 0.14 mid) scales every layer's half-width and half-thickness.  Centred on
// WM_IDLE_AMP like the heights, so rest is exactly 1.0.
//
// 1.04 IS MEASURED, together with WRM_ACC_H below: with everything at its
// loudest at once -- WRM_DRIVE_MAX, WRM_AMP_MAX, this, and the accent sitting
// on the crest along the whole band -- the near layer's top edge measured
// -0.053 in clip space, just inside the same 0.05 margin under the midline
// WRM_AMP_MAX keeps.  test_wave_layers.c re-runs that worst case.
#define WRM_THICK_MAX   1.04f
#define WRM_THICK_GAIN  ((WRM_THICK_MAX - 1.0f) / (1.0f - WM_IDLE_AMP))
#define WRM_THICK_MIN   (1.0f - WRM_THICK_GAIN * WM_IDLE_AMP)

// --- the accent (JellyWave 2.0) ----------------------------------------
// A stage B pulse is an object: it spawns on an onset, travels the band over
// two beats and fades on a C1 envelope.  Here it becomes a smooth compact
// bump, (1 - q^2)^2, added to the solver's displacement -- a swell that runs
// along the ribbon, which is the "brief controlled accent" rather than a
// flash of the whole screen.
//
//   WRM_ACC_H      bump height in the solver's own units at pulse amp 1.
//                  0.10 is about 15% of the chain's nominal peak (0.653): an
//                  accent, not a second wave.  Measured -- see WRM_THICK_MAX.
//   WRM_ACC_WIDEN  the ribbon's bump is twice stage B's pulse width, 0.15 to
//                  0.30 in u.  JellyWave re-samples its geometry every third
//                  frame, and a pulse at 120 BPM moves 0.057 u between
//                  samples; against a bump this broad that reads as travel,
//                  against stage B's raw width it read as stepping.
//   WRM_ACC_LAYER  full on the near/widest layer, less on the others, so the
//                  accent reads as one gesture through the band rather than
//                  three.
//
// Overlapping pulses are summed and then clamped to 1, so the accent can never
// exceed WRM_ACC_H however many are live.
#define WRM_ACC_H       0.10f
#define WRM_ACC_WIDEN   2.0f

static const float WRM_ACC_LAYER[3] = { 1.00f, 0.80f, 0.60f };

// Slots [0, WM_PULSES) are the map's travelling pulses; the rest belong to the
// sub-bass ripples (wrm_distinct), so the two never compete for a slot.
#define WRM_RIPPLES     2
#define WRM_ACC_SLOTS   (WM_PULSES + 2 * WRM_RIPPLES)

typedef struct {
    float x[WRM_ACC_SLOTS];     // pulse centre, u along the solver's [0,1]
    float inv[WRM_ACC_SLOTS];   // 1 / bump half-width in u
    float a[WRM_ACC_SLOTS];     // amplitude 0..1, 0 when not live
} wrm_accent_set;

typedef struct {
    float dt_scale;     // multiplies WAVE_FIELD_DT
    float perturb;      // straight to wf_step
    float drive;        // straight to wf_step
    float amp[3];       // per solver layer, multiplies the ribbon's height
    float lum;          // multiplies the ribbon's colour
    float thick;        // multiplies the layer's scale (width and thickness)
    wrm_accent_set acc; // travelling accents, see wrm_accent()
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
// RESPONSE GAIN.  wrm_map_gain() multiplies every response's DEVIATION FROM
// REST by `gain` before the same clamps apply -- so a higher gain reaches the
// caps at lower music levels (a steeper, more visible response) but can never
// pass them.  Every ceiling above was measured against the framing box, and
// none of them moves; rest is still exactly rest at any gain, because the
// deviation there is zero.  gain 1 is wrm_map exactly.  Set at runtime from
// ui_wave_audio.cpp's gate file, because how visible the response should be
// is judged on a TV.
#define WRM_GAIN_MIN    0.25f
#define WRM_GAIN_MAX    4.00f

static inline void wrm_map_gain(const wm_params *p, float gain, wrm_out *out);

static inline void wrm_map(const wm_params *p, wrm_out *out)
{
    wrm_map_gain(p, 1.0f, out);
}

static inline void wrm_map_gain(const wm_params *p, float g, wrm_out *out)
{
    if (!out) return;
    g = (g == g) ? wrm_clamp(g, WRM_GAIN_MIN, WRM_GAIN_MAX) : 1.0f;
    if (!p) {
        out->dt_scale = WRM_TS_IDLE;
        out->perturb  = WM_IDLE_PERTURB;
        out->drive    = WRM_DRIVE_IDLE;
        out->amp[0]   = out->amp[1] = out->amp[2] = 1.0f;
        out->lum      = 1.0f;
        out->thick    = 1.0f;
        memset(&out->acc, 0, sizeof out->acc);
        return;
    }
    out->dt_scale = wrm_clamp(
        WRM_TS_IDLE + (p->timescale - WM_IDLE_TIME) * WRM_TS_GAIN * g,
        WRM_TS_MIN, WRM_TS_MAX);
    out->perturb  = wrm_clamp(
        WM_IDLE_PERTURB + (p->perturb - WM_IDLE_PERTURB) * g,
        0.0f, WM_MAX_PERTURB);
    out->drive    = wrm_clamp(
        WRM_DRIVE_IDLE + (p->drive - WM_IDLE_DRIVE) * WRM_DRIVE_GAIN * g,
        WRM_DRIVE_IDLE, WRM_DRIVE_MAX);

    // The clamp comes AFTER the sum so one NaN anywhere lands on the floor
    // rather than escaping through an arithmetic that happens to absorb it.
    {
        const float a[3] = {
            p->amp[0],
            0.5f * (p->amp[1] + p->amp[2]),
            p->amp[3]
        };
        int i;
        for (i = 0; i < 3; i++)
            out->amp[i] = wrm_clamp(1.0f + (a[i] - WM_IDLE_AMP) * WRM_AMP_GAIN * g,
                                    WRM_AMP_MIN, WRM_AMP_MAX);
    }
    out->lum = wrm_clamp(1.0f + ((p->bright - WM_IDLE_BRIGHT) * WRM_LUM_BRIGHT
                              + p->glow * WRM_LUM_GLOW) * g,
                         WRM_LUM_MIN, WRM_LUM_MAX);
    out->thick = wrm_clamp(1.0f + (p->amp[1] - WM_IDLE_AMP) * WRM_THICK_GAIN * g,
                           WRM_THICK_MIN, WRM_THICK_MAX);
    {
        int j;
        for (j = 0; j < WM_PULSES; j++) {
            const wm_pulse *q = &p->pulse[j];
            float w = WRM_ACC_WIDEN * wrm_clamp(q->width, 0.02f, 0.5f);
            out->acc.x[j]   = wrm_clamp(q->x, -0.5f, 1.5f);
            out->acc.inv[j] = 1.0f / w;
            out->acc.a[j]   = q->live ? wrm_clamp(q->amp * g, 0.0f, 1.0f) : 0.0f;
        }
        for (; j < WRM_ACC_SLOTS; j++) {
            out->acc.x[j] = 0.0f; out->acc.inv[j] = 1.0f; out->acc.a[j] = 0.0f;
        }
    }
}

// The accent at one point u of the solver's [0,1], in the solver's units, for
// one layer (0 near .. 2 far).  EXACTLY 0.0f when no pulse is live, so adding
// it leaves a displacement bit-identical -- the idle look depends on that.
static inline float wrm_accent_at(const wrm_accent_set *a, int layer, float u)
{
    float acc = 0.0f;
    int   j;
    if (!a) return 0.0f;
    if (layer < 0) layer = 0;
    if (layer > 2) layer = 2;
    for (j = 0; j < WRM_ACC_SLOTS; j++) {
        float q = (u - a->x[j]) * a->inv[j];
        float t = 1.0f - q * q;
        t    = (t > 0.0f) ? t : 0.0f;
        acc += a->a[j] * t * t;
    }
    acc = (acc < 1.0f) ? acc : 1.0f;
    return WRM_ACC_H * WRM_ACC_LAYER[layer] * acc;
}

// out[k] = in[k] + the accent at sample k, with the samples spanning u in
// [0,1] as wave_field.h's do.  in and out may not alias; n < 2 copies nothing.
static inline void wrm_accent(const wrm_accent_set *a, int layer,
                              const float *in, float *out, int n)
{
    float du;
    int   k;
    if (!in || !out || n < 2) return;
    du = 1.0f / (float)(n - 1);
    for (k = 0; k < n; k++)
        out[k] = in[k] + wrm_accent_at(a, layer, (float)k * du);
}

// --- distinct bands --------------------------------------------------------
//
// Under wrm_map_gain() alone the SHARED terms (drive, timescale) dominate, so
// all three layers move together and lows, mids and highs are
// indistinguishable (measurements: docs/wave-renderer-notes.md, "Audio
// mapping").  This is applied on top of that result by the glue:
//
//   * The shared terms are held near rest: tempo no longer speeds the wave
//     (dt_scale 1.0), loudness keeps only a fifth of its drive swing (capped
//     at WRM_DB_DRIVE_MAX), and the broadband ripple and brightness keep a
//     fraction.  The base motion stays slow and calm under any music.
//   * Each solver layer then follows ONE part of the spectrum, taken straight
//     from stage A's bands, with its own timing -- so the three read as three
//     different instruments:
//
//       layer 0 (near, widest)   sub + bass       slow heavy swell   att 60 ms / rel 420 ms
//       layer 1 (middle)         low-mid + mid    body               att 45 ms / rel 260 ms
//       layer 2 (far, finest)    high + air       quick flicker      att 15 ms / rel 120 ms
//
//     height from WRM_DB_AMP_QUIET (a band that is quiet sinks BELOW rest,
//     which is what makes the loud one stand out) to WRM_DB_AMP_MAX, and a
//     per-layer brightness, strongest on the highs.
//   * At rest (no audio, silence latched) every output is exactly the rest
//     value, as before.
//
// FRAMING.  Height is linear in drive and in disp_gain, so the budget
// test_look_framing measured (drive 1.10 x amp 1.30) is spent differently,
// not raised: with drive held at WRM_DB_DRIVE_MAX the per-layer caps can go
// much higher.  test_distinct_framing re-runs the same measurement -- real
// solver, real loft, the fullest swell, the accent on the crest -- with THESE
// caps, and must stay inside the same box with the same 0.05 margin.
// Per layer: the band level (self-referenced) that reads as 0, and a response
// multiplier.  The lows start lower and climb faster; the caps (and so the
// framing) do not depend on these.
static const float WRM_DB_FLOOR[3] = { 0.18f, 0.28f, 0.28f };
static const float WRM_DB_RESP[3]  = { 1.40f, 1.00f, 1.00f };
#define WRM_DB_AMP_QUIET    0.72f   // 0.78 before: more range below rest
#define WRM_DB_DRIVE_MAX    0.70f
#define WRM_DB_DRIVE_KEEP   0.20f
#define WRM_DB_PERTURB_KEEP 0.30f
#define WRM_DB_LUM_KEEP     0.30f
#define WRM_DB_ACC_KEEP     0.60f

// 2026-09-25 v4: "slightly too subtle, especially in high-BPM songs with lots
// of bass and songs that are just louder -- more reactive even if less flowy".
// Two causes.  (1) The bands are self-referenced, so a loud, dense track sits
// near a CONSTANT high level: its kicks were a steady swell, not beats.  (2) The
// 420 ms bass release smeared anything faster than ~140 BPM into that swell.
// So: quicker envelopes, a sustained part that no longer fills the whole
// range, and PUNCH on top -- each layer's fast level minus its own recent
// level, which is the beat itself and is loudness-independent -- plus the
// tempo speeding the base motion up.  Same caps: the framing does not move.
static const float WRM_DB_ATT[3]     = { 0.025f, 0.018f, 0.012f };   // vocal: syllable-quick (v8)
static const float WRM_DB_REL[3]     = { 0.170f, 0.110f, 0.090f };
static const float WRM_DB_SUS_W[3]   = { 0.58f, 0.66f, 0.80f };   // sustained share of the range
static const float WRM_DB_PUNCH_W[3] = { 1.00f, 1.10f, 0.60f };   // beat share (vocal 0.85 -> 1.1, v8)
#define WRM_PUNCH_GAIN      3.0f
#define WRM_PUNCH_TAU       0.30f    // s, the "recent level" the punch is measured from
// CONTRAST NORMALISATION (2026-09-25, loudness-war masters).  A brickwalled
// mix has small beats relative to its sustained level, so the same punch gain
// that works on dynamic music leaves it tame.  Each layer tracks its own
// typical punch (mean |fast - recent|, ~4 s) and a song whose beats are
// smaller than WRM_PUNCH_REF_DEV has them scaled up to match, by up to
// WRM_PUNCH_BOOST_MAX.  Dynamic music (at or above the reference) is untouched.
#define WRM_PUNCH_REF_DEV   0.050f
#define WRM_PUNCH_BOOST_MAX 3.0f
#define WRM_PUNCH_DEV_TAU   4.0f
#define WRM_PUNCH_DEV_FLOOR 0.006f

// ENERGY.  A loud, compressed master should look MORE energetic than a quiet,
// dynamic one, not merely as lively.  energy_eff rises only above a
// mid-loudness master (so a -12 LUFS mix is untouched) and with how
// compressed the beats are; it raises the beat gain and stiffens the height
// spring.  With a locked tempo, each layer's release is also held under the
// beat period so a fast beat never smears into a swell.
#define WRM_ENERGY_PUNCH    2.00f    // punch gain x (1 + this x energy_eff)
#define WRM_ENERGY_SPRING   0.35f    // spring Hz x (1 + this x energy_eff)
#define WRM_REL_OF_PERIOD   0.40f

// THE HIT (2026-09-25 v6: "808s still aren't high-energy enough -- reduce
// the attack, make it dynamic per song").  Measured on the real masters: the
// bass layer rose 0.33 per beat on a -3 LUFS trap track and took 81 ms to get
// most of the way, with the average height at 1.21 of a 1.75 cap.  So, scaled
// by the song's own energy (a dynamic mix keeps close to what it had):
//   * the height spring is STIFFER ON THE WAY UP than on the way down -- a hit
//     snaps up, the fall stays the soft, physical settle
//   * the envelope and punch attacks shorten
//   * the bass layer keeps more of its sustained level, so an 808's tail holds
//     the wave up instead of it dropping straight back
#define WRM_RISE_BASE       1.60f    // rising stiffness x this at energy 0
#define WRM_RISE_ENERGY     1.20f    // ... plus this x energy_eff (1.8 tried in v8: worse bass -- reverted)
#define WRM_ATT_ENERGY      0.60f    // attacks shortened by up to this fraction
#define WRM_SUS_ENERGY     (-0.20f)  // bass sustain weight + this x energy_eff: LESS, headroom for the hits

// DOUBLE KICKS (v7: "when a double kick or 808 hits, the second one doesn't
// look distinct enough").  Two hits 150 ms apart left the layer at its cap
// with a 13% dip between them: the punch released over 90 ms, the falling
// spring was soft, and the first hit raised the "recent" level the second
// was measured against.  So: a shorter punch release (shorter still with
// energy), a falling spring that stiffens with energy (and is damped more,
// so it does not bounce), and a recent level that rises slowly but falls
// quickly -- a hit no longer eats the next one's punch.
#define WRM_PUNCH_REL       0.060f   // s at energy 0 ...
#define WRM_PUNCH_REL_E     0.040f   // ... and at full energy (30 ms read a touch robotic, v10)
#define WRM_FALL_ENERGY     0.80f    // falling stiffness x (1 + this x energy_eff); 1.1 read robotic (v10)
#define WRM_RECENT_UP       0.35f    // s: the recent level rises at about the old rate (a held 808 must not read as punch for long)
#define WRM_RECENT_DN       0.12f    // s: and falls quickly, so the next hit meets a low reference

// VOCAL SEPARATION (v8: "the visual separation on vocals needs to be
// better").  All three layers were pumping with the beat: the loud-master
// punch boost applied to each alike, and the vocal layer's band carried the
// 808's harmonics.  Now the boost is weighted per layer (the bass takes it
// all), and the vocal layer ducks under a landing bass hit -- so between the
// kicks it moves with the voice, and on them the bass layer owns the moment.
// v9 ("the bass still looks weaker than the good build"): the ribbons pumping
// TOGETHER on a kick was much of the hit.  Measured as the three ribbons'
// mean rise per beat, the v8 duck (0.6) and weights (0.35 / 0.6) cost
// SLAM PUNK 0.517 -> 0.445; duck 0.15 with weights 0.9 / 1.0 gives 0.517
// back with the vocals still apart (bass/vocal correlation 0.04 - 0.08).
static const float WRM_ENERGY_PUNCH_W[3] = { 1.00f, 0.90f, 1.00f };   // v9: the whole-wave hit is back
// THE DROP (v10: "when the beat drops after a vocal section the bass doesn't
// hit hard enough and takes a couple of seconds to get the height" --
// Rockstar Lifestyle).  Measured: after the drop the 808 is HELD -- its band
// input barely moves -- and the self-referenced level re-references to it
// within a second, so a held 808 read as ordinary height.  The absolute bass
// level against its own last ~3 s says the bass has just arrived: that lifts
// the near ribbon at once and eases out over ~3 s while the contrast boost and
// the energy (which need a moment) take over.
#define WRM_ARRIVAL_H       0.65f    // extra bass-layer level at a full arrival
// v11: measured on Rockstar Lifestyle the drop is a WALL -- one held,
// distorted 808 at a flat -5..-6 dB for four seconds, no separate kicks
// under it, the drums proper only at ~35 s.  Nothing to hit, so the drop
// LANDS: the arrival holds near the top for WRM_ARRIVAL_HOLD, breathing at
// the tempo so the wall is alive, then eases out as the hits take over.
#define WRM_ARRIVAL_HOLD    2.5f
#define WRM_ARRIVAL_BREATH  0.18f    // of the arrival height, at the beat rate
#define WRM_ARRIVAL_TAU     1.5f     // the arrival hands over to the hits within ~2-3 s

// BEATS KICK THE NEAR RIBBON (v11).  Under a held 808 the bass band barely
// moves, so its punch cannot show the kicks -- but the beat detector hears
// them across the spectrum.  Each detected beat sets a floor under the bass
// layer's punch.
#define WRM_ONSET_KICK      0.60f    // punch floor at a full-strength beat

#define WRM_VOCAL_DUCK      0.15f    // vocal punch x (1 - this x bass punch); 0.6 thinned the whole-wave hit 12-14%
#define WRM_TEMPO_TS_MAX    1.40f    // base motion at ~180 BPM, locked
#define WRM_TEMPO_TAU       1.50f    // s, the tempo speed-up eases in and out
#define WRM_PUNCH_ATT       0.030f   // s, a hit swells in over ~2 frames, not one

// 2026-09-25 v5: "better but more robotic/rigid".  The height now follows its
// target through a lightly damped spring (per layer: natural frequency, Hz,
// and damping ratio): quick enough to land every beat, but it carries
// momentum into the peak and eases out of it.  The output is clamped to the
// same caps, so the framing is unchanged.
static const float WRM_SPRING_HZ[3]   = { 3.4f, 3.2f, 4.8f };
static const float WRM_SPRING_ZETA[3] = { 0.58f, 0.64f, 0.72f };
static const float WRM_DB_AMP_MAX[3] = { 1.75f, 1.65f, 2.10f };   // measured: test_distinct_framing
static const float WRM_DB_LUM_MAX[3] = { 1.10f, 1.18f, 1.35f };

typedef struct {
    float env[3];
    float lvl[3];      // this frame's shaped level per layer, 0..1 (for the snow)
    // Set by the caller before wrm_distinct (zero = none): each layer's FAST
    // level, and the beat estimate.  The punch is fast minus its own recent.
    float in_fast[3];
    float tempo_hz, tempo_conf;
    float recent[3];
    float punch[3];
    float energy;      // set by the caller: 0..1 absolute loudness (wa_features.level)
    float energy_eff;  // loudness x compression, what the mapping uses (read by the look)
    float energy_hold; // the song's loudness, remembered through a breakdown (~20 s)
    float comp_hold;   // ... and its compression
    float bass_rel;    // set by the caller: absolute bass vs its own peak (wa_features.bass_rel)
    float babs_slow;   // that, over the last ~3 s
    float lvl_slow;    // the absolute loudness, over the last ~3 s
    float arrival;     // 0..1: the bass has just ARRIVED (a drop) -- extra height, easing out
    float onset, onset_strength;   // set by the caller each frame (wa_features)
    float arr_hold;    // s the arrival stays up (a drop lands and HOLDS)
    float arr_ph;      // beat phase of its breathing
    float pdev[3];     // each layer's typical |fast - recent|: its own contrast
    float boost[3];    // the contrast boost applied, 1 .. WRM_PUNCH_BOOST_MAX
    // the physical part: each layer's height rides a spring-damper toward
    // its target, so it has momentum and a soft settle instead of following
    // the envelope rigidly; the tempo speed-up eases too
    float ax[3], av[3];
    float ts;
    // the sub-bass shock (2026-09-25: "the subbass needs to be more
    // distinctive -- when it bumps, send extra shocks through the whole wave")
    float sub_slow;    // the sub band's recent level
    float shock;       // 0..1, the strongest live ripple (for the log)
    float shock_x;     // 0 only in a zeroed state (see the ripple block)
    float refr;        // s until the next hit may fire
    float kick;        // non-zero on the frame a hit lands (read by the snow)
    // the ripples (2026-09-25 v2: the whole-wave jolt was "way too noticeable";
    // "more of a ripple across the wave that stems from a flowy spike")
    float rt[WRM_RIPPLES];   // s since this ripple's hit; < 0 = not live
    float rx[WRM_RIPPLES];   // where it started, u
    float rm[WRM_RIPPLES];   // its strength, 0..1
    int   rn;                // hits so far (picks the next origin)
    int   armed;             // re-armed once the sub falls back (a held note fires once)
} wrm_db_state;

// A hit is a fast rise of the sub band above its own recent level.  Each hit
// starts a RIPPLE: a narrow spike rises smoothly (~0.1 s) at a point on the
// band, then splits into two crests that run outward in both directions,
// easing out as they go -- widening and fading -- over ~1.5 s.  Two ripples
// can be live at once, so a fast bass line layers them instead of cutting the
// last one off.  The layers get only a faint, smoothed lift (no whole-wave
// jolt), and the accent total is still capped at the measured bound (see the
// end of wrm_distinct), so the framing holds.
#define WRM_SUB_TAU        0.35f
#define WRM_SUB_THRESH     0.14f
#define WRM_SUB_REFR       0.15f      // 0.28 before: fast kicks each get a ripple
#define WRM_RIP_ATT        0.10f      // s, the spike's smooth rise
#define WRM_RIP_DECAY      0.45f      // s, 1/(1+t/this)^2 fall-off
#define WRM_RIP_END        1.20f      // s, gone (faded to 0 over the last 0.5 s)
#define WRM_RIP_SPREAD     0.70f      // u each crest travels, asymptotically
#define WRM_RIP_EASE       0.35f      // s, how quickly the spreading eases out
#define WRM_RIP_W0         0.055f     // half-width at the spike, u
#define WRM_RIP_W1         0.17f      // extra half-width by the end
#define WRM_RIP_A          0.34f      // per crest (the spike is two, overlapping)
#define WRM_RIP_PUSH       0.10f      // faint lift toward each layer's cap
#define WRM_RIP_LUM        0.05f
#define WRM_ACC_TOTAL_MAX  WRM_DB_ACC_KEEP   // test_distinct_framing measured this

// WAVE SPEED FROM THE MUSIC (2026-09-25: "make the wave move faster with the
// BPM").  The base motion's time scale, from the tempo -- 0.85x at 70 BPM,
// 1.0x at 100, 1.2x at 130, 1.4x at 160, 1.5x at 190 -- trusted in
// proportion to the beat confidence (from 0.2 up), then up to +25% for a
// loud, dense master, whose pace lives in its density as much as its BPM
// (a rage track's autocorrelation honestly reads its triplet grid, 2/3 of
// the pulse).  Capped at WRM_TS_MAX, the ceiling the framing was measured at.
// Shared with wave_deform.h, so the ripples travel at the same pace.
static inline float wrm_tempo_speed(float hz, float conf, float energy)
{
    static const float BPM[5] = { 70.0f, 100.0f, 130.0f, 160.0f, 190.0f };
    static const float TS[5]  = { 0.85f, 1.00f, 1.20f, 1.40f, 1.50f };
    float bpm = hz * 60.0f, t = 1.0f, g, e;
    int i;
    if (!(hz > 0.0f)) bpm = 100.0f;
    if (bpm <= BPM[0]) t = TS[0];
    else if (bpm >= BPM[4]) t = TS[4];
    else for (i = 0; i < 4; i++)
        if (bpm < BPM[i + 1]) { t = TS[i] + (TS[i + 1] - TS[i]) * (bpm - BPM[i]) / (BPM[i + 1] - BPM[i]); break; }
    g = (conf - 0.2f) / 0.35f;
    g = g < 0.0f ? 0.0f : (g > 1.0f ? 1.0f : g);
    g = g * g * (3.0f - 2.0f * g);
    e = energy < 0.0f ? 0.0f : (energy > 1.0f ? 1.0f : energy);
    t = (1.0f + (t - 1.0f) * g) * (1.0f + 0.25f * e);
    return t > WRM_TS_MAX ? WRM_TS_MAX : t;
}

// cos(2 pi t), libm-free (Bhaskara on the folded phase)
static inline float wdf_like_cos(float t)
{
    float f = t + 0.25f, sg = 1.0f, x;
    f -= (float)(int)f;
    if (f < 0.0f) f += 1.0f;
    if (f >= 0.5f) { f -= 0.5f; sg = -1.0f; }
    x = f * (0.5f - f);
    return sg * 16.0f * x / (1.25f - 4.0f * x);
}

static inline float wrm_smooth01(float x)
{
    x = wrm_clamp(x, 0.0f, 1.0f);
    return x * x * (3.0f - 2.0f * x);
}

// src[3]: 0..1 band levels for layers 0..2.  present: 0 at rest .. 1 with
// audio.  resp: the intensity level's response (1 = default).  Rewrites the
// shared terms of *o and its amp[], and fills lum3[] (per-layer brightness,
// exactly 1.0 at rest).
static inline void wrm_distinct(wrm_db_state *st, const float src[3],
                                float sub_fast, float present, float resp,
                                float dt, wrm_out *o, float lum3[3])
{
    int i;
    if (!st || !o || !lum3) return;
    present = wrm_clamp(present, 0.0f, 1.0f);
    resp    = wrm_clamp(resp, 0.25f, 2.0f);
    if (!(dt > 0.0f)) dt = 0.0f;

    // Tempo (and energy) set the base motion's pace: wrm_tempo_speed().
    {
        const float tgt  = present > 0.0f
                         ? wrm_tempo_speed(st->tempo_hz, st->tempo_conf, st->energy_eff)
                         : WRM_TS_IDLE;
        if (!(st->ts >= WRM_TS_IDLE)) st->ts = WRM_TS_IDLE;
        st->ts += (tgt - st->ts) * (dt / (WRM_TEMPO_TAU + dt));
        if (present <= 0.0f && st->ts - WRM_TS_IDLE < 1e-4f) st->ts = WRM_TS_IDLE;
        o->dt_scale = present > 0.0f ? st->ts : WRM_TS_IDLE;
    }
    o->drive    = wrm_clamp(WRM_DRIVE_IDLE + (o->drive - WRM_DRIVE_IDLE) * WRM_DB_DRIVE_KEEP,
                            WRM_DRIVE_IDLE, WRM_DB_DRIVE_MAX);
    o->perturb  = WM_IDLE_PERTURB + (o->perturb - WM_IDLE_PERTURB) * WRM_DB_PERTURB_KEEP;
    o->lum      = 1.0f + (o->lum - 1.0f) * WRM_DB_LUM_KEEP;
    for (i = 0; i < WM_PULSES; i++) o->acc.a[i] *= WRM_DB_ACC_KEEP;

    {
        // The song's loudness is remembered through a breakdown (slow 20 s
        // decay), so the drop does not have to earn its energy back (v10).
        st->energy_hold *= 1.0f / (1.0f + dt / 20.0f);
        if (st->energy > st->energy_hold) st->energy_hold = st->energy;
        const float loud = wrm_clamp((st->energy_hold - 0.55f) / 0.40f, 0.0f, 1.0f);
        const float pd   = (st->pdev[0] + st->pdev[1] + st->pdev[2]) * (1.0f / 3.0f);
        float comp = wrm_clamp((WRM_PUNCH_REF_DEV - pd) / 0.025f, 0.0f, 1.0f);
        // the song's compression is remembered through a breakdown too
        st->comp_hold *= 1.0f / (1.0f + dt / 20.0f);
        if (comp > st->comp_hold) st->comp_hold = comp;
        comp = st->comp_hold;
        const float e    = present * wrm_clamp(0.65f * loud + 0.35f * comp * loud + 0.15f * comp, 0.0f, 1.0f);
        st->energy_eff += (e - st->energy_eff) * (dt / ((e > st->energy_eff ? 0.30f : 4.0f) + dt));
    }
    {
        // A drop is the bass arriving OR the whole mix stepping up in
        // loudness after a softer section (Rockstar Lifestyle: the intro has
        // 808s; what changes at the drop is ~+8 dB) -- whichever rose more
        // against its own last ~3 s.
        const float br = wrm_clamp(st->bass_rel, 0.0f, 1.0f);
        const float lv = wrm_clamp(st->energy, 0.0f, 1.0f);
        float a;
        st->babs_slow += (br - st->babs_slow) * (dt / (WRM_ARRIVAL_TAU + dt));
        st->lvl_slow  += (lv - st->lvl_slow)  * (dt / (WRM_ARRIVAL_TAU + dt));
        a = wrm_clamp((br - st->babs_slow) * 2.5f, 0.0f, 1.0f);
        {
            const float al = wrm_clamp((lv - st->lvl_slow) * 3.0f, 0.0f, 1.0f);
            if (al > a) a = al;
        }
        // only a loud master drops: a dynamic mix (Blonde) swings in loudness
        // all the time and must not read every swing as a drop
        a *= present * wrm_clamp((lv - 0.35f) / 0.25f, 0.0f, 1.0f);   // loud enough NOW (a drop gets there in ~0.35 s)
        // up at once; a real drop HOLDS, then eases out
        if (a > st->arrival) {
            st->arrival = a;
        } else if (st->arr_hold > 0.0f) {
            st->arr_hold -= dt;
            if (st->arr_hold <= 0.0f) st->arr_hold = -1.0f;   // spent until this arrival ends
        } else {
            st->arrival += (a - st->arrival) * (dt / (1.5f + dt));
        }
        // only a REAL drop holds -- a big jump that becomes genuinely loud
        // while it is up (once per arrival); a dynamic song's ordinary
        // swells get the lift, not the hold
        if (st->arr_hold == 0.0f && st->arrival > 0.50f && lv > 0.78f) st->arr_hold = WRM_ARRIVAL_HOLD;
        if (st->arrival < 0.20f && st->arr_hold < 0.0f) st->arr_hold = 0.0f;   // re-armed
        st->arr_ph += dt * (st->tempo_hz > 0.5f ? st->tempo_hz : 2.0f);
        if (st->arr_ph > 1.0f) st->arr_ph -= (float)(int)st->arr_ph;
    }
    for (i = 0; i < 3; i++) {
        const float x   = wrm_clamp(src ? src[i] : 0.0f, 0.0f, 1.0f);
        float rel = WRM_DB_REL[i];
        if (st->tempo_conf > 0.35f && st->tempo_hz > 0.5f) {
            const float cap = WRM_REL_OF_PERIOD / st->tempo_hz;
            if (cap < rel) rel = cap < 0.06f ? 0.06f : cap;
        }
        const float att = WRM_DB_ATT[i] * (1.0f - WRM_ATT_ENERGY * st->energy_eff);
        const float tau = (x > st->env[i]) ? att : rel;
        const float k   = dt / (tau + dt);
        float s, amp;
        st->env[i] += (x - st->env[i]) * k;
        {
            const float xf = wrm_clamp(st->in_fast[i], 0.0f, 1.0f);
            float p;
            {
                const float rt = xf > st->recent[i] ? WRM_RECENT_UP : WRM_RECENT_DN;
                st->recent[i] += (xf - st->recent[i]) * (dt / (rt + dt));
            }
            {
                const float d  = xf - st->recent[i];
                const float ad = d < 0.0f ? -d : d;
                float b;
                if (!(st->pdev[i] > 0.0f)) st->pdev[i] = WRM_PUNCH_REF_DEV;   // zeroed state
                // Winsorised: one frame counts for at most 2.5x the typical value,
                // so the transition INTO a drop cannot inflate it and rob the
                // drop of its boost for seconds (v10).
                {
                    const float cap = 2.5f * st->pdev[i] + 0.01f;
                    const float a2 = ad < cap ? ad : cap;
                    // rises slowly, FALLS fast: a compressed drop after a
                    // dynamic intro gets its boost within ~1 s (v10)
                    // (fast only on a loud master: a dynamic mix keeps the 4 s memory)
                    float tdev = a2 > st->pdev[i] ? WRM_PUNCH_DEV_TAU
                               : 1.2f + (WRM_PUNCH_DEV_TAU - 1.2f) * (1.0f - st->energy_eff);
                    if (a2 <= st->pdev[i] && st->arrival > 0.2f) tdev = 0.25f;   // a drop: boost now (v11)
                    st->pdev[i] += (a2 - st->pdev[i]) * (dt / (tdev + dt));
                }
                b = WRM_PUNCH_REF_DEV / (st->pdev[i] > WRM_PUNCH_DEV_FLOOR ? st->pdev[i] : WRM_PUNCH_DEV_FLOOR);
                b = wrm_clamp(b, 1.0f, WRM_PUNCH_BOOST_MAX);
                st->boost[i] = b;
                p = wrm_clamp(d * WRM_PUNCH_GAIN * resp * b
                              * (1.0f + WRM_ENERGY_PUNCH * WRM_ENERGY_PUNCH_W[i] * st->energy_eff),
                              0.0f, 1.0f);
                if (i == 1) p *= 1.0f - WRM_VOCAL_DUCK * st->punch[0];   // layer 0 is done first
            }
            // fast in, a little slower out, so a hit reads as a hit
            if (i == 0 && st->onset > 0.0f) {
                const float fl = WRM_ONSET_KICK * (0.55f + 0.45f * wrm_clamp(st->onset_strength, 0.0f, 1.0f))
                               * (0.6f + 0.4f * st->energy_eff);
                if (fl > p) p = fl;
            }
            {
                const float pa = WRM_PUNCH_ATT * (1.0f - WRM_ATT_ENERGY * st->energy_eff);
                const float pr = WRM_PUNCH_REL + (WRM_PUNCH_REL_E - WRM_PUNCH_REL) * st->energy_eff;
                st->punch[i] += (p - st->punch[i])
                              * (p > st->punch[i] ? dt / (pa + dt) : dt / (pr + dt));
            }
        }
        s   = wrm_clamp(wrm_clamp((st->env[i] - WRM_DB_FLOOR[i]) / (1.0f - WRM_DB_FLOOR[i])
                                  * resp * WRM_DB_RESP[i], 0.0f, 1.0f)
                        * (WRM_DB_SUS_W[i] + (i == 0 ? WRM_SUS_ENERGY * st->energy_eff : 0.0f))
                        + st->punch[i] * (i == 1 ? 0.85f + 0.25f * st->energy_eff : WRM_DB_PUNCH_W[i])
                        + (i == 0 ? WRM_ARRIVAL_H * st->arrival
                                    * (1.0f - WRM_ARRIVAL_BREATH * (0.5f + 0.5f * wdf_like_cos(st->arr_ph)))
                                  : 0.0f), 0.0f, 1.0f);
        amp = WRM_DB_AMP_QUIET + (WRM_DB_AMP_MAX[i] - WRM_DB_AMP_QUIET) * s;
        st->lvl[i] = s;
        if (present <= 0.0f) {
            o->amp[i] = 1.0f;                   // rest is exactly rest
            lum3[i]   = 1.0f;
            st->ax[i] = 1.0f; st->av[i] = 0.0f;
        } else {
            const float target = 1.0f + present * (amp - 1.0f);
            const float w0 = 6.2831853f * WRM_SPRING_HZ[i] * (1.0f + WRM_ENERGY_SPRING * st->energy_eff);
            // stiffer rising than falling: the hit snaps, the settle stays soft
            const bool  up = target > st->ax[i];
            const float w  = up ? w0 * (WRM_RISE_BASE + WRM_RISE_ENERGY * st->energy_eff)
                                : w0 * (1.0f + WRM_FALL_ENERGY * st->energy_eff);
            const float zf = WRM_SPRING_ZETA[i] + (0.85f - WRM_SPRING_ZETA[i]) * st->energy_eff;
            const float zw = 2.0f * (up ? 0.72f : zf) * w;
            float left = dt;
            if (st->ax[i] == 0.0f) { st->ax[i] = 1.0f; st->av[i] = 0.0f; }   // zeroed state
            while (left > 0.0f) {                // semi-implicit, stable steps
                const float h = left > 0.008f ? 0.008f : left;
                st->av[i] += (w * w * (target - st->ax[i]) - zw * st->av[i]) * h;
                st->ax[i] += st->av[i] * h;
                left -= h;
            }
            if (st->ax[i] > WRM_DB_AMP_MAX[i]) { st->ax[i] = WRM_DB_AMP_MAX[i]; if (st->av[i] > 0.0f) st->av[i] = 0.0f; }
            if (st->ax[i] < WRM_DB_AMP_QUIET) { st->ax[i] = WRM_DB_AMP_QUIET; if (st->av[i] < 0.0f) st->av[i] = 0.0f; }
            o->amp[i] = st->ax[i];
            lum3[i]   = 1.0f + present * (WRM_DB_LUM_MAX[i] - 1.0f) * s;
        }
    }

    // --- the sub-bass ripples ---
    {
        static const float ORIGIN[5] = { 0.50f, 0.36f, 0.62f, 0.44f, 0.56f };
        const float sf = wrm_clamp(sub_fast, 0.0f, 1.0f);
        const float ks = dt / (WRM_SUB_TAU + dt);
        const float excess = sf - st->sub_slow;
        float lift = 0.0f;
        int r;
        st->sub_slow += (sf - st->sub_slow) * ks;
        st->kick  = 0.0f;
        st->refr -= dt;
        if (st->shock_x == 0.0f && st->rt[0] == 0.0f && st->rt[1] == 0.0f) {
            // a zeroed state: no ripple is live yet
            for (r = 0; r < WRM_RIPPLES; r++) st->rt[r] = -1.0f;
            st->shock_x = 1.0f;
        }
        if (excess < 0.5f * WRM_SUB_THRESH) st->armed = 1;
        if (present > 0.0f && st->armed && st->refr <= 0.0f && excess > WRM_SUB_THRESH) {
            const float m = wrm_clamp(0.35f + (excess - WRM_SUB_THRESH) * 3.0f, 0.0f, 1.0f);
            int use = 0;
            for (r = 0; r < WRM_RIPPLES; r++) {
                if (st->rt[r] < 0.0f) { use = r; break; }
                if (st->rt[r] > st->rt[use]) use = r;   // else the oldest
            }
            st->rt[use] = 0.0f;
            st->rx[use] = ORIGIN[st->rn % 5];
            st->rm[use] = m;
            st->rn++;
            st->refr = WRM_SUB_REFR;
            st->kick = m * present;
            st->armed = 0;
        }
        for (r = 0; r < WRM_RIPPLES; r++) {
            const int s0 = WM_PULSES + 2 * r;
            o->acc.a[s0] = o->acc.a[s0 + 1] = 0.0f;
            o->acc.x[s0] = o->acc.x[s0 + 1] = 0.0f;
            o->acc.inv[s0] = o->acc.inv[s0 + 1] = 1.0f;
            if (st->rt[r] < 0.0f) continue;
            if (present <= 0.0f || st->rt[r] >= WRM_RIP_END) { st->rt[r] = -1.0f; continue; }
            {
                const float t    = st->rt[r];
                const float dq   = 1.0f + t / WRM_RIP_DECAY;
                const float env  = st->rm[r] * present
                                 * wrm_smooth01(t / WRM_RIP_ATT)
                                 * (1.0f / (dq * dq))
                                 * (1.0f - wrm_smooth01((t - (WRM_RIP_END - 0.5f)) / 0.5f));
                const float prog = t / (t + WRM_RIP_EASE);
                const float d    = WRM_RIP_SPREAD * prog;
                const float w    = WRM_RIP_W0 + WRM_RIP_W1 * prog;
                o->acc.x[s0]       = st->rx[r] - d;
                o->acc.x[s0 + 1]   = st->rx[r] + d;
                o->acc.inv[s0]     = o->acc.inv[s0 + 1] = 1.0f / w;
                o->acc.a[s0]       = o->acc.a[s0 + 1]   = WRM_RIP_A * env;
                if (env > lift) lift = env;
            }
            st->rt[r] += dt;
        }
        st->shock = lift;
        if (lift > 0.0f) {
            for (i = 0; i < 3; i++) {
                o->amp[i] += (WRM_DB_AMP_MAX[i] - o->amp[i]) * WRM_RIP_PUSH * lift;
                if (o->amp[i] > WRM_DB_AMP_MAX[i]) o->amp[i] = WRM_DB_AMP_MAX[i];
                lum3[i] += WRM_RIP_LUM * lift;
            }
        }
    }

    // The accents may add up to the measured bound and no further.
    {
        float sum = 0.0f;
        for (i = 0; i < WRM_ACC_SLOTS; i++) sum += o->acc.a[i];
        if (sum > WRM_ACC_TOTAL_MAX) {
            const float k = WRM_ACC_TOTAL_MAX / sum;
            for (i = 0; i < WRM_ACC_SLOTS; i++) o->acc.a[i] *= k;
        }
    }
}

#endif // WAVE_RENDER_MAP_H
