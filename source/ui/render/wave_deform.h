// JellyWave 2.x: deformation -- the living part of the wave's shape.
//
// wave_render_map.h's wrm_distinct() gives every layer its height (bass on
// the near layer, mids the middle, highs the far one), the beat punch and the
// sub-bass ripples.  This file is everything that changes the SHAPE along the
// band, as several overlapping influences rather than one dominant one:
//
//   swell     the lows, with INERTIA: a long, low undulation whose strength
//             rides an underdamped spring (~0.9 Hz), so it keeps rolling after
//             the bass has passed and never snaps
//   ripple    the mids: a travelling ripple plus a secondary one running the
//             other way, PHASE-MODULATED by the curve itself and by the
//             neighbouring layer's curve -- so it bends with the wave it rides
//             on and the layers visibly act on each other
//   warps     the beats: each onset spawns a local distortion (a crest with
//             troughs either side) that eases in, spreads, drifts and decays
//             over ~1 s -- a displacement, not a scale -- with a faint echo
//             0.12 s behind it (the afterimage, without any framebuffer read);
//             it reaches the near layer first and the far one ~0.14 s later,
//             which is what makes the three strips read as one volume
//   waveform  fine deformation from the scope trace, strongest far
//   pulse     a broad macro swell on the beat, eased in and out
//   stereo    the band leans toward, and ripples more on, the louder side
//   treble    the rim-pass brightness gain
//   glow      a brightness pulse that runs outward along the band from each
//             warp, layer by layer, plus a faint treble shimmer -- applied as
//             a per-station colour gain in the emit (wdf_glow)
//
// AUDIO SETS THE INTENSITY, TIME SETS THE EVOLUTION.  A few very slow,
// mutually incommensurate oscillators (periods 17-97 s) keep changing the
// ripple wavelength per layer, the overall deformation strength, the
// phase-modulation depth and each layer's phase drift, so the wave never
// settles into one loop -- within fixed bounds, with or without music.  The
// audio then scales everything through a NONLINEAR drama curve, so a loud
// passage is disproportionately more dramatic than a medium one.
//
// FRAMING.  Every sample's total is soft-clipped into
// (-WDF_NEG_MAX, +WDF_POS_MAX) x the layer weight -- smoothly, so a loud
// section rounds off instead of flattening -- and test_distinct_framing
// measures the band with that whole positive allowance on the crest.
//
// REST IS REST.  With no audio every amplitude eases to exactly 0, live goes
// false, and wdf_apply()/wdf_glow() touch nothing: the build stays
// bit-identical to the resting wave.
//
// House rules: header-only, pure C, no libm, no PS3 headers, caller-owned
// state.  tests/test_wave_scope.c.

#ifndef WAVE_DEFORM_H
#define WAVE_DEFORM_H

#include "wave_scope.h"
#include "wave_render_map.h"     // wrm_tempo_speed: the ripples keep the wave's pace

// amplitudes, in the solver's displacement units (as the accents)
#define WDF_MID_A      0.030f
#define WDF_SEC        0.40f     // secondary ripple, of the main one
#define WDF_WAVE_A     0.016f
#define WDF_PULSE_A    0.014f
#define WDF_TILT_A     0.020f
#define WDF_SWELL_A    0.034f
#define WDF_WARP_A     0.050f
#define WDF_POS_MAX    0.022f    // the framing allowance (per sample, x layer weight) -- measured
#define WDF_NEG_MAX    0.060f
#define WDF_RIM_GAIN   0.40f
#define WDF_GLOW_A     0.32f     // peak brightness gain of a travelling glow
#define WDF_SHIMMER_A  0.10f

#define WDF_WARPS      4
#define WDF_WARP_LIFE  1.10f     // s
#define WDF_LAYER_LAG  0.07f     // s, near -> mid -> far
#define WDF_ECHO_LAG   0.12f     // s
#define WDF_ECHO       0.35f

static const float WDF_LAYER[3]  = { 1.00f, 0.85f, 0.70f };
static const float WDF_WAVE_W[3] = { 0.40f, 0.70f, 1.00f };
static const float WDF_K1[3]     = { 1.70f, 2.40f, 3.10f };   // ripple cycles along the band
static const float WDF_K2[3]     = { 3.30f, 4.20f, 5.20f };
static const float WDF_PH[3]     = { 0.00f, 0.31f, 0.62f };

typedef struct {
    float wave[WSC_POINTS];
    float wave_a;           // waveform amplitude
    float mid_a;            // ripple amplitude (audio only)
    float mid_ph, sec_ph;   // ripple phases, cycles (travel opposite ways)
    float k1m[3];           // evolving wavelength multiplier per layer
    float phs[3];           // evolving per-layer phase drift, cycles
    float pm;               // phase-modulation depth, cycles per unit of curve
    float swell_a, swell_ph;
    float tilt, side;       // stereo
    float pulse;            // beat macro pulse
    float gain;             // drama x evolution strength, applied to the dynamic terms
    float warp_x[WDF_WARPS], warp_a[WDF_WARPS], warp_age[WDF_WARPS], warp_dir[WDF_WARPS];
    float shimmer, shimmer_ph;
    float rim;              // rim brightness gain, exactly 1 at rest
    float tint;             // -1 dark (violet) .. +1 bright (blue); 0 at rest
    float section;          // -1 calm .. +1 peak (chorus / drop)
    float bloom;            // 0..1, a drop's glow burst, decays
    float calm;             // 0..1, a vocal-led passage: the wave softens, particles slow
    float hat, hat_ph;      // hi-hat glints along the far ribbon's rim
    float bend;             // 808 glide: the near ribbon tilts with the slide, -1..1
    float width;            // stereo width: the ribbons spread apart in depth, 0..1
    int   live;             // 0 = add nothing, glow nothing
} wdf_look;

typedef struct {
    float lvl[3];       // wrm_db_state.lvl: shaped level per layer, 0..1
    float punch[3];     // wrm_db_state.punch: the beat per layer, 0..1
    float onset;        // 1 on the frame an onset fires
    float onset_strength;
    float tempo_hz, tempo_conf;
    float present;      // 0 at rest .. 1 with audio
    float energy;       // wrm_db_state.energy_eff: loud, compressed = high
    float centroid;     // wa_features.centroid: 0 dark .. 1 bright
    float bass_rel;     // wa_features.bass_rel: absolute bass vs its own peak (0 = none of it)
    float mid_rel;      // wa_features.mid_rel
    float air;          // wa_features.band_fast[WA_AIR]: the hi-hats live here
    const wsc_out *scope;   // may be NULL
} wdf_in;

typedef struct {
    float mid_a, pulse_tgt, pulse, rim, mid_ph, sec_ph;
    float t;                        // evolution clock, s (never reset)
    float phs[3];
    float sw_x, sw_v, swell_ph;     // the inertial swell
    float wx[WDF_WARPS], wa[WDF_WARPS], wage[WDF_WARPS], wdir[WDF_WARPS];
    int   wn;
    float shimmer, shimmer_ph;
    float drama;
    float calm, orate;              // vocal calm; onsets per second, smoothed
    float air_recent, hat, hat_ph;  // hi-hat detector
    float key_hue, bend;
    float tint;                     // smoothed centroid
    float e_mid, e_long, sect;      // section energy
    float calm_t, bloom, bloom_ref; // drop detection
    int   bloom_armed;              // re-armed once the rise has settled
    int   warm;                     // frames since the state was zeroed
} wdf_state;

// SECTIONS (2026-09-25).  MilkDrop-style players switch presets when the
// music's energy shifts; here the same signal eases the wave's character
// instead.  A mid-term (~1.5 s) and a long-term (~12 s) average of the level
// and the beat: their difference says whether this part of the song is
// calmer or fuller than the song around it.  Calm: longer, slower ripples,
// more swell, less drama.  Peak: shorter, faster ripples, more drama.  A
// calm stretch followed by a sharp rise is a DROP: one big central warp and a
// glow burst -- at most one per WDF_BLOOM_REFR.
#define WDF_SECT_SCALE  0.12f
#define WDF_BLOOM_CALM  2.0f     // s of calm before a rise counts as a drop
#define WDF_BLOOM_RISE  0.10f
#define WDF_BLOOM_SHARP 0.25f
#define WDF_BLOOM_REFR  8.0f
#define WDF_BLOOM_TAU   1.2f

// BEAT LOCK (2026-09-25).  With a trusted tempo the main ripple travels
// WDF_BEAT_WL of a wavelength per beat -- so the wave visibly pulses in
// time -- and each detected beat pulls the ripple's phase a little toward
// the beat grid, so the crests land ON the beats rather than merely at the
// right rate.  Blended in by the tempo confidence; free-running otherwise.
#define WDF_BEAT_WL     0.5f
#define WDF_BEAT_PULL   0.30f

// VOCAL CALM.  A passage carried by the mids (voice) while the lows, the
// highs and the beat fall away -- an a cappella line, a sung intro, a
// breakdown -- eases the wave: less deformation, slower ripples, and
// (through wdf_look.calm) slower, stiller particles.  Rises over ~1.5 s,
// lets go in ~0.6 s, so a drum fill ends it at once.
#define WDF_CALM_GAIN   0.40f    // deformation x (1 - this x calm)
#define WDF_CALM_SPEED  0.35f    // ripple speed x (1 - this x calm)

// 2026-09-26 hints (all small on purpose):
//   HATS   a fast rise in the air band (a hi-hat) lights glints that hop along
//          the far ribbon's RIM only -- rolls read as a shimmer on the edge
//   KEY    the tonal centre (wave_scope.h) picks a hue on the circle of
//          fifths, blended into the tint; a key change eases it over ~0.35 s
//          with a faint glow -- the "snap"
//   BEND   an 808 sliding in pitch tilts the near ribbon with the slide
//   WIDTH  a wide mix spreads the ribbons apart in depth (ui_wave.cpp)
#define WDF_HAT_GAIN    11.0f
#define WDF_HAT_RIM     1.20f    // peak rim gain of a glint (0.35 / 0.8 too subtle on a TV)
#define WDF_KEY_TINT    0.80f
#define WDF_BEND_A      0.028f

static inline float wdf_clamp(float v, float lo, float hi)
{
    if (v != v) return lo;
    return v < lo ? lo : (v > hi ? hi : v);
}

static inline float wdf_k(float dt, float tau) { return dt / (tau + dt); }

// sin(2 pi t) for any t, libm-free (Bhaskara on the folded phase, < 0.2%)
static inline float wdf_sin2pi(float t)
{
    float f = t - (float)(int)t, s = 1.0f, x;
    if (f < 0.0f) f += 1.0f;
    if (f >= 0.5f) { f -= 0.5f; s = -1.0f; }
    x = f * (0.5f - f);
    return s * 16.0f * x / (1.25f - 4.0f * x);
}

static inline float wdf_wrap(float p) { return p - (float)(int)p; }

static inline float wdf_smooth01(float x)
{
    x = wdf_clamp(x, 0.0f, 1.0f);
    return x * x * (3.0f - 2.0f * x);
}

// Smooth bound: linear up to 60% of the limit, then an ever-flatter approach
// that never reaches it -- a loud crest rounds off, it is never cut flat.
static inline float wdf_soft(float x, float hi)
{
    const float k = 0.6f * hi, r = hi - k;
    if (x <= k) return x;
    {
        const float e = (x - k) / r;
        return k + r * (e / (1.0f + e));
    }
}

static inline float wdf_softclip(float x, float lo, float hi)
{
    if (x != x) return 0.0f;
    return x >= 0.0f ? wdf_soft(x, hi) : -wdf_soft(-x, -lo);
}

// A warp's envelope at age a (s): eases in over 50 ms, decays, gone at the end.
static inline float wdf_warp_env(float a)
{
    float d;
    if (a <= 0.0f || a >= WDF_WARP_LIFE) return 0.0f;
    d = 1.0f + a / 0.22f;
    return wdf_smooth01(a / 0.05f) / (d * d)
         * (1.0f - wdf_smooth01((a - (WDF_WARP_LIFE - 0.35f)) / 0.35f));
}

// Crest with a trough either side (a compact "Mexican hat"), q in half-widths.
static inline float wdf_hat(float q)
{
    const float q2 = q * q;
    float t = 1.0f - q2 * 0.25f;
    if (t <= 0.0f) return 0.0f;
    t *= t;
    return (1.0f - 2.0f * q2) * t * t;
}

static inline void wdf_rest(wdf_look *o)
{
    int i;
    if (!o) return;
    for (i = 0; i < WSC_POINTS; i++) o->wave[i] = 0.0f;
    o->wave_a = o->mid_a = o->mid_ph = o->sec_ph = 0.0f;
    for (i = 0; i < 3; i++) { o->k1m[i] = 1.0f; o->phs[i] = 0.0f; }
    o->pm = 0.0f;
    o->swell_a = o->swell_ph = 0.0f;
    o->tilt = o->side = o->pulse = 0.0f;
    o->gain = 0.0f;
    for (i = 0; i < WDF_WARPS; i++) { o->warp_x[i] = 0.5f; o->warp_a[i] = 0.0f; o->warp_age[i] = 9.0f; o->warp_dir[i] = 1.0f; }
    o->shimmer = o->shimmer_ph = 0.0f;
    o->rim = 1.0f;
    o->tint = o->section = o->bloom = 0.0f;
    o->calm = 0.0f;
    o->hat = o->hat_ph = o->bend = o->width = 0.0f;
    o->live = 0;
}

static inline void wdf_map(wdf_state *st, const wdf_in *in, float dt, wdf_look *o)
{
    int i, anywarp = 0;
    float present, mids, highs, lows, fast, loud, ev_str;
    if (!st || !o) return;
    if (!in) { wdf_rest(o); return; }
    if (!(dt > 0.0f)) dt = 0.0f;
    present = wdf_clamp(in->present, 0.0f, 1.0f);
    lows  = wdf_clamp(0.6f * in->lvl[0] + 0.7f * in->punch[0], 0.0f, 1.0f);
    mids  = wdf_clamp(0.5f * in->lvl[1] + 0.8f * in->punch[1], 0.0f, 1.0f);
    highs = wdf_clamp(0.55f * in->lvl[2] + 0.9f * in->punch[2], 0.0f, 1.0f);
    // 0 at the resting pace .. 1 at the fastest, from the same mapping as the
    // base motion, so the ripples travel at the wave's own pace
    fast  = wdf_clamp((wrm_tempo_speed(in->tempo_hz, in->tempo_conf, in->energy) - 0.85f) / 0.65f,
                      0.0f, 1.0f) * present;

    // --- sections (and the drop) --------------------------------------------
    {
        const float lv = (in->lvl[0] + in->lvl[1] + in->lvl[2]) * (1.0f / 3.0f);
        const float pu = (in->punch[0] + in->punch[1] + in->punch[2]) * (1.0f / 3.0f);
        const float e  = present * (lv + 0.5f * pu);
        if (st->warm < 2) { st->e_mid = st->e_long = e; }
        if (st->warm < 1000000) st->warm++;
        st->e_mid  += (e - st->e_mid)  * wdf_k(dt, 1.5f);
        st->e_long += (e - st->e_long) * wdf_k(dt, 12.0f);
        {
            const float d = st->e_mid - st->e_long;
            const float sx = wdf_clamp(d / WDF_SECT_SCALE, -1.0f, 1.0f) * present;
            st->sect += (sx - st->sect) * wdf_k(dt, 1.2f);
            if (st->sect < -0.3f) st->calm_t += dt;
            else if (st->sect > 0.0f) st->calm_t -= 2.0f * dt;
            if (st->calm_t < 0.0f) st->calm_t = 0.0f;
            st->bloom_ref -= dt;
            st->bloom *= 1.0f / (1.0f + dt / WDF_BLOOM_TAU);
            // a drop: a rise after a calm stretch, or a rise sharp enough on its
            // own (the first drop after an intro has no louder "before")
            if (d < 0.5f * WDF_BLOOM_RISE) st->bloom_armed = 1;
            if (present > 0.0f && st->bloom_ref <= 0.0f && st->warm > 180 && st->bloom_armed &&
                ((st->calm_t >= WDF_BLOOM_CALM && d > WDF_BLOOM_RISE) || d > WDF_BLOOM_SHARP)) {
                st->bloom = 1.0f;
                st->bloom_ref = WDF_BLOOM_REFR;
                st->bloom_armed = 0;
                st->calm_t = 0.0f;
                {   // one big warp at the centre
                    int use = 0, j;
                    for (j = 0; j < WDF_WARPS; j++) if (st->wage[j] > st->wage[use]) use = j;
                    st->wx[use] = 0.5f; st->wdir[use] = 1.0f; st->wa[use] = 1.6f; st->wage[use] = 0.0f;
                }
            }
        }
    }

    // --- evolution: slow, incommensurate, independent of the beat ---------
    st->t += dt;
    if (st->t > 100000.0f) st->t -= 100000.0f;
    {
        const float t = st->t;
        ev_str = 1.0f + 0.15f * wdf_sin2pi(t / 41.0f) + 0.07f * wdf_sin2pi(t / 17.3f + 0.2f);
        for (i = 0; i < 3; i++) {
            o->k1m[i] = (1.0f + 0.22f * wdf_sin2pi(t / 53.0f + 0.33f * (float)i)
                              + 0.06f * wdf_sin2pi(t / 23.0f + 0.51f * (float)i))
                      * (1.0f - 0.12f * st->sect);    // peak: shorter ripples
            st->phs[i] = wdf_wrap(st->phs[i] + dt * 0.013f
                                  * (1.0f + 0.5f * wdf_sin2pi(t / 71.0f + (float)i * 0.29f)));
            o->phs[i] = st->phs[i];
        }
        o->pm = 1.0f + 0.4f * wdf_sin2pi(t / 29.0f + 0.1f);
        st->swell_ph = wdf_wrap(st->swell_ph + dt * (0.030f + 0.018f * wdf_sin2pi(t / 37.0f)));
        st->shimmer_ph = wdf_wrap(st->shimmer_ph + dt * (0.22f + 0.1f * wdf_sin2pi(t / 97.0f)));
    }

    // --- the drama curve: loud is disproportionately more ------------------
    loud = wdf_clamp(0.60f * lows + 0.15f * mids + 0.25f * highs, 0.0f, 1.0f);   // v8: the vocal layer no longer pumps, so the lows carry the drama
    {
        const float en = wdf_clamp(in->energy, 0.0f, 1.0f);
        const float d = present * (0.25f + 0.75f * loud * loud) * (1.0f + 0.8f * loud * loud * loud)
                      * (1.0f + 1.1f * en);          // a loud master is more dramatic (0.9 before v8)
        st->drama += (d - st->drama) * wdf_k(dt, d > st->drama ? 0.08f : 0.45f);
    }

    // mids -> the ripple: quick to swell, slower to settle
    {
        const float t = present * (0.30f + 0.70f * mids) * WDF_MID_A;
        st->mid_a += (t - st->mid_a) * wdf_k(dt, t > st->mid_a ? 0.05f : 0.35f);
    }
    // vocal calm: the mids lead, lows / highs / beat recede
    {
        const float side = in->lvl[0] > in->lvl[2] ? in->lvl[0] : in->lvl[2];
        const float pav  = (in->punch[0] + in->punch[1] + in->punch[2]) * (1.0f / 3.0f);
        float c;
        st->orate += ((in->onset > 0.0f ? 1.0f / (dt > 1e-4f ? dt : 1e-4f) : 0.0f) - st->orate)
                   * wdf_k(dt, 1.0f);
        // The bands are self-referenced, so neither "the mids lead" nor "the
        // bass dropped" shows in lvl[] on real mixes: they renormalise.  The
        // ABSOLUTE levels against their own recent peaks do: the bass well
        // under its peak (~6+ dB) while the mids stay near theirs, and the
        // beat's punch gone -- a vocal-led passage.
        (void)side;
        c = wdf_clamp((0.80f - in->bass_rel) * 4.0f, 0.0f, 1.0f)
          * wdf_clamp((in->mid_rel - 0.60f) * 3.0f, 0.0f, 1.0f)
          * wdf_clamp(1.0f - 2.5f * pav, 0.0f, 1.0f)
          * wdf_clamp(1.3f - 0.25f * st->orate, 0.0f, 1.0f) * present;   // sung phrases fire onsets too
        st->calm += (c - st->calm) * wdf_k(dt, c > st->calm ? 1.5f : 0.6f);
        if (present <= 0.0f && st->calm < 1e-3f) st->calm = 0.0f;
    }
    {
        float spd = (0.16f + 0.22f * fast + 0.14f * mids) * (1.0f + 0.30f * st->sect)
                  * (1.0f - WDF_CALM_SPEED * st->calm);
        const float g = wdf_smooth01((in->tempo_conf - 0.35f) / 0.30f) * present * (1.0f - st->calm);
        if (g > 0.0f && in->tempo_hz > 0.5f)
            spd = spd * (1.0f - g) + in->tempo_hz * WDF_BEAT_WL * g;
        st->mid_ph = wdf_wrap(st->mid_ph + dt * spd);
        if (g > 0.0f && in->onset > 0.0f) {
            // pull toward the nearest point of the beat grid (multiples of WL)
            float q = st->mid_ph / WDF_BEAT_WL;
            float e = q - (float)(int)(q + 0.5f);          // -0.5 .. 0.5 beats
            st->mid_ph = wdf_wrap(st->mid_ph - e * WDF_BEAT_WL * WDF_BEAT_PULL * g);
        }
    }
    st->sec_ph = wdf_wrap(st->sec_ph + dt * (0.07f + 0.15f * fast));

    // lows -> the swell, through an underdamped spring: inertia, overshoot,
    // rolls on after the peak
    {
        const float target = present * lows * WDF_SWELL_A * (1.0f - 0.20f * st->sect);
        const float w = 6.2831853f * 0.9f, zw = 2.0f * 0.45f * w;
        float left = dt;
        while (left > 0.0f) {
            const float h = left > 0.01f ? 0.01f : left;
            st->sw_v += (w * w * (target - st->sw_x) - zw * st->sw_v) * h;
            st->sw_x += st->sw_v * h;
            left -= h;
        }
    }

    // beats -> warps (and the broad pulse)
    if (in->onset > 0.0f && present > 0.0f) {
        const float s = 0.45f + 0.55f * wdf_clamp(in->onset_strength, 0.0f, 1.0f);
        static const float POS[7] = { 0.50f, 0.28f, 0.71f, 0.39f, 0.62f, 0.22f, 0.80f };
        int use = 0;
        for (i = 0; i < WDF_WARPS; i++) {
            if (st->wage[i] >= WDF_WARP_LIFE + 0.3f) { use = i; break; }
            if (st->wage[i] > st->wage[use]) use = i;
        }
        st->wx[use]   = POS[st->wn % 7];
        st->wdir[use] = (st->wn & 1) ? 1.0f : -1.0f;
        st->wa[use]   = s * (1.0f + 0.6f * wdf_clamp(in->energy, 0.0f, 1.0f));   // loud: bigger hits
        st->wage[use] = 0.0f;
        st->wn++;
        if (s > st->pulse_tgt) st->pulse_tgt = s;
    }
    for (i = 0; i < WDF_WARPS; i++) {
        if (st->wa[i] <= 0.0f) { st->wage[i] = 9.0f; continue; }
        st->wage[i] += dt;
        if (st->wage[i] >= WDF_WARP_LIFE + 3.0f * WDF_LAYER_LAG + WDF_ECHO_LAG) st->wa[i] = 0.0f;
        else anywarp = 1;
    }
    st->pulse_tgt *= 1.0f / (1.0f + dt / 0.20f);
    st->pulse += (st->pulse_tgt * present - st->pulse) * wdf_k(dt, 0.04f);

    // treble -> the rim, and the shimmer
    {
        const float t = 1.0f + WDF_RIM_GAIN * highs * present;
        st->rim += (t - st->rim) * wdf_k(dt, t > st->rim ? 0.02f : 0.16f);
        st->shimmer += (highs * present - st->shimmer) * wdf_k(dt, 0.12f);
    }

    if (present <= 0.0f) {
        // ease out, then snap to exact rest so the build is bit-identical
        if (st->mid_a < 1e-4f) st->mid_a = 0.0f;
        if (st->pulse < 1e-4f) { st->pulse = 0.0f; st->pulse_tgt = 0.0f; }
        if (st->rim - 1.0f < 1e-4f) st->rim = 1.0f;
        if (st->drama < 1e-3f) st->drama = 0.0f;
        if (st->shimmer < 1e-3f) st->shimmer = 0.0f;
        if (st->sw_x * st->sw_x + st->sw_v * st->sw_v < 1e-8f) { st->sw_x = 0.0f; st->sw_v = 0.0f; }
    }

    o->gain     = st->drama * wdf_clamp(ev_str, 0.75f, 1.25f) * (1.0f + 0.25f * st->sect)
                * (1.0f - WDF_CALM_GAIN * st->calm);
    o->calm     = st->calm;
    // hi-hats: the air band's rise over its own recent level
    {
        const float a = wdf_clamp(in->air, 0.0f, 1.0f);
        const float h = wdf_clamp((a - st->air_recent) * WDF_HAT_GAIN, 0.0f, 1.0f) * present;
        st->air_recent += (a - st->air_recent) * wdf_k(dt, a > st->air_recent ? 0.20f : 0.06f);
        if (h > st->hat + 0.25f) st->hat_ph = wdf_wrap(st->hat_ph + 0.137f);   // a new hit hops
        st->hat += (h - st->hat) * wdf_k(dt, h > st->hat ? 0.01f : 0.12f);
        st->hat_ph = wdf_wrap(st->hat_ph + dt * 0.9f);
        if (present <= 0.0f && st->hat < 1e-3f) st->hat = 0.0f;
        o->hat = st->hat; o->hat_ph = st->hat_ph;
    }
    // key -> hue on the circle of fifths; a change eases (the snap) + glows
    {
        float target = 0.0f;
        if (in->scope && in->scope->key >= 0 && present > 0.0f) {
            const int k5 = (in->scope->key * 7) % 12;
            target = ((float)k5 / 11.0f) * 2.0f - 1.0f;
            if (in->scope->key_changed && st->bloom < 0.60f) st->bloom = 0.60f;
        }
        st->key_hue += (target - st->key_hue) * wdf_k(dt, 0.35f);
        if (present <= 0.0f && st->key_hue * st->key_hue < 1e-6f) st->key_hue = 0.0f;
    }
    // 808 glide -> bend
    {
        const float g = in->scope ? wdf_clamp(in->scope->bass_glide * 0.45f, -1.0f, 1.0f) * present : 0.0f;
        st->bend += (g - st->bend) * wdf_k(dt, 0.06f);
        if (present <= 0.0f && st->bend * st->bend < 1e-6f) st->bend = 0.0f;
        o->bend = st->bend;
    }
    o->width = in->scope ? wdf_clamp(in->scope->width, 0.0f, 1.0f) * present : 0.0f;
    // brightness -> tint, slow so a hi-hat does not flicker the colour
    st->tint += (wdf_clamp(in->centroid, 0.0f, 1.0f) - st->tint) * wdf_k(dt, 1.2f);
    if (st->warm < 3) st->tint = wdf_clamp(in->centroid, 0.0f, 1.0f);
    o->tint    = wdf_clamp((st->tint - 0.5f) * 2.2f + WDF_KEY_TINT * st->key_hue, -1.0f, 1.0f) * present;
    o->section = st->sect;
    o->bloom   = st->bloom < 1e-3f ? 0.0f : st->bloom;
    o->mid_a    = st->mid_a;
    o->mid_ph   = st->mid_ph;
    o->sec_ph   = st->sec_ph;
    o->swell_a  = st->sw_x;
    o->swell_ph = st->swell_ph;
    o->pulse    = st->pulse * WDF_PULSE_A;
    o->rim      = st->rim;
    o->shimmer  = st->shimmer * WDF_SHIMMER_A;
    o->shimmer_ph = st->shimmer_ph;
    for (i = 0; i < WDF_WARPS; i++) {
        o->warp_x[i] = st->wx[i]; o->warp_a[i] = st->wa[i] * WDF_WARP_A;
        o->warp_age[i] = st->wage[i]; o->warp_dir[i] = st->wdir[i];
    }
    o->wave_a = present * WDF_WAVE_A * (0.35f + 0.65f * highs);
    if (in->scope) {
        o->tilt = in->scope->balance * WDF_TILT_A * present;
        o->side = in->scope->balance * present;
        for (i = 0; i < WSC_POINTS; i++) o->wave[i] = in->scope->wave[i];
    } else {
        o->tilt = o->side = 0.0f;
        for (i = 0; i < WSC_POINTS; i++) o->wave[i] = 0.0f;
    }
    if (present <= 0.0f) { o->wave_a = 0.0f; o->tilt = 0.0f; o->side = 0.0f; }
    // The dynamic terms all ride on gain; the rest stand on their own.
    o->live = (o->gain > 0.0f && (o->mid_a > 0.0f || o->pulse > 0.0f
                                  || o->swell_a != 0.0f || anywarp))
           || o->wave_a > 0.0f || o->tilt != 0.0f || o->shimmer > 0.0f || o->rim != 1.0f
           || anywarp || o->tint != 0.0f || o->bloom > 0.0f
           || o->hat > 0.0f || o->bend != 0.0f || o->width > 0.0f;
}

// One warp's contribution at u for a layer, including its echo.
static inline float wdf_warp_at(const wdf_look *d, int j, int layer, float u)
{
    const float age = d->warp_age[j] - (float)layer * WDF_LAYER_LAG;
    float acc = 0.0f;
    int   e;
    if (d->warp_a[j] <= 0.0f) return 0.0f;
    for (e = 0; e < 2; e++) {
        const float a   = age - (e ? WDF_ECHO_LAG : 0.0f);
        const float env = wdf_warp_env(a);
        if (env <= 0.0f) continue;
        {
            const float w  = 0.05f + 0.16f * a;               // spreads as it ages
            const float xc = d->warp_x[j] + d->warp_dir[j] * 0.10f * a;   // and drifts
            acc += (e ? WDF_ECHO : 1.0f) * env * wdf_hat((u - xc) / w);
        }
    }
    return acc * d->warp_a[j];
}

// Add the deformation to one layer's copy of the curve.  disp spans u in
// [0,1] over n samples, as wave_field.h's do.  couple: the neighbouring
// layer's curve over the same samples (NULL = none) -- it bends this layer's
// ripple, which is how the layers act on each other.
static inline void wdf_apply2(const wdf_look *d, int layer, float *disp,
                              const float *couple, int n)
{
    int k, j;
    float du, wl, hi, lo, g;
    if (!d || !d->live || !disp || n < 2) return;
    if (layer < 0) layer = 0;
    if (layer > 2) layer = 2;
    wl = WDF_LAYER[layer];
    hi = WDF_POS_MAX * wl;
    lo = -WDF_NEG_MAX * wl;
    du = 1.0f / (float)(n - 1);
    g  = d->gain;
    for (k = 0; k < n; k++) {
        const float u  = (float)k * du;
        const float c  = 2.0f * u - 1.0f;                 // -1 left .. +1 right
        const float cv = couple ? 0.6f * disp[k] + 0.4f * couple[k] : disp[k];
        float side = 1.0f + 0.8f * d->side * c;
        float x, dyn;
        if (side < 0.2f) side = 0.2f;
        // the ripple, bent by the curve it rides on (phase modulation)
        dyn = d->mid_a * wl * side
            * (wdf_sin2pi(WDF_K1[layer] * d->k1m[layer] * u - d->mid_ph
                          + WDF_PH[layer] + d->phs[layer] + d->pm * cv)
               + WDF_SEC * wdf_sin2pi(WDF_K2[layer] * u + d->sec_ph - WDF_PH[layer]
                                      - 0.5f * d->pm * cv));
        // the swell: long, slow, offset per layer
        dyn += d->swell_a * wl * wdf_sin2pi(0.8f * u + d->swell_ph + 0.12f * (float)layer + d->phs[layer]);
        // the warps
        for (j = 0; j < WDF_WARPS; j++) dyn += wl * wdf_warp_at(d, j, layer, u);
        // the broad beat pulse
        dyn += d->pulse * wl * (1.0f - c * c);
        x = dyn * g;
        // fine detail and the lean are not dramatised
        {
            const float s  = u * (float)(WSC_POINTS - 1);
            int         i  = (int)s;
            float       fr;
            if (i > WSC_POINTS - 2) i = WSC_POINTS - 2;
            fr = s - (float)i;
            x += d->wave_a * WDF_WAVE_W[layer]
               * (d->wave[i] + (d->wave[i + 1] - d->wave[i]) * fr);
        }
        x += d->tilt * wl * c;
        if (layer == 0) x += WDF_BEND_A * d->bend * c;    // the 808's slide
        disp[k] += wdf_softclip(x, lo, hi);
    }
}

static inline void wdf_apply(const wdf_look *d, int layer, float *disp, int n)
{
    wdf_apply2(d, layer, disp, 0, n);
}

// The travelling glow: a per-station brightness gain along the band (station
// i at u = i/(n-1)), >= 1.  Returns 0 when every gain is exactly 1 (the
// caller then skips the multiply altogether).
// The hi-hat glints: a rim-only gain per station, far ribbon only.  Returns 0
// when every gain is exactly 1.
static inline int wdf_rim_glow(const wdf_look *d, int layer, float *gain, int n)
{
    int i;
    float du;
    float k;
    if (!d || !d->live || layer < 1 || !(d->hat > 0.01f) || !gain || n < 2) return 0;
    k = layer == 2 ? 1.0f : 0.5f;                       // far ribbon full, middle half
    du = 1.0f / (float)(n - 1);
    for (i = 0; i < n; i++) {
        float s = wdf_sin2pi(7.0f * (float)i * du - d->hat_ph - 0.21f * (float)layer);
        s = s > 0.0f ? s * s : 0.0f;
        s *= s;                                         // glints (a touch wider, v10)
        gain[i] = 1.0f + WDF_HAT_RIM * k * d->hat * s;
    }
    return 1;
}

static inline int wdf_glow(const wdf_look *d, int layer, float *gain, int n)
{
    int i, j, any = 0;
    float du;
    if (!d || !d->live || !gain || n < 2) return 0;
    if (layer < 0) layer = 0;
    if (layer > 2) layer = 2;
    du = 1.0f / (float)(n - 1);
    for (i = 0; i < n; i++) {
        const float u = (float)i * du;
        float g = 0.0f;
        for (j = 0; j < WDF_WARPS; j++) {
            const float a = d->warp_age[j] - (float)layer * WDF_LAYER_LAG;
            float env, q1, q2, t1, t2, r;
            if (d->warp_a[j] <= 0.0f) continue;
            env = wdf_warp_env(a);
            if (env <= 0.0f) continue;
            r  = 0.55f * a;                                 // runs outward, both ways
            q1 = (u - (d->warp_x[j] - r)) * 9.0f;
            q2 = (u - (d->warp_x[j] + r)) * 9.0f;
            t1 = 1.0f - q1 * q1; t2 = 1.0f - q2 * q2;
            t1 = t1 > 0.0f ? t1 * t1 : 0.0f;
            t2 = t2 > 0.0f ? t2 * t2 : 0.0f;
            g += (d->warp_a[j] / WDF_WARP_A) * env * (t1 + t2);
        }
        g *= WDF_GLOW_A * WDF_LAYER[layer];
        if (d->bloom > 0.0f) {
            const float c = 2.0f * u - 1.0f;
            g += d->bloom * 0.30f * WDF_LAYER[layer] * (1.0f - c * c);
        }
        if (d->shimmer > 0.0f) {
            float s = wdf_sin2pi(2.6f * u - d->shimmer_ph + 0.2f * (float)layer);
            s = s > 0.0f ? s * s : 0.0f;
            g += d->shimmer * s * s;
        }
        if (g > WDF_GLOW_A + WDF_SHIMMER_A + 0.15f) g = WDF_GLOW_A + WDF_SHIMMER_A + 0.15f;
        gain[i] = 1.0f + g;
        any |= g > 0.0f;
    }
    return any;
}

#endif // WAVE_DEFORM_H
