# The listening wave — audio-reactive XMB wave, design and derivation

Companion to `wave-spec.md`, which derives the *motion* model. This one derives
the *response* model: how the wave hears.

`wave-spec.md` established one solver and many looks. This document adds one
more principle on top of it, and everything else follows from it:

> **Frequency is depth.**
>
> Low frequencies move the far, large, slow geometry. High frequencies live on
> the near, fine, fast geometry. The spectrum is never drawn as a row of bars —
> it is drawn as *distance*.

That is the whole idea. A kick drum does not make a bar jump; it makes the
horizon swell. A hi-hat does not make a bar jump; it puts a grain on the
nearest filament. Nothing on screen is a readout of the audio, and yet
everything on screen is moving because of it.

---

## 0. Provenance

The reference material for this work is `audio_reactive_analysis.md` — notes on
a CRT-styled Wallpaper Engine music visualiser, produced outside this repo. It
is short and it is honest about being observational: a `project.json` property
list plus four sentences of visual description and a section explicitly headed
"Hypotheses".

What is actually transferable from it:

| Observation | What we take |
|---|---|
| `audioprocessing: true` | nothing; it is a feature flag |
| `animateWhenSilent: true` + `animateWhenSilentThreshold: 1` | **the principle that silence is a state with its own animation, and that the silence decision needs an explicit threshold.** §6 below. |
| "highs, mids and lows clearly separated" | that a small number of wide bands is enough; six, not twenty-eight. §2. |
| "8 lines representing the frequencies" | **rejected.** See below. |
| "dark blue → bright red by intensity" | **rejected.** See §5. |
| `wec_hue` / `wec_sa` / `wec_brs` / `wec_con` as separate knobs | that colour response is worth separating into hue / saturation / brightness terms driven by different features rather than one ramp. §5. |

The two rejections matter more than the acceptances. "One line per frequency
band" and "intensity ramps dark blue to bright red" *are* the generic
visualiser — they are a direct FFT-to-vertex mapping with a heat ramp on top,
which is precisely what the brief for this work rules out. They are recorded
here so nobody re-derives them later and thinks they were missed.

No code, shader, asset, geometry or visual design from that reference is
reproduced. Same standing rule as `wave-spec.md` §0 and `.clinerules` rule 10.

---

## 1. What the visual system looks like

### 1.1 The horizon

The wave occupies the bottom ~26 % of the screen and nothing else. The
remaining three quarters stay empty. This is not a compromise for performance —
it is the single strongest XMB cue there is. XMB's wave is a *horizon*: you
read the screen as a lit space with a far edge, and the furniture floats above
it. A wave that climbs into the middle of the screen stops being a horizon and
becomes wallpaper.

Design handoff §2.2 already fixes this at "bottom 200px" with `wave_alpha` 28
(≈11 % opacity) and a suppression to a third whenever a hero backdrop is on
screen. Both are kept exactly.

### 1.2 Four layers, one solver

`wave-spec.md` §4.1 concluded one solver, many looks. The layers are that
conclusion plus the depth principle:

| # | Name | Depth | Reads as | Driven by |
|---|---|---|---|---|
| 0 | **Swell** | furthest | a slow, almost-flat rise near the bottom edge; the lowest spatial frequency on screen | sub + bass, slowest envelope |
| 1 | **Body** | mid | the ribbon you would point at if asked "where is the wave" | bass + low-mid, slow-medium |
| 2 | **Filament** | near | a thin bright crest crossing the body at a shallow angle | mid, medium-fast |
| 3 | **Sheen** | nearest | a short travelling highlight, mostly absent | onsets only |

All four sample the **same spring chain**, stepped once per frame. They differ
in four cheap post-transforms: amplitude, vertical placement, an added
secondary sine at their own wavenumber, and colour. That is the entire cost of
a layer — no second solver, no second spline.

Depth is carried by the classical aerial-perspective cues, all of which happen
to be free:

- **contrast** falls with distance (layer 0 is dimmest)
- **spatial frequency** falls with distance (layer 0 has the longest wavelength)
- **speed** falls with distance (layer 0 has the slowest phase advance)
- **thickness** rises with distance (near things are thin and sharp)

Those four cues are also, in the same order, what the frequency bands do. Bass
is slow, large, broad and quiet at the back; treble is fast, fine, sharp and
bright at the front. **The perspective and the spectrum are the same gradient**,
which is why this reads as one image rather than as four meters.

### 1.3 Crossings

The XMB wave's real signature is not that curves move — it is that several
curves **cross each other at shallow angles**, and the crossings brighten
briefly as they pass. Parallel bands look like a graph. Crossing bands look
like light.

Crossings are produced by giving each layer its own secondary wavenumber
(§4.3) over the shared solver output, its own **horizontal offset** into that
output, and a layout tight enough for the two to reach across.

**All three of those are needed, and the first version had only the first.**
`test_wave_layers.c` counts sign changes in the difference between adjacent
layers' crests, and on the first run it found **zero crossings in four minutes
of music**. Two compounding causes, neither obvious:

1. Every layer carries the *same* base curve scaled by a *similar* amount
   (`WL_AMP` spanned only 0.036–0.075), so the base terms very nearly cancel in
   the difference between two layers. Layer 0 minus layer 1 kept 0.007 of it.
   The layers were separated by their rest positions and differentiated only by
   their much smaller secondary waves.
2. The excursion the layers actually use is about **a quarter** of what the
   amplitude budget allows — the budget is sized for a worst case needing the
   solver at its peak, every amplitude at 1 and all four pulses stacked on one
   sample. A quarter of it did not span a 0.078 gap.

The fix is `WL_SHIFT`, a per-layer horizontal offset into the shared curve, plus
closing the rest-position gaps to 0.036–0.047. Offsetting *where* a layer
samples the curve stops the base terms cancelling, because they are no longer
the same number — and it is what XMB itself visibly does, the same wave shape at
different horizontal offsets. It costs one lerp per sample.

Closing the gaps was the right second half rather than inflating amplitudes to
reach across them: overlapping translucent veils whose bright crests cross *is*
the XMB image, whereas ribbons far enough apart to need large amplitudes to meet
would be four separate waves that occasionally collide.

After both: **6,643 / 587 / 2,581** crossings for the three adjacent pairs over
four minutes. Not evenly distributed, and that is fine — an occasional crossing
is an event, and an event is what the eye notices.

### 1.4 CRT atmosphere, restrained

Two cues only, both free:

- **Chromatic spread at the crest.** `wave-spec.md` §3 recorded that the
  reference's bloom used a wider Gaussian radius for blue than for red in every
  single configuration, and that the resulting colour fringe is what the eye
  reads as glare. We do not get a bloom chain (§7), but we get the fringe for
  nothing: the crest's blue channel is ramped over a slightly taller vertical
  span than its red channel, so the top edge of each ribbon fringes cool and
  the underside fringes warm. One extra colour computation per vertex pair.
- **Scanline-scale banding in the crest ramp — DESIGNED, NOT BUILT.** The
  intent was to quantise the crest gradient to `ramp_steps` (a theme token that
  already exists) so it reads as phosphor banding on a CRT. It is not in the
  code, because it cannot be: `wr_build` emits exactly **two** vertices per
  column by contract, the GPU interpolates linearly between them, and steps
  need somewhere to live. Getting it would mean tessellating the ribbon
  vertically — a change to a tested stage, for a cosmetic gain, made blind
  without a TV to check it against. If it is wanted later it is a new builder
  alongside `wr_build`, not an edit to it. Recorded here rather than quietly
  dropped, because the first bullet alone is a thinner CRT reference than this
  section originally promised.

What we are **not** doing: scanline overlays, barrel distortion, phosphor
masks, chromatic aberration on the whole frame, bloom. Every one of those is a
fullscreen pixel pass, and §19 of the renderer brief plus every measurement in
`UI-BRIEF.md` says pixel traffic is this renderer's constraint. An atmosphere
built from three extra flops in a vertex colour is affordable; one built from
a second pass over 2 million pixels is not.

---

## 2. Which audio features drive which parameters

### 2.1 Six bands, not twenty-eight

`source/music/music_fft.cpp` already produces 28 log-spaced bands for the music
screen's bar display. **The wave does not reuse them**, for three specific
reasons, and this is the most important architectural decision in this document:

1. **It normalises to the frame maximum.** `music_viz_bands()` divides every
   band by the loudest band in that frame. That is correct for bars — it makes
   them dance at full scale regardless of level — and it is *fatal* here,
   because it destroys absolute loudness. A solo flute and a wall of
   distortion produce the same numbers. The wave's whole premise is that
   overall energy drives overall intensity, so it needs a level, not a shape.
2. **It costs a 2048-point FFT and 43 ms of latency.** The window is the
   latency: the newest sample is at the end of the window, so the envelope of a
   transient is smeared over the window length. For beats that is audible as
   lag.
3. **Twenty-eight bands is a resolution the wave cannot use.** Four layers and
   a handful of scalars need six drivers. Twenty-eight is twenty-two numbers to
   throw away.

So the wave gets its own front end, and it is cheaper than the FFT rather than
more expensive.

### 2.2 The bands

| i | Name | Nominal span | What lives there |
|---|---|---|---|
| 0 | `SUB` | 25 – 80 Hz | the deep swell, room tone, sub-bass |
| 1 | `BASS` | 80 – 250 Hz | kick body, bass guitar, low piano |
| 2 | `LOWMID` | 250 – 700 Hz | the body of most instruments |
| 3 | `MID` | 700 – 2 000 Hz | vocals, leads, snare crack |
| 4 | `HIGH` | 2 000 – 6 000 Hz | presence, consonants, attack |
| 5 | `AIR` | 6 000 – 14 000 Hz | cymbals, breath, space |

Spans are nominal. See §7.2 for why the filter is a difference of cascaded
one-pole lowpasses and what that does to the edges.

### 2.3 The mapping

| Feature | Drives | Why |
|---|---|---|
| `SUB` + `BASS` | layer 0/1 amplitude, and the solver's `drive` | the largest, slowest geometry answers the largest, slowest sound |
| `LOWMID` | layer 1's secondary wavenumber amplitude | adds a second, shorter wavelength to the body so it folds rather than just rising |
| `MID` | layer 2 amplitude and phase drift rate | the filament tracks the part of the mix a listener is following |
| `HIGH` + `AIR` | the solver's `perturb`, and the per-layer fine ripple | broadband forcing → fine spatial detail. This is the one place a band maps to something that looks like its own frequency. |
| broadband RMS | crest brightness, global alpha scale, `lift` | loudness is intensity, everywhere |
| onsets (spectral flux) | spawns a **travelling pulse**, never a flash | §3.4 |
| spectral centroid | hue, violet ↔ cyan | timbre is colour, slowly |
| estimated beat rate | the solver's `timescale` | §2.4 |

### 2.4 The one that makes it feel like listening

**Beat rate sets the drift rate, not the beat.**

Everything above is a level responding to a level. The thing that separates a
system that reacts from a system that *listens* is that the wave's baseline
motion — the speed at which its travelling waves drift when nothing in
particular is happening — settles to the song's tempo over several seconds.

A fast song's wave drifts faster. A slow song's wave drifts slower. Nothing
pulses on the beat, nothing flashes; the whole field simply moves at the pace
of the music. It is not consciously noticeable and that is exactly the point:
it is what makes the wave feel like it is in the same room as the music rather
than pointed at it.

Implementation is deliberately modest (§3.5): inter-onset intervals, octave
folded, heavily smoothed, gated on a confidence measure, and degrading to the
idle rate when confidence is low. It is not a beat tracker and does not pretend
to be one. It only needs to be right about *fast versus slow*, and it only
moves `timescale` between 0.7× and 1.6×.

---

## 3. How the signals are smoothed

### 3.1 The rule

> Nothing that comes out of the analyser touches geometry. Every driver passes
> through at least one envelope, and every geometry parameter passes through a
> slew limiter.

The slew limiter is the load-bearing one. It is what makes the elegance
guarantee *structural* rather than a matter of having picked good constants: no
matter what the analyser produces — a corrupt buffer, a codec glitch, a 0 dBFS
square wave — a geometry parameter physically cannot move faster than its
declared rate. Elegance is not tuned in; it is enforced.

### 3.2 The one-pole

```
k = dt / (tau + dt)          y += (x - y) * k
```

This is backward-Euler one-pole smoothing. It is chosen over the usual
`1 - exp(-dt/tau)` for three reasons: it needs no libm (house rule), `k` is in
`[0, 1)` for every positive `dt` and `tau` so it is unconditionally stable and
cannot overshoot, and it is correctly frame-rate independent — a dropped frame
with a doubled `dt` lands in the same place, which matters on a console that
drops frames when a poster decodes.

Attack and release use different `tau`. Everywhere in this system attack is
faster than release, because that is how hearing works: onsets are sharp and
decays are gradual, and a symmetric envelope makes music look like it is
breathing backwards.

### 3.3 Three response layers

Every driver is smoothed at three timescales simultaneously, and different
consumers read different ones:

| Layer | τ attack / release | Consumers |
|---|---|---|
| **SLOW** | 320 / 900 ms | structural amplitude, colour, `timescale`, global brightness |
| **MEDIUM** | 110 / 280 ms | secondary wave amplitudes, layer 2 |
| **FAST** | 25 / 70 ms | `perturb`, fine ripple, onset detection |

The difference `FAST - SLOW` is not noise — it *is* the transient content, and
it is what the onset detector consumes (§3.4). Keeping all three and
subtracting is cheaper and better behaved than a separate differentiator.

### 3.4 Onsets become travelling pulses

An onset does not set a parameter. It spawns an object:

```
pulse { x, velocity, amplitude, width, age, life }
```

which travels along the ribbon's normalised length, is added to the geometry as
a compact smooth bump, and fades out over its life. Up to four are alive at
once; a fifth onset replaces the oldest.

This is the single design decision that most separates this from a generic
visualiser. A beat that brightens the screen is a strobe. A beat that launches
something that then *travels for two beats and fades* is a gesture. The eye
reads the second one as the wave having been disturbed by the music — cause and
effect — rather than as the wave being redrawn from the music.

Two details make it work:

- **Direction alternates.** Left-to-right, then right-to-left. A fixed
  direction develops into a conveyor belt within about ten seconds.
- **Speed is tied to the beat estimate** so a pulse crosses the screen in
  roughly two beats. Pulses therefore overlap in a pattern that relates to the
  music's metre without anything being drawn on the beat.

Pulse amplitude is limited and pulses sum, so a dense passage produces a
continuously disturbed ribbon rather than four visible bumps — which is the
correct behaviour: dense music should look busy, not look like it has four
bumps in it.

### 3.5 Self-calibration, and the compression curve

Absolute level is useless without a reference: a quiet acoustic track and a
mastered-to-the-limit pop track differ by 20 dB, and either the first does
nothing or the second pins everything.

Each band tracks its own slow reference level (τ 2 s up, 12 s down, floored),
and the level handed onward is

```
level = energy / (energy + reference)
```

This is a soft-knee compressor. It is bounded in `[0, 1)` by construction, it
is monotone, it needs no libm, and it behaves like a logarithm across the two
decades in the middle — which is the range musical dynamics actually occupy.
Its fixed point is the useful part: **when a band sits at its own typical
level, it reads 0.5.** The mid-scale of the visual response automatically lands
on whatever "normal" is for the current track, within a few seconds of it
starting.

The reference's **floor is not optional**. Without it, silence drives the
reference to zero, and then the first floating-point denormal of noise reads as
full scale. That is how a silent screen ends up with a wave thrashing at
maximum amplitude. The floor is set at roughly −60 dBFS of band energy.

---

## 4. How the geometry responds

### 4.1 The seam — what is *not* changed

`wave_kernel.h`, `wave_spline.h` and `wave_ribbon.h` are not modified. Not one
line. They are correct, tested, documented and expensive to have got right, and
the audio system attaches to them at three places that already exist:

```
                      ┌──────────────── wave_audio.h ── PCM in, features out
                      │
                      ▼
             ┌─── wave_motion.h ── features + dt -> wm_params (smoothed, slewed)
             │                              │
   drive ────┤                              │  amp / detail / pulses / colour
   perturb ──┤                              │
   dt scale ─┘                              │
             ▼                              ▼
   wk_step ──> ws_build ──────────> wave_layers.h ──> wr_build ──> the renderer
   (unchanged) (unchanged)          (per-layer post-transform)     (unchanged)
```

Three of the audio-derived parameters go **into** the existing kernel through
arguments it already takes — `wk_step(c, dt, perturb, drive)`. `wave-spec.md`
§4.6 had already picked `TIMESTEP` and `PERTURBATION` as the two knobs worth
exposing; they turn out to be exactly the two an audio system wants, which is a
pleasant confirmation that the kernel was factored correctly.

Everything else attaches **after** the spline, as a post-transform on the dense
polyline. That placement is deliberate:

- The solver keeps its physics. Its stability argument, its energy bound and
  its CFL analysis are all untouched, because it never sees an audio-derived
  quantity other than two scalars it already clamps.
- The spline keeps its convex-hull guarantee, so the base curve still cannot
  overshoot its control points.
- Each layer's cost is its own post-transform and nothing else. One solve, one
  spline, four cheap passes.

### 4.2 The per-layer transform

For layer `L`, at normalised position `u ∈ [0,1]` along the ribbon:

```
y(u) =  base(u)  · amp[L] · LAYER_AMP[L]                  shared solver output
      + sec(u)   · LAYER_SEC_AMP[L] · med_drive           per-layer secondary wave
      + ripple(u)· LAYER_RIPPLE[L]  · detail[L]           treble grain
      + Σ pulses  bump(u − p.x, p.width) · p.amp · LAYER_PULSE[L]
      + LAYER_BASE_Y[L] + lift · LAYER_LIFT[L]            vertical placement
```

then soft-clipped into the band's vertical extent.

`sec()` is one sine at the layer's own wavenumber. `ripple()` is one sine at a
high wavenumber with a small amplitude. `bump()` is a compact quartic
`(1 − t²)²` rather than a Gaussian: it is C¹, it needs no `exp`, and it has
**compact support**, so a pulse only touches the samples within its own width
instead of every sample on the ribbon.

### 4.3 Wavenumbers

| Layer | rest y | secondary k | horizontal offset | drift rate | ripple k | half-thickness |
|---|---:|---:|---:|---:|---:|---:|
| 0 Swell | −0.855 | 0.75 | −0.12 | 0.093 | — | 0.105 |
| 1 Body | −0.808 | 1.60 | 0.00 | 0.171 | 11 | 0.082 |
| 2 Filament | −0.762 | 2.90 | +0.09 | 0.311 | 19 | 0.055 |
| 3 Sheen | −0.726 | 4.30 | +0.17 | 0.482 | 29 | 0.030 |

Rest y and half-thickness are clip space; drift rate is radians per second at
timescale 1; the horizontal offset is in normalised `u` along the ribbon.

The offsets are **clamped, not wrapped**. Wrapping would splice `u=1` onto `u=0`
and put a discontinuity at the seam; clamping repeats the curve's pinned end
value, so a shifted layer is flat *in its base term only* over the outermost
`|shift|` of one edge. Nothing is actually flat there — the secondary wave, the
ripple and the pulses are all still computed from the true `u` — and the outer
edge of the screen is where the wave flattens anyway.

The wavenumbers are mutually irrational-ish (0.75 / 1.60 / 2.90 / 4.30 share no
small common factor) so the crossing pattern does not repeat on a short cycle,
and they rise monotonically with depth, which is the spatial-frequency half of
the aerial-perspective cue in §1.2. The drift rates rise the same way: the
nearest layer moves fastest, as parallax requires.

### 4.4 Amplitude discipline

Every layer's total amplitude is bounded at build time, not clamped at runtime:
`WL_AMP·WL_BASE_MAX + WL_SEC_AMP + WL_RIP_AMP + WM_PULSES·WL_PULSE_AMP +
WL_LIFT` is chosen so the sum cannot leave the band even with every term at
maximum simultaneously — a combination that cannot actually occur. The clamp
stays in as a safety net but must never engage. Same discipline as
`wk_softclip`: a clip that runs is a bug report, not a feature.

`WL_BASE_MAX` is a **measured** constant, not `wk_softclip`'s 1.0 bound. Driving
the solver at the motion stage's worst case for ten minutes at 60 fps, the
spline peaks at **0.739**; 0.75 is that with margin, and sizing the layers
against 1.0 instead would waste a quarter of the band. Two things fell out of
that measurement that are not visible from the kernel:

- **Perturbation contributes almost nothing to peak amplitude.** 0.030 and 0.085
  both give 0.840 at drive 1.30, to three decimals. It is purely a liveliness
  term, exactly as `WK_NOISE_SCALE`'s own comment claims — now confirmed.
- **Peak amplitude is linear in drive**, at ≈0.672 per unit. That is what pinned
  `WM_MAX_DRIVE` at **1.10** rather than the 1.30 first written: at 1.30 the
  chain reaches 0.840, past `WK_KNEE` (0.80), so it spends part of its time
  inside its own soft clip. A solver sitting in its own clip has stopped being a
  solver — the clip is monotone but compressive, so loud passages would stop
  growing and the top of the dynamic range would flatten.

The test asserts the **margin**, not the range. The clamp makes "in range"
trivially true and would pass against a badly sized layer; how close the crest
came to the clamp over a long run is the claim actually being made. Measured
worst case over twelve minutes of silence, loud music and a full-scale square
wave: **0.105 below, 0.220 above**, against a band 0.54 tall.

---

## 5. Colour

Three independent terms, driven by three different features — the one idea
worth keeping from the reference's separate hue/saturation/brightness knobs:

| Term | Driven by | Range |
|---|---|---|
| **Hue** | spectral centroid, τ 1.5 s | deep indigo-violet ↔ cool cyan |
| **Brightness** | broadband RMS, slow | crest luminance only; the body barely changes |
| **Glow** | onset strength, fast decay | a brief lift on the crest of layers 2–3 |

Hue runs between the theme's own `accent` (violet `AA5CC3`) and `accent_alt`
(cyan `00A4DC`). It never reaches either endpoint — it travels the middle 60 %
of that line — because those two colours mean something specific in this UI
(handoff §2.1: "purple for what the user is pointing at, blue for what the file
is") and the wave must not compete with a focus ring. A theme that redefines
those two tokens gets a wave in its own palette for free, which is the whole
point of the theme layer.

**The centroid → hue mapping is slow on purpose.** A bright cymbal-heavy bar
should not turn the screen cyan. τ = 1.5 s means the colour describes the
*track*, not the bar: a dark dub record sits violet all the way through, a
bright acoustic record sits cool, and a track that opens sparse and builds
drifts across as it does. That is a colour that carries information and still
never draws attention to itself.

**Rejected:** the reference's dark-blue-to-bright-red intensity ramp. A hue
ramp driven by intensity is a heat map. Heat maps say "measurement". Also, red
does not exist anywhere in this UI's palette, and introducing it at the bottom
of every screen would wreck the restraint the rest of the design is built on.

---

## 6. Silence

Silence is a state with its own behaviour, not the absence of one — the one
genuinely useful idea in the reference material.

- The silence decision needs an explicit threshold and hysteresis, because
  level hovers around any single threshold during fades and gaps between
  tracks. Enter silence below the floor for 400 ms; leave it immediately above
  a higher floor.
- `silence ∈ [0, 1]` ramps over ≈1.2 s rather than switching, and blends every
  parameter toward the idle set.
- The idle set is **not zero**. It is `drive` 0.35, `perturb` 0.02,
  `timescale` 0.8, hue held at its last value, brightness at 0.55 of nominal.
  The spring chain's own perturbation term means that state still drifts
  indefinitely and never repeats — that behaviour was designed into
  `wave_kernel.h` from the start and is why the idle case needs no separate
  code path at all.
- Returning from silence is slower than entering it (τ 1.8 s rising). A track
  starting should feel like a room lighting up, not like a switch.

Concretely: with no music playing anywhere in the client, the wave looks like
the wave does today, slightly calmer. Nothing about the XMB is worse for the
audio system existing, which is the requirement.

---

## 7. Cost on the PS3

### 7.1 Budget

`UI-BRIEF.md`, measured 2026-09-19 at 1920×1080: 1,147 µs of real CPU work
against a 16.68 ms frame — **6.9 % utilised**, 11,087 µs of headroom. The wave
itself is 161 µs of GPU time for 586 vertices.

### 7.2 Analysis: a filterbank, not an FFT

The front end is **fourteen one-pole lowpass filters** — seven band edges,
cascaded twice each for a 12 dB/octave skirt — with each band taken as the
difference of two adjacent cascades.

```
a = w / (1 + w)          w = 2π·f_edge / f_s
lp += a · (x − lp)                                    ×2 per edge
band_i = lp_i − lp_{i+1}
```

Why this and not an FFT:

| | 2048-pt FFT | 14 one-poles |
|---|---|---|
| work / frame @ 48 kHz, 60 fps | ~22,500 butterflies | ~800 samples × 28 mul-add |
| latency | 43 ms (the window *is* the latency) | τ per band, chosen per band |
| coefficient cost | sine/cosine tables, bit-reversal table | `w/(1+w)`, no transcendentals at all |
| stability | n/a | `a ∈ (0,1)` for every `f` and every `f_s`; unconditional |
| artifacts | bin-boundary chatter, window scalloping | none; the skirts overlap smoothly |
| host-testable | needs the tables built | pure arithmetic, trivially |

Estimated ~15–25 µs per frame on the PPU, against 11,087 µs of headroom. If
that ever matters, the low four bands can run on audio decimated by 4 for a
further 3× cut, because a 700 Hz lowpass has no business seeing 48 kHz.

The overlapping skirts are a *feature*, not a compromise, and the measured
numbers say so precisely. At 48 kHz with a full-scale sine at each band's
geometric centre, every band's maximum is its own band — but an adjacent band
is only **0.3 – 4.6 dB** down, a band two away is 7 – 10 dB down going up and
19 – 26 dB going down, and anything further is 25 – 60 dB down.

Weak between neighbours, strong between the pairs that drive different things:
SUB/BASS against HIGH/AIR — the far geometry against the fine detail — are
**27 – 60 dB** apart. Neighbours blurring is what makes adjacent layers move as
one family with a phase gradient across them instead of independently, which is
what a physical medium does. Surgical separation would look like six meters.

A third cascaded pole was measured and is **worse**: the extra pole drags each
cascade's effective corner further below its nominal one, which moves every
band's peak up into the band above. Two poles is the right number, not a
budget compromise.

Note the honest caveat: `a = w/(1+w)` is the backward-Euler pole, which warps
the effective cutoff downward as `w` approaches 1 — so the AIR band's nominal
14 kHz upper edge is not its true −3 dB point at 44.1 kHz. That is fine; the
band edges here are aesthetic choices, and the host test asserts *relative
selectivity* (a 10 kHz tone puts its maximum in AIR, and AIR reads ≥20 dB above
MID) rather than exact corner frequencies.

### 7.3 Motion and geometry

| Stage | Per frame |
|---|---|
| features → envelopes → params | ~60 scalars, three one-poles each |
| pulse pool | 4 objects × ~8 flops |
| solver | unchanged — one 96-node chain |
| spline | unchanged — one pass to 72 samples |
| per-layer transform | 4 × 72 samples × ~14 flops ≈ 4,000 flops |
| ribbon build | 4 × 144 vertices = 576 |

**576 vertices against the current 586.** The audio-reactive wave draws no more
geometry than the wave drawing today, because the layers replace the current
three ribbons rather than adding to them. `test_wave_layers.c` asserts the
count.

Measured end to end on an x86 host — analysis, response, solver, spline, four
layer transforms, *and* the test's own signal generation, which is a
non-trivial share of it: **64 µs/frame**. That is not a PS3 number and the test
says so; the PPU is in-order with no speculation, so expect several times it.
Even at 5× it is ~320 µs against 11,087 µs of headroom, and that figure includes
work the console does not do.

### 7.4 Tiering

Per handoff §11 / §557:

| | FULL | REDUCED | MINIMAL |
|---|---|---|---|
| layers | 4 | 2 (Body, Filament) | 0 |
| filterbank | 6 bands | 3 bands (SUB+BASS, LOWMID+MID, HIGH+AIR) | not run |
| pulses | 4 | 2 | — |
| analysis rate | every frame | every other frame | — |
| ripple | on | off | — |

REDUCED halves the analysis rate rather than the band count first, because the
envelopes are frame-rate independent by construction (§3.2) — running the
filterbank every other frame over a double-length sample block costs the same
per sample and produces the same envelopes.

---

## 8. What is being built now, and what is not

**Built — host-testable, no console required:**

| File | Stage |
|---|---|
| `source/ui/render/wave_audio.h` | PCM → features |
| `source/ui/render/wave_motion.h` | features → slew-limited parameters |
| `source/ui/render/wave_layers.h` | parameters + spline → per-layer geometry and colour |
| `source/ui/render/wave_render_map.h` | parameters → this renderer's calibration |
| `tests/test_wave_audio.c` | selectivity, AGC convergence, onset counts, silence |
| `tests/test_wave_motion.c` | boundedness, **slew limits**, idle liveliness, determinism |
| `tests/test_wave_layers.c` | end-to-end, the mapping, and the knee assertion |

**Built — the console glue:**

| File | Role |
|---|---|
| `source/ui/render/ui_wave_audio.{h,cpp}` | the tap, the mutex, the clock and the gate |
| `source/music/music_player.cpp` | one added line, beside the existing visualiser tap |
| `source/ui/render/ui_wave.cpp` | the two `wf_step` literals become the live values |

It is live on the `wave_field` seam (§9), which is the conservative first step
that recommendation called for: the existing **three** ribbons, audio-reactive,
with `wave_layers.h` held back until there is a TV to judge it on.

All three headers follow `.clinerules` rule 7: header-only, pure C, no libm, no
PS3 headers, struct-of-arrays where it matters, deterministic, caller-owned
state and **no globals** — so the host test runs the same code the console
would, not a stand-in.

**How the three open questions were resolved:**

1. **The tap** sits in `music_read_pcm()`, one line below `music_viz_push()`,
   because that function's own comment already argues the case: those are the
   samples about to reach the hardware DMA ring, ~40 ms out, while the decode
   cursor can be ~700 ms ahead. The wave moves with what is *audible*.
2. **It is a music feature**, as expected — full-screen video covers the wave,
   so no tap was wired into the video decoder. This matches handoff §5.
3. **The gate defaults ON**, which departs from the card/text/vertex-array
   gates. Those guard RSX state binds, where a mistake wedges the GPU and needs
   a power cycle, so off-by-default is right. This path touches no GPU state;
   its worst failure is an ugly wave and its fallback is literally the constants
   the renderer passed before. `jellyfin_wavereact.txt` containing `0` turns it
   off over FTP with no reflash.

**The calibration decision, which was not obvious.** Stage B's ranges are
chosen for the *model*; `ui_wave.cpp`'s are chosen for the *screen* — it
divides `wf_disp` by `WF_NOMINAL_PEAK` to land the ribbons back on their
authored pixel heights, so pixel amplitude is proportional to drive and drive
1.0 is "today's look". Feeding one into the other directly gets both ends wrong:

- **At rest**, stage B idles at drive 0.35 → 35 % of today's ribbon height, about
  10/8/5 px. That does not read as calm, it reads as broken — and the XMB with
  nothing playing is the state this client spends most of its life in.
- **At full**, any mapping that scales past 1.10 puts the solver inside its own
  soft clip, where it stops responding to level at all.

`wave_render_map.h` is that compressed lerp: rest at **0.62** (visibly quieter,
still a wave — 19/14/9 px) and full at **1.10** (the knee limit). `dt_scale` and
`perturb` are instead normalised so **rest is bit-identical to today** — 1.0 and
0.02 — because the audio system existing must not make anything worse when there
is no audio. `test_wave_layers.c` asserts both endpoints, and re-measures the
knee property through `wf_step` (which scales `dt` by `WF_RATE` up to 1.34 and
`drive` by `WF_DRIVE` per layer, so checking a bare `wk_chain` would not have
checked what the renderer actually drives). Measured peaks across a dt sweep:
0.690–0.707 against `WK_KNEE` 0.80.

**Still not done:** nothing has been seen on a TV. Everything above is measured,
not looked at.

---

## 9. Overlap with `wave_field.h`

`source/ui/render/wave_field.h` and `tests/test_wave_field.c` appeared in this
tree **while this work was in progress**, from separate work. They are the other
half of the job in §8.3 — the renderer seam — and they are not a duplicate of
anything here, but they do overlap `wave_layers.h` and the two should not both
land unreconciled.

**Where they compose cleanly.** `wf_step(f, dt, perturb, drive)` takes exactly
the two knobs `wave_motion.h` produces, plus a `dt` that `timescale` scales.
So stage A and stage B drop straight onto the seam with no adapter at all:

```c
wm_update(&motion, &features, frame_dt);
wf_step(&field, WF_BASE_DT * motion.p.timescale,
        motion.p.perturb, motion.p.drive);
```

That alone makes the existing three-ribbon wave audio-reactive, and it is by far
the smallest first step worth shipping.

**Where they disagree.** Both answer "how do you get several distinct layers
from one solver", and they answer differently:

| | `wave_field.h` | `wave_layers.h` |
|---|---|---|
| layers | 3, matching `ui_wave.cpp`'s existing tables | 4 |
| solver instances | 3 chains | 1 chain |
| how layers differ | each chain stepped at its own `dt` rate | horizontal offset, secondary wavenumber, amplitude, colour |
| renderer interface | `wf_disp(f, l, u)` — crest height at a column | a polyline per layer, straight into `wr_build` |
| colour | left in `ui_wave.cpp` | derived from audio in `wl_shade` |

Neither is wrong. `wave_field.h` is the more conservative seam and changes less
on the renderer side; `wave_layers.h` carries the depth reading, the crossings
and the colour response that §1 is actually about, and costs one solver instead
of three.

**Resolved, for now, in `wave_field.h`'s favour** — and only as a first step.
The shipping path is its three chains, driven by stages A and B through
`wave_render_map.h`. That is the smallest change that makes the wave listen, it
leaves the renderer's authored amplitudes and colours exactly as they are, and
it can be turned off with one file over FTP.

`wave_layers.h` is **not** wired in and is not dead code: it is the four-layer
depth treatment, the crossings and the audio-driven colour, all tested, waiting
on the one thing no test can supply — somebody looking at the three-layer
version on a television first. The comparison above is the brief for that
decision. Do not merge the two layer models without making it.
